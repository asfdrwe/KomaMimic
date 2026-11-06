// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <QObject>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <array>
#include <atomic>
#include <functional>
#include <memory>

// GUI-thread request queues. Only running work is submitted to each pool, so
// obsolete waiting work can be removed without occupying a worker or a future.
class DisplayJobs : public QObject {
public:
    enum class Lane { Cache, Display, Background, Update, Store, Navigation, Render };
    using Cancel = std::function<bool()>;
    explicit DisplayJobs(QObject *parent=nullptr);
    ~DisplayJobs() override;
    template<class T, class Work, class Done>
    bool submit(Lane lane, const QString &scope, const QString &id, Work work, Done done,bool retryErrors=false,bool offscreen=false) {
        auto value=std::make_shared<T>();
        return add(lane,scope,id,
            [value,work=std::move(work)](const Cancel &cancel) { *value=work(cancel); },
            [value,done=std::move(done)](bool cancelled) { done(std::move(*value),cancelled); },
            [value,retryErrors] {
                if constexpr(requires {value->error.isEmpty();})return retryErrors && !value->error.isEmpty();
                else return false;
            },offscreen);
    }
    bool contains(const QString &id) const;
    void retain(const QString &scope,const QSet<QString> &ids={});
    // Keep requested work when it leaves the viewport; only its priority changes.
    void visible(const QString &scope,const QSet<QString> &ids={});
    void cancelBackground();
    void stop();
    void waitForDone();
    bool displayBusy() const;
    bool active() const;
private:
    struct Job {
        Lane lane,foreground; QString scope,id;
        bool restart=false, persistent=false;
        qint64 queuedAt=0,startedAt=0,readyAt=0;
        std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
        std::function<void(const Cancel &)> work;
        std::function<void(bool)> done;
        std::function<bool()> retry;
    };
    bool add(Lane,const QString &,const QString &,std::function<void(const Cancel &)>,std::function<void(bool)>,std::function<bool()>,bool);
    void pump();
    std::array<QThreadPool,7> m_pools;
    std::array<QList<std::shared_ptr<Job>>,7> m_waiting;
    std::array<std::shared_ptr<Job>,7> m_running;
    QHash<QString,std::shared_ptr<Job>> m_jobs;
    QTimer m_idleTimer;
    QElapsedTimer m_activity,m_clock;
    bool m_stopping=false;
};
