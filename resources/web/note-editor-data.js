// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// NOTE EDITOR — DATA / GEOMETRY HELPERS
// Triplet-zone lookup, quantization, note selection, move proposals, row-grid drawing.
// =========================================

// Triplet zones: block.tripletLayers[engineGroup] = [{from,to}, ...]. Exclusive model:
// outside: ambient snap; inside: opposite type at group's instrument subdivision.
// Playhead and loop range are per-block; null start/end means loop whole block.
let nePlayheadBeat = 0;
let neLoopStart = null, neLoopEnd = null;
let neLastPlayheadBlockId = null;
// neZoom is global; Break auto-fit is a one-block exception, restored on leaving block.
let neAutoFitZoomBlockId = null;
let neZoomBeforeAutoFit = null;
// True when loop range is auto-derived from Break tail; manual drag overrides.
let neLoopFollowsBreak = false;
function isTripletSubdivision(subdivision) { return subdivision === 3 || subdivision === 6; }
// Use raw beat to avoid zone-lookup vs quantization dependency.
function tripletZoneAt(block, engineGroup, beat) {
  const zones = block.tripletLayers && block.tripletLayers[engineGroup];
  if (!zones) return false;
  return zones.some(z => beat >= z.from - 1e-6 && beat < z.to - 1e-6);
}
// The triplet-zone key a track's notes quantize against: the mapped engine group for grouped
// tracks, or a dedicated per-instrument key for custom tracks (one track = one instrument).
// Custom keys have no sidebar subdivision knob, so zone resolution falls back to 4 steps/beat —
// the same default the regular groups' knobs start at.
function engineGroupKeyForTrack(track) {
  if (!track) return null;
  if (track.group) return UI_GROUP_TO_ENGINE_GROUP[track.group] || null;
  if (track.role === 'custom' && track.customInstrument) return 'custom:' + track.customInstrument;
  return null;
}

// Outside a zone: quantize with the Snap dropdown as-is. Inside a zone:
// the OPPOSITE type from the dropdown, at this track's own instrument resolution.
function neQuantizeForTrack(beat, g, block, trackId) {
  const engineGroup = engineGroupKeyForTrack(trackList.find(t => t.id === trackId));
  const inZone = engineGroup && tripletZoneAt(block, engineGroup, beat);
  if (!inZone) return neQuantize(beat, g.snap);
  const zoneSubdivision = isTripletSubdivision(g.subdivision)
    ? engineGroupStraightSubdivision(engineGroup)
    : engineGroupTripletSubdivision(engineGroup);
  return neQuantize(beat, 1/zoneSubdivision);
}

// Stat override mechanism: newer hint wins, block switch clears.
let neStatOverrideToken = 0;
let neStatOverrideTimer = null;
let neStatOverrideBlockId = null;

function neSetStatOverride(applyFn, durationMs) {
  const pills = document.getElementById('statPills');
  if (!pills) return;
  clearTimeout(neStatOverrideTimer);
  const myToken = ++neStatOverrideToken;
  neStatOverrideBlockId = activeBlockId;
  applyFn(pills);
  neStatOverrideTimer = setTimeout(() => {
    if (myToken !== neStatOverrideToken) return;
    renderStats(readParams(), readGrid());
  }, durationMs);
}

// Cancels override; safe to call every render.
function neClearStatOverride() {
  if (!neStatOverrideTimer) return;
  clearTimeout(neStatOverrideTimer);
  neStatOverrideTimer = null;
  neStatOverrideToken++;
  neStatOverrideBlockId = null;
  renderStats(readParams(), readGrid());
}

function neMsg(t) {
  neSetStatOverride((pills) => {
    const span = document.createElement('span');
    span.className = 'pill ne-msg-pill';
    span.textContent = t;
    pills.innerHTML = '';
    pills.appendChild(span);
  }, 1400);
}

function neFindCollision(arr, beat, skipIdx) {
  for (let i = 0; i < arr.length; i++) {
    if (i === skipIdx) continue;
    if (Math.abs(arr[i].beat - beat) < NE_MERGE_EPS) return i;
  }
  return -1;
}

