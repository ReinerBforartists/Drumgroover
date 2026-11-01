// TR-909 Hand Clap: live voice with a store of prepared noise seeds.
//
// The hybrid HC model (tr909/HcHybrid.h) needs about 0.18 s of compute for the first 40 ms of a hit (engine phase and full-model windows) and about 0.05 s for the remaining 310 ms.
// The first part is the same work for every hit of a (accent anchor, noise seed), so it is done ahead of time: background threads keep, for each of the 8 velocity levels
// (kTr909VelocityLevels), a few prepared entries (the model state at 40 ms and the host-rate samples of the first 40 ms, about 0.3 MB). A hit takes an entry (no compute in the audio thread):
// the audio thread copies the 40 ms head into a hit slot and queues one render job per anchor run; a render thread continues the hit from the state and fills the slot ahead of
// the playback (4 x faster than real time). The audio thread only reads what the render thread has published. Entries are used once and replaced with new seeds; when the store of a level
// is empty, the first entry of that level is played again (a repeated noise seed) and a hit without any entry falls back to the baked set (Tr909CachedVoice).
//
// Audio thread: no locks, no allocation (slots, jobs and queues are allocated when the engine starts), the render threads are woken with a notify that takes no lock.
// The engine is shared by all plugin instances of the process (Tr909HcLiveEngine::Get); it is created and destroyed on an ordinary thread (the last user releases it), never in a static destructor.
#pragma once
#include "909ModelRunner.h"
#include "909HcPackages.h"
#include "tr909/HcLanes.h"
#include "909Playback.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dgg {

namespace hclive {

// bounded lock-free multi-producer multi-consumer queue of pointers (D. Vyukov)
template <class T>
class MpmcQueue
{
public:
  explicit MpmcQueue(size_t capacity = 64) : cap_(capacity), cells_(new Cell[capacity]) { for (size_t i = 0; i < cap_; ++i) cells_[i].seq.store(i, std::memory_order_relaxed); head_.store(0); tail_.store(0); }
  bool Push(T v)
  {
    Cell* c; size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;)
    {
      c = &cells_[pos % cap_]; const size_t seq = c->seq.load(std::memory_order_acquire); const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
      if (dif == 0) { if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break; }
      else if (dif < 0) return false;
      else pos = tail_.load(std::memory_order_relaxed);
    }
    c->v = v; c->seq.store(pos + 1, std::memory_order_release); return true;
  }
  bool Pop(T& v)
  {
    Cell* c; size_t pos = head_.load(std::memory_order_relaxed);
    for (;;)
    {
      c = &cells_[pos % cap_]; const size_t seq = c->seq.load(std::memory_order_acquire); const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
      if (dif == 0) { if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break; }
      else if (dif < 0) return false;
      else pos = head_.load(std::memory_order_relaxed);
    }
    v = c->v; c->seq.store(pos + cap_, std::memory_order_release); return true;
  }
private:
  struct Cell { std::atomic<size_t> seq; T v{}; };
  size_t cap_; std::unique_ptr<Cell[]> cells_; std::atomic<size_t> head_, tail_;
};

// one anchor's HitStream behind a model-independent interface
struct StreamBase
{
  virtual ~StreamBase() = default;
  virtual bool Load(const hch::HcPackage& pkg) = 0;
  virtual bool LoadFrom(StreamBase& master, bool warmEngine) = 0;   // shares the tables of a master stream of the same anchor
  virtual bool Prepare(uint64_t seed) = 0;
  virtual bool MakeBase(uint64_t seed, hch::HcHandover& out) = 0;
  virtual bool PrepareFrom(const hch::HcHandover& base, uint64_t noiseSeed) = 0;
  virtual bool Export(uint64_t seed, hch::HcHandover& out) = 0;
  virtual void Import(uint64_t seed, const hch::HcHandover& h) = 0;
  virtual void Erase(uint64_t seed) = 0;
  virtual bool Begin(uint64_t seed, double fs, double seconds, long long tailNoiseSeed = -1) = 0;
  virtual int Pull(float* out, int n) = 0;
  // Tails of n (<= 8) prepared entries of this anchor in one lane batch (HcLanes.h): the complete hits at the host rate, from the trigger on. false: not possible (no AVX2 build, entries not suited); nothing is changed then.
  virtual bool RenderTails(int n, const hch::HcHandover* const* ho, const long long* tailSeed, double fs, std::vector<float>* out) = 0;
};
template <class Mdl>
struct StreamImpl : StreamBase
{
  bool Load(const hch::HcPackage& pkg) override { return hs.Load(pkg); }
  bool LoadFrom(StreamBase& master, bool warmEngine) override { return hs.LoadFrom(static_cast<StreamImpl<Mdl>&>(master).hs, warmEngine); }
  bool Prepare(uint64_t seed) override { return hs.Prepare(seed); }
  bool MakeBase(uint64_t seed, hch::HcHandover& out) override { return hs.MakeBase(seed, out); }
  bool PrepareFrom(const hch::HcHandover& base, uint64_t noiseSeed) override { return hs.PrepareFrom(base, noiseSeed); }
  bool Export(uint64_t seed, hch::HcHandover& out) override { return hs.ExportEntry(seed, out); }
  void Import(uint64_t seed, const hch::HcHandover& h) override { hs.ImportEntry(seed, h); }
  void Erase(uint64_t seed) override { hs.EraseEntry(seed); }
  bool Begin(uint64_t seed, double fs, double seconds, long long tailNoiseSeed = -1) override { return hs.Begin(seed, fs, seconds, tailNoiseSeed); }
  int Pull(float* out, int n) override { return hs.Pull(out, n); }
  bool RenderTails(int n, const hch::HcHandover* const* ho, const long long* tailSeed, double fs, std::vector<float>* out) override
  {
#if DGG_LANES_AVAILABLE
    if (n < 1 || n > hclanes::NL) return false;
    hclanes::MiniTail mt(hs.FullRed(), hs.MiniRed(), hs.QuietMask());
    if (!mt.Begin(n, ho, tailSeed, fs, ho[0]->idle) || !mt.Run(hclive::kHitSeconds + 0.002)) return false;
    for (int l = 0; l < n; ++l) out[l] = mt.Host(l);
    return true;
#else
    (void)n; (void)ho; (void)tailSeed; (void)fs; (void)out; return false;
#endif
  }
  hch::HitStream<Mdl> hs;
};
inline std::unique_ptr<StreamBase> MakeStream(int anchorIdx)
{
  switch (anchorIdx)
  {
    case 0: return std::unique_ptr<StreamBase>(new StreamImpl<Tr909_HC_A25>());
    case 1: return std::unique_ptr<StreamBase>(new StreamImpl<Tr909_HC_A50>());
    case 2: return std::unique_ptr<StreamBase>(new StreamImpl<Tr909_HC_A75>());
    case 3: return std::unique_ptr<StreamBase>(new StreamImpl<Tr909_HC_A100>());
    case 4: return std::unique_ptr<StreamBase>(new StreamImpl<Tr909_HC_A127>());
  }
  return nullptr;
}
// The tables of an anchor, loaded once for all threads (0.7 s and about 14 MB each, read-only afterwards)
class Masters
{
public:
  StreamBase* Get(int a)
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (a < 0 || a >= kTr909AnchorCount || failed_[a]) return nullptr;
    if (!m_[a])
    {
      const hch::HcPackage* pkg = Tr909HcPackageStore::Instance().Get(kTr909Anchors[a]);
      std::unique_ptr<StreamBase> n = pkg ? MakeStream(a) : nullptr;
      if (!n || !n->Load(*pkg)) { failed_[a] = true; return nullptr; }
      m_[a] = std::move(n);
    }
    return m_[a].get();
  }
