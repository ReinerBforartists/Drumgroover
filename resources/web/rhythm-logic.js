// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
class RNG {
  constructor(seed) { this.s = seed >>> 0 || (Date.now() & 0xffffffff); }
  next() { this.s = (this.s * 1664525 + 1013904223) >>> 0; return this.s / 0xffffffff; }
  int(a,b) { return a + Math.floor(this.next() * (b - a + 1)); }
  chance(p) { return this.next() < p; }
}

function hasNoteNear(arr, beat, eps=0.06) { return arr.some(n => Math.abs(n.beat - beat) < eps); }

function backbeatPositions(numerator) {
  const out = [];
  for (let b = 1; b < numerator; b += 2) out.push(b);
  return out;
}

// =========================================
// GROUP-BASED GENERATION FOUNDATION
// Shared groundwork for genre/feel generation.
// =========================================

// Distributes `hits` evenly across `steps` positions (Bresenham-style approximation).
// Rotates to anchor first hit on step 0 for realistic drum patterns. Returns seeded shuffle if provided.
function euclideanRhythm(hits, steps, seed) {
  if (steps <= 0) return [];
  if (hits <= 0) return new Array(steps).fill(false);
  if (hits >= steps) return new Array(steps).fill(true);
  const raw = [];
  let bucket = 0;
  for (let i = 0; i < steps; i++) {
    bucket += hits;
    if (bucket >= steps) { bucket -= steps; raw.push(true); }
    else raw.push(false);
  }
  const firstHit = raw.indexOf(true);
  const mask = raw.slice(firstHit).concat(raw.slice(0, firstHit));
  return seed ? seededShuffleMask(mask, seed) : mask;
}

// Nudges a fraction of hits to neighbors while preserving exact hit count.
// Makes the Seed knob effective since the base Euclidean algorithm is deterministic.
function seededShuffleMask(mask, seed) {
  const rng = new RNG(seed);
  const out = mask.slice();
  const hitCount = out.filter(Boolean).length;
  const shuffles = Math.max(1, Math.round(hitCount * 0.3));
  for (let s = 0; s < shuffles; s++) {
    const hitIdxs = []; out.forEach((v,i) => { if (v) hitIdxs.push(i); });
    if (!hitIdxs.length) break;
    const pick = hitIdxs[rng.int(0, hitIdxs.length-1)];
    const target = (pick + (rng.chance(0.5) ? 1 : -1) + out.length) % out.length;
    if (!out[target]) { out[pick] = false; out[target] = true; }
  }
  return out;
}

// =========================================
// PATTERN MODES
// A mode scores every step; the `hits` best-scoring steps get a hit, so density stays exact. Equal scores are
// ordered by a seed hash. Scores are integers or quantized, so RhythmEngine.cpp produces the same masks.
// =========================================
const PATTERN_MODES = ['classic', 'metric', 'sine', 'bit', 'logistic'];

function mix32(x) {
  x = Math.imul(x ^ (x >>> 16), 0x7feb352d);
  x = Math.imul(x ^ (x >>> 15), 0x846ca68b);
  return (x ^ (x >>> 16)) >>> 0;
}
function seedStream(seed, k) { return mix32(((seed >>> 0) + Math.imul(k + 1, 0x9e3779b9)) >>> 0); }

// Parabolic sine approximation on [-PI, PI]; basic arithmetic only.
function fastSin(x) {
  const y = 1.2732395447351628 * x - 0.4052847345693511 * x * Math.abs(x);
  return 0.225 * (y * Math.abs(y) - y) + y;
}

function scoreMetric(positions, seed, spb) {
  return positions.map(p => {
    const inBeat = p % spb;
    let w;
    if (p === 0) w = 1000;
    else if (inBeat === 0) w = (Math.floor(p / spb) % 2 === 0) ? 48 : 40;
    else if (spb % 2 === 0 && inBeat === spb / 2) w = 24;
    else if (spb % 4 === 0 && inBeat % (spb / 4) === 0) w = 12;
    else w = 8;
    return w + seedStream(seed, 100 + p) % 40;
  });
}