// =========================================
// MULTI-NOTE MOVE (arrow-key nudge, articulation switch)
// =========================================
// selectedNotes keys are trackId|beat; batch ops resolve references, then rebuild.
// Deletes notes from keys as atomic edit; shared by shortcuts and delete-mode.
function neDeleteNoteKeys(block, keys, label) {
  if (!block || !keys.length) return;
  withBlockDataUndo(label, block, () => {
    keys.forEach(key => {
      const sep = key.indexOf('|');
      const tid = key.slice(0, sep);
      const beat = parseFloat(key.slice(sep + 1));
      const idx = (block.data[tid]||[]).findIndex(n => n.beat === beat);
      if (idx >= 0) {
        block.data[tid].splice(idx, 1);
      }
    });
    selectedNotes.clear();
    block.generated = false;
  });
}

// =========================================
// COPY / PASTE
// In-memory, offsets from earliest beat, trackId preserved.
let noteClipboard = null; // { notes: [{trackId, offset, vel}] } | null

function neCopySelection(block) {
  const items = neResolveSelection(block);
  if (!items.length) return false;
  const anchor = Math.min(...items.map(it => it.note.beat));
  noteClipboard = { notes: items.map(it => ({ trackId: it.trackId, offset: it.note.beat - anchor, vel: it.note.vel })) };
  return true;
}

// Paste at atBeat; snap to grid, replace collisions, skip out-of-range/locked.
function nePasteClipboard(block, atBeat) {
  if (!noteClipboard || !noteClipboard.notes.length) return false;
  const g = readGrid();
  const numerator = block.numerator;
  const blockBeats = Math.max(1, block.length || 1) * numerator;
  const isBreak = isBreakBlock(block);
  const breakLocked = isBreak && !block.breakUnlocked;
  const grooveBeats = isBreak ? Math.max(0, blockBeats - (block.breakBeats ?? BREAK_DEFAULT_BEATS)) : 0;

  let placed = 0, skipped = 0, replaced = 0;
  const touchedTracks = new Set();
  const editedNotes = new Map();
  const newKeys = [];
  noteClipboard.notes.forEach(cn => {
    const beat = neQuantizeForTrack(atBeat + cn.offset, g, block, cn.trackId);
    if (beat < 0 || beat >= blockBeats) { skipped++; return; }
    if (breakLocked && beat < grooveBeats) { skipped++; return; }
    const arr = block.data[cn.trackId] || (block.data[cn.trackId] = []);
    const collide = neFindCollision(arr, beat, -1);
    if (collide >= 0) {
      arr.splice(collide, 1);
      replaced++;
    }
    const pasted = { beat, vel: cn.vel };
    arr.push(pasted);
    (editedNotes.get(cn.trackId) || editedNotes.set(cn.trackId, []).get(cn.trackId)).push(pasted);
    touchedTracks.add(cn.trackId);
    newKeys.push(cn.trackId + '|' + beat);
    placed++;
  });
  if (!placed) { neMsg('Paste skipped — target is out of range or locked'); return false; }

  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));
  touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid, editedNotes.get(tid)));
  block.generated = false;
  selectedNotes.clear();
  newKeys.forEach(k => selectedNotes.add(k));
  neMsg(skipped
    ? `Pasted ${placed} note${placed>1?'s':''} (${skipped} skipped — out of range or locked)`
    : replaced ? `Pasted ${placed} note${placed>1?'s':''} — replaced ${replaced}` : `Pasted ${placed} note${placed>1?'s':''}`);
  return true;
}

function neResolveSelection(block) {
  const items = [];
  selectedNotes.forEach(key => {
    const sep = key.indexOf('|');
    const tid = key.slice(0, sep);
    const beat = parseFloat(key.slice(sep + 1));
    const arr = block.data[tid] || [];
    const idx = arr.findIndex(n => n.beat === beat);
    if (idx >= 0) items.push({ trackId: tid, note: arr[idx] });
  });
  return items;
}