private:
  std::mutex mu_; std::unique_ptr<StreamBase> m_[kTr909AnchorCount]; bool failed_[kTr909AnchorCount] = {false, false, false, false, false};
};
// the streams of one thread: its own solver state and table windows on the shared tables; warmEngine for a thread that prepares seeds (engine phase)
struct StreamSet
{
  StreamSet(Masters& m, bool warm) : masters(m), warmEngine(warm) {}
  StreamBase* Get(int a)
  {
    if (a < 0 || a >= kTr909AnchorCount || failed[a]) return nullptr;
    if (!s[a])
    {
      StreamBase* master = masters.Get(a); std::unique_ptr<StreamBase> n = master ? MakeStream(a) : nullptr;
      if (!n || !n->LoadFrom(*master, warmEngine)) { failed[a] = true; return nullptr; }
      s[a] = std::move(n);
    }
    return s[a].get();
  }
  Masters& masters; bool warmEngine; std::unique_ptr<StreamBase> s[kTr909AnchorCount]; bool failed[kTr909AnchorCount] = {false, false, false, false, false};
};

// CPU time of the calling thread in microseconds (diagnosis counters; Windows, 0 elsewhere)
inline long long ThreadCpuUs()
{
#ifdef _WIN32
  FILETIME c, e, k, u; if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  return static_cast<long long>(((static_cast<unsigned long long>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<unsigned long long>(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 10);
#else
  return 0;
#endif
}
inline thread_local long long tlBaseUs = 0;   // CPU time this thread spent building bases (to separate it from the entry it was building)

// Load of the whole machine (all processes) over the last few hundred milliseconds, 0..1; Windows only (elsewhere 0). The host and the other
// plug-ins come first: the preparers work with a share of their time that falls with the load (PrepDuty), the stores fill at full speed again when there is room. Called by the preparer threads only.
inline double SystemLoad()
{
#ifdef _WIN32
  static std::mutex mu; static unsigned long long lastIdle = 0, lastTotal = 0; static double load = 0.0; static std::chrono::steady_clock::time_point lastT{};
  std::lock_guard<std::mutex> lk(mu); const auto now = std::chrono::steady_clock::now();
  if (lastTotal != 0 && now - lastT < std::chrono::milliseconds(200)) return load;
  FILETIME i, k, u; if (!GetSystemTimes(&i, &k, &u)) return load;
  auto v = [](FILETIME f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  const unsigned long long idle = v(i), total = v(k) + v(u);   // kernel time includes the idle time
  if (lastTotal != 0 && total > lastTotal) load = 1.0 - static_cast<double>(idle - lastIdle) / static_cast<double>(total - lastTotal);
  lastIdle = idle; lastTotal = total; lastT = now; return load;
#else
  return 0.0;
#endif
}
// Share of its time a preparer may work: 1 up to a machine load of kLoadFree, falling linearly to kMinDuty at kLoadFull and above (never 0: the stores keep filling, slowly).
constexpr double kLoadFree = 0.75, kLoadFull = 0.97, kMinDuty = 0.15;
inline double PrepDuty(double load) { return std::clamp((kLoadFull - load) / (kLoadFull - kLoadFree), kMinDuty, 1.0); }

// A counting semaphore that does not lose a post (a notify without a waiter is lost): the audio thread posts, a worker waits. Windows: kernel semaphore (a post is one system call, no lock);
// elsewhere a mutex / condition variable with a counter (the post then takes a lock for a moment).
#ifdef _WIN32
class Semaphore
{
public:
  Semaphore() : h_(CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr)) {}
  ~Semaphore() { if (h_) CloseHandle(h_); }
  Semaphore(const Semaphore&) = delete; Semaphore& operator=(const Semaphore&) = delete;
  void Post(int n = 1) { ReleaseSemaphore(h_, n, nullptr); }
  bool Wait(int ms) { return WaitForSingleObject(h_, static_cast<DWORD>(ms)) == WAIT_OBJECT_0; }
private:
  HANDLE h_;
};
#else
class Semaphore
{
public:
  void Post(int n = 1) { { std::lock_guard<std::mutex> lk(m_); c_ += n; } cv_.notify_all(); }
  bool Wait(int ms) { std::unique_lock<std::mutex> lk(m_); if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return c_ > 0; })) return false; --c_; return true; }
private:
  std::mutex m_; std::condition_variable cv_; int c_ = 0;
};
#endif