// Three sine waves with distinct integer ratios from {2,3,4,5,7} cycles per bar; the seed picks ratios and phases.
function scoreSine(positions, seed, total) {
  const ratioSet = [2, 3, 4, 5, 7];
  const a = seedStream(seed, 0) % 5;
  const b = (a + 1 + seedStream(seed, 1) % 4) % 5;
  const rest = [0, 1, 2, 3, 4].filter(i => i !== a && i !== b);
  const ratios = [ratioSet[a], ratioSet[b], ratioSet[rest[seedStream(seed, 2) % 3]]];
  const phases = [3, 4, 5].map(k => (seedStream(seed, k) % 4096) / 4096);
  return positions.map(p => {
    let sum = 0;
    for (let k = 0; k < 3; k++) {
      let u = ((ratios[k] * p) % total) / total + phases[k];
      u -= Math.floor(u);
      sum += fastSin((u - 0.5) * 6.283185307179586);
    }
    return Math.floor(sum * 100000 + 0.5);
  });
}

function popcount16(v) {
  let c = 0;
  for (; v; v >>>= 1) c += v & 1;
  return c;
}
function scoreBit(positions, seed) {
  const a = (seedStream(seed, 0) & 0xFFFF) | 1;
  const b = 1 + seedStream(seed, 1) % 3;
  return positions.map(p => popcount16((Math.imul(p + 1, a) ^ ((p + 1) >> b)) & 0xFFFF));
}

// Logistic map x <- r*x*(1-x) in 16-bit fixed point (r between 3.7 and 3.99), so every step is exact integer math.
function scoreLogistic(positions, seed) {
  const r = 242483 + seedStream(seed, 1) % 19006;
  let x = 1 + seedStream(seed, 0) % 65534;
  const step = (v, k) => {
    const next = Math.floor(r * v * (65536 - v) / 4294967296);
    return next > 0 ? next : 1 + seedStream(seed, 2000 + k) % 65534;
  };
  for (let k = 0; k < 8; k++) x = step(x, k);
  let maxPos = 0;
  positions.forEach(p => { if (p > maxPos) maxPos = p; });
  const xs = [];
  for (let p = 0; p <= maxPos; p++) { xs.push(x); x = step(x, 8 + p); }
  return positions.map(p => xs[p]);
}

// Mask over `positions` (absolute step indices within the bar grid). Classic keeps the Euclidean distribution.
// anchorEnd forces the last step to be a hit (Break/Fill zones end on a hit).
function patternMask(mode, hits, positions, seed, spb, total, anchorEnd = false) {
  const n = positions.length;
  if (!PATTERN_MODES.includes(mode) || mode === 'classic') return euclideanRhythm(hits, n, seed);
  if (n <= 0) return [];
  if (hits <= 0) return new Array(n).fill(false);
  if (hits >= n) return new Array(n).fill(true);
  const stepsPerBeat = Math.max(1, Math.round(spb));
  const cycle = Math.max(1, total);
  const scores = mode === 'metric' ? scoreMetric(positions, seed, stepsPerBeat)
    : mode === 'sine' ? scoreSine(positions, seed, cycle)
    : mode === 'bit' ? scoreBit(positions, seed)
    : scoreLogistic(positions, seed);
  if (anchorEnd) scores[n - 1] += 1000000000;
  const keys = positions.map(p => seedStream(seed, 1000 + p));
  const order = positions.map((_, i) => i).sort((i, j) => (scores[j] - scores[i]) || (keys[j] - keys[i]) || (i - j));
  const mask = new Array(n).fill(false);
  for (let k = 0; k < hits; k++) mask[order[k]] = true;
  return mask;
}
// End-anchored mask over steps 0..steps-1. Classic keeps buildEndAnchoredEuclidean (defined in generator-core.js).
function endAnchoredMask(mode, hits, steps, seed, spb) {
  if (!PATTERN_MODES.includes(mode) || mode === 'classic') return buildEndAnchoredEuclidean(hits, steps, seed);
  return patternMask(mode, hits, stepRange(steps), seed, spb, steps, true);
}
function stepRange(n) { return Array.from({ length: Math.max(0, n) }, (_, i) => i); }

