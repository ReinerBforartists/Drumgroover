// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Background worker pool that renders and persists new TR808*VariantPool entries (see 808VariantPoolCache.h).
// Never runs on the audio thread: rendering a variant is a full circuit simulation.
//
// One job renders the variants of one (instrument, parameter-key) combination on one persistent voice that is
// never reset between triggers, so each variant starts at a different oscillator phase. That sequence must stay
// inside one job: split across threads, two variants would start from the same phase and come out identical.
// Different jobs are independent and run in parallel on the worker pool (CPU-bound work).
namespace dgg
{

class TR808VariantPoolBaker
{
public:
  static TR808VariantPoolBaker& Instance() { static TR808VariantPoolBaker inst; return inst; }

  // active/done/total/label: the jobs running right now. pendingJobs: jobs enqueued and not finished (running or waiting). unitsDone/unitsPlanned:
  // the sounds (variants) of the current work session, planned when their jobs are queued (PlanUnits) and counted when they are baked
  // (FinishUnits), so a progress bar over everything queued does not jump when chained jobs are enqueued later. labels: the labels of the
  // running jobs, each once.
  struct ProgressSnapshot { bool active; int done; int total; std::string label; int pendingJobs; int unitsDone; int unitsPlanned; std::vector<std::string> labels; };

  using ProgressFn = std::function<void(int done, int total)>;
  // The worker threads start with the first job.
  void Enqueue(std::string label, std::function<void(const ProgressFn&)> job)
  {
    mPendingJobs.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(mQueueMutex);
      StartWorkersLocked();
      mQueue.push_back({std::move(label), std::move(job)});
    }
    mCv.notify_one();
  }

  // Drops queued jobs, waits (up to maxWait) for running jobs and joins the worker threads. Call it when the last
  // plugin instance is destroyed, from the host's thread: joining inside a static destructor during DLL unload
  // would deadlock on the loader lock. A later Enqueue() starts the workers again. If a job is still running after
  // maxWait, the threads are detached instead.
  void Shutdown(std::chrono::milliseconds maxWait = std::chrono::milliseconds(10000))
  {
    CancelPending();
    std::vector<std::thread> threads;
    {
      std::lock_guard<std::mutex> lock(mQueueMutex);
      if (mThreads.empty()) return;
      mStop.store(true);
      threads.swap(mThreads);
    }
    mCv.notify_all();

    const auto deadline = std::chrono::steady_clock::now() + maxWait;
    while (mLiveWorkers.load() > 0 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const bool allExited = mLiveWorkers.load() == 0;
    for (auto& t : threads)
    {
      if (!t.joinable()) continue;
      if (allExited) t.join(); else t.detach();
    }
  }

  // A work session starts with the first PlanUnits after everything before it has finished and ends when the baker is idle again.
  void PlanUnits(int n)
  {
    std::lock_guard<std::mutex> lock(mProgressMutex);
    if (mPendingJobs.load() == 0 && mUnitsPlanned == mUnitsDone) { mUnitsPlanned = 0; mUnitsDone = 0; }
    mUnitsPlanned += n;
    mLastPlan = std::chrono::steady_clock::now();
  }
  void FinishUnits(int n)
  {
    std::lock_guard<std::mutex> lock(mProgressMutex);
    mUnitsDone = std::min(mUnitsDone + n, mUnitsPlanned);   // a job that was not planned (or whose plan was reset by a pause) cannot push the display beyond 100 %
  }
  // Pause of a bake: the jobs that were dropped keep their planned units otherwise, and the sets queued again by Resume would be planned a second time
  // (the displayed total doubled with every Pause / Resume).
  void ResetPlan()
  {
    std::lock_guard<std::mutex> lock(mProgressMutex);
    mUnitsPlanned = 0; mUnitsDone = 0;
  }

  // True once every enqueued job has finished, including jobs a running
  // job chains on (those are enqueued before their parent finishes, so
  // there is no moment where a set is half-baked and this reads true).
  bool IsIdle() const { return mPendingJobs.load() == 0; }

  int ThreadCount() const { return mWorkerCount; }
  // Threads not occupied by a queued or running job right now.
  int FreeThreads() const { return std::max(0, ThreadCount() - mPendingJobs.load()); }

  // Bake statistics over the process lifetime (fed by the bake jobs).
  void RecordBake(double ms) { mBakeMicros.fetch_add((long long)(ms * 1000.0)); mBakeCount.fetch_add(1); }
  double AverageBakeMs() const
  {
    const long long n = mBakeCount.load();
    return n > 0 ? (mBakeMicros.load() / 1000.0) / (double)n : 0.0;
  }

  // Drops every job that has not started yet; running jobs finish. Jobs
  // that chain follow-up work compare CancelGeneration() with the value
  // they captured when enqueued and stop chaining once it has changed.
  void CancelPending()
  {
    size_t dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mQueueMutex);
      dropped = mQueue.size();
      mQueue.clear();
      mCancelGen.fetch_add(1);
    }
    mPendingJobs.fetch_sub((int)dropped);
  }
  uint64_t CancelGeneration() const { return mCancelGen.load(); }

  // Aggregated across every job currently in flight (there can be several
  // now) -- `done`/`total` are summed, `label` names one active job (or
  // "N jobs" once more than one is running) since the UI only shows one
  // combined progress bar/status line, not one per instrument.
  ProgressSnapshot GetSnapshot()
  {
    std::lock_guard<std::mutex> lock(mProgressMutex);
    const int pending = mPendingJobs.load();
    // Idle: the work session is over -- once all planned sounds are done, or when the plan was not followed by jobs for a while (cancelled).
    if (pending == 0 && mRunningJobs.empty() &&
        (mUnitsPlanned == mUnitsDone || std::chrono::steady_clock::now() - mLastPlan > std::chrono::seconds(2)))
    { mUnitsPlanned = 0; mUnitsDone = 0; }
    if (mRunningJobs.empty()) return { false, 0, 0, "", pending, mUnitsDone, mUnitsPlanned, {} };
    int done = 0, total = 0;
    std::string label;
    std::vector<std::string> labels;
    for (const auto& [jobId, state] : mRunningJobs)
    {
      done += state.done;
      total += state.total;
      label = label.empty() ? state.label : (std::to_string(mRunningJobs.size()) + " jobs");
      if (std::find(labels.begin(), labels.end(), state.label) == labels.end()) labels.push_back(state.label);
    }
    return { true, done, total, label, pending, mUnitsDone, mUnitsPlanned, labels };
  }