// The states at the end of the engine phase (1 ms) that entries are built on: kBases per anchor, the same noise seed for every anchor of a base (an accent between two anchors is a blend of two runs
// that need the same noise). Built when first needed, read-only afterwards.
constexpr int kMaxBases = 64;
class Bases
{
public:
  std::shared_ptr<const hch::HcHandover> Get(StreamSet& set, int anchor, int k)
  {
    Slot& s = slot_[anchor][k]; std::lock_guard<std::mutex> lk(s.mu);
    if (!s.b) { StreamBase* st = set.Get(anchor); std::shared_ptr<hch::HcHandover> n(new hch::HcHandover()); const long long c0 = ThreadCpuUs(); if (!st || !st->MakeBase(kSeeds[k], *n)) return nullptr; const long long d = ThreadCpuUs() - c0; tlBaseUs += d; cpuUs_.fetch_add(d); built_.fetch_add(1); s.b = n; }
    return s.b;
  }
  // Base seeds: 64 chosen from 1600 candidates (seeds 9200000...9201599) as a stratified sample: the pool is cut into 64 cells of equal size (splitting at the median of the feature with the largest spread, 25 features:
  // log rms of three sub-windows of the first 0.75 ms, end value and end slope, for each of the five anchors), one random member of each cell, the best of 20000 such draws (mean and spread of all 25 features closest
  // to the pool: mean differences at most 0.03 standard deviations, spread ratios 0.97-1.01). tools/_sweep/hc_live/hc_base_select.cpp. Same seed for every anchor.
  static constexpr uint64_t kSeeds[64] = {9201422, 9201480, 9200273, 9201373, 9200230, 9201351, 9200462, 9201463, 9201076, 9201016, 9201293, 9200499, 9200276, 9200858, 9200738, 9200642, 9200740, 9201427, 9200386, 9201250, 9200532, 9200725, 9201492, 9201522, 9200197, 9200786, 9200059, 9200733, 9200874, 9200888, 9201056, 9200244, 9201519, 9200010, 9200153, 9200240, 9201364, 9201528, 9201246, 9200095, 9201338, 9200194, 9201468, 9200382, 9200096, 9201212, 9201350, 9200046, 9200544, 9200425, 9201192, 9200620, 9201490, 9200734, 9200168, 9200648, 9200638, 9201203, 9201301, 9201388, 9200696, 9200113, 9200955, 9200019};
  long long CpuUs() const { return cpuUs_.load(); }   // CPU time of all base builds so far
  int Built() const { return built_.load(); }
private:
  std::atomic<long long> cpuUs_{0}; std::atomic<int> built_{0};
  struct Slot { std::mutex mu; std::shared_ptr<const hch::HcHandover> b; };
  Slot slot_[kTr909AnchorCount][kMaxBases];
};

constexpr double kHitSeconds = 0.35;      // length of a hit (as the baked sets)
constexpr double kHeadSeconds = 0.0385;   // the part of a hit that a prepared entry carries as host samples (the handover state is at 40 ms)
constexpr int kMaxRate = 192000;
constexpr int kSlots = 24;
constexpr int kLanes = 8;   // lanes of a batch (hclanes::NL)

// an atomic counter that can be copied (entries are copied for the shared fallback)
struct AtomicInt { std::atomic<int> v; AtomicInt(int x = 0) : v(x) {} AtomicInt(const AtomicInt& o) : v(o.v.load()) {} AtomicInt& operator=(const AtomicInt& o) { v.store(o.v.load()); return *this; } };

// a prepared hit: handover state and head samples of one or two anchor runs (accent between two anchors = blend of two runs with the same seed)
struct Entry
{
  int level = 0, code = 0, fs = 0, epoch = 0, n = 0, anchor[2] = {0, 0}; double w[2] = {1.0, 0.0}; uint64_t seed = 0; bool shared = false;   // shared: the fallback entry of a level, never freed, played again
  hch::HcHandover ho[2]; std::vector<float> head[2];
  // lane batch: the complete hit (head and tail with its own noise) rendered ahead; hasPre: valid, the first play of the entry copies it and renders nothing (the entry then plays on with the render jobs, fresh tail each time)
  std::vector<float> pre[2]; long long tailSeed = -1; bool hasPre = false; AtomicInt runsLeft{0}; AtomicInt preFail{0};
};

struct Hit;
struct Job { Hit* hit = nullptr; int run = 0; };
struct Hit
{
  std::atomic<int> state{0};      // 0 free, 1 played by a voice, 2 voice finished (reusable when no job is pending)
  std::atomic<int> pending{0};    // render jobs not finished
  std::atomic<int> produced[2];   // samples of run r that are valid in buf[r]
  std::vector<float> buf[2];
  std::atomic<int> len{0};        // samples of the hit; the render lowers it to the real end (the decimator ends a few samples before seconds x fs)
  int n = 0, head = 0, fs = 0; double w[2] = {1.0, 0.0};
  Entry* entry = nullptr; Job job[2]; uint64_t seed = 0; long long tailSeed = -1; int anchor[2] = {0, 0};   // seed and anchors of the hit (diagnosis); tailSeed >= 0: the noise after 40 ms is another sequence than the one of the entry
  Hit() { produced[0].store(0); produced[1].store(0); }
};

}  // namespace hclive

class Tr909HcLiveEngine
{
public:
  static constexpr int kLevels = kTr909VelocityLevels, kRing = 8, kRecycle = 8;
  // preparer threads: two by default, at low priority (the host's audio threads come first); render threads have the deadline of the hit and run at normal priority
  // (each hit with fresh noise costs about 0.2-0.4 s of preparation: the number of preparers decides how many hits per second get a new seed and how much CPU the engine takes; the rest repeat used entries)
  static std::atomic<int>& PrepOverride() { static std::atomic<int> n{0}; return n; }   // tests: number of preparer threads of the next engine
  static int PrepThreads() { const int o = PrepOverride().load(); return o > 0 ? o : 2; }
  // Entries are built on one of this many bases per anchor (the first millisecond is shared by the entries of a base, everything after it has its own noise); 0: every entry runs its own engine phase.
  static std::atomic<int>& BasesOverride() { static std::atomic<int> n{-1}; return n; }
  static int BaseCount() { const int o = BasesOverride().load(); return o < 0 ? 64 : std::min(o, hclive::kMaxBases); }   // 64 bases: the first millisecond of an entry comes from its base; 4 bases gave too few values for the statistics (tools handoff R310, R312)
  // render threads: half of the cores, 2..6 (each tail needs about 1/6 of a core for its 0.35 s; a hit of two anchors has two)
  static int RenderThreads() { const int hw = static_cast<int>(std::thread::hardware_concurrency()); return std::clamp(hw / 2, 2, 6); }
  int RenderThreadCount() const { return nRender_; }