// Builds one bar's beat positions via Euclidean distribution, then applies the current Feel's grid warp.
// Feel defines the frame every group's pattern is placed into, not a post-hoc shift.
const FEEL_WARPS = {
  straight: (frac) => frac,
  // Approximates triplet-swing (2:1) by delaying every 2nd logical step.
  swing: (frac) => {
    const stepFrac = frac * 2;
    const pairIdx = Math.floor(stepFrac);
    const within = stepFrac - pairIdx;
    const warpedWithin = within < 0.5 ? within * (4/3) : (2/3) + (within-0.5)*(2/3);
    return (pairIdx + warpedWithin) / 2;
  },
};
function applyFeelWarp(frac, feel) {
  const fn = FEEL_WARPS[feel] || FEEL_WARPS.straight;
  return fn(((frac % 1) + 1) % 1);
}
// Warps an ABSOLUTE beat position (not a zone-relative offset) against the
// real bar-beat grid, then returns it still in absolute terms. Needed
// anywhere a fill/pattern's step-0 doesn't itself sit on a beat boundary —
// e.g. a Break's tail starting at grooveBeats=2.5 — so swing bends the same
// on/off pairs it would if this were an ordinary in-bar pattern, not pairs
// re-zeroed to whatever beat the window happens to start on.
function warpAbsoluteBeat(absBeat, feel) {
  const beatIdx = Math.floor(absBeat);
  return beatIdx + applyFeelWarp(absBeat - beatIdx, feel);
}
function generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed, mode = 'classic') {
  if (feel === 'halftime') {
    const halfSteps = Math.round((numerator/2) * stepsPerBeat);
    const halfHits = Math.max(1, Math.round(hits/2));
    const mask = patternMask(mode, halfHits, stepRange(halfSteps), seed, stepsPerBeat, halfSteps);
    const beats = [];
    mask.forEach((on, i) => {
      if (!on) return;
      const beatIdx = Math.floor(i / stepsPerBeat);
      const fracWithinBeat = (i % stepsPerBeat) / stepsPerBeat;
      beats.push((beatIdx + fracWithinBeat) * 2);
    });
    return beats;
  }
  const totalSteps = Math.round(numerator * stepsPerBeat);
  const mask = patternMask(mode, hits, stepRange(totalSteps), seed, stepsPerBeat, totalSteps);
  const beats = [];
  mask.forEach((on, i) => {
    if (!on) return;
    const beatIdx = Math.floor(i / stepsPerBeat);
    const fracWithinBeat = (i % stepsPerBeat) / stepsPerBeat;
    beats.push(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  });
  return beats;
}

// Strong beats (Kick 1&3, Snare 2&4) are structural, not probabilistic.
// Anchors are included if density > 0; density knob only controls additional Euclidean-distributed hits.
// Returns {beat, isAnchor} so callers can apply different velocities.
function generateAnchoredPattern(numerator, stepsPerBeat, hits, feel, seed, anchorBeats, mode = 'classic') {
  const totalSteps = Math.round(numerator * stepsPerBeat);
  if (!anchorBeats || !anchorBeats.length || hits <= 0) {
    return generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed, mode).map(b => ({ beat: b, isAnchor: false }));
  }
  const anchorSteps = new Set(anchorBeats.map(b => Math.round(b*stepsPerBeat)).filter(s => s >= 0 && s < totalSteps));
  const embellishHits = Math.max(0, hits - anchorSteps.size);
  const freeSteps = [];
  for (let i = 0; i < totalSteps; i++) if (!anchorSteps.has(i)) freeSteps.push(i);
  const embellishMask = patternMask(mode, embellishHits, freeSteps, seed, stepsPerBeat, totalSteps);
  const chosen = new Map();
  anchorSteps.forEach(s => chosen.set(s, true));
  embellishMask.forEach((on, i) => { if (on) chosen.set(freeSteps[i], false); });
  const beats = [];
  chosen.forEach((isAnchor, step) => {
    const beatIdx = Math.floor(step / stepsPerBeat);
    const fracWithinBeat = (step % stepsPerBeat) / stepsPerBeat;
    beats.push({ beat: beatIdx + applyFeelWarp(fracWithinBeat, feel), isAnchor });
  });
  return beats.sort((a,b) => a.beat - b.beat);
}

