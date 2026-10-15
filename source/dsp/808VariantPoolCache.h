#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <random>
#include <filesystem>
#include <string>

// Shared multi-variant render-cache for the 808 voices (Ride/Cymbal share
// TR808RideVoice; HiHat is its own class). The real 808 metal-bank/
// MNA oscillators never repeat a hit identically, so this stores up to
// `TargetVariantCount()` independent recordings per (quantized) key and
// serves a random one per Trigger() instead of always the same one.
//
// This class never simulates anything -- callers (the voice's Trigger(),
// or a background baker) hand it finished buffers, recorded off the audio
// thread. This pool is the final-playback path (several variants, no
// interpolation); a hit without a baked variant is synthesised live by the
// voice's fast model.
//
// Thread-safety: every method locks internally. GetRandom() is meant to be
// called from the audio thread (lock-free would be nicer, but contention
// is negligible -- a handful of voices, no allocation under the lock,
// same tradeoff the single-buffer caches already made). AddVariant() is
// meant to be called from a background bake thread, never the audio
// thread (rendering a variant runs a full multi-second simulation).
namespace dgg
{

template <typename Key, typename KeyHash = std::hash<Key>>
class TR808VariantPoolCache
{
public:
  struct Status { int ready = 0; int target = 0; };

  // n clamped to [1,20] (a free-running oscillator can't be fully captured by any finite pool, but a pool this size
  // already gives audible variance without unbounded memory/disk/bake time).
  static constexpr int kMinVariants = 1;
  static constexpr int kMaxVariants = 20;

  explicit TR808VariantPoolCache(int defaultTarget = 7) { SetTargetVariantCount(defaultTarget); }

  void SetTargetVariantCount(int n)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mTarget = std::clamp(n, kMinVariants, kMaxVariants);
  }
  int TargetVariantCount() { std::lock_guard<std::mutex> lock(mMutex); return mTarget; }

  // Returns a random ready variant for `key`, or nullptr if none are
  // ready yet (caller's job to fall back -- e.g. to the reference
  // library, or to enqueue a bake job -- this class doesn't know about
  // either).
  std::shared_ptr<const std::vector<float>> GetRandom(const Key& key)
  {
    int unused;
    return GetRandomIndexed(key, unused);
  }

  // Same as GetRandom(), but also reports which pool index was picked
  // (and how many are in the pool) via outIndex/outPoolSize -- debug-only,
  // lets a caller log "which of the N baked variants played this hit"
  // without adding any behavior. outIndex is -1 and outPoolSize is 0 on
  // a miss.
  std::shared_ptr<const std::vector<float>> GetRandomIndexed(const Key& key, int& outIndex, int* outPoolSize = nullptr)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mMap.find(key);
    if (it == mMap.end() || it->second.empty())
    {
      outIndex = -1;
      if (outPoolSize) *outPoolSize = 0;
      return nullptr;
    }
    const size_t idx = (size_t)(mRng() % it->second.size());
    outIndex = (int)idx;
    if (outPoolSize) *outPoolSize = (int)it->second.size();
    return it->second[idx];
  }

  Status GetStatus(const Key& key)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mMap.find(key);
    return { (it == mMap.end()) ? 0 : (int)it->second.size(), mTarget };
  }

  // How many more variants `key` needs right now to reach the current
  // target -- 0 once full. A bake job reads this once at start (not
  // re-checked per variant), so a mid-bake target change takes effect on
  // the NEXT bake request for that key, not retroactively -- simplest
  // behaviour, matches how decayKnob01/tone/level etc. are already only
  // read at Trigger()-time elsewhere in this codebase.
  int VariantsNeeded(const Key& key)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mMap.find(key);
    const int ready = (it == mMap.end()) ? 0 : (int)it->second.size();
    return std::max(0, mTarget - ready);
  }


  // Appends one freshly rendered variant. No-op past the current target
  // (guards against an in-flight bake job finishing after a target
  // decrease, or two bake jobs racing on the same key).
  void AddVariant(const Key& key, std::vector<float>&& buf)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto& variants = mMap[key];
    if ((int)variants.size() >= mTarget) return;
    variants.push_back(std::make_shared<std::vector<float>>(std::move(buf)));
  }

  // Drops every variant of `key` (a rebake replaces the whole set).
  void ClearKey(const Key& key) { std::lock_guard<std::mutex> lock(mMutex); mMap.erase(key); }

  void ClearForTest() { std::lock_guard<std::mutex> lock(mMutex); mMap.clear(); }