  // The engine of the process; the last holder destroys it. Call on an ordinary thread (plugin construction / UI), not in a static initialiser or destructor.
  static std::shared_ptr<Tr909HcLiveEngine> Get()
  {
    static std::mutex mu; static std::weak_ptr<Tr909HcLiveEngine> weak;
    std::lock_guard<std::mutex> lk(mu); std::shared_ptr<Tr909HcLiveEngine> e = weak.lock();
    if (!e) { e = std::shared_ptr<Tr909HcLiveEngine>(new Tr909HcLiveEngine()); weak = e; }
    return e;
  }
  ~Tr909HcLiveEngine()
  {
    running_.store(false); jobSem_.Post(64); prepSem_.Post(64);
    for (std::thread& t : threads_) if (t.joinable()) t.join();
    for (int l = 0; l < kLevels; ++l) { hclive::Entry* e; while (rings_[l]->Pop(e)) delete e; while (recycle_[l]->Pop(e)) delete e; delete shared_[l].load(); }
    hclive::Job* j; while (jobs_.Pop(j)) {}
    { hclive::Entry* e; while (spent_.Pop(e)) delete e; while (graveyard_.Pop(e)) delete e; }
    { std::set<hclive::Entry*> held; for (auto& p : pack_) for (const Held& h : p) held.insert(h.e); for (hclive::Entry* e : held) delete e; }   // entries whose tails were still waiting for a batch
  }
  Tr909HcLiveEngine(const Tr909HcLiveEngine&) = delete; Tr909HcLiveEngine& operator=(const Tr909HcLiveEngine&) = delete;

  // Sets the sample rate and starts the worker threads (once); non-real-time. A change of the rate drops the prepared entries (they are specific to the rate).
  void Configure(double sampleRate)
  {
    const int fs = std::clamp(static_cast<int>(std::lround(sampleRate)), 8000, hclive::kMaxRate);
    std::lock_guard<std::mutex> lk(cfgMu_);
    if (fs != fs_.load())
    {
      ready_.store(false); fs_.store(fs); epoch_.fetch_add(1);
      for (int l = 0; l < kLevels; ++l) { hclive::Entry* e; while (rings_[l]->Pop(e)) delete e; while (recycle_[l]->Pop(e)) delete e; fill_[l].store(0); hclive::Entry* s = shared_[l].exchange(nullptr); retired_.push_back(std::unique_ptr<hclive::Entry>(s)); }
      ready_.store(true);
    }
    if (threads_.empty())
    {
      running_.store(true);
      for (int i = 0; i < PrepThreads(); ++i) threads_.emplace_back([this, i] { LowPriority(); PrepLoop(i); });
      nRender_ = RenderThreads();
      for (int i = 0; i < nRender_; ++i) threads_.emplace_back([this] { RenderPriority(); RenderLoop(); });
    }
    Wake();
  }

  // Audio thread. Takes a prepared entry for the velocity, starts the render of its tail and returns the hit slot (nullptr: nothing prepared for this level yet, a slot is not free, or the engine is not ready).
  hclive::Hit* Trigger(double velocity01)
  {
    if (!ready_.load(std::memory_order_acquire)) return nullptr;
    const int level = std::clamp(static_cast<int>(std::lround(std::clamp(velocity01, 0.0, 1.0) * (kLevels - 1))), 0, kLevels - 1);
    if (used_[level].load(std::memory_order_relaxed) < 8) used_[level].fetch_add(1, std::memory_order_relaxed);
    hclive::Entry* e = nullptr; bool own = true;
    if (rings_[level]->Pop(e)) fill_[level].fetch_sub(1, std::memory_order_relaxed);
    else if (recycle_[level]->Pop(e)) ++repeats_;   // a used entry of this level plays again (its noise seed repeats), after the others of the recycle ring
    else { e = shared_[level].load(std::memory_order_acquire); own = false; ++repeats_; ++sharedRepeats_; }
    Wake();   // the store of this level wants a new entry
    if (!e || e->fs != fs_.load(std::memory_order_relaxed)) { if (own && e) graveyard_.Push(e); ++misses_; return nullptr; }
    hclive::Hit* h = nullptr;
    for (int i = 0; i < hclive::kSlots && !h; ++i)
    {
      hclive::Hit& s = slots_[i]; int st = s.state.load(std::memory_order_acquire);
      if ((st == 0 || (st == 2 && s.pending.load(std::memory_order_acquire) == 0)) && s.state.compare_exchange_strong(st, 1, std::memory_order_acq_rel)) h = &s;
    }
    if (!h) { if (own) graveyard_.Push(e); ++misses_; return nullptr; }   // an entry that is not used is freed by a worker
    if (e->hasPre && freshTail_.load(std::memory_order_relaxed))   // rendered ahead in a lane batch: copy, no render job (the entry goes to a worker that frees the samples and passes it on to the recycle ring)
    {
      int len = std::min(static_cast<int>(hclive::kHitSeconds * e->fs), static_cast<int>(h->buf[0].size()));
      for (int r = 0; r < e->n; ++r) len = std::min(len, static_cast<int>(e->pre[r].size()));
      for (int r = 0; r < e->n; ++r) { std::copy(e->pre[r].begin(), e->pre[r].begin() + len, h->buf[r].begin()); h->produced[r].store(len, std::memory_order_release); }
      h->n = e->n; h->fs = e->fs; h->head = static_cast<int>(e->head[0].size()); h->entry = nullptr; h->tailSeed = e->tailSeed; h->w[0] = e->w[0]; h->w[1] = e->w[1]; h->seed = e->seed; h->anchor[0] = e->anchor[0]; h->anchor[1] = e->anchor[1];
      h->len.store(len, std::memory_order_release); h->pending.store(0, std::memory_order_release);
      if (!spent_.Push(e)) graveyard_.Push(e);
      ++preHits_; return h;
    }
    h->n = e->n; h->fs = e->fs; h->len.store(std::min(static_cast<int>(hclive::kHitSeconds * e->fs), static_cast<int>(h->buf[0].size())), std::memory_order_release); h->head = static_cast<int>(e->head[0].size());
    h->tailSeed = freshTail_.load(std::memory_order_relaxed) ? static_cast<long long>(tailCounter_.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFFFFFLL) : -1;
    h->entry = e; h->w[0] = e->w[0]; h->w[1] = e->w[1]; h->seed = e->seed; h->anchor[0] = e->anchor[0]; h->anchor[1] = e->anchor[1];
    for (int r = 0; r < e->n; ++r)
    {
      std::copy(e->head[r].begin(), e->head[r].end(), h->buf[r].begin());
      h->produced[r].store(h->head, std::memory_order_release);
    }
    h->pending.store(e->n, std::memory_order_release);
    for (int r = 0; r < e->n; ++r) { h->job[r].hit = h; h->job[r].run = r; jobs_.Push(&h->job[r]); jobSem_.Post(1); }
    return h;
  }
  // Audio thread: the voice is done with the slot.
  static void Release(hclive::Hit* h) { h->state.store(2, std::memory_order_release); }
  static int Available(const hclive::Hit* h)
  {
    int a = h->produced[0].load(std::memory_order_acquire);
    if (h->n > 1) a = std::min(a, h->produced[1].load(std::memory_order_acquire));
    return a;
  }

