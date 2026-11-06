// SPDX-License-Identifier: GPL-3.0-only
#include "displayjobs.h"
#include <QLoggingCategory>
#include <algorithm>

Q_LOGGING_CATEGORY(displayJobsLog,"komamimic.display",QtWarningMsg)

DisplayJobs::DisplayJobs(QObject *parent):QObject(parent) {
    for(auto &pool:m_pools){pool=std::make_shared<QThreadPool>();pool->setMaxThreadCount(1);}
    for(auto lane:{Lane::Background,Lane::Update,Lane::Store})m_pools[int(lane)]->setThreadPriority(QThread::LowPriority);
    m_activity.start();m_clock.start();m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer,&QTimer::timeout,this,[this]{pump();});
}
DisplayJobs::~DisplayJobs() {
    stop();
    waitForDone();
}
void DisplayJobs::waitForDone() {
    for(auto &pool:m_pools)pool->waitForDone();
    for(const auto &job:m_retired)job->pool->waitForDone();
}
bool DisplayJobs::contains(const QString &id) const {
    const auto job=m_jobs.value(id);return job && !job->cancelled->load();
}
bool DisplayJobs::displayBusy() const {
    for(int lane:{0,1,5,6}) {
        if(m_running[lane] && !m_running[lane]->cancelled->load())return true;
        for(const auto &job:m_waiting[lane])if(!job->cancelled->load() && job->readyAt<=m_clock.elapsed())return true;
    }
    return false;
}
bool DisplayJobs::active() const {
    for(const auto &job:m_running)if(job)return true;
    return !m_retired.isEmpty();
}
bool DisplayJobs::add(Lane lane,const QString &scope,const QString &id,
                      std::function<void(const Cancel &)> work,std::function<void(bool)> done,std::function<bool()> retry,bool offscreen) {
    if(m_stopping || contains(id))return false;
    auto job=std::make_shared<Job>();job->lane=lane;job->scope=scope;job->id=id;
    if(offscreen)return false;
    job->work=std::move(work);job->done=std::move(done);
    job->retry=std::move(retry);
    job->queuedAt=m_clock.elapsed();
    m_jobs[id]=job;m_waiting[int(job->lane)].append(job);
    qCDebug(displayJobsLog)<<"queued"<<int(job->lane)<<scope<<id;
    if(!offscreen && (lane==Lane::Cache || lane==Lane::Display || lane==Lane::Navigation || lane==Lane::Render)) {m_activity.restart();cancelBackground();}
    pump();return true;
}
void DisplayJobs::retain(const QString &scope,const QSet<QString> &ids) {
    QList<std::shared_ptr<Job>> removed;
    for(auto &queue:m_waiting)for(auto i=queue.begin();i!=queue.end();) {
        auto job=*i;
        if(job->scope!=scope || ids.contains(job->id)){++i;continue;}
        job->cancelled->store(true);i=queue.erase(i);
        if(m_jobs.value(job->id)==job)m_jobs.remove(job->id);
        removed.append(job);
    }
    for(const auto &job:m_running)
        if(job && job->scope==scope && !ids.contains(job->id)){job->cancelled->store(true);}
    // Cancelled native I/O may take time to return. Retire its foreground
    // executor so the new view never waits for the old invocation to finish.
    // Keep the future/pool alive until completion; never resume the old work.
    for(int lane:{0,1,5,6})if(const auto job=m_running[lane];job && job->cancelled->load()) {
        m_retired.append(job);m_running[lane].reset();
        m_pools[lane]=std::make_shared<QThreadPool>();m_pools[lane]->setMaxThreadCount(1);
    }
    // Cancellation callbacks run after queue mutation and never run worker code.
    for(const auto &job:removed)job->done(true);
    pump();
}
void DisplayJobs::visible(const QString &scope,const QSet<QString> &ids) {
    retain(scope,ids);
}

void DisplayJobs::cancelBackground() {
    if(m_running[2]) {
        m_running[2]->cancelled->store(true);
    }
}
void DisplayJobs::stop() {
    m_stopping=true;m_idleTimer.stop();
    for(auto &queue:m_waiting) {for(auto &job:queue)job->cancelled->store(true);queue.clear();}
    for(auto &job:m_running)if(job)job->cancelled->store(true);
    for(const auto &job:m_retired)job->cancelled->store(true);
    m_jobs.clear();
}
void DisplayJobs::pump() {
    if(m_stopping)return;
    for(int lane:{0,6,5,1,2,3,4}) {
        // Deliver the current viewport cache before starting source work or writes.
        if(lane!=0 && (m_running[0] || !m_waiting[0].isEmpty()))continue;
        if(m_running[lane] || m_waiting[lane].isEmpty())continue;
        auto ready=std::find_if(m_waiting[lane].begin(),m_waiting[lane].end(),[this](const auto &job){return job->readyAt<=m_clock.elapsed();});
        if(ready==m_waiting[lane].end()){m_idleTimer.start(25);continue;}
        if(ready!=m_waiting[lane].begin())m_waiting[lane].move(int(ready-m_waiting[lane].begin()),0);
        if(lane==2 || lane==3 || lane==4) {
            // Current navigation and visible misses outrank all speculative,
            // validation and persistence work, regardless of queue age.
            if(displayBusy() || m_activity.elapsed()<75 || (lane==4 && (m_running[2] || !m_waiting[2].isEmpty()))) {
                m_idleTimer.start(25);continue;
            }
        }
        auto job=m_waiting[lane].takeFirst();m_running[lane]=job;job->startedAt=m_clock.elapsed();
        qCDebug(displayJobsLog)<<"start"<<lane<<job->scope<<job->id<<"queue-ms"<<job->startedAt-job->queuedAt;
        auto *watcher=new QFutureWatcher<void>(this);
        connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,job,lane] {
            watcher->deleteLater();if(m_running[lane]==job)m_running[lane].reset();
            // The future has finished all user work. Join only a retired
            // executor before releasing its last owner on the GUI thread;
            // otherwise its destructor could run on its own worker.
            if(m_retired.contains(job))job->pool->waitForDone();
            m_retired.removeAll(job);
            qCDebug(displayJobsLog)<<"finish"<<lane<<job->scope<<job->id<<"run-ms"<<m_clock.elapsed()-job->startedAt<<"cancelled"<<job->cancelled->load();
            if(!m_stopping && !job->cancelled->load() && job->retry()) {
                job->queuedAt=m_clock.elapsed();job->readyAt=job->queuedAt+1000;
                m_waiting[int(job->lane)].append(job);
                job->done(false);pump();return;
            }
            if(m_jobs.value(job->id)==job)m_jobs.remove(job->id);
            if(lane==0 || lane==1 || lane==5 || lane==6)m_activity.restart();
            if(!m_stopping)job->done(job->cancelled->load());
            pump();
        });
        job->pool=m_pools[lane];
        watcher->setFuture(QtConcurrent::run(job->pool.get(),[job] {
            if(!job->cancelled->load())job->work([stop=job->cancelled]{return stop->load();});
        }));
    }
}