// Applies a batch of moves ({ trackId, note, targetTrackId, targetBeat }) as
// one atomic, order-independent operation.
// Phase 1: compute all targets WITHOUT writing, so collision checks never
// see a partially-moved batch. If two selected notes target the same
// track+beat, the later one is left at its original position (not merged).
// Phase 2: replace any bystander note already at a target slot, then commit
// the track/beat changes.
function neApplyNoteMoves(block, proposals) {
  if (!proposals.length) return false;
  const movingNotes = new Set(proposals.map(p => p.note));

  // Phase 1 — claim target slots; drop self-collisions.
  const claimed = new Map();
  let selfSkipped = 0;
  const finalized = [];
  proposals.forEach(p => {
    const claimKey = p.targetTrackId + '|' + p.targetBeat.toFixed(4);
    if (claimed.has(claimKey)) { selfSkipped++; return; }
    claimed.set(claimKey, p);
    finalized.push(p);
  });
  if (!finalized.length) { if (selfSkipped) neMsg('Move skipped — selected notes would overlap each other'); return false; }

  // Phase 2a — replace any bystander note already occupying a target slot.
  let replaced = 0;
  finalized.forEach(p => {
    const targetArr = block.data[p.targetTrackId] || (block.data[p.targetTrackId] = []);
    const collideIdx = targetArr.findIndex(n => !movingNotes.has(n) && Math.abs(n.beat - p.targetBeat) < NE_MERGE_EPS);
    if (collideIdx >= 0) {
      targetArr.splice(collideIdx, 1); replaced++;
    }
  });

  // Phase 2b — commit the track/beat changes.
  const touchedTracks = new Set();
  const editedNotes = new Map();
  const noteEdited = (tid, n) => (editedNotes.get(tid) || editedNotes.set(tid, []).get(tid)).push(n);
  finalized.forEach(p => {
    touchedTracks.add(p.trackId);
    touchedTracks.add(p.targetTrackId);
    noteEdited(p.trackId, { beat: p.note.beat, turnaround: p.note.turnaround }); // where the note was
    noteEdited(p.targetTrackId, p.note);
    if (p.note.turnaround) delete p.note.turnaround; // manually edited — clear generator-placed flag
    if (p.targetTrackId !== p.trackId) {
      const oldArr = block.data[p.trackId];
      const oldIdx = oldArr ? oldArr.indexOf(p.note) : -1;
      if (oldIdx >= 0) oldArr.splice(oldIdx, 1);
      p.note.beat = p.targetBeat;
      block.data[p.targetTrackId].push(p.note);
    } else {
      p.note.beat = p.targetBeat;
    }
  });
  Object.keys(block.data).forEach(tid => { if (touchedTracks.has(tid)) block.data[tid].sort((a,b) => a.beat - b.beat); });

  if (selfSkipped > 0) neMsg(`${selfSkipped} note${selfSkipped>1?'s':''} skipped — would overlap another selected note`);
  else if (replaced > 0) neMsg(`Replaced ${replaced} existing note${replaced>1?'s':''}`);

  // Rebuild selection: finalized moves get new key, self-collision skips keep original.
  selectedNotes.clear();
  const finalizedSet = new Set(finalized);
  proposals.forEach(p => {
    const tid = finalizedSet.has(p) ? p.targetTrackId : p.trackId;
    selectedNotes.add(tid + '|' + p.note.beat);
  });

  touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid, editedNotes.get(tid)));
  block.generated = false;
  return true;
}

// Nudge selected notes in time only, with snap/clamp.
function neMoveSelectedNotesTime(block, g, deltaBeat) {
  const items = neResolveSelection(block);
  if (!items.length) return false;
  const totalBeats = (block.length || 1) * g.numerator;
  const proposals = items.map(it => ({
    trackId: it.trackId,
    note: it.note,
    targetTrackId: it.trackId,
    targetBeat: Math.max(0, Math.min(totalBeats - 0.01, neQuantizeForTrack(it.note.beat + deltaBeat, g, block, it.trackId)))
  }));
  return neApplyNoteMoves(block, proposals);
}