// Generates independent rhythmic layers overlaid on a group's pattern.
// Anchored to a global 1/3-beat grid for phase consistency across ranges.
function generateTripletLayerBeats(ranges, densityPct, tripletSubdivision) {
  if (!ranges || !ranges.length) return [];
  const stepSize = 1/(tripletSubdivision || 3);
  const out = [];
  ranges.forEach(range => {
    const startStep = Math.round(range.from / stepSize);
    const endStep = Math.round(range.to / stepSize);
    const localSteps = endStep - startStep;
    if (localSteps <= 0) return;
    const hits = Math.round((densityPct/100) * localSteps);
    const mask = euclideanRhythm(hits, localSteps);
    mask.forEach((on, i) => { if (on) out.push((startStep + i) * stepSize); });
  });
  return out;
}

// =========================================
// BLOCK MANAGEMENT
// =========================================
function neQuantize(beat, snap) { return snap ? Math.round(beat/snap)*snap : Math.round(beat*1000)/1000; }

// Generates ghost notes in empty slots of the main pattern.
// Must use a related seed to avoid collision with the main pattern.
function generateSnareGhostBeats(numerator, stepsPerBeat, mainHits, ghostDensityPct, feel, seed, mode = 'classic') {
  const totalSteps = Math.round(numerator * stepsPerBeat);
  const mainMask = patternMask(mode, mainHits, stepRange(totalSteps), seed, stepsPerBeat, totalSteps);
  const emptyIdx = [];
  mainMask.forEach((on, i) => { if (!on) emptyIdx.push(i); });
  if (!emptyIdx.length) return [];
  const ghostHits = Math.round((ghostDensityPct/100) * emptyIdx.length);
  const ghostMask = patternMask(mode, ghostHits, emptyIdx, seed ? seed+1 : 0, stepsPerBeat, totalSteps);
  const beats = [];
  ghostMask.forEach((on, i) => {
    if (!on) return;
    const step = emptyIdx[i];
    const beatIdx = Math.floor(step / stepsPerBeat);
    const fracWithinBeat = (step % stepsPerBeat) / stepsPerBeat;
    beats.push(beatIdx + applyFeelWarp(fracWithinBeat, feel));
  });
  return beats;
}

function seededHash(str) {
  let h = 2166136261;
  for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 16777619); }
  return h >>> 0;
}

// Controls articulation variation probability. Tier 0 is default (dominant at low bias).
// Tiers 1-3 grow progressively (linear -> quadratic -> cubic) to model subtle to extreme alternates.
const ARTICULATION_TIER_WEIGHT = [
  bias => Math.pow(1-bias, 1.3) * 10,
  bias => bias * (1.3 - bias*0.3) * 4,
  bias => Math.pow(bias, 2) * 2.5,
  bias => Math.pow(bias, 3.5) * 3,
];
function pickWeightedArticulation(rng, bias, tiers) {
  const weights = tiers.map(t => Math.max(0, ARTICULATION_TIER_WEIGHT[t.tier](bias)));
  const total = weights.reduce((a,b) => a+b, 0);
  if (total <= 0) return tiers[0].id;
  let r = rng.next() * total;
  for (let i = 0; i < tiers.length; i++) { r -= weights[i]; if (r <= 0) return tiers[i].id; }
  return tiers[tiers.length-1].id;
}

// After regenerating bar 0, builds subsequent bars with per-bar variation:
//   - Velocity Build: ramps toward block end
//   - Turnaround: accent on every Nth bar
// Only runs for generated groups; locked groups are skipped. Idempotent for given knob/seed state.

