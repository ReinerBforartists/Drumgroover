// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#include "RhythmEngine.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>

// The formula iterations (mandel, julia, logi) must round exactly like the JavaScript version: no fused multiply-add.
#ifdef _MSC_VER
#pragma fp_contract(off)
#endif

namespace dgg {

// ---- RNG ----
// JS: this.s = seed >>> 0 || (Date.now() & 0xffffffff)
// uint32_t arithmetic wraps the same way `>>> 0` masks in JS, so the LCG
// step below is bit-identical to `(this.s * 1664525 + 1013904223) >>> 0`.
RNG::RNG(uint32_t seed) {
  s = seed != 0 ? seed
                : static_cast<uint32_t>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() &
                      0xffffffffu);
}

double RNG::next() {
  s = static_cast<uint32_t>(s * 1664525u + 1013904223u);
  return static_cast<double>(s) / 4294967295.0; // 0xffffffff
}

int RNG::intRange(int a, int b) {
  return a + static_cast<int>(std::floor(next() * (b - a + 1)));
}

bool RNG::chance(double p) { return next() < p; }

bool hasNoteNear(const std::vector<double>& beats, double beat, double eps) {
  return std::any_of(beats.begin(), beats.end(),
                      [&](double n) { return std::fabs(n - beat) < eps; });
}

std::vector<int> backbeatPositions(int numerator) {
  std::vector<int> out;
  for (int b = 1; b < numerator; b += 2) out.push_back(b);
  return out;
}

std::vector<bool> euclideanRhythm(int hits, int steps, uint32_t seed) {
  if (steps <= 0) return {};
  if (hits <= 0) return std::vector<bool>(steps, false);
  if (hits >= steps) return std::vector<bool>(steps, true);

  std::vector<bool> raw;
  raw.reserve(steps);
  int bucket = 0;
  for (int i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) { bucket -= steps; raw.push_back(true); }
    else raw.push_back(false);
  }

  int firstHit = 0;
  while (firstHit < steps && !raw[firstHit]) firstHit++;
  // JS `raw.indexOf(true)` returns -1 if none found, then `.slice(-1)` /
  // `.slice(0,-1)` behave oddly — but that branch is unreachable here since
  // hits>0 guarantees at least one true, so firstHit < steps always holds.

  std::vector<bool> mask;
  mask.reserve(steps);
  for (int i = firstHit; i < steps; i++) mask.push_back(raw[i]);
  for (int i = 0; i < firstHit; i++) mask.push_back(raw[i]);

  return seed ? seededShuffleMask(std::move(mask), seed) : mask;
}

std::vector<bool> seededShuffleMask(std::vector<bool> mask, uint32_t seed) {
  RNG rng(seed);
  std::vector<bool> out = mask;
  int hitCount = static_cast<int>(std::count(out.begin(), out.end(), true));
  int shuffles = std::max(1, static_cast<int>(std::round(hitCount * 0.3)));
  const int n = static_cast<int>(out.size());

  for (int sIdx = 0; sIdx < shuffles; sIdx++) {
    std::vector<int> hitIdxs;
    for (int i = 0; i < n; i++) if (out[i]) hitIdxs.push_back(i);
    if (hitIdxs.empty()) break;

    int pick = hitIdxs[rng.intRange(0, static_cast<int>(hitIdxs.size()) - 1)];
    int dir = rng.chance(0.5) ? 1 : -1;
    int target = ((pick + dir) % n + n) % n;
    if (!out[target]) { out[pick] = false; out[target] = true; }
  }
  return out;
}