  // Drops the prepared entries (tests, a change of the settings above); non-real-time.
  void Flush()
  {
    std::lock_guard<std::mutex> lk(cfgMu_); ready_.store(false);
    for (int l = 0; l < kLevels; ++l) { hclive::Entry* e; while (rings_[l]->Pop(e)) delete e; while (recycle_[l]->Pop(e)) delete e; fill_[l].store(0); hclive::Entry* s = shared_[l].exchange(nullptr); retired_.push_back(std::unique_ptr<hclive::Entry>(s)); }
    epoch_.fetch_add(1); ready_.store(true); Wake();
  }
  bool Ready() const { return ready_.load(std::memory_order_acquire); }
  // CPU seconds used so far by the preparer threads / the render threads (diagnosis, Windows only; 0 elsewhere)
  double CpuSeconds(bool render) const
  {
#ifdef _WIN32
    double s = 0; const int np = PrepThreads();
    for (size_t i = 0; i < threads_.size(); ++i)
    {
      const bool isRender = static_cast<int>(i) >= np; if (isRender != render) continue;
      FILETIME c, e, k, u; if (GetThreadTimes(const_cast<std::thread&>(threads_[i]).native_handle(), &c, &e, &k, &u)) s += ((static_cast<unsigned long long>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<unsigned long long>(u.dwHighDateTime) << 32 | u.dwLowDateTime)) * 1e-7;
    }
    return s;
#else
    (void)render; return 0.0;
#endif
  }
  // A hit continues after its first 40 ms with a fresh noise sequence of its own (default); false: with the sequence of its entry (the hit is then the same as the one the bake path renders for that seed).
  void SetFreshTail(bool on) { freshTail_.store(on); }
  // number of prepared entries of a level, and counters (diagnosis)
  int Prepared(int level) const { return fill_[level].load(); }
  unsigned long Misses() const { return misses_.load(); }
  unsigned long Repeats() const { return repeats_.load(); }
  unsigned long SharedRepeats() const { return sharedRepeats_.load(); }   // repeats that had to use the first entry of the level (recycle ring empty)
  unsigned long Underruns() const { return underruns_.load(); }
  void CountUnderrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }
  unsigned long Prepares() const { return prepares_.load(); }
  unsigned long Throttled() const { return throttled_.load(); }   // preparer cycles skipped because the machine was busy
  // CPU accounting (thread times, microseconds -> seconds): the bases (once, 16 per anchor), the entries without their bases, the render jobs
  double BaseCpuSeconds() const { return bases_.CpuUs() * 1e-6; }
  int BasesBuilt() const { return bases_.Built(); }
  double EntryCpuSeconds() const { return entryCpuUs_.load() * 1e-6; }
  unsigned long EntryRuns() const { return entryRuns_.load(); }
  double RenderCpuSeconds() const { return renderCpuUs_.load() * 1e-6; }
  // tails rendered ahead: CPU of the batches (and of the scalar renders of runs that did not fill a batch), lane batches, runs in them, runs rendered one by one, hits played from ahead
  double LaneCpuSeconds() const { return laneCpuUs_.load() * 1e-6; }
  unsigned long LaneBatches() const { return laneBatches_.load(); }
  unsigned long LaneRuns() const { return laneRuns_.load(); }
  unsigned long ScalarPreRuns() const { return scalarPre_.load(); }
  unsigned long PreHits() const { return preHits_.load(); }
  // tests: false renders every tail one by one (scalar); true (default): in lane batches where the build has AVX2; PreRender false: no rendering ahead (the render jobs do it at the trigger)
  static std::atomic<int>& HeldMsOverride() { static std::atomic<int> n{-1}; return n; }   // tests: longest wait of a run for a full lane batch (ms, default 250)
  static std::atomic<int>& LanesOverride() { static std::atomic<int> n{-1}; return n; }
  static bool LanesOn() { const int o = LanesOverride().load(); return o < 0 ? DGG_LANES_AVAILABLE != 0 : o > 0; }
  static std::atomic<int>& PreRenderOverride() { static std::atomic<int> n{-1}; return n; }
  static bool PreRenderOn() { return PreRenderOverride().load() != 0; }
  unsigned long RenderJobs() const { return renderJobs_.load(); }