private:
  struct Job { std::string label; std::function<void(const ProgressFn&)> fn; };
  struct JobState { std::string label; int done = 0; int total = 0; };

  TR808VariantPoolBaker()
  {
    // Leave a couple of cores for the audio thread/host/UI. The workers run
    // below normal priority (see WorkerLoop), so the UI stays responsive
    // without giving up cores. Falls back to 1 if hardware_concurrency() is
    // unknown (returns 0) or the machine genuinely has only 1-2 cores.
    const unsigned hw = std::thread::hardware_concurrency();
    mWorkerCount = (int)((hw > 2) ? (hw - 2) : std::max(1u, hw));
  }
  // Process-wide singleton. Normally Shutdown() has already joined the workers; any thread left over (a job that
  // did not finish in time) must be detached here, because a joinable std::thread destroyed with its vector calls
  // std::terminate.
  ~TR808VariantPoolBaker() { for (auto& t : mThreads) if (t.joinable()) t.detach(); }

  // Caller holds mQueueMutex.
  void StartWorkersLocked()
  {
    if (!mThreads.empty()) return;
    mStop.store(false);
    mLiveWorkers.store(mWorkerCount);
    for (int i = 0; i < mWorkerCount; i++) mThreads.emplace_back([this]() { WorkerLoop(); });
  }

  void WorkerLoop()
  {
#ifdef _WIN32
    // Yield to normal-priority threads (audio, DAW UI, WebView) even when the
    // machine runs busier than the pool's core budget -- the progress dialog
    // must stay responsive while the bake runs.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;)
    {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mCv.wait(lock, [this]() { return mStop.load() || !mQueue.empty(); });
        if (mQueue.empty())
        {
          mLiveWorkers.fetch_sub(1);   // stop requested and nothing left to do; this thread touches no member after this
          return;
        }
        job = std::move(mQueue.front());
        mQueue.pop_front();
      }
      const uint64_t jobId = mNextJobId.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(mProgressMutex);
        mRunningJobs[jobId] = { job.label, 0, 0 };
      }
      // An exception escaping a worker thread would terminate the host process (jobs write files). A failed job
      // is dropped; the pending count below still goes down, so IsIdle() cannot hang.
      try
      {
        job.fn([this, jobId](int done, int total) {
          std::lock_guard<std::mutex> lock(mProgressMutex);
          auto it = mRunningJobs.find(jobId);
          if (it != mRunningJobs.end()) { it->second.done = done; it->second.total = total; }
        });
      }
      catch (...) {}
      {
        std::lock_guard<std::mutex> lock(mProgressMutex);
        mRunningJobs.erase(jobId);
      }
      mPendingJobs.fetch_sub(1);
    }
  }

  int mWorkerCount = 1;
  std::vector<std::thread> mThreads;   // guarded by mQueueMutex
  std::atomic<bool> mStop{false};
  std::atomic<int> mLiveWorkers{0};
  std::mutex mQueueMutex;
  std::condition_variable mCv;
  std::deque<Job> mQueue;

  std::mutex mProgressMutex;
  std::unordered_map<uint64_t, JobState> mRunningJobs;
  std::atomic<uint64_t> mNextJobId{0};
  std::atomic<int> mPendingJobs{0};
  std::atomic<uint64_t> mCancelGen{0};
  int mUnitsPlanned = 0, mUnitsDone = 0;   // guarded by mProgressMutex
  std::chrono::steady_clock::time_point mLastPlan{};
  std::atomic<long long> mBakeMicros{0};
  std::atomic<long long> mBakeCount{0};
};

}  // namespace dgg