// =========================================
// ARTICULATION LOCK — per-block whitelist; disabling migrates notes to first-enabled.
// =========================================

// Default enabled articulations for new blocks (groups not listed start fully enabled).
const DEFAULT_ENABLED_ARTICULATIONS = {
  snare: ['snare', 'snareOffCenter'],
  hihat: ['chh', 'ohh'],
  ride: ['ride', 'rideBowShank'],
  rackTom1: ['tomHi', 'rackTom1RimShot'],
  rackTom2: ['rackTom2Center', 'rackTom2RimShot'],
  rackTom3: ['tomMid', 'rackTom3RimShot'],
  floorTom1: ['floorTom1Center', 'floorTom1RimShot'],
  floorTom2: ['tomLow', 'floorTom2RimShot'],
};

// Get enabled articulations for group; fallback to defaults or all.
function getEnabledArticulations(block, groupId) {
  const groupTrackIds = trackList.filter(t => t.group === groupId).map(t => t.id);
  const stored = block && block.enabledArticulations && block.enabledArticulations[groupId];
  if (!stored || !stored.length) {
    const def = DEFAULT_ENABLED_ARTICULATIONS[groupId];
    if (def) {
      const filtered = groupTrackIds.filter(id => def.includes(id));
      if (filtered.length) return filtered;
    }
    return groupTrackIds;
  }
  const filtered = groupTrackIds.filter(id => stored.includes(id));
  return filtered.length ? filtered : groupTrackIds;
}

function isArticulationEnabled(block, trackId) {
  const track = trackList.find(t => t.id === trackId);
  if (!track || !track.group) return true;
  return getEnabledArticulations(block, track.group).includes(trackId);
}

// Low-level set whitelist membership; no render/undo; returns false if would leave zero enabled.
function setArticulationEnabled(block, trackId, enabled) {
  const track = trackList.find(t => t.id === trackId);
  if (!track || !track.group) return false;
  const groupTrackIds = trackList.filter(t => t.group === track.group).map(t => t.id);
  const current = getEnabledArticulations(block, track.group);
  const has = current.includes(trackId);
  if (has === enabled) return true;
  if (has && !enabled && current.length <= 1) return false;
  const next = enabled ? current.concat([trackId]) : current.filter(id => id !== trackId);
  block.enabledArticulations = block.enabledArticulations || {};
  block.enabledArticulations[track.group] = groupTrackIds.filter(id => next.includes(id));
  if (has && !enabled) migrateNotesOffDisabledArticulation(block, trackId);
  return true;
}

// Migrate notes from disabled track to first-enabled articulation.
function migrateNotesOffDisabledArticulation(block, trackId) {
  const track = trackList.find(t => t.id === trackId);
  if (!track || !track.group) return;
  const arr = block.data[trackId];
  if (!arr || !arr.length) return;
  const enabled = getEnabledArticulations(block, track.group);
  const targetId = enabled.find(id => id !== trackId);
  if (!targetId) return;
  block.data[targetId] = (block.data[targetId] || []).concat(arr);
  block.data[trackId] = [];
}

// Toggle articulation whitelist with undo.
function toggleArticulationEnabled(block, trackId) {
  const track = trackList.find(t => t.id === trackId);
  if (!track || !track.group) return false;
  const wasEnabled = isArticulationEnabled(block, trackId);
  if (wasEnabled && getEnabledArticulations(block, track.group).length <= 1) { neMsg('At least one articulation must stay enabled.'); return false; }
  const beforeStr = JSON.stringify({ enabledArticulations: block.enabledArticulations || {}, data: block.data });
  if (!setArticulationEnabled(block, trackId, !wasEnabled)) return false;
  const afterStr = JSON.stringify({ enabledArticulations: block.enabledArticulations, data: block.data });
  renderNoteEditor();
  pushUndo(`${wasEnabled ? 'Disable' : 'Enable'} ${track.groupLabel || track.label}`,
    () => { const s = JSON.parse(beforeStr); block.enabledArticulations = s.enabledArticulations; block.data = s.data; renderNoteEditor(); },
    () => { const s = JSON.parse(afterStr); block.enabledArticulations = s.enabledArticulations; block.data = s.data; renderNoteEditor(); }
  );
  return true;
}

