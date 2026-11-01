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
#include "909Playback.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
  virtual bool Export(uint64_t seed, hch::HcHandover& out) = 0;
  virtual void Import(uint64_t seed, const hch::HcHandover& h) = 0;
  virtual void Erase(uint64_t seed) = 0;
  virtual bool Begin(uint64_t seed, double fs, double seconds) = 0;
  virtual int Pull(float* out, int n) = 0;
};
template <class Mdl>
struct StreamImpl : StreamBase
{
  bool Load(const hch::HcPackage& pkg) override { return hs.Load(pkg); }
  bool LoadFrom(StreamBase& master, bool warmEngine) override { return hs.LoadFrom(static_cast<StreamImpl<Mdl>&>(master).hs, warmEngine); }
  bool Prepare(uint64_t seed) override { return hs.Prepare(seed); }
  bool Export(uint64_t seed, hch::HcHandover& out) override { return hs.ExportEntry(seed, out); }
  void Import(uint64_t seed, const hch::HcHandover& h) override { hs.ImportEntry(seed, h); }
  void Erase(uint64_t seed) override { hs.EraseEntry(seed); }
  bool Begin(uint64_t seed, double fs, double seconds) override { return hs.Begin(seed, fs, seconds); }
  int Pull(float* out, int n) override { return hs.Pull(out, n); }
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

constexpr double kHitSeconds = 0.35;      // length of a hit (as the baked sets)
constexpr double kHeadSeconds = 0.0385;   // the part of a hit that a prepared entry carries as host samples (the handover state is at 40 ms)
constexpr int kMaxRate = 192000;
constexpr int kSlots = 24;

// a prepared hit: handover state and head samples of one or two anchor runs (accent between two anchors = blend of two runs with the same seed)
struct Entry
{
  int level = 0, code = 0, fs = 0, n = 0, anchor[2] = {0, 0}; double w[2] = {1.0, 0.0}; uint64_t seed = 0; bool shared = false;   // shared: the fallback entry of a level, never freed, played again
  hch::HcHandover ho[2]; std::vector<float> head[2];
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
  Entry* entry = nullptr; Job job[2]; uint64_t seed = 0; int anchor[2] = {0, 0};   // seed and anchors of the hit (diagnosis)
  Hit() { produced[0].store(0); produced[1].store(0); }
};

}  // namespace hclive