// ---- Pattern modes ----
namespace {

uint32_t mix32(uint32_t x) {
  x = (x ^ (x >> 16)) * 0x7feb352du;
  x = (x ^ (x >> 15)) * 0x846ca68bu;
  return x ^ (x >> 16);
}
uint32_t seedStream(uint32_t seed, uint32_t k) { return mix32(seed + (k + 1u) * 0x9e3779b9u); }

double fastSin(double x) {
  const double y = 1.2732395447351628 * x - 0.4052847345693511 * x * std::fabs(x);
  return 0.225 * (y * std::fabs(y) - y) + y;
}

std::vector<int64_t> scoreMetric(const std::vector<int>& positions, uint32_t seed, int spb) {
  std::vector<int64_t> out;
  out.reserve(positions.size());
  for (int p : positions) {
    const int inBeat = p % spb;
    int w;
    if (p == 0) w = 1000;
    else if (inBeat == 0) w = ((p / spb) % 2 == 0) ? 48 : 40;
    else if (spb % 2 == 0 && inBeat == spb / 2) w = 24;
    else if (spb % 4 == 0 && inBeat % (spb / 4) == 0) w = 12;
    else w = 8;
    out.push_back(w + seedStream(seed, 100u + static_cast<uint32_t>(p)) % 40u);
  }
  return out;
}

std::vector<int64_t> scoreSine(const std::vector<int>& positions, uint32_t seed, int total) {
  const int ratioSet[5] = {2, 3, 4, 5, 7};
  const int a = static_cast<int>(seedStream(seed, 0) % 5u);
  const int b = (a + 1 + static_cast<int>(seedStream(seed, 1) % 4u)) % 5;
  int rest[3], r = 0;
  for (int i = 0; i < 5; i++) if (i != a && i != b) rest[r++] = i;
  const int ratios[3] = {ratioSet[a], ratioSet[b], ratioSet[rest[seedStream(seed, 2) % 3u]]};
  double phases[3];
  for (int k = 0; k < 3; k++) phases[k] = (seedStream(seed, 3u + static_cast<uint32_t>(k)) % 4096u) / 4096.0;
  std::vector<int64_t> out;
  out.reserve(positions.size());
  for (int p : positions) {
    double sum = 0;
    for (int k = 0; k < 3; k++) {
      double u = static_cast<double>((ratios[k] * p) % total) / total + phases[k];
      u -= std::floor(u);
      sum += fastSin((u - 0.5) * 6.283185307179586);
    }
    out.push_back(static_cast<int64_t>(std::floor(sum * 100000 + 0.5)));
  }
  return out;
}

int popcount16(uint32_t v) {
  int c = 0;
  for (; v; v >>= 1) c += static_cast<int>(v & 1u);
  return c;
}
std::vector<int64_t> scoreBit(const std::vector<int>& positions, uint32_t seed) {
  const uint32_t a = (seedStream(seed, 0) & 0xFFFFu) | 1u;
  const int b = 1 + static_cast<int>(seedStream(seed, 1) % 3u);
  std::vector<int64_t> out;
  out.reserve(positions.size());
  for (int p : positions) {
    const uint32_t up = static_cast<uint32_t>(p) + 1u;
    out.push_back(popcount16(((up * a) ^ (up >> b)) & 0xFFFFu));
  }
  return out;
}

std::vector<int64_t> scoreLogistic(const std::vector<int>& positions, uint32_t seed) {
  const int64_t r = 242483 + seedStream(seed, 1) % 19006u;
  int64_t x = 1 + seedStream(seed, 0) % 65534u;
  auto step = [&](int64_t v, uint32_t k) {
    const int64_t next = (r * v * (65536 - v)) >> 32;
    return next > 0 ? next : static_cast<int64_t>(1 + seedStream(seed, 2000u + k) % 65534u);
  };
  for (uint32_t k = 0; k < 8; k++) x = step(x, k);
  int maxPos = 0;
  for (int p : positions) maxPos = std::max(maxPos, p);
  std::vector<int64_t> xs;
  xs.reserve(maxPos + 1);
  for (int p = 0; p <= maxPos; p++) { xs.push_back(x); x = step(x, 8u + static_cast<uint32_t>(p)); }
  std::vector<int64_t> out;
  out.reserve(positions.size());
  for (int p : positions) out.push_back(xs[p]);
  return out;
}

// ---- Formula mode (port of compileFormula / evalFormula in rhythm-logic.js; the grammar is documented there) ----
enum class FOpType { Num, Var, Neg, Op, Fn };
struct FOp {
  FOpType type;
  double  value = 0;  // Num
  int     id    = 0;  // Var: index into the variable array; Fn: function id
  char    op    = 0;  // Op
};
enum FFunc { kFnSin, kFnCos, kFnAbs, kFnFloor, kFnFrac, kFnSqrt, kFnMin, kFnMax, kFnMod, kFnMandel, kFnJulia, kFnLogi };
int formulaArity(int fn) {
  switch (fn) {
    case kFnMin: case kFnMax: case kFnMod: case kFnMandel: return 2;
    case kFnLogi: return 3;
    case kFnJulia: return 4;
    default: return 1;
  }
}

struct FormulaParser {
  const std::string& src;
  size_t pos = 0;
  std::vector<FOp> ops;
  bool failed = false;