// Shift articulation up/down; expanded group cycles enabled articulations, collapsed skips group.
function neShiftArticulation(trackId, dir, block) {
  const idx = trackList.findIndex(t => t.id === trackId);
  if (idx < 0) return trackId;
  const track = trackList[idx];
  const groupOpen = !track.group || expandedGroups.has(track.group);
  const firstEnabledOf = (groupId) => {
    const enabled = block ? getEnabledArticulations(block, groupId) : null;
    return (enabled ? trackList.find(t => t.group === groupId && enabled.includes(t.id)) : null)
      || trackList.find(t => t.group === groupId);
  };
  if (groupOpen) {
    let i = idx;
    while (i + dir >= 0 && i + dir < trackList.length) {
      i += dir;
      const candidate = trackList[i];
      if (candidate.group === track.group) {
        if (!block || isArticulationEnabled(block, candidate.id)) return candidate.id;
        continue;
      }
      if (!candidate.group) return candidate.id;
      return firstEnabledOf(candidate.group).id;
    }
    return trackList[Math.max(0, Math.min(trackList.length - 1, i))].id;
  }
  let i = idx;
  while (i+dir >= 0 && i+dir < trackList.length && trackList[i+dir].group === track.group) i += dir;
  const nextIdx = Math.max(0, Math.min(trackList.length - 1, i + dir));
  const targetGroup = trackList[nextIdx].group;
  const firstOfTargetGroup = targetGroup ? firstEnabledOf(targetGroup) : trackList[nextIdx];
  return firstOfTargetGroup.id;
}

// Move selected notes to next/prev articulation; beat unchanged.
function neMoveSelectedNotesArticulation(block, g, dir) {
  const items = neResolveSelection(block);
  if (!items.length) return false;
  let anyChanged = false;
  const proposals = items.map(it => {
    const targetTrackId = neShiftArticulation(it.trackId, dir, block);
    if (targetTrackId !== it.trackId) anyChanged = true;
    return { trackId: it.trackId, note: it.note, targetTrackId, targetBeat: it.note.beat };
  });
  if (!anyChanged) { neMsg('Already at the first/last instrument'); return false; }
  return neApplyNoteMoves(block, proposals);
}

// Micro-nudge: step = snap/8, no collision handling.
function neNudgeSelection(block, g, dirSteps) {
  const items = neResolveSelection(block);
  if (!items.length) return false;
  const stepBeats = (g.snap / 8) * dirSteps;
  const totalBeats = (block.length || 1) * g.numerator;
  let anyChanged = false;
  items.forEach(it => {
    const newBeat = Math.max(0, Math.min(totalBeats - 0.0001, it.note.beat + stepBeats));
    if (newBeat !== it.note.beat) { it.note.beat = newBeat; anyChanged = true; }
  });
  if (!anyChanged) return false;

  const touchedTracks = new Set(items.map(it => it.trackId));
  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));

  selectedNotes.clear();
  items.forEach(it => selectedNotes.add(it.trackId + '|' + it.note.beat));

  items.forEach(it => { if (it.note.turnaround) delete it.note.turnaround; }); // manually edited — clear generator-placed flag
  touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid, items.filter(it => it.trackId === tid).map(it => it.note)));
  block.generated = false;
  return true;
}



