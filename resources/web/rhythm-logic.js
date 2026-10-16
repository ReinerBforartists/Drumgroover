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
function generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed) {
  if (feel === 'halftime') {
    const halfSteps = Math.round((numerator/2) * stepsPerBeat);
    const halfHits = Math.max(1, Math.round(hits/2));
    const mask = euclideanRhythm(halfHits, halfSteps, seed);
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
  const mask = euclideanRhythm(hits, totalSteps, seed);
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
function generateAnchoredPattern(numerator, stepsPerBeat, hits, feel, seed, anchorBeats) {
  const totalSteps = Math.round(numerator * stepsPerBeat);
  if (!anchorBeats || !anchorBeats.length || hits <= 0) {
    return generateGroupPattern(numerator, stepsPerBeat, hits, feel, seed).map(b => ({ beat: b, isAnchor: false }));
  }
  const anchorSteps = new Set(anchorBeats.map(b => Math.round(b*stepsPerBeat)).filter(s => s >= 0 && s < totalSteps));
  const embellishHits = Math.max(0, hits - anchorSteps.size);
  const freeSteps = [];
  for (let i = 0; i < totalSteps; i++) if (!anchorSteps.has(i)) freeSteps.push(i);
  const embellishMask = euclideanRhythm(embellishHits, freeSteps.length, seed);
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
function generateSnareGhostBeats(numerator, stepsPerBeat, mainHits, ghostDensityPct, feel, seed) {
  const totalSteps = Math.round(numerator * stepsPerBeat);
  const mainMask = euclideanRhythm(mainHits, totalSteps, seed);
  const emptyIdx = [];
  mainMask.forEach((on, i) => { if (!on) emptyIdx.push(i); });
  if (!emptyIdx.length) return [];
  const ghostHits = Math.round((ghostDensityPct/100) * emptyIdx.length);
  const ghostMask = euclideanRhythm(ghostHits, emptyIdx.length, seed ? seed+1 : 0);
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

