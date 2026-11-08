#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace editor::core {

// Un gruppo di thread fissi per i lavori a bande (compositore, conversioni di colore): creare e distruggere thread a ogni
// fotogramma costa quanto il lavoro risparmiato. Il thread che chiama partecipa al lavoro e aspetta la fine.
class ThreadPool
{
public:
    static ThreadPool& instance()
    {
        static ThreadPool pool;
        return pool;
    }

    // Quanti thread lavorano in tutto (il chiamante compreso): i core del computer, al massimo 8.
    int size() const { return static_cast<int>(workers_.size()) + 1; }

    // Esegue fn(0) ... fn(tasks - 1), ognuna una volta, su thread diversi; ritorna quando sono finite tutte.
    // fn non deve lanciare eccezioni. Si può chiamare da più thread insieme (ognuno ha il suo lavoro).
    void run(int tasks, const std::function<void(int)>& fn)
    {
        if (tasks <= 0)
            return;
        if (tasks == 1 || workers_.empty()) {
            for (int i = 0; i < tasks; ++i)
                fn(i);
            return;
        }
        const auto job = std::make_shared<Job>(&fn, tasks);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current_ = job;
            ++generation_;
        }
        wake_.notify_all();
        work(*job);
        std::unique_lock<std::mutex> lock(job->mutex);
        job->done.wait(lock, [&] { return job->pending.load() == 0; });
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    struct Job
    {
        Job(const std::function<void(int)>* f, int n) : fn(f), tasks(n), pending(n) {}
        const std::function<void(int)>* fn;
        int tasks;
        std::atomic<int> next{0};
        std::atomic<int> pending;
        std::mutex mutex;
        std::condition_variable done;
    };

    ThreadPool()
    {
        const int cores = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        for (int i = 1; i < cores; ++i)
            workers_.emplace_back([this] { loop(); });
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (std::thread& w : workers_)
            w.join();
    }

    static void work(Job& job)
    {
        for (int i = job.next.fetch_add(1); i < job.tasks; i = job.next.fetch_add(1)) {
            (*job.fn)(i);
            if (job.pending.fetch_sub(1) == 1) {
                { std::lock_guard<std::mutex> lock(job.mutex); }
                job.done.notify_all();
            }
        }
    }

    void loop()
    {
        unsigned long long seen = 0;
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_)
                    return;
                seen = generation_;
                job = current_;
            }
            if (job)
                work(*job);
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::shared_ptr<Job> current_;
    unsigned long long generation_ = 0;
    bool stop_ = false;
};

// Come un ciclo for da 0 a tasks - 1, ma le iterazioni girano su più thread (vedi ThreadPool::run).
template <typename F>
void parallelFor(int tasks, F&& fn)
{
    const std::function<void(int)> wrapper = [&fn](int i) { fn(i); };
    ThreadPool::instance().run(tasks, wrapper);
}

}  // namespace editor::core