  explicit FormulaParser(const std::string& s) : src(s) {}

  static bool isDigit(char c) { return c >= '0' && c <= '9'; }
  static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
  char peek() {
    while (pos < src.size() && src[pos] == ' ') pos++;
    return pos < src.size() ? src[pos] : '\0';
  }
  void pushOp(char c) { FOp o{FOpType::Op}; o.op = c; ops.push_back(o); }

  void parseExpr() {
    parseTerm();
    for (char c = peek(); !failed && (c == '+' || c == '-'); c = peek()) { pos++; parseTerm(); pushOp(c); }
  }
  void parseTerm() {
    parseUnary();
    for (char c = peek(); !failed && (c == '*' || c == '/' || c == '%'); c = peek()) { pos++; parseUnary(); pushOp(c); }
  }
  void parseUnary() {
    if (peek() == '-') { pos++; parseUnary(); ops.push_back(FOp{FOpType::Neg}); }
    else parsePower();
  }
  void parsePower() {
    parseAtom();
    if (!failed && peek() == '^') { pos++; parseUnary(); pushOp('^'); }
  }
  void parseAtom() {
    if (failed) return;
    const char c = peek();
    if (c == '(') {
      pos++; parseExpr();
      if (failed) return;
      if (peek() != ')') { failed = true; return; }
      pos++;
    } else if (isDigit(c) || c == '.') {
      const size_t start = pos;
      while (pos < src.size() && isDigit(src[pos])) pos++;
      if (pos < src.size() && src[pos] == '.') { pos++; while (pos < src.size() && isDigit(src[pos])) pos++; }
      const std::string numText = src.substr(start, pos - start);
      if (numText == ".") { failed = true; return; }
      FOp o{FOpType::Num};
      std::from_chars(numText.data(), numText.data() + numText.size(), o.value);
      ops.push_back(o);
    } else if (isAlpha(c)) {
      const size_t start = pos;
      while (pos < src.size() && (isAlpha(src[pos]) || isDigit(src[pos]))) pos++;
      const std::string id = src.substr(start, pos - start);
      if (peek() == '(') {
        struct F { const char* name; int fn; int arity; };
        static const F funcs[] = {{"sin", kFnSin, 1}, {"cos", kFnCos, 1}, {"abs", kFnAbs, 1}, {"floor", kFnFloor, 1},
                                  {"frac", kFnFrac, 1}, {"sqrt", kFnSqrt, 1}, {"min", kFnMin, 2}, {"max", kFnMax, 2},
                                  {"mod", kFnMod, 2}, {"mandel", kFnMandel, 2}, {"julia", kFnJulia, 4}, {"logi", kFnLogi, 3}};
        const F* found = nullptr;
        for (const auto& f : funcs) if (id == f.name) found = &f;
        if (!found) { failed = true; return; }
        pos++;
        for (int k = 0; k < found->arity; k++) {
          parseExpr();
          if (failed) return;
          if (k < found->arity - 1) { if (peek() != ',') { failed = true; return; } pos++; }
        }
        if (peek() != ')') { failed = true; return; }
        pos++;
        FOp o{FOpType::Fn};
        o.id = found->fn;
        ops.push_back(o);
      } else {
        static const char* vars[] = {"i", "n", "hits", "s1", "s2", "s3", "r", "pi", "bar", "bars"};
        int idx = -1;
        for (int k = 0; k < 10; k++) if (id == vars[k]) idx = k;
        if (idx < 0) { failed = true; return; }
        FOp o{FOpType::Var};
        o.id = idx;
        ops.push_back(o);
      }
    } else {
      failed = true;
    }
  }
};

bool compileExpr(const std::string& src, std::vector<FOp>& out) {
  FormulaParser parser(src);
  parser.parseExpr();
  if (parser.failed || parser.peek() != '\0') return false;
  out = std::move(parser.ops);
  return true;
}

struct CompiledFormula {
  std::vector<FOp> ops;
  std::vector<FOp> countOps;
  bool hasCount = false;
};

// "score" or "score ; count" (see compileFormula in rhythm-logic.js). Returns false if it does not parse or is too long.
bool compileFormula(const std::string& src, CompiledFormula& out) {
  if (src.size() > 500) return false;
  const size_t semi = src.find(';');
  if (!compileExpr(semi == std::string::npos ? src : src.substr(0, semi), out.ops)) return false;
  out.hasCount = false;
  if (semi != std::string::npos) {
    const std::string countText = src.substr(semi + 1);
    if (countText.find_first_not_of(" \t\r\n") != std::string::npos) {
      if (!compileExpr(countText, out.countOps)) return false;
      out.hasCount = true;
    }
  }
  return true;
}

double formulaFinite(double x) { return std::isfinite(x) ? x : 0.0; }
double formulaPow(double a, double b) {
  int k = static_cast<int>(std::floor(b + 0.5));
  k = k < -8 ? -8 : (k > 8 ? 8 : k);
  double r = 1;
  for (int j = 0; j < std::abs(k); j++) r *= a;
  return k < 0 ? (r == 0 ? 0.0 : 1 / r) : r;
}
// Escape count of z <- z*z + c starting at z (Julia), or at 0 (Mandelbrot), capped at 32 iterations.
double formulaJulia(double zx, double zy, double cx, double cy) {
  double x = zx, y = zy;
  int n = 0;
  while (n < 32 && x * x + y * y <= 4) {
    const double t = x * x - y * y + cx;
    y = 2 * x * y + cy;
    x = t;
    n++;
  }
  return n;
}
double formulaLogi(double r, double x, double k) {
  int count = static_cast<int>(std::floor(k + 0.5));
  count = count < 0 ? 0 : (count > 64 ? 64 : count);
  for (int j = 0; j < count; j++) x = r * x * (1 - x);
  return x;
}
double formulaSin(double x) { return fastSin(x - 6.283185307179586 * std::floor(x / 6.283185307179586 + 0.5)); }

double evalFormula(const std::vector<FOp>& ops, const double* vars) {
  std::vector<double> st;
  for (const auto& o : ops) {
    switch (o.type) {
      case FOpType::Num: st.push_back(o.value); break;
      case FOpType::Var: st.push_back(vars[o.id]); break;
      case FOpType::Neg: st.back() = -st.back(); break;
      case FOpType::Op: {
        const double b = st.back(); st.pop_back();
        const double a = st.back(); st.pop_back();
        double v;
        if (o.op == '+') v = a + b;
        else if (o.op == '-') v = a - b;
        else if (o.op == '*') v = a * b;
        else if (o.op == '/') v = b == 0 ? 0.0 : a / b;
        else if (o.op == '%') v = b == 0 ? 0.0 : a - b * std::floor(a / b);
        else v = formulaPow(a, b);
        st.push_back(formulaFinite(v));
        break;
      }
      case FOpType::Fn: {
        const int arity = formulaArity(o.id);
        double args[4] = {0, 0, 0, 0};
        for (int k = arity - 1; k >= 0; k--) { args[k] = st.back(); st.pop_back(); }
        const double a = args[0], b = args[1];
        double v;
        switch (o.id) {
          case kFnSin:   v = formulaSin(a); break;
          case kFnCos:   v = formulaSin(a + 1.5707963267948966); break;
          case kFnAbs:   v = std::fabs(a); break;
          case kFnFloor: v = std::floor(a); break;
          case kFnFrac:  v = a - std::floor(a); break;
          case kFnSqrt:  v = a < 0 ? 0.0 : std::sqrt(a); break;
          case kFnMin:   v = a < b ? a : b; break;
          case kFnMax:   v = a > b ? a : b; break;
          case kFnMandel: v = formulaJulia(0, 0, a, b); break;
          case kFnJulia:  v = formulaJulia(a, b, args[2], args[3]); break;
          case kFnLogi:   v = formulaLogi(a, b, args[2]); break;
          default:       v = b == 0 ? 0.0 : a - b * std::floor(a / b); break;
        }
        st.push_back(formulaFinite(v));
        break;
      }
    }
  }
  return st.back();
}

// The number of hits the count expression asks for in this bar (rounded; the caller limits it to the steps available).
int formulaHitCount(const CompiledFormula& compiled, int hits, int total, uint32_t seed, int bar, int bars) {
  double s[3];
  for (int k = 0; k < 3; k++) s[k] = (seedStream(seed, 10u + static_cast<uint32_t>(k)) % 65536u) / 65536.0;
  const double vars[10] = {0, static_cast<double>(total), static_cast<double>(hits), s[0], s[1], s[2], 0,
                           3.141592653589793, static_cast<double>(bar), static_cast<double>(bars)};
  const double v = evalFormula(compiled.countOps, vars);
  const double clamped = v > 1e6 ? 1e6 : (v < -1e6 ? -1e6 : v);
  return static_cast<int>(std::floor(clamped + 0.5));
}

std::vector<int64_t> scoreFormula(const std::vector<int>& positions, uint32_t seed, int total, int hits,
                                  const CompiledFormula* compiled, int bar, int bars) {
  if (!compiled) return std::vector<int64_t>(positions.size(), 0);
  const std::vector<FOp>& ops = compiled->ops;
  double s[3];
  for (int k = 0; k < 3; k++) s[k] = (seedStream(seed, 10u + static_cast<uint32_t>(k)) % 65536u) / 65536.0;
  std::vector<int64_t> out;
  out.reserve(positions.size());
  for (int p : positions) {
    const double vars[10] = {static_cast<double>(p), static_cast<double>(total), static_cast<double>(hits),
                             s[0], s[1], s[2],
                             seedStream(seed, 500u + static_cast<uint32_t>(p)) / 4294967296.0, 3.141592653589793,
                             static_cast<double>(bar), static_cast<double>(bars)};
    const double v = evalFormula(ops, vars);
    const double clamped = v > 1e6 ? 1e6 : (v < -1e6 ? -1e6 : v);
    out.push_back(static_cast<int64_t>(std::floor(clamped * 100000 + 0.5)));
  }
  return out;
}

}  // namespace

