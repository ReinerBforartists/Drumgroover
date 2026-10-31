// Cooperative abort of the cache bake (Pause and Close of the bake dialog), valid for every engine that renders a hit in a bake job.
//
// The host raises the counter; a bake job registers the value it was ENQUEUED with (BakeScope) and every long loop of a render asks BakeAborted(). A job that was
// enqueued before the last Pause / Close is therefore dead for its whole life: it does not start, or it stops within a few milliseconds, and it never stores a file.
// Threads that are not bake jobs (audio thread, live synthesis, tests) never see an abort: BakeAborted() is false there.
#pragma once
#include <atomic>

namespace dgg {

inline std::atomic<unsigned>& BakeAbortCounter() { static std::atomic<unsigned> g{0}; return g; }

struct BakeThreadState { bool active = false; unsigned gen = 0; };
inline BakeThreadState& BakeState() { thread_local BakeThreadState s; return s; }

// True when the current thread runs a bake job that was enqueued before the last abort.
inline bool BakeAborted()
{
  const BakeThreadState& s = BakeState();
  return s.active && BakeAbortCounter().load(std::memory_order_relaxed) != s.gen;
}

// Registers a bake job (enqueue-time generation) on the current thread for the lifetime of the scope.
struct BakeScope
{
  explicit BakeScope(unsigned genAtEnqueue) { BakeState() = BakeThreadState{ true, genAtEnqueue }; }
  ~BakeScope() { BakeState() = BakeThreadState{}; }
  BakeScope(const BakeScope&) = delete; BakeScope& operator=(const BakeScope&) = delete;
};

}  // namespace dgg
