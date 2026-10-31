// Table packages of the hand clap (HC) hybrid model (tr909/HcHybrid.h): one file per accent anchor, "hc_a<anchor>.hcpk", read once and shared by all bake threads.
// The packages are part of the model: Tr909FullModelRunner renders the HC at the 5 us noise path only from a package (a missing or unreadable package makes the run fail,
// so that no set baked without it is stored under the model version that means "HC on the hybrid model"; see kTr909ModelVersion).
#pragma once
#include "tr909/HcHybrid.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#if defined(_WIN32) && defined(DGG_HC_EMBEDDED)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dgg {

#if defined(_WIN32) && defined(DGG_HC_EMBEDDED)
// The packages are RCDATA resources "DGG_HC_A<anchor>" of the module that contains this code (the plugin DLL); the memory is mapped by the loader and read in place.
inline bool Tr909EmbeddedHcPackage(int anchorCode, const uint8_t** data, size_t* size)
{
  HMODULE mod = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&Tr909EmbeddedHcPackage), &mod)) return false;
  const std::string name = "DGG_HC_A" + std::to_string(anchorCode);
  HRSRC r = FindResourceA(mod, name.c_str(), reinterpret_cast<LPCSTR>(RT_RCDATA)); if (!r) return false;
  HGLOBAL g = LoadResource(mod, r); if (!g) return false;
  const void* p = LockResource(g); const DWORD n = SizeofResource(mod, r); if (!p || n == 0) return false;
  *data = static_cast<const uint8_t*>(p); *size = n; return true;
}
#endif

class Tr909HcPackageStore
{
public:
  static Tr909HcPackageStore& Instance() { static Tr909HcPackageStore s; return s; }
  // Directory with the hc_a<anchor>.hcpk files (builds without embedded packages, tests); may be set again (packages already loaded stay). Ignored when the packages are embedded.
  void SetDirectory(const std::filesystem::path& dir) { std::lock_guard<std::mutex> lk(mu_); if (dir_ != dir) { dir_ = dir; failed_.clear(); } }
  // The package of an anchor, nullptr when the file is missing or invalid. Thread safe; the first call per anchor reads the file (about 11 MB).
  const hch::HcPackage* Get(int anchorCode)
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pkgs_.find(anchorCode); if (it != pkgs_.end()) return it->second.get();
#if defined(_WIN32) && defined(DGG_HC_EMBEDDED)
    {   // the plugin carries its packages
      const uint8_t* d = nullptr; size_t n = 0;
      if (failed_.count(anchorCode) || !Tr909EmbeddedHcPackage(anchorCode, &d, &n)) { failed_[anchorCode] = true; return nullptr; }
      std::unique_ptr<hch::HcPackage> e(new hch::HcPackage());
      try { if (!e->Open(d, n) || e->meta["model"] != hch::kModelVer) { failed_[anchorCode] = true; return nullptr; } }   // a package of another model version is not used
      catch (const std::exception&) { failed_[anchorCode] = true; return nullptr; }
      return (pkgs_[anchorCode] = std::move(e)).get();
    }
#endif
    if (dir_.empty() || failed_.count(anchorCode)) return nullptr;
    std::unique_ptr<hch::HcPackage> p(new hch::HcPackage());
    try
    {
      std::ifstream f(dir_ / ("hc_a" + std::to_string(anchorCode) + ".hcpk"), std::ios::binary); if (!f) { failed_[anchorCode] = true; return nullptr; }
      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (!p->Open(std::move(bytes)) || p->meta["model"] != hch::kModelVer) { failed_[anchorCode] = true; return nullptr; }   // a package of another model version is not used
    }
    catch (const std::exception&) { failed_[anchorCode] = true; return nullptr; }
    return (pkgs_[anchorCode] = std::move(p)).get();
  }
  std::function<const hch::HcPackage*(int)> Provider() { return [this](int code) { return Get(code); }; }

private:
  std::mutex mu_; std::filesystem::path dir_; std::map<int, std::unique_ptr<hch::HcPackage>> pkgs_; std::map<int, bool> failed_;
};

}  // namespace dgg