PatternMode patternModeFromString(const std::string& s) {
  if (s == "metric")   return PatternMode::Metric;
  if (s == "sine")     return PatternMode::Sine;
  if (s == "bit")      return PatternMode::Bit;
  if (s == "logistic") return PatternMode::Logistic;
  if (s == "formula")  return PatternMode::Formula;
  return PatternMode::Classic;
}

const char* patternModeToString(PatternMode mode) {
  switch (mode) {
    case PatternMode::Metric:   return "metric";
    case PatternMode::Sine:     return "sine";
    case PatternMode::Bit:      return "bit";
    case PatternMode::Logistic: return "logistic";
    case PatternMode::Formula:  return "formula";
    default:                    return "classic";
  }
}

std::vector<int> stepRange(int n) {
  std::vector<int> out(n > 0 ? n : 0);
  for (int i = 0; i < static_cast<int>(out.size()); i++) out[i] = i;
  return out;
}

std::vector<bool> patternMask(const PatternSpec& spec, int hits, const std::vector<int>& positions,
                              uint32_t seed, double stepsPerBeat, int total, bool anchorEnd) {
  const PatternMode mode = spec.mode;
  const int n = static_cast<int>(positions.size());
  if (mode == PatternMode::Classic) return euclideanRhythm(hits, n, seed);
  if (n <= 0) return {};
  const int cycle = std::max(1, total);
  const int requestedHits = hits;
  CompiledFormula compiled;
  bool formulaOk = false;
  if (mode == PatternMode::Formula) {
    formulaOk = compileFormula(spec.formula, compiled);
    if (formulaOk && compiled.hasCount) hits = formulaHitCount(compiled, requestedHits, cycle, seed, spec.bar, spec.bars);
  }
  if (hits <= 0) return std::vector<bool>(n, false);
  if (hits >= n) return std::vector<bool>(n, true);

  const int spb = std::max(1, static_cast<int>(std::round(stepsPerBeat)));
  std::vector<int64_t> scores;
  switch (mode) {
    case PatternMode::Metric:   scores = scoreMetric(positions, seed, spb); break;
    case PatternMode::Sine:     scores = scoreSine(positions, seed, cycle); break;
    case PatternMode::Bit:      scores = scoreBit(positions, seed); break;
    case PatternMode::Formula:  scores = scoreFormula(positions, seed, cycle, requestedHits, formulaOk ? &compiled : nullptr, spec.bar, spec.bars); break;
    default:                    scores = scoreLogistic(positions, seed); break;
  }
  if (anchorEnd) scores[n - 1] += 1000000000;
  std::vector<uint32_t> keys(n);
  for (int i = 0; i < n; i++) keys[i] = seedStream(seed, 1000u + static_cast<uint32_t>(positions[i]));
  std::vector<int> order = stepRange(n);
  std::sort(order.begin(), order.end(), [&](int i, int j) {
    if (scores[i] != scores[j]) return scores[i] > scores[j];
    if (keys[i] != keys[j]) return keys[i] > keys[j];
    return i < j;
  });
  std::vector<bool> mask(n, false);
  for (int k = 0; k < hits; k++) mask[order[k]] = true;
  return mask;
}