private:
  Tr909HcLiveEngine() : jobs_(256), graveyard_(256), spent_(256)
  {
    for (int l = 0; l < kLevels; ++l)
    {
      rings_[l].reset(new hclive::MpmcQueue<hclive::Entry*>(kRing)); recycle_[l].reset(new hclive::MpmcQueue<hclive::Entry*>(kRecycle)); fill_[l].store(0); used_[l].store(0); inflight_[l].store(0); heldE_[l].store(0); shared_[l].store(nullptr);
      levelCode_[l] = Tr909CodeForVelocity(Tr909Voice::Hc, static_cast<double>(l) / (kLevels - 1));
    }
    slots_.reset(new hclive::Hit[hclive::kSlots]);
    for (int i = 0; i < hclive::kSlots; ++i) for (int r = 0; r < 2; ++r) slots_[i].buf[r].assign(static_cast<size_t>(hclive::kHitSeconds * hclive::kMaxRate) + 64, 0.0f);
  }
  void Wake() { prepSem_.Post(1); }   // a level lost an entry (or the rate changed): a preparer looks at the stores
  static void RenderPriority()
  {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // a hit has a deadline
#endif
  }
  static void LowPriority()
  {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  }

  // Preparer: keeps every level stocked (1 entry for a level that was never played, up to 4 for a level in use); a level is served in the order of its shortage.
  void PrepLoop(int idx)
  {
    hclive::StreamSet set(masters_, true); (void)idx;
    while (running_.load())
    {
      FreeGraveyard();
      int best = -1, bestScore = 1 << 30;
      for (int l = 0; l < kLevels; ++l)
      {
        const int target = 1 + std::min(used_[l].load(std::memory_order_relaxed), 3), have = fill_[l].load() + inflight_[l].load() + std::max(0, heldE_[l].load() - kHeldAhead);   // entries that wait for a batch count as stock beyond kHeldAhead per level: the stock is built ahead meanwhile
        if (have >= target) continue;
        const int score = have * 16 - std::min(used_[l].load(), 8);
        if (score < bestScore) { bestScore = score; best = l; }
      }
      if (best < 0) { FlushAged(set, false); prepSem_.Wait(heldCount_.load() > 0 ? 50 : 1000); continue; }
      inflight_[best].fetch_add(1);
      const auto tb0 = std::chrono::steady_clock::now();
      hclive::Entry* e = BuildEntry(set, best);
      if (!e) inflight_[best].fetch_sub(1);
      {   // a busy machine: rest in proportion to the work (duty cycle), the host comes first
        const double duty = hclive::PrepDuty(hclive::SystemLoad());
        if (duty < 1.0) { throttled_.fetch_add(1); const double work = std::chrono::duration<double>(std::chrono::steady_clock::now() - tb0).count(); std::this_thread::sleep_for(std::chrono::duration<double>(std::min(1.0, work * (1.0 / duty - 1.0)))); }
      }
      if (!e) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
      if (e->fs != fs_.load() || e->epoch != epoch_.load()) { inflight_[best].fetch_sub(1); delete e; continue; }   // built for another rate or before a flush
      prepares_.fetch_add(1);
      if (!shared_[best].load()) { hclive::Entry* c = new hclive::Entry(*e); c->shared = true; hclive::Entry* none = nullptr; if (!shared_[best].compare_exchange_strong(none, c)) delete c; }
      if (freshTail_.load() && PreRenderOn()) SubmitTails(set, e); else FinishEntry(e, false);
      FlushAged(set, false);
    }
  }
  hclive::Entry* BuildEntry(hclive::StreamSet& set, int level)
  {
    const long long c0 = hclive::ThreadCpuUs(), b0 = hclive::tlBaseUs; struct Acct { Tr909HcLiveEngine* e; long long c0, b0; unsigned long runs = 0; ~Acct() { e->entryCpuUs_.fetch_add(hclive::ThreadCpuUs() - c0 - (hclive::tlBaseUs - b0)); e->entryRuns_.fetch_add(runs); } } acct{this, c0, b0};
    const int fs = fs_.load(); const int code = levelCode_[level];
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(Tr909Voice::Hc, code, runs); if (n == 0) return nullptr;
    std::unique_ptr<hclive::Entry> e(new hclive::Entry()); e->level = level; e->code = code; e->fs = fs; e->epoch = epoch_.load(); e->n = n; e->seed = nextSeed_.fetch_add(1);
    const long headLen = static_cast<long>(hclive::kHeadSeconds * fs);
    const int nBases = BaseCount(); const int kb = nBases > 0 ? static_cast<int>(baseCursor_.fetch_add(1) % static_cast<unsigned>(nBases)) : 0;
    for (int i = 0; i < n; ++i)
    {
      int a = -1; for (int k = 0; k < kTr909AnchorCount; ++k) if (kTr909Anchors[k] == runs[i].anchorCode) a = k;
      hclive::StreamBase* s = set.Get(a); if (!s) return nullptr;
      e->anchor[i] = a; e->w[i] = runs[i].weight; ++acct.runs;
      if (nBases > 0)
      {
        std::shared_ptr<const hch::HcHandover> base = bases_.Get(set, a, kb); if (!base) return nullptr;
        if (!s->PrepareFrom(*base, e->seed) || !s->Export(e->seed, e->ho[i])) return nullptr;
      }
      else if (!s->Prepare(e->seed) || !s->Export(e->seed, e->ho[i])) return nullptr;
      if (!s->Begin(e->seed, static_cast<double>(fs), hclive::kHitSeconds)) { s->Erase(e->seed); return nullptr; }
      e->head[i].assign(static_cast<size_t>(headLen), 0.0f);
      long got = 0; while (got < headLen) { const int w = s->Pull(e->head[i].data() + got, static_cast<int>(std::min<long>(headLen - got, 1024))); if (w <= 0) break; got += w; }
      s->Erase(e->seed);
      if (got < headLen) return nullptr;
    }
    return e.release();
  }

  // ---- tails rendered ahead (lane batch) ----
  // A built entry gets the complete hit rendered before it is stocked: the tails of up to 8 runs of the same anchor go through the lane batch together (HcLanes.h, about 4x less CPU per tail than the
  // scalar render); a batch with fewer than kMinLanes runs, a batch that fails or a build without AVX2 renders its runs one by one with the scalar path. The audio thread then only copies samples.
  struct Held { hclive::Entry* e; int run; std::chrono::steady_clock::time_point t; };
  static constexpr int kMinLanes = 3, kHeldAhead = 1000;   // 1000: held entries never count as stock (2 gave T3b and T4 failures, R314)
  static int MaxHeldMs() { const int o = HeldMsOverride().load(); return o < 0 ? 250 : o; }
  void FinishEntry(hclive::Entry* e, bool fromHeld)
  {
    const int level = e->level; if (fromHeld) heldE_[level].fetch_sub(1); else inflight_[level].fetch_sub(1);
    if (e->fs != fs_.load() || e->epoch != epoch_.load()) { delete e; return; }
    if (rings_[level]->Push(e)) fill_[level].fetch_add(1); else delete e;
  }
  // scalar tail of one run, written to e->pre[run] (false: failed, the run plays through the render job)
  bool ScalarTail(hclive::StreamSet& set, hclive::Entry* e, int run)
  {
    hclive::StreamBase* s = set.Get(e->anchor[run]); if (!s) return false;
    s->Import(e->seed, e->ho[run]); bool ok = false;
    if (s->Begin(e->seed, static_cast<double>(e->fs), hclive::kHitSeconds, e->tailSeed))
    {
      const int len = static_cast<int>(hclive::kHitSeconds * e->fs); std::vector<float>& p = e->pre[run]; p.assign(static_cast<size_t>(len), 0.0f); int got = 0;
      while (got < len) { const int w = s->Pull(p.data() + got, std::min(1024, len - got)); if (w <= 0) break; got += w; }
      p.resize(static_cast<size_t>(got)); ok = got > 0;
    }
    s->Erase(e->seed); return ok;
  }
  void RunDone(hclive::Entry* e, bool ok)
  {
    if (!ok) e->preFail.v.store(1);
    if (e->runsLeft.v.fetch_sub(1, std::memory_order_acq_rel) == 1) { e->hasPre = e->preFail.v.load() == 0; FinishEntry(e, true); }   // the entry is visible to the audio thread from here on
  }
  void RunBatch(hclive::StreamSet& set, int anchor, std::vector<Held>& b)
  {
    const long long cpu0 = hclive::ThreadCpuUs(); const int n = static_cast<int>(b.size());
    bool ok = false; std::vector<float> out[hclive::kLanes];
    if (n >= kMinLanes && n <= hclive::kLanes && LanesOn())
    {
      const hch::HcHandover* ho[hclive::kLanes]; long long sd[hclive::kLanes]; bool same = true;
      for (int l = 0; l < n; ++l) { ho[l] = &b[l].e->ho[b[l].run]; sd[l] = b[l].e->tailSeed; if (b[l].e->fs != b[0].e->fs) same = false; }
      hclive::StreamBase* s = same ? set.Get(anchor) : nullptr;
      ok = s && s->RenderTails(n, ho, sd, static_cast<double>(b[0].e->fs), out);
      if (ok) { laneBatches_.fetch_add(1); laneRuns_.fetch_add(n); }
    }
    for (int l = 0; l < n; ++l)
    {
      hclive::Entry* e = b[l].e; const int run = b[l].run;
      bool good = ok; if (ok) { e->pre[run] = std::move(out[l]); good = !e->pre[run].empty(); } else { good = ScalarTail(set, e, run); if (good) scalarPre_.fetch_add(1); }
      heldCount_.fetch_sub(1); RunDone(e, good);
    }
    laneCpuUs_.fetch_add(hclive::ThreadCpuUs() - cpu0);
  }
  void SubmitTails(hclive::StreamSet& set, hclive::Entry* e)
  {
    e->tailSeed = static_cast<long long>(tailCounter_.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFFFFFLL);
    const int n = e->n; int anchor[2] = {e->anchor[0], e->anchor[1]};
    e->runsLeft.v.store(n); e->preFail.v.store(0); heldCount_.fetch_add(n); inflight_[e->level].fetch_sub(1); heldE_[e->level].fetch_add(1);
    for (int i = 0; i < n; ++i)   // after the last push the entry may be finished and played by other threads: only the copies above are used
    {
      std::vector<Held> batch;
      {
        std::lock_guard<std::mutex> lk(packMu_[anchor[i]]); pack_[anchor[i]].push_back(Held{e, i, std::chrono::steady_clock::now()});
        if (!LanesOn() || static_cast<int>(pack_[anchor[i]].size()) >= hclive::kLanes) { batch = std::move(pack_[anchor[i]]); pack_[anchor[i]].clear(); }
      }
      if (!batch.empty()) RunBatch(set, anchor[i], batch);
    }
  }
  // batches that have waited too long (or all of them: force) go as they are
  void FlushAged(hclive::StreamSet& set, bool force)
  {
    if (heldCount_.load() <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    for (int a = 0; a < kTr909AnchorCount; ++a)
    {
      std::vector<Held> batch;
      {
        std::lock_guard<std::mutex> lk(packMu_[a]);
        if (!pack_[a].empty() && (force || now - pack_[a].front().t > std::chrono::milliseconds(MaxHeldMs()))) { batch = std::move(pack_[a]); pack_[a].clear(); }
      }
      if (!batch.empty()) RunBatch(set, a, batch);
    }
  }

  // Renderer: continues a hit from its prepared state and publishes the samples as it goes.
  void RenderLoop()
  {
    hclive::StreamSet set(masters_, false);
    while (running_.load())
    {
      FreeGraveyard();
      hclive::Job* j = nullptr;
      if (!jobs_.Pop(j)) { jobSem_.Wait(1000); continue; }
      RenderJob(set, *j);
    }
  }
  void RenderJob(hclive::StreamSet& set, hclive::Job& j)
  {
    const long long cpu0 = hclive::ThreadCpuUs();
    hclive::Hit& h = *j.hit; hclive::Entry* const ent = h.entry; const hclive::Entry& e = *ent; const int r = j.run, len = h.len.load(std::memory_order_acquire), head = h.head; const bool shared = e.shared;
    hclive::StreamBase* s = set.Get(e.anchor[r]);
    int pos = 0;
    if (s)
    {
      s->Import(e.seed, e.ho[r]);
      if (s->Begin(e.seed, static_cast<double>(e.fs), hclive::kHitSeconds, h.tailSeed))
      {
        float tmp[256];
        while (pos < len)
        {
          const int w = s->Pull(tmp, std::min(256, len - pos)); if (w <= 0) break;
          for (int i = 0; i < w; ++i) if (pos + i >= head) h.buf[r][static_cast<size_t>(pos + i)] = tmp[i];
          pos += w; if (pos > head) h.produced[r].store(pos, std::memory_order_release);
        }
      }
      if (!e.shared) s->Erase(e.seed);
    }
    const int end = std::max(pos, head);
    for (int i = end; i < len; ++i) h.buf[r][static_cast<size_t>(i)] = 0.0f;   // a short or failed run ends in silence
    { int cur = h.len.load(); while (end < cur && !h.len.compare_exchange_weak(cur, end)) {} }   // the hit ends where its stream ends
    h.produced[r].store(end, std::memory_order_release);
    renderCpuUs_.fetch_add(hclive::ThreadCpuUs() - cpu0); renderJobs_.fetch_add(1);
    const int entLevel = e.level; const int entFs = e.fs;
    if (h.pending.fetch_sub(1, std::memory_order_acq_rel) == 1 && !shared)   // the last run of the hit gives its entry to the recycle ring (or frees it); the slot may be reused from here on, so nothing of it is read again
    {
      if (ent->hasPre) { std::vector<float>().swap(ent->pre[0]); std::vector<float>().swap(ent->pre[1]); ent->hasPre = false; }   // played with the render jobs (fresh tail off): the samples ahead are of no use any more
      if (entFs != fs_.load() || !recycle_[entLevel]->Push(ent)) delete ent;
    }
  }
  void FreeGraveyard()
  {
    hclive::Entry* e; while (graveyard_.Pop(e)) delete e;
    while (spent_.Pop(e))   // an entry whose rendered hit was played: the samples go, the entry plays again through the render jobs
    {
      std::vector<float>().swap(e->pre[0]); std::vector<float>().swap(e->pre[1]); e->hasPre = false;
      if (e->fs != fs_.load() || !recycle_[e->level]->Push(e)) delete e;
    }
  }

  hclive::Masters masters_; hclive::Bases bases_; std::atomic<unsigned> baseCursor_{0};
  int nRender_ = 0;
  std::atomic<bool> running_{false}, ready_{false};
  std::atomic<int> fs_{0}, epoch_{0};
  std::mutex cfgMu_; hclive::Semaphore jobSem_, prepSem_;
  std::vector<std::thread> threads_;
  std::unique_ptr<hclive::MpmcQueue<hclive::Entry*>> rings_[kLevels], recycle_[kLevels];
  std::atomic<int> fill_[kLevels], inflight_[kLevels], heldE_[kLevels], used_[kLevels];
  std::atomic<hclive::Entry*> shared_[kLevels];
  std::vector<std::unique_ptr<hclive::Entry>> retired_;
  int levelCode_[kLevels];
  std::unique_ptr<hclive::Hit[]> slots_;
  hclive::MpmcQueue<hclive::Job*> jobs_;
  hclive::MpmcQueue<hclive::Entry*> graveyard_, spent_;
  std::mutex packMu_[kTr909AnchorCount]; std::vector<Held> pack_[kTr909AnchorCount]; std::atomic<int> heldCount_{0};
  std::atomic<unsigned long> laneBatches_{0}, laneRuns_{0}, scalarPre_{0}, preHits_{0}; std::atomic<long long> laneCpuUs_{0};
  std::atomic<uint64_t> nextSeed_{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) + 100000};
  std::atomic<bool> freshTail_{true}; std::atomic<uint64_t> tailCounter_{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000003) * 7919 + 500000000ULL};
  std::atomic<unsigned long> throttled_{0}, misses_{0}, repeats_{0}, sharedRepeats_{0}, underruns_{0}, prepares_{0}, entryRuns_{0}, renderJobs_{0}; std::atomic<long long> entryCpuUs_{0}, renderCpuUs_{0};
};