private:
  std::mutex mMutex;
  std::unordered_map<Key, std::vector<std::shared_ptr<const std::vector<float>>>, KeyHash> mMap;
  int mTarget;
  std::mt19937 mRng{std::random_device{}()};
};

// Result of EvictCacheIfOverBudget() -- what it actually deleted, so a
// caller (or a test) can assert on it directly instead of re-scanning the
// directory.
struct CacheEvictionResult
{
  int groupsEvicted = 0;
  uintmax_t bytesEvicted = 0;
  uintmax_t bytesRemaining = 0;
};

// Size-cap eviction for the variant cache folders (the directories and the protected sets are arguments).
// Deletes whole sets (all "<baseName>_v<k>.f32" files sharing a base name in one directory), never single files:
// a partially evicted set is as useless as an empty one. Oldest set first, by the newest last-write-time among its
// files. Sets in `protectedBaseNames` (the default sets) are never deleted but still count toward the total.
// Stops when the total is at or under `maxBytes` or nothing evictable is left.
inline CacheEvictionResult EvictCacheIfOverBudget(const std::vector<std::filesystem::path>& cacheDirs, uintmax_t maxBytes,
                                                  const std::unordered_set<std::string>& protectedBaseNames)
{
  // newestWrite starts at the true minimum time_point: a default-constructed
  // one is epoch 0, which real file times can compare LESS than, so the
  // `mtime > newestWrite` update below would never fire. The parentheses
  // around `min` guard against <windows.h>'s min/max macros.
  struct KeyGroup { std::vector<std::filesystem::path> files; uintmax_t totalBytes = 0; std::filesystem::file_time_type newestWrite = (std::filesystem::file_time_type::min)(); };
  std::unordered_map<std::string, KeyGroup> groups;
  uintmax_t totalBytes = 0;

  for (const auto& dir : cacheDirs)
  {
    std::error_code dirEc;
    for (const auto& entry : std::filesystem::directory_iterator(dir, dirEc))
    {
      if (dirEc || !entry.is_regular_file()) continue;
      const auto& path = entry.path();
      if (path.extension() != ".f32") continue;
      const std::string nameNoExt = path.stem().string();  // "<baseName>_v<k>"
      const size_t vPos = nameNoExt.rfind("_v");
      if (vPos == std::string::npos) continue;

      std::error_code sizeEc, timeEc;
      const auto size = std::filesystem::file_size(path, sizeEc);
      const auto mtime = std::filesystem::last_write_time(path, timeEc);
      if (sizeEc || timeEc) continue;
      totalBytes += size;

      const std::string baseName = nameNoExt.substr(0, vPos);
      if (protectedBaseNames.count(baseName)) continue;

      auto& group = groups[dir.string() + "/" + baseName];
      group.files.push_back(path);
      group.totalBytes += size;
      if (mtime > group.newestWrite) group.newestWrite = mtime;
    }
  }

  CacheEvictionResult result;
  result.bytesRemaining = totalBytes;
  if (totalBytes <= maxBytes) return result;

  std::vector<KeyGroup*> ordered;
  ordered.reserve(groups.size());
  for (auto& [id, group] : groups) ordered.push_back(&group);
  std::sort(ordered.begin(), ordered.end(), [](const KeyGroup* a, const KeyGroup* b) {
    return a->newestWrite < b->newestWrite;
  });

  for (KeyGroup* group : ordered)
  {
    if (totalBytes <= maxBytes) break;
    std::error_code ec;
    for (const auto& f : group->files) std::filesystem::remove(f, ec);
    totalBytes -= group->totalBytes;
    result.groupsEvicted++;
    result.bytesEvicted += group->totalBytes;
  }
  result.bytesRemaining = totalBytes;
  return result;
}

}  // namespace dgg