double applyFeelWarp(double frac, Feel feel) {
  double f = std::fmod(std::fmod(frac, 1.0) + 1.0, 1.0);
  if (feel == Feel::Swing) {
    double stepFrac = f * 2.0;
    double pairIdx = std::floor(stepFrac);
    double within = stepFrac - pairIdx;
    double warpedWithin = within < 0.5 ? within * (4.0 / 3.0)
                                        : (2.0 / 3.0) + (within - 0.5) * (2.0 / 3.0);
    return (pairIdx + warpedWithin) / 2.0;
  }
  // Straight (and Halftime, which never reaches here — see generateGroupPattern)
  return f;
}

double warpAbsoluteBeat(double absBeat, Feel feel) {
  double beatIdx = std::floor(absBeat);
  return beatIdx + applyFeelWarp(absBeat - beatIdx, feel);
}

std::vector<double> generateGroupPattern(int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed,
                                         const PatternSpec& mode) {
  if (feel == Feel::Halftime) {
    int halfSteps = static_cast<int>(std::round((numerator / 2.0) * stepsPerBeat));
    int halfHits = std::max(1, static_cast<int>(std::round(hits / 2.0)));
    auto mask = patternMask(mode, halfHits, stepRange(halfSteps), seed, stepsPerBeat, halfSteps);
    std::vector<double> beats;
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (!mask[i]) continue;
      int beatIdx = static_cast<int>(std::floor(i / stepsPerBeat));
      double fracWithinBeat = std::fmod(i, stepsPerBeat) / stepsPerBeat;
      beats.push_back((beatIdx + fracWithinBeat) * 2.0);
    }
    return beats;
  }

  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));
  auto mask = patternMask(mode, hits, stepRange(totalSteps), seed, stepsPerBeat, totalSteps);
  std::vector<double> beats;
  for (int i = 0; i < static_cast<int>(mask.size()); i++) {
    if (!mask[i]) continue;
    int beatIdx = static_cast<int>(std::floor(i / stepsPerBeat));
    double fracWithinBeat = std::fmod(i, stepsPerBeat) / stepsPerBeat;
    beats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  }
  return beats;
}