class Tr909HcLiveEngine
{
public:
  static constexpr int kLevels = kTr909VelocityLevels, kPrepThreads = 2, kRing = 8;
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
    running_.store(false); { std::lock_guard<std::mutex> lk(wakeMu_); } wake_.notify_all();
    for (std::thread& t : threads_) if (t.joinable()) t.join();
    for (int l = 0; l < kLevels; ++l) { hclive::Entry* e; while (rings_[l]->Pop(e)) delete e; delete shared_[l].load(); }
    hclive::Job* j; while (jobs_.Pop(j)) {}
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
      for (int l = 0; l < kLevels; ++l) { hclive::Entry* e; while (rings_[l]->Pop(e)) delete e; fill_[l].store(0); hclive::Entry* s = shared_[l].exchange(nullptr); retired_.push_back(std::unique_ptr<hclive::Entry>(s)); }
      ready_.store(true);
    }
    if (threads_.empty())
    {
      running_.store(true);
      for (int i = 0; i < kPrepThreads; ++i) threads_.emplace_back([this, i] { PrepLoop(i); });
      nRender_ = RenderThreads();
      for (int i = 0; i < nRender_; ++i) threads_.emplace_back([this] { RenderLoop(); });
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
    else { e = shared_[level].load(std::memory_order_acquire); own = false; ++repeats_; }
    Wake();   // the store of this level wants a new entry
    if (!e || e->fs != fs_.load(std::memory_order_relaxed)) { if (own && e) graveyard_.Push(e); ++misses_; return nullptr; }
    hclive::Hit* h = nullptr;
    for (int i = 0; i < hclive::kSlots && !h; ++i)
    {
      hclive::Hit& s = slots_[i]; int st = s.state.load(std::memory_order_acquire);
      if ((st == 0 || (st == 2 && s.pending.load(std::memory_order_acquire) == 0)) && s.state.compare_exchange_strong(st, 1, std::memory_order_acq_rel)) h = &s;
    }
    if (!h) { if (own) graveyard_.Push(e); ++misses_; return nullptr; }   // an entry that is not used is freed by a worker
    h->n = e->n; h->fs = e->fs; h->len.store(std::min(static_cast<int>(hclive::kHitSeconds * e->fs), static_cast<int>(h->buf[0].size())), std::memory_order_release); h->head = static_cast<int>(e->head[0].size());
    h->entry = e; h->w[0] = e->w[0]; h->w[1] = e->w[1]; h->seed = e->seed; h->anchor[0] = e->anchor[0]; h->anchor[1] = e->anchor[1];
    for (int r = 0; r < e->n; ++r)
    {
      std::copy(e->head[r].begin(), e->head[r].end(), h->buf[r].begin());
      h->produced[r].store(h->head, std::memory_order_release);
    }
    h->pending.store(e->n, std::memory_order_release);
    for (int r = 0; r < e->n; ++r) { h->job[r].hit = h; h->job[r].run = r; jobs_.Push(&h->job[r]); }
    Wake();
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

  bool Ready() const { return ready_.load(std::memory_order_acquire); }
  // number of prepared entries of a level, and counters (diagnosis)
  int Prepared(int level) const { return fill_[level].load(); }
  unsigned long Misses() const { return misses_.load(); }
  unsigned long Repeats() const { return repeats_.load(); }
  unsigned long Underruns() const { return underruns_.load(); }
  void CountUnderrun() { underruns_.fetch_add(1, std::memory_order_relaxed); }
  unsigned long Prepares() const { return prepares_.load(); }

private:
  Tr909HcLiveEngine() : jobs_(256), graveyard_(256)
  {
    for (int l = 0; l < kLevels; ++l)
    {
      rings_[l].reset(new hclive::MpmcQueue<hclive::Entry*>(kRing)); fill_[l].store(0); used_[l].store(0); shared_[l].store(nullptr);
      levelCode_[l] = Tr909CodeForVelocity(Tr909Voice::Hc, static_cast<double>(l) / (kLevels - 1));
    }
    slots_.reset(new hclive::Hit[hclive::kSlots]);
    for (int i = 0; i < hclive::kSlots; ++i) for (int r = 0; r < 2; ++r) slots_[i].buf[r].assign(static_cast<size_t>(hclive::kHitSeconds * hclive::kMaxRate) + 64, 0.0f);
  }
  void Wake() { wake_.notify_all(); }

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
        const int target = 1 + std::min(used_[l].load(std::memory_order_relaxed), 3), have = fill_[l].load() + inflight_[l].load();
        if (have >= target) continue;
        const int score = have * 16 - std::min(used_[l].load(), 8);
        if (score < bestScore) { bestScore = score; best = l; }
      }
      if (best < 0) { std::unique_lock<std::mutex> lk(wakeMu_); wake_.wait_for(lk, std::chrono::milliseconds(50)); continue; }
      inflight_[best].fetch_add(1);
      hclive::Entry* e = BuildEntry(set, best);
      inflight_[best].fetch_sub(1);
      if (!e) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
      if (e->fs != fs_.load()) { delete e; continue; }
      prepares_.fetch_add(1);
      if (!shared_[best].load()) { hclive::Entry* c = new hclive::Entry(*e); c->shared = true; hclive::Entry* none = nullptr; if (!shared_[best].compare_exchange_strong(none, c)) delete c; }
      if (rings_[best]->Push(e)) fill_[best].fetch_add(1); else delete e;
    }
  }
  hclive::Entry* BuildEntry(hclive::StreamSet& set, int level)
  {
    const int fs = fs_.load(); const int code = levelCode_[level];
    Tr909AccentRun runs[2]; const int n = Tr909AccentRuns(Tr909Voice::Hc, code, runs); if (n == 0) return nullptr;
    std::unique_ptr<hclive::Entry> e(new hclive::Entry()); e->level = level; e->code = code; e->fs = fs; e->n = n; e->seed = nextSeed_.fetch_add(1);
    const long headLen = static_cast<long>(hclive::kHeadSeconds * fs);
    for (int i = 0; i < n; ++i)
    {
      int a = -1; for (int k = 0; k < kTr909AnchorCount; ++k) if (kTr909Anchors[k] == runs[i].anchorCode) a = k;
      hclive::StreamBase* s = set.Get(a); if (!s) return nullptr;
      e->anchor[i] = a; e->w[i] = runs[i].weight;
      if (!s->Prepare(e->seed) || !s->Export(e->seed, e->ho[i])) return nullptr;
      if (!s->Begin(e->seed, static_cast<double>(fs), hclive::kHitSeconds)) { s->Erase(e->seed); return nullptr; }
      e->head[i].assign(static_cast<size_t>(headLen), 0.0f);
      long got = 0; while (got < headLen) { const int w = s->Pull(e->head[i].data() + got, static_cast<int>(std::min<long>(headLen - got, 1024))); if (w <= 0) break; got += w; }
      s->Erase(e->seed);
      if (got < headLen) return nullptr;
    }
    return e.release();
  }

  // Renderer: continues a hit from its prepared state and publishes the samples as it goes.
  void RenderLoop()
  {
    hclive::StreamSet set(masters_, false);
    while (running_.load())
    {
      FreeGraveyard();
      hclive::Job* j = nullptr;
      if (!jobs_.Pop(j)) { std::unique_lock<std::mutex> lk(wakeMu_); wake_.wait_for(lk, std::chrono::milliseconds(5)); continue; }
      RenderJob(set, *j);
    }
  }
  void RenderJob(hclive::StreamSet& set, hclive::Job& j)
  {
    hclive::Hit& h = *j.hit; hclive::Entry* const ent = h.entry; const hclive::Entry& e = *ent; const int r = j.run, len = h.len.load(std::memory_order_acquire), head = h.head; const bool shared = e.shared;
    hclive::StreamBase* s = set.Get(e.anchor[r]);
    int pos = 0;
    if (s)
    {
      s->Import(e.seed, e.ho[r]);
      if (s->Begin(e.seed, static_cast<double>(e.fs), hclive::kHitSeconds))
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
    if (h.pending.fetch_sub(1, std::memory_order_acq_rel) == 1 && !shared) delete ent;   // the last run of the hit frees its entry (the slot may be reused from here on, so nothing of it is read again)
  }
  void FreeGraveyard() { hclive::Entry* e; while (graveyard_.Pop(e)) delete e; }

  hclive::Masters masters_;
  int nRender_ = 0;
  std::atomic<bool> running_{false}, ready_{false};
  std::atomic<int> fs_{0}, epoch_{0};
  std::mutex cfgMu_, wakeMu_; std::condition_variable wake_;
  std::vector<std::thread> threads_;
  std::unique_ptr<hclive::MpmcQueue<hclive::Entry*>> rings_[kLevels];
  std::atomic<int> fill_[kLevels], inflight_[kLevels], used_[kLevels];
  std::atomic<hclive::Entry*> shared_[kLevels];
  std::vector<std::unique_ptr<hclive::Entry>> retired_;
  int levelCode_[kLevels];
  std::unique_ptr<hclive::Hit[]> slots_;
  hclive::MpmcQueue<hclive::Job*> jobs_;
  hclive::MpmcQueue<hclive::Entry*> graveyard_;
  std::atomic<uint64_t> nextSeed_{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) + 100000};
  std::atomic<unsigned long> misses_{0}, repeats_{0}, underruns_{0}, prepares_{0};
};

// The voice of the Hand Clap 909: the live engine when a prepared entry is available, the baked set of the cache otherwise.
class Tr909HcVoice : public Tr909CachedVoice
{
public:
  explicit Tr909HcVoice(Tr909Voice v = Tr909Voice::Hc) : Tr909CachedVoice(v) {}
  void SetEngine(Tr909HcLiveEngine* e) { engine_.store(e, std::memory_order_release); }
  ~Tr909HcVoice() override = default;

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