// Snap selection to grid; respects triplet zones.
function neSnapSelectionToGrid(block, g) {
  const items = neResolveSelection(block);
  if (!items.length) return false;
  const totalBeats = (block.length || 1) * g.numerator;
  const touchedTracks = new Set();
  let anyChanged = false;
  items.forEach(it => {
    const snapped = Math.max(0, Math.min(totalBeats - 0.0001, neQuantizeForTrack(it.note.beat, g, block, it.trackId)));
    if (Math.abs(snapped - it.note.beat) > 1e-6) {
      it.note.beat = snapped;
      anyChanged = true;
      if (it.note.turnaround) delete it.note.turnaround; // manually edited — clear generator-placed flag
    }
    touchedTracks.add(it.trackId);
  });
  if (!anyChanged) return false;
  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));

  selectedNotes.clear();
  items.forEach(it => selectedNotes.add(it.trackId + '|' + it.note.beat));

  touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid, items.filter(it => it.trackId === tid).map(it => it.note)));
  block.generated = false;
  return true;
}

function fadedColor(hex, alpha) {
  const h = hex.replace('#','');
  const r = parseInt(h.substring(0,2),16), g = parseInt(h.substring(2,4),16), b = parseInt(h.substring(4,6),16);
  return `rgba(${r},${g},${b},${Math.max(0.35,alpha)})`;
}

// Cache grid-line fragments by fingerprint for performance; evict oldest.
const NE_GRID_CACHE_MAX = 24; // bounded so the cache can't grow unboundedly over a long session
const neGridLineCache = new Map(); // fingerprint -> DocumentFragment, cloned per row

function neGridLineFingerprint(totalBeats, pxPerBeat, snap, numerator, zones, engineGroup, g) {
  const zoneKey = (zones && zones.length) ? zones.map(z => z.from.toFixed(4)+'-'+z.to.toFixed(4)).join(',') : '';
  return [totalBeats, pxPerBeat.toFixed(4), snap, numerator, zoneKey, engineGroup||'', g ? g.subdivision : ''].join('|');
}

function neBuildGridLineFragment(totalBeats, pxPerBeat, snap, numerator, zones, engineGroup, g) {
  const frag = document.createDocumentFragment();
  const inAnyZone = (beat) => zones && zones.some(z => beat >= z.from - 1e-6 && beat < z.to - 1e-6);
  for (let b = 0; b < totalBeats; b++) {
    const tick = document.createElement('div');
    tick.className = 'ne-tick';
    tick.style.left = (NE_LEFT_PAD + b*pxPerBeat)+'px';
    tick.style.background = 'var(--border2)';
    frag.appendChild(tick);
  }
  // Ambient grid: skip positions covered by zones.
  for (let x = 0; x <= totalBeats; x += snap) {
    if (inAnyZone(x)) continue;
    const sl = document.createElement('div');
    sl.className = 'ne-snap-line';
    sl.style.left = (NE_LEFT_PAD + x*pxPerBeat)+'px';
    frag.appendChild(sl);
  }
  // stronger division line at every bar boundary so multi-bar blocks read clearly
  if (numerator) {
    for (let bar = 1; bar < Math.round(totalBeats/numerator); bar++) {
      const bl = document.createElement('div');
      bl.className = 'ne-bar-line';
      bl.style.left = (NE_LEFT_PAD + bar*numerator*pxPerBeat)+'px';
      frag.appendChild(bl);
    }
  }
  // Zone grid: opposite type from ambient, at group's resolution.
  if (zones && zones.length && engineGroup && g) {
    const zoneSubdivision = isTripletSubdivision(g.subdivision)
      ? engineGroupStraightSubdivision(engineGroup)
      : engineGroupTripletSubdivision(engineGroup);
    const zoneStep = 1/zoneSubdivision;
    zones.forEach(z => {
      const zoneEl = document.createElement('div');
      zoneEl.className = 'ne-triplet-zone-bg';
      zoneEl.style.left = (NE_LEFT_PAD + z.from*pxPerBeat)+'px';
      zoneEl.style.width = ((z.to-z.from)*pxPerBeat)+'px';
      frag.appendChild(zoneEl);
      for (let x = z.from; x <= z.to + 1e-6; x += zoneStep) {
        const tl = document.createElement('div');
        tl.className = 'ne-triplet-grid-line';
        tl.style.left = (NE_LEFT_PAD + x*pxPerBeat)+'px';
        frag.appendChild(tl);
      }
    });
  }
  return frag;
}