std::vector<AnchoredBeat> generateAnchoredPattern(
    int numerator, double stepsPerBeat, int hits, Feel feel, uint32_t seed,
    const std::vector<double>& anchorBeats, const PatternSpec& mode) {

  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));

  if (anchorBeats.empty() || hits <= 0) {
    auto beats = generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed, mode);
    std::vector<AnchoredBeat> out;
    out.reserve(beats.size());
    for (double b : beats) out.push_back({b, false});
    return out;
  }

  // anchorSteps: dedupe + range-clip, port of the JS `new Set(...)` build.
  std::vector<int> anchorStepsVec;
  for (double b : anchorBeats) {
    int st = static_cast<int>(std::round(b * stepsPerBeat));
    if (st >= 0 && st < totalSteps) anchorStepsVec.push_back(st);
  }
  std::sort(anchorStepsVec.begin(), anchorStepsVec.end());
  anchorStepsVec.erase(std::unique(anchorStepsVec.begin(), anchorStepsVec.end()), anchorStepsVec.end());

  std::vector<bool> isAnchorStep(totalSteps, false);
  for (int st : anchorStepsVec) isAnchorStep[st] = true;

  int embellishHits = std::max(0, hits - static_cast<int>(anchorStepsVec.size()));

  std::vector<int> freeSteps;
  for (int i = 0; i < totalSteps; i++) if (!isAnchorStep[i]) freeSteps.push_back(i);

  auto embellishMask = patternMask(mode, embellishHits, freeSteps, seed, stepsPerBeat, totalSteps);

  // chosen: anchors first, then embellish hits — freeSteps/anchorSteps are
  // disjoint by construction so no overwrite ordering to worry about
  // (unlike the JS Map, which relies on that same disjointness).
  std::vector<AnchoredBeat> beats;
  beats.reserve(anchorStepsVec.size() + embellishMask.size());

  for (int st : anchorStepsVec) {
    int beatIdx = static_cast<int>(std::floor(st / stepsPerBeat));
    double fracWithinBeat = std::fmod(st, stepsPerBeat) / stepsPerBeat;
    beats.push_back({beatIdx + applyFeelWarp(fracWithinBeat, feel), true});
  }
  for (int i = 0; i < static_cast<int>(embellishMask.size()); i++) {
    if (!embellishMask[i]) continue;
    int st = freeSteps[i];
    int beatIdx = static_cast<int>(std::floor(st / stepsPerBeat));
    double fracWithinBeat = std::fmod(st, stepsPerBeat) / stepsPerBeat;
    beats.push_back({beatIdx + applyFeelWarp(fracWithinBeat, feel), false});
  }

  std::sort(beats.begin(), beats.end(), [](const AnchoredBeat& a, const AnchoredBeat& b) {
    return a.beat < b.beat;
  });
  return beats;
}

