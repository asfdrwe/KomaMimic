// SPDX-License-Identifier: GPL-3.0-only
#include "displayjobs.h"
#include <QLoggingCategory>
#include <algorithm>

Q_LOGGING_CATEGORY(displayJobsLog,"komamimic.display",QtWarningMsg)

DisplayJobs::DisplayJobs(QObject *parent):QObject(parent) {
    for(auto &pool:m_pools)pool.setMaxThreadCount(1);
    for(auto lane:{Lane::Background,Lane::Update,Lane::Store})m_pools[int(lane)].setThreadPriority(QThread::LowPriority);
    m_activity.start();m_clock.start();m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer,&QTimer::timeout,this,[this]{pump();});
}
DisplayJobs::~DisplayJobs() {
    stop();
    waitForDone();
}
void DisplayJobs::waitForDone() {for(auto &pool:m_pools)pool.waitForDone();}
bool DisplayJobs::contains(const QString &id) const {
    const auto job=m_jobs.value(id);return job && (job->restart || !job->cancelled->load());
}
bool DisplayJobs::displayBusy() const {
    return m_running[0] || m_running[1] || !m_waiting[0].isEmpty() || !m_waiting[1].isEmpty();
}
bool DisplayJobs::active() const {
    for(const auto &job:m_running)if(job)return true;
    return false;
}
bool DisplayJobs::add(Lane lane,const QString &scope,const QString &id,
                      std::function<void(const Cancel &)> work,std::function<void(bool)> done,std::function<bool()> retry,bool offscreen) {
    if(m_stopping || contains(id))return false;
    auto job=std::make_shared<Job>();job->lane=job->foreground=lane;job->scope=scope;job->id=id;
    if(offscreen){job->lane=Lane::Background;job->persistent=true;}
    job->work=std::move(work);job->done=std::move(done);
    job->retry=std::move(retry);
    job->queuedAt=m_clock.elapsed();
    m_jobs[id]=job;m_waiting[int(job->lane)].append(job);
    qCDebug(displayJobsLog)<<"queued"<<int(job->lane)<<scope<<id;
    if(!offscreen && (lane==Lane::Cache || lane==Lane::Display)) {m_activity.restart();cancelBackground();}
    pump();return true;
}
void DisplayJobs::retain(const QString &scope,const QSet<QString> &ids) {
    QList<std::shared_ptr<Job>> removed;
    for(auto &queue:m_waiting)for(auto i=queue.begin();i!=queue.end();) {
        auto job=*i;
        if(job->scope!=scope || ids.contains(job->id)){++i;continue;}
        job->restart=false;job->cancelled->store(true);i=queue.erase(i);
        if(m_jobs.value(job->id)==job)m_jobs.remove(job->id);
        removed.append(job);
    }
    for(const auto &job:m_running)
        if(job && job->scope==scope && !ids.contains(job->id)){job->restart=false;job->cancelled->store(true);}
    // Cancellation callbacks run after queue mutation and never run worker code.
    for(const auto &job:removed)job->done(true);
    pump();
}
void DisplayJobs::visible(const QString &scope,const QSet<QString> &ids) {
    QList<std::shared_ptr<Job>> moved;
    for(auto &queue:m_waiting)for(auto i=queue.begin();i!=queue.end();) {
        const auto job=*i;
        if(job->scope==scope)job->persistent=true;
        const auto target=ids.contains(job->id)?job->foreground:Lane::Background;
        if(job->scope!=scope || job->lane==target){++i;continue;}
        i=queue.erase(i);job->lane=target;moved.append(job);
    }
    for(const auto &job:moved)m_waiting[int(job->lane)].append(job);
    for(const auto &job:m_running)if(job && job->scope==scope) {
        job->persistent=true;
        const auto target=ids.contains(job->id)?job->foreground:Lane::Background;
        if(job->lane!=target) {
            job->lane=target;job->restart=true;job->cancelled->store(true);
        }
    }
    pump();
}
void DisplayJobs::cancelBackground() {
    if(m_running[2] && !m_running[2]->guaranteed) {
        if(m_running[2]->persistent)m_running[2]->restart=true;
        m_running[2]->cancelled->store(true);
    }
}
void DisplayJobs::stop() {
    m_stopping=true;m_idleTimer.stop();
    for(auto &queue:m_waiting) {for(auto &job:queue)job->cancelled->store(true);queue.clear();}
    for(auto &job:m_running)if(job)job->cancelled->store(true);
    m_jobs.clear();
}
void DisplayJobs::pump() {
    if(m_stopping)return;
    for(int lane:{0,1,3,4,2}) {
        if(m_running[lane] || m_waiting[lane].isEmpty())continue;
        auto ready=std::find_if(m_waiting[lane].begin(),m_waiting[lane].end(),[this](const auto &job){return job->readyAt<=m_clock.elapsed();});
        if(ready==m_waiting[lane].end()){m_idleTimer.start(25);continue;}
        if(ready!=m_waiting[lane].begin())m_waiting[lane].move(int(ready-m_waiting[lane].begin()),0);
        const auto age=m_clock.elapsed()-m_waiting[lane].first()->queuedAt;
        if(lane==2 || lane==3) {
            const int maximumWait=lane==3?250:1000;
            if(age<maximumWait && (displayBusy() || m_activity.elapsed()<75)) {
                m_idleTimer.start(25);continue;
            }
        }
        auto job=m_waiting[lane].takeFirst();m_running[lane]=job;job->startedAt=m_clock.elapsed();
        job->guaranteed=lane==2 && age>=1000;
        qCDebug(displayJobsLog)<<"start"<<lane<<job->scope<<job->id<<"queue-ms"<<job->startedAt-job->queuedAt;
        auto *watcher=new QFutureWatcher<void>(this);
        connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,job,lane] {
            watcher->deleteLater();m_running[lane].reset();
            qCDebug(displayJobsLog)<<"finish"<<lane<<job->scope<<job->id<<"run-ms"<<m_clock.elapsed()-job->startedAt<<"cancelled"<<job->cancelled->load();
            if(job->restart && !m_stopping && m_jobs.value(job->id)==job) {
                job->restart=false;job->cancelled=std::make_shared<std::atomic<bool>>(false);
                m_waiting[int(job->lane)].append(job);pump();return;
            }
            if(!m_stopping && !job->cancelled->load() && job->retry()) {
                job->queuedAt=m_clock.elapsed();job->readyAt=job->queuedAt+1000;
                m_waiting[int(job->lane)].append(job);
                job->done(false);pump();return;
            }
            if(m_jobs.value(job->id)==job)m_jobs.remove(job->id);
            if(lane==0 || lane==1)m_activity.restart();
            if(!m_stopping)job->done(job->cancelled->load());
            pump();
        });
        watcher->setFuture(QtConcurrent::run(&m_pools[lane],[job] {
            if(!job->cancelled->load())job->work([stop=job->cancelled]{return stop->load();});
        }));
    }
}
