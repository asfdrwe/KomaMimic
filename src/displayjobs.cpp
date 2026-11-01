// SPDX-License-Identifier: GPL-3.0-only
#include "displayjobs.h"
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(displayJobsLog,"komamimic.display",QtWarningMsg)

DisplayJobs::DisplayJobs(QObject *parent):QObject(parent) {
    for(auto &pool:m_pools)pool.setMaxThreadCount(1);
    m_activity.start();m_clock.start();m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer,&QTimer::timeout,this,[this]{pump();});
}
DisplayJobs::~DisplayJobs() {
    stop();
    for(auto &pool:m_pools)pool.waitForDone();
}
bool DisplayJobs::contains(const QString &id) const {
    const auto job=m_jobs.value(id);return job && !job->cancelled->load();
}
bool DisplayJobs::displayBusy() const {
    return m_running[0] || m_running[1] || !m_waiting[0].isEmpty() || !m_waiting[1].isEmpty();
}
bool DisplayJobs::add(Lane lane,const QString &scope,const QString &id,
                      std::function<void(const Cancel &)> work,std::function<void(bool)> done) {
    if(m_stopping || contains(id))return false;
    auto job=std::make_shared<Job>();job->lane=lane;job->scope=scope;job->id=id;
    job->work=std::move(work);job->done=std::move(done);
    job->queuedAt=m_clock.elapsed();
    m_jobs[id]=job;m_waiting[int(lane)].append(job);
    qCDebug(displayJobsLog)<<"queued"<<int(lane)<<scope;
    if(lane!=Lane::Background) {m_activity.restart();cancelBackground();}
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
        if(job && job->scope==scope && !ids.contains(job->id))job->cancelled->store(true);
    // Cancellation callbacks run after queue mutation and never run worker code.
    for(const auto &job:removed)job->done(true);
    pump();
}
void DisplayJobs::cancelBackground() {
    if(m_running[2])m_running[2]->cancelled->store(true);
}
void DisplayJobs::stop() {
    m_stopping=true;m_idleTimer.stop();
    for(auto &queue:m_waiting) {for(auto &job:queue)job->cancelled->store(true);queue.clear();}
    for(auto &job:m_running)if(job)job->cancelled->store(true);
    m_jobs.clear();
}
void DisplayJobs::pump() {
    if(m_stopping)return;
    for(int lane=0;lane<3;++lane) {
        if(m_running[lane] || m_waiting[lane].isEmpty())continue;
        if(lane==2) {
            if(displayBusy())continue;
            if(m_activity.elapsed()<75) {m_idleTimer.start(int(75-m_activity.elapsed()));continue;}
        }
        auto job=m_waiting[lane].takeFirst();m_running[lane]=job;job->startedAt=m_clock.elapsed();
        qCDebug(displayJobsLog)<<"start"<<lane<<job->scope<<"queue-ms"<<job->startedAt-job->queuedAt;
        auto *watcher=new QFutureWatcher<void>(this);
        connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,job,lane] {
            watcher->deleteLater();m_running[lane].reset();
            qCDebug(displayJobsLog)<<"finish"<<lane<<job->scope<<"run-ms"<<m_clock.elapsed()-job->startedAt<<"cancelled"<<job->cancelled->load();
            if(m_jobs.value(job->id)==job)m_jobs.remove(job->id);
            if(lane!=2)m_activity.restart();
            if(!m_stopping)job->done(job->cancelled->load());
            pump();
        });
        watcher->setFuture(QtConcurrent::run(&m_pools[lane],[job] {
            if(!job->cancelled->load())job->work([stop=job->cancelled]{return stop->load();});
        }));
    }
}