std::vector<double> generateTripletLayerBeats(
    const std::vector<BeatRange>& ranges, double densityPct, int tripletSubdivision) {
  if (ranges.empty()) return {};
  double stepSize = 1.0 / (tripletSubdivision > 0 ? tripletSubdivision : 3);

  std::vector<double> out;
  for (const auto& range : ranges) {
    int startStep = static_cast<int>(std::round(range.from / stepSize));
    int endStep = static_cast<int>(std::round(range.to / stepSize));
    int localSteps = endStep - startStep;
    if (localSteps <= 0) continue;

    int hits = static_cast<int>(std::round((densityPct / 100.0) * localSteps));
    auto mask = euclideanRhythm(hits, localSteps);
    for (int i = 0; i < static_cast<int>(mask.size()); i++) {
      if (mask[i]) out.push_back((startStep + i) * stepSize);
    }
  }
  return out;
}

std::vector<double> generateSnareGhostBeats(
    int numerator, double stepsPerBeat, int mainHits, double ghostDensityPct,
    Feel feel, uint32_t seed, const PatternSpec& mode) {
  int totalSteps = static_cast<int>(std::round(numerator * stepsPerBeat));
  auto mainMask = patternMask(mode, mainHits, stepRange(totalSteps), seed, stepsPerBeat, totalSteps);

  std::vector<int> emptyIdx;
  for (int i = 0; i < static_cast<int>(mainMask.size()); i++)
    if (!mainMask[i]) emptyIdx.push_back(i);
  if (emptyIdx.empty()) return {};

  int ghostHits = static_cast<int>(std::round((ghostDensityPct / 100.0) * emptyIdx.size()));
  auto ghostMask = patternMask(mode, ghostHits, emptyIdx, seed ? seed + 1 : 0, stepsPerBeat, totalSteps);

  std::vector<double> beats;
  for (int i = 0; i < static_cast<int>(ghostMask.size()); i++) {
    if (!ghostMask[i]) continue;
    int step = emptyIdx[i];
    int beatIdx = static_cast<int>(std::floor(step / stepsPerBeat));
    double fracWithinBeat = std::fmod(step, stepsPerBeat) / stepsPerBeat;
    beats.push_back(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  }
  return beats;
}

uint32_t seededHash(const std::string& str) {
  uint32_t h = 2166136261u;
  for (char c : str) {
    h ^= static_cast<uint8_t>(c);
    h = h * 16777619u; // unsigned overflow wraps like Math.imul + >>> 0
  }
  return h;
}

double articulationTierWeight(int tier, double bias) {
  switch (tier) {
    case 0: return std::pow(1.0 - bias, 1.3) * 10.0;
    case 1: return bias * (1.3 - bias * 0.3) * 4.0;
    case 2: return std::pow(bias, 2.0) * 2.5;
    case 3: return std::pow(bias, 3.5) * 3.0;
    default: return 0.0;
  }
}

std::string pickWeightedArticulation(RNG& rng, double bias, const std::vector<ArticulationTier>& tiers) {
  if (tiers.empty()) return {};

  std::vector<double> weights;
  weights.reserve(tiers.size());
  double total = 0.0;
  for (const auto& t : tiers) {
    double w = std::max(0.0, articulationTierWeight(t.tier, bias));
    weights.push_back(w);
    total += w;
  }

  if (total <= 0.0) return tiers[0].id;

  double r = rng.next() * total;
  for (size_t i = 0; i < tiers.size(); i++) {
    r -= weights[i];
    if (r <= 0.0) return tiers[i].id;
  }
  return tiers.back().id;
}

std::vector<bool> buildEndAnchoredEuclidean(int hits, int steps, uint32_t seed) {
  if (steps <= 0) return {};
  if (hits <= 0) return std::vector<bool>(steps, false);
  if (hits >= steps) return std::vector<bool>(steps, true);

  std::vector<bool> raw;
  raw.reserve(steps);
  int bucket = 0;
  for (int i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) { bucket -= steps; raw.push_back(true); }
    else raw.push_back(false);
  }

  // Port of `raw.lastIndexOf(true)` — hits>0 and hits<steps (both handled
  // above) guarantee at least one true, so lastHit >= 0 always holds, same
  // reasoning as euclideanRhythm's firstHit comment in this file.
  int lastHit = -1;
  for (int i = steps - 1; i >= 0; i--) { if (raw[i]) { lastHit = i; break; } }
  const int shift = steps - 1 - lastHit;

  std::vector<bool> mask(steps);
  for (int i = 0; i < steps; i++) mask[((i + shift) % steps + steps) % steps] = raw[i];

  return seed ? seededShuffleMask(mask, seed) : mask;
}

std::vector<bool> endAnchoredMask(const PatternSpec& mode, int hits, int steps, uint32_t seed, double stepsPerBeat) {
  if (mode.mode == PatternMode::Classic) return buildEndAnchoredEuclidean(hits, steps, seed);
  return patternMask(mode, hits, stepRange(steps), seed, stepsPerBeat, steps, true);
}

} // namespace dgg