// The grid of a row (beat ticks, snap lines, bar lines) as a single element: three repeating gradients, sized from the zoom
// (see .ne-row-grid in style.css). snapOnly = just the snap lines (velocity lane).
// Level of detail: a line level is only drawn when its lines are at least this far apart; bar lines are thinned out in powers of
// two (like the ruler's labels), so a long block never shows a mush of hairlines.
const NE_GRID_MIN_SNAP_PX = 6, NE_GRID_MIN_BEAT_PX = 8, NE_GRID_MIN_BAR_PX = 14;

function neGridBarStride(barPx) {
  let stride = 1;
  while (barPx * stride < NE_GRID_MIN_BAR_PX && stride < 4096) stride *= 2;
  return stride;
}

function neBuildCssGrid(totalBeats, pxPerBeat, snap, numerator, snapOnly) {
  const el = document.createElement('div');
  el.className = 'ne-row-grid';
  el.style.left = NE_LEFT_PAD + 'px';
  el.style.width = (totalBeats * pxPerBeat + 1) + 'px';
  const line = (color) => `linear-gradient(to right, ${color} 1px, transparent 1px)`;
  const SNAP_COLOR = 'rgba(30,64,175,.3)';
  const snapPx = snap * pxPerBeat;
  const barSpan = (numerator || 1) * pxPerBeat * neGridBarStride((numerator || 1) * pxPerBeat);
  const layers = [], sizes = [];
  if (snapOnly) {
    // One level of lines: the finest one that is still readable.
    const step = snapPx >= NE_GRID_MIN_SNAP_PX ? snapPx : (pxPerBeat >= NE_GRID_MIN_SNAP_PX ? pxPerBeat : barSpan);
    layers.push(line(SNAP_COLOR)); sizes.push(step + 'px 100%');
  } else {
    layers.push(line('color-mix(in srgb, var(--border2) 55%, transparent)')); sizes.push(barSpan + 'px 100%');
    if (snapPx >= NE_GRID_MIN_SNAP_PX) { layers.push(line(SNAP_COLOR)); sizes.push(snapPx + 'px 100%'); }
    if (pxPerBeat >= NE_GRID_MIN_BEAT_PX) { layers.push(line('var(--border2)')); sizes.push(pxPerBeat + 'px 100%'); }
  }
  el.style.backgroundImage = layers.join(',');
  el.style.backgroundSize = sizes.join(',');
  return el;
}

// Draws the beat-tick + snap-line grid into a row element. Used by both
// normal leaf rows and collapsed group-header rows, so the vertical grid
// lines always run continuously down the whole editor. Thin cache-lookup
// wrapper around neBuildGridLineFragment.
function neDrawRowGrid(rowEl, totalBeats, pxPerBeat, snap, numerator, zones, engineGroup, g) {
  // Rows without triplet zones get the grid as ONE element with a CSS background (cost independent of the block length); rows
  // with zones keep the line elements, because the ambient lines have to be left out inside the zones.
  if (!zones || !zones.length) {
    rowEl.appendChild(neBuildCssGrid(totalBeats, pxPerBeat, snap, numerator, false));
    return;
  }
  const key = neGridLineFingerprint(totalBeats, pxPerBeat, snap, numerator, zones, engineGroup, g);
  let cached = neGridLineCache.get(key);
  if (!cached) {
    cached = neBuildGridLineFragment(totalBeats, pxPerBeat, snap, numerator, zones, engineGroup, g);
    if (neGridLineCache.size >= NE_GRID_CACHE_MAX) {
      neGridLineCache.delete(neGridLineCache.keys().next().value);
    }
    neGridLineCache.set(key, cached);
  }
  rowEl.appendChild(cached.cloneNode(true));
}