// The voice of the Hand Clap 909: the live engine when a prepared entry is available, the baked set of the cache otherwise.
class Tr909HcVoice : public Tr909CachedVoice
{
public:
  explicit Tr909HcVoice(Tr909Voice v = Tr909Voice::Hc) : Tr909CachedVoice(v) {}
  // Another engine (or none): the slot of a hit that is still playing goes back to the engine it came from (call while that engine exists).
  void SetEngine(Tr909HcLiveEngine* e) { Tr909HcLiveEngine* old = engine_.load(std::memory_order_acquire); if (e != old && mHit) { if (old) Tr909HcLiveEngine::Release(mHit); mHit = nullptr; } engine_.store(e, std::memory_order_release); }
  ~Tr909HcVoice() override { if (mHit && engine_.load(std::memory_order_acquire)) Tr909HcLiveEngine::Release(mHit); mHit = nullptr; }

  void Trigger(double velocity01) override
  {
    if (mHit) { Tr909HcLiveEngine::Release(mHit); mHit = nullptr; }
    Tr909HcLiveEngine* e = engine_.load(std::memory_order_acquire);
    if (e) mHit = e->Trigger(velocity01);
    if (mHit)
    {
      mPos = 0; mFrac = 0.0; mRate = std::clamp(pitchRatio, 0.25, 4.0); mGain = std::clamp(levelKnob, 0.0, 1.0) / kLevelKnobBaked; return;
    }
    Tr909CachedVoice::Trigger(velocity01);
  }
  void Process(double* out, int nFrames) override
  {
    if (!mHit) { Tr909CachedVoice::Process(out, nFrames); return; }
    Tr909HcLiveEngine* e = engine_.load(std::memory_order_acquire);
    const hclive::Hit& h = *mHit; const int len = h.len.load(std::memory_order_acquire); int avail = Tr909HcLiveEngine::Available(mHit);
    auto at = [&](long j) -> double
    {
      if (j < 0 || j >= len) return 0.0;
      if (j >= avail) { avail = Tr909HcLiveEngine::Available(mHit); if (j >= avail) return 0.0; }
      double v = h.w[0] * static_cast<double>(h.buf[0][static_cast<size_t>(j)]);
      if (h.n > 1) v += h.w[1] * static_cast<double>(h.buf[1][static_cast<size_t>(j)]);
      return v;
    };
    if (std::fabs(mRate - 1.0) < 1e-12)
    {
      for (int i = 0; i < nFrames && mPos < static_cast<size_t>(len); ++i, ++mPos)
      {
        if (static_cast<int>(mPos) >= avail) { avail = Tr909HcLiveEngine::Available(mHit); if (static_cast<int>(mPos) >= avail && e && static_cast<int>(mPos) < len) e->CountUnderrun(); }
        out[i] += kTr909VoltToSample * mGain * at(static_cast<long>(mPos));
      }
    }
    else
    {
      for (int i = 0; i < nFrames && mPos < static_cast<size_t>(len); ++i)
      {
        const long j = static_cast<long>(mPos); const double t = mFrac, y0 = at(j - 1), y1 = at(j), y2 = at(j + 1), y3 = at(j + 2);
        out[i] += kTr909VoltToSample * mGain * (y1 + 0.5 * t * (y2 - y0 + t * (2.0 * y0 - 5.0 * y1 + 4.0 * y2 - y3 + t * (3.0 * (y1 - y2) + y3 - y0))));
        mFrac += mRate; const size_t adv = static_cast<size_t>(mFrac); mPos += adv; mFrac -= static_cast<double>(adv);
      }
    }
    if (mPos >= static_cast<size_t>(len)) { Tr909HcLiveEngine::Release(mHit); mHit = nullptr; }
  }
  bool IsActive() const override { return mHit != nullptr || Tr909CachedVoice::IsActive(); }
  void Choke() { if (mHit) { Tr909HcLiveEngine::Release(mHit); mHit = nullptr; } Tr909CachedVoice::Choke(); }
  bool Live() const { return mHit != nullptr; }

private:
  std::atomic<Tr909HcLiveEngine*> engine_{nullptr};
  hclive::Hit* mHit = nullptr; size_t mPos = 0; double mFrac = 0.0, mRate = 1.0, mGain = 1.0;
};

}  // namespace dgg
