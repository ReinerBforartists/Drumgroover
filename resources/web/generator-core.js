// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// Shared filename timestamp — local time, format: 2026-07-15_143052.
// Used for exports and saves so all downloaded files share a naming convention.
function timestampSlug() {
  const d = new Date();
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

// Confirmation dialog setting – positive polarity (checked = show dialogs).
const SHOW_CONFIRM_DIALOGS_KEY = 'dggShowConfirmDialogs';
function migrateConfirmDialogSetting() {
  if (dggGetItem(SHOW_CONFIRM_DIALOGS_KEY) !== null) return;
  if (dggGetItem('dggSkipUnlockConfirm') === '1') dggSetItem(SHOW_CONFIRM_DIALOGS_KEY, '0');
}
if (typeof dggOnReady === 'function') {
  dggOnReady(migrateConfirmDialogSetting);
} else {
  migrateConfirmDialogSetting();
}
function shouldShowConfirmDialogs() {
  return dggGetItem(SHOW_CONFIRM_DIALOGS_KEY) !== '0';
}
function setShowConfirmDialogs(show) {
  dggSetItem(SHOW_CONFIRM_DIALOGS_KEY, show ? '1' : '0');
  syncShowConfirmDialogsToggle();
}

// Subdivision knob values (straight resolutions): 8th through 64th.
const SUBDIVISION_KNOB_VALUES = [2, 4, 8, 16];
const SUBDIVISION_KNOB_LABELS = ['8th', '16th', '32nd', '64th'];

// Break Kick layer subdivision range (includes 4th note).
const BREAK_KICK_SUBDIVISION_VALUES = [1, 2, 4, 8];
const BREAK_KICK_SUBDIVISION_LABELS = ['4th', '8th', '16th', '32nd'];

function readSubdivisionKnob(elId) { return SUBDIVISION_KNOB_VALUES[getParamNum(elId)] || 4; }

// Map engine group to its Subdivision knob element id.
const ENGINE_GROUP_SUBDIVISION_KNOB = {
  kick: 'kickSubdivision', snare: 'snareSubdivision', hihat: 'hihatSubdivision',
  ride: 'rideSubdivision', toms: 'tomsSubdivision', cymbals: 'cymbalsSubdivision'
};

// Returns the group's straight subdivision in steps/beat.
function engineGroupStraightSubdivision(engineGroup) {
  const elId = ENGINE_GROUP_SUBDIVISION_KNOB[engineGroup];
  return elId ? readSubdivisionKnob(elId) : 4;
}

// Returns the group's matching triplet subdivision (straight × 1.5).
function engineGroupTripletSubdivision(engineGroup) {
  return engineGroupStraightSubdivision(engineGroup) * 1.5;
}

// Kit tracks: label, colour and note-editor grouping of every track by its id. The roster itself (which tracks exist, in which order, with which
// GM note) is the instrument table's (bridge.js INSTRUMENT_TABLE); trackList is built from both once the table has arrived.
const TRACK_LAYOUT = {
  kick: { label:'Kick', color:'#0ea5e9', group:'kick', groupLabel:'High Stack' },
  kickOpen: { label:'Open', color:'#38bdf8', group:'kick', groupLabel:'Open' },
  snare: { label:'Snare', color:'#fbbf24', group:'snare', groupLabel:'Center' },
  snareOffCenter: { label:'Off Center', color:'#fcd34d', group:'snare', groupLabel:'Off Center' },
  snareEdge: { label:'Edge', color:'#fde68a', group:'snare', groupLabel:'Edge' },
  snareRimShot: { label:'Rim Shot', color:'#fb923c', group:'snare', groupLabel:'Rimshot' },
  rim: { label:'Rim', color:'#f472b6', group:'snare', groupLabel:'Sidestick' },
  snareRimOnly: { label:'Rim Only', color:'#fdba74', group:'snare', groupLabel:'Rim Only' },
  chh: { label:'Closed Edge', color:'#10b981', group:'hihat', groupLabel:'Closed Edge' },
  hihatTightEdge: { label:'Tight Edge', color:'#34d399', group:'hihat', groupLabel:'Tight Edge' },
  ohh: { label:'Open Edge 0', color:'#6ee7b7', group:'hihat', groupLabel:'Open Edge 0' },
  hihatOpenEdge4: { label:'Open Edge 4', color:'#0d9488', group:'hihat', groupLabel:'Open Edge 4' },
  hihatClosedTip: { label:'Closed Tip', color:'#99f6e4', group:'hihat', groupLabel:'Closed Tip' },
  hihatTightTip: { label:'Tight Tip', color:'#ccfbf1', group:'hihat', groupLabel:'Tight Tip' },
  tomHi: { label:'Center', color:'#fb7185', group:'rackTom1', groupLabel:'Center' },
  rackTom1RimShot: { label:'Rimshot', color:'#fda4af', group:'rackTom1', groupLabel:'Rimshot' },
  rackTom1RimOnly: { label:'Rim Only', color:'#fecdd3', group:'rackTom1', groupLabel:'Rim Only' },
  rackTom2Center: { label:'Center', color:'#f87171', group:'rackTom2', groupLabel:'Center' },
  rackTom2RimShot: { label:'Rimshot', color:'#fca5a5', group:'rackTom2', groupLabel:'Rimshot' },
  rackTom2RimOnly: { label:'Rim Only', color:'#fecaca', group:'rackTom2', groupLabel:'Rim Only' },
  tomMid: { label:'Center', color:'#ef4444', group:'rackTom3', groupLabel:'Center' },
  rackTom3RimShot: { label:'Rimshot', color:'#f87171', group:'rackTom3', groupLabel:'Rimshot' },
  rackTom3RimOnly: { label:'Rim Only', color:'#fca5a5', group:'rackTom3', groupLabel:'Rim Only' },
  floorTom1Center: { label:'Center', color:'#dc2626', group:'floorTom1', groupLabel:'Center' },
  floorTom1RimShot: { label:'Rimshot', color:'#ef4444', group:'floorTom1', groupLabel:'Rimshot' },
  floorTom1RimOnly: { label:'Rim Only', color:'#f87171', group:'floorTom1', groupLabel:'Rim Only' },
  tomLow: { label:'Center', color:'#b91c1c', group:'floorTom2', groupLabel:'Center' },
  floorTom2RimShot: { label:'Rimshot', color:'#dc2626', group:'floorTom2', groupLabel:'Rimshot' },
  floorTom2RimOnly: { label:'Rim Only', color:'#ef4444', group:'floorTom2', groupLabel:'Rim Only' },
  cymbal1Crash: { label:'Crash', color:'#f8fafc', group:'cymbal1', groupLabel:'Crash' },
  cymbal1MuteHit: { label:'Mute Hit', color:'#e2e8f0', group:'cymbal1', groupLabel:'Mute Hit' },
  crash1: { label:'Crash', color:'#f1f5f9', group:'cymbal2', groupLabel:'Crash' },
  cymbal2MuteHit: { label:'Mute Hit', color:'#e2e8f0', group:'cymbal2', groupLabel:'Mute Hit' },
  cymbal3Crash: { label:'Crash', color:'#f8fafc', group:'cymbal3', groupLabel:'Crash' },
  cymbal3MuteHit: { label:'Mute Hit', color:'#e2e8f0', group:'cymbal3', groupLabel:'Mute Hit' },
  ride: { label:'Bow Tip', color:'#22d3ee', group:'ride', groupLabel:'Bow Tip' },
  rideBowShank: { label:'Bow Shank', color:'#67e8f9', group:'ride', groupLabel:'Bow Shank' },
  rideBellTip: { label:'Bell Tip', color:'#a5f3fc', group:'ride', groupLabel:'Bell Tip' },
  rideBell: { label:'Bell Shank', color:'#cffafe', group:'ride', groupLabel:'Bell Shank' },
  rideEdge: { label:'Edge', color:'#06b6d4', group:'ride', groupLabel:'Edge' },
  rideMuteHit: { label:'Mute Hit', color:'#0e7490', group:'ride', groupLabel:'Mute Hit' },
  crash2: { label:'Crash', color:'#cbd5e1', group:'cymbal4', groupLabel:'Crash' },
  cymbal4MuteHit: { label:'Mute Hit', color:'#e2e8f0', group:'cymbal4', groupLabel:'Mute Hit' },
  cymbal5Crash: { label:'Crash', color:'#f8fafc', group:'cymbal5', groupLabel:'Crash' },
  cymbal5MuteHit: { label:'Mute Hit', color:'#e2e8f0', group:'cymbal5', groupLabel:'Mute Hit' },
};
let trackList = [];
function buildTrackListFromTable() {
  if (trackList.length || !INSTRUMENT_TABLE) return;
  for (const r of INSTRUMENT_TABLE.instruments) {
    if (r.gm === null) continue;   // custom instruments get their tracks from the Add Track dialog
    const l = TRACK_LAYOUT[r.id];
    if (!l) { console.error('instrument table row without track layout: ' + r.id); continue; }
    trackList.push({ id: r.id, label: l.label, color: l.color, gm: r.gm, role: r.id, group: l.group, groupLabel: l.groupLabel });
  }
}
dggOnInstrumentTable(buildTrackListFromTable);


let blocksList = [];
// Generation counter prevents stale default generation from overwriting a restored project.
let projectGeneration = 0;
let activeBlockId = null;
let focusedTrackId = null;

// =========================================
// Song Engine – manages the timeline arrangement
// =========================================
class SongEngine {
  constructor() {
    this.songSequence = [];
  }

  // Returns the number of bars a slot occupies.
  slotSpanBars(slot) {
    const block = findBlock(slot.blockId);
    return (block ? (block.length || 1) : 1) * (slot.loopCount || 1);
  }

  // End bar (exclusive) of a slot.
  slotEndBar(slot) {
    return slot.startBar + this.slotSpanBars(slot);
  }

  // Checks if a range overlaps any existing slot except the one excluded.
  overlapsAny(startBar, spanBars, excludeSlot) {
    return this.songSequence.some(s => {
      if (s === excludeSlot) return false;
      return startBar < this.slotEndBar(s) && (startBar + spanBars) > s.startBar;
    });
  }

  // Finds the nearest free bar to place a slot of given span.
  findNearestFreeBar(desiredStartBar, spanBars, excludeSlot) {
    desiredStartBar = Math.max(0, desiredStartBar);
    if (!this.overlapsAny(desiredStartBar, spanBars, excludeSlot)) return desiredStartBar;
    const others = this.songSequence.filter(s => s !== excludeSlot);
    const candidates = [0, ...others.flatMap(s => [this.slotEndBar(s), Math.max(0, s.startBar - spanBars)])];
    let best = null, bestDist = Infinity;
    candidates.forEach(c => {
      if (this.overlapsAny(c, spanBars, excludeSlot)) return;
      const dist = Math.abs(c - desiredStartBar);
      if (dist < bestDist) { bestDist = dist; best = c; }
    });
    return best !== null ? best : desiredStartBar;
  }

  // Adds a new slot, defaulting to append after the last slot.
  addSlot(blockId, startBar) {
    const span = this.slotSpanBars({ blockId, loopCount: 1 });
    if (startBar === undefined) {
      startBar = this.songSequence.length ? Math.max(...this.songSequence.map(s => this.slotEndBar(s))) : 0;
    }
    const finalBar = this.findNearestFreeBar(startBar, span, null);
    this.songSequence.push({ blockId, startBar: finalBar, loopCount: 1 });
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
  }

  // Places a ready-made slot object at a specific position.
  placeSlotAt(startBar, slot) {
    const span = this.slotSpanBars(slot);
    const finalBar = this.findNearestFreeBar(startBar, span, null);
    this.songSequence.push({ ...slot, startBar: finalBar });
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
  }

  updateSlot(index, updates) {
    if (index < 0 || index >= this.songSequence.length) return;
    this.songSequence[index] = { ...this.songSequence[index], ...updates };
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
  }

  removeSlot(index) {
    this.songSequence.splice(index, 1);
  }

  // Moves an existing slot to (or nearest to) `desiredStartBar`.
  moveSlot(index, desiredStartBar) {
    const slot = this.songSequence[index];
    if (!slot) return;
    slot.startBar = this.findNearestFreeBar(desiredStartBar, this.slotSpanBars(slot), slot);
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
  }

  // Duplicates an existing slot at (or nearest to) `desiredStartBar`.
  duplicateSlotAt(index, desiredStartBar) {
    const orig = this.songSequence[index];
    if (!orig) return;
    const finalBar = this.findNearestFreeBar(desiredStartBar, this.slotSpanBars(orig), null);
    this.songSequence.push({ ...orig, startBar: finalBar });
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
  }

  // Returns the nearest slot boundary to `bar`, but only if `bar` is inside an existing slot.
  nearestBoundary(bar, excludeSlot) {
    const others = this.songSequence.filter(s => s !== excludeSlot);
    const covering = others.find(s => bar > s.startBar && bar < this.slotEndBar(s));
    if (!covering) return bar;
    const distToStart = bar - covering.startBar;
    const distToEnd = this.slotEndBar(covering) - bar;
    return distToStart <= distToEnd ? covering.startBar : this.slotEndBar(covering);
  }

  // Ripple-insert: makes room for a slot by pushing only overlapping slots.
  insertWithRipple(desiredStartBar, spanBars, excludeSlot) {
    const boundary = this.nearestBoundary(Math.max(0, desiredStartBar), excludeSlot);
    let frontier = boundary + spanBars;
    const others = this.songSequence
      .filter(s => s !== excludeSlot && this.slotEndBar(s) > boundary)
      .sort((a, b) => a.startBar - b.startBar);
    others.forEach(s => {
      if (s.startBar < frontier) {
        s.startBar = frontier;
        frontier = s.startBar + this.slotSpanBars(s);
      }
    });
    return boundary;
  }

  placeSlotAtRipple(desiredStartBar, slot) {
    const span = this.slotSpanBars(slot);
    const boundary = this.insertWithRipple(desiredStartBar, span, null);
    this.songSequence.push({ ...slot, startBar: boundary });
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
    return boundary;
  }

  moveSlotRipple(index, desiredStartBar) {
    const slot = this.songSequence[index];
    if (!slot) return;
    const span = this.slotSpanBars(slot);
    const boundary = this.insertWithRipple(desiredStartBar, span, slot);
    slot.startBar = boundary;
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
    return boundary;
  }

  duplicateSlotAtRipple(index, desiredStartBar) {
    const orig = this.songSequence[index];
    if (!orig) return;
    const span = this.slotSpanBars(orig);
    const boundary = this.insertWithRipple(desiredStartBar, span, null);
    this.songSequence.push({ ...orig, startBar: boundary });
    this.songSequence.sort((a,b) => a.startBar - b.startBar);
    return boundary;
  }

  // Expanded sequence: loops expanded into consecutive entries.
  getExpandedSequence() {
    const expanded = [];
    this.songSequence.forEach(slot => {
      const block = findBlock(slot.blockId);
      const len = block ? (block.length || 1) : 1;
      for (let i = 0; i < (slot.loopCount || 1); i++) {
        expanded.push({ blockId: slot.blockId, startBar: slot.startBar + i*len });
      }
    });
    return expanded;
  }
}

const songEngine = new SongEngine();

let selectedSegmentIdx = null;
const BLOCK_PALETTE = ['#0ea5e9','#fbbf24','#ec4899','#10b981','#f97316','#a78bfa','#f43f5e','#22d3ee','#84cc16','#e879f9'];
function blockColor(blockId) { const idx = blocksList.findIndex(b => b.id === blockId); return BLOCK_PALETTE[Math.max(0,idx) % BLOCK_PALETTE.length]; }

let fullPattern = null;

// =========================================
// SEEDED RNG & GRID HELPERS
// =========================================
function emptyBlockData() { const d = {}; trackList.forEach(t => d[t.id] = []); return d; }
function readGrid() {
  const numerator = getParamNum('numerator');
  const subdivision = getParamNum('subdivision');
  const denominator = getParamNum('denominator', 4) || 4;
  return { numerator, denominator, subdivision, snap: 1/subdivision };
}

function buildFill(rng, p, g) {
  const d = emptyBlockData();
  d.kick.push({ beat:0, vel:110 });
  const bb = backbeatPositions(g.numerator);
  if (bb[0]!==undefined) d.snare.push({ beat:bb[0], vel:108 });
  return d;
}

// =========================================
// BLOCK UTILITIES
// =========================================
function findBlock(id) { return blocksList.find(b => b.id === id); }

// A block is a Break iff it has a breakParentId.
function isBreakBlock(block) { return !!(block && block.breakParentId); }

// Break blocks and standalone fill blocks (Intro/Outro) are fill-generated.
function isFillGeneratedBlock(block) { return isBreakBlock(block) || !!(block && block.isFillBlock); }

function breakParentOf(block) { return block ? findBlock(block.breakParentId) : null; }

// Display name for a block; Breaks derive from parent.
function blockDisplayName(block) {
  if (!block) return '';
  if (block.breakParentId) {
    const parent = findBlock(block.breakParentId);
    if (parent) {
      const siblings = blocksList.filter(b => b.breakParentId === block.breakParentId);
      const idx = siblings.indexOf(block);
      const suffix = idx > 0 ? ' ' + (idx + 1) : '';
      return blockDisplayName(parent) + ' Break' + suffix;
    }
  }
  return block.name;
}

// Modal helper (placeholder – actual implementation elsewhere).
async function ensureDefaultBlocksAndSequence(g, rng, p) {

  const myGeneration = projectGeneration;
// Create default blocks (Intro, Verse, Chorus, Bridge, Outro, and their Breaks) if missing.
const BLOCK_DEFAULT_TURNAROUND = { intro:4, verse:4, chorus:4, bridge:4, outro:0, fill:4 }; // Breaks/Empty/Outro default to 0 (off) — a 1-bar block has nothing to turn around into.

  for (const type of ['intro','verse','verseBreak','chorus','chorusBreak','bridge','bridgeBreak','outro']) {
    if (projectGeneration !== myGeneration) return;
    if (!findBlock(type)) {
      if (DEFAULT_BREAK_PARENT[type]) {
        const b = generateBreakBlock(DEFAULT_BREAK_PARENT[type], g, type, BLOCK_DEFAULT_LENGTH[type] || 8, BREAK_DEFAULT_BEATS);
        if (b && projectGeneration === myGeneration) blocksList.push(b);
        continue;
      }
      if (type === 'intro' || type === 'outro') {
        const b = generateFillBlock(g, type, SECTION_LABELS[type], BLOCK_DEFAULT_LENGTH[type] || 1, BREAK_DEFAULT_BEATS, type === 'intro' ? 'end' : 'start', {});
        if (b && projectGeneration === myGeneration) blocksList.push(b);
        continue;
      }
      const b = await generateTypedBlock(type, g, type, {
        length: BLOCK_DEFAULT_LENGTH[type] || 8,
        turnaroundInterval: BLOCK_DEFAULT_TURNAROUND[type] || 0,
        velocityBuild: 0,
      });
      if (b && projectGeneration === myGeneration) blocksList.push(b);
    }
  }

  if (projectGeneration !== myGeneration) return;

  if (!activeBlockId || !findBlock(activeBlockId)) activeBlockId = 'verse';

  if (songEngine.songSequence.length === 0) {
      ['intro','verse','verseBreak','chorus','chorusBreak','bridge','bridgeBreak','outro'].forEach(id => songEngine.addSlot(id));
  }
}

// =========================================
// GENERATION PIPELINE
// =========================================
let sessionSeed = Math.floor(Math.random()*999999);
function readParams() {
  return { seed: sessionSeed };
}

async function generateSequence(regenGenerated = true) {
  const p = readParams();
  const g = readGrid();
  const rng = new RNG(p.seed);

  if (regenGenerated) await ensureDefaultBlocksAndSequence(g, rng, p);
  else resizeAllBlocks(g);

  // Apply six engine groups to the main block (if it exists).
  applyKickGroupToBlock(findBlock('main'), g);
  applySnareGroupToBlock(findBlock('main'), g);
  applyHihatGroupToBlock(findBlock('main'), g);
  applyRideGroupToBlock(findBlock('main'), g);
  applyTomsGroupToBlock(findBlock('main'), g);
  applyCymbalsGroupToBlock(findBlock('main'), g);

  assembleFullPattern(p, g, rng);
  renderBlockTabs();
  renderNoteEditor();
  renderTimeline();
  renderStats(p, g);
}

// =========================================
// ASSEMBLE FULL PATTERN
// =========================================
function assembleFullPattern(p, g, rng) {
  const events = [];
  const chain = songEngine.getExpandedSequence();

  let maxEndBeat = 0;
  chain.forEach(slot => {
    const block = findBlock(slot.blockId);
    const startBeat = slot.startBar * g.numerator;
    if (!block) return;
    const src = block.data;
    trackList.forEach(t => {
      (src[t.id]||[]).forEach(note => {
        events.push({ trackId: t.id, beatAbs: startBeat + note.beat, vel: note.vel });
      });
    });
    maxEndBeat = Math.max(maxEndBeat, startBeat + (block.length || 1) * g.numerator);
  });

  events.sort((a,b) => a.beatAbs - b.beatAbs);
  fullPattern = { events, totalBeats: maxEndBeat, params: p, grid: g };

  // Send the updated pattern to the native host-sync scheduler.
  if (typeof hostBridge !== 'undefined') {
    syncCustomInstruments();
    hostBridge.send('syncPlaybackPattern', {
      fullPattern,
      loopStart: typeof tlLoopStart !== 'undefined' ? tlLoopStart : null,
      loopEnd: typeof tlLoopEnd !== 'undefined' ? tlLoopEnd : null,
    });
  }
}

let dataGeneration = 0;

// Rebuild full pattern after manual edits; commit false skips audio sync during drag.
function rebuildFullFromEditedBlocks(commit = true) {
  if (!commit) return;
  dataGeneration++;
  const p = readParams(); const g = readGrid();
  regenerateAllSyncedBreaks(g);
  const rng = new RNG(p.seed + 1);
  assembleFullPattern(p, g, rng);
  if (typeof syncStateToHost === 'function') syncStateToHost();
}

// Refresh all locked Breaks to mirror their parent.
function regenerateAllSyncedBreaks(g) {
  blocksList.filter(b => isBreakBlock(b) && !b.breakUnlocked).forEach(b => regenerateBreakBlockInPlace(b, g));
}

// =========================================
// BLOCK PALETTE & TRACK LOCKING
// =========================================
const UI_GROUP_TO_ENGINE_GROUP = {
  kick: 'kick',
  snare: 'snare',
  hihat: 'hihat',
  ride: 'ride',
  rackTom1: 'toms', rackTom2: 'toms', rackTom3: 'toms', floorTom1: 'toms', floorTom2: 'toms',
  cymbal1: 'cymbals', cymbal2: 'cymbals', cymbal3: 'cymbals', cymbal4: 'cymbals', cymbal5: 'cymbals',
};

// Mark a track as manually edited, locking its engine group.
// Custom tracks lock their own namespaced group ('custom:<trackId>').
// `touchedNotes` (optional) are the notes the edit changed. With them, an edit inside the turnaround area locks only the group's
// turnaround, an edit elsewhere only the group's plain notes, and an edit in both locks both. Without them both are locked.
function markTrackManuallyEdited(block, trackId, touchedNotes) {
  if (!block) return;
  const track = trackList.find(t => t.id === trackId);
  let groupId;
  if (track && track.role === 'custom') groupId = customGroupId(track.id);
  else groupId = track ? UI_GROUP_TO_ENGINE_GROUP[track.group] : undefined;
  if (!groupId) return;
  let lockMain = true, lockTurnaround = true;
  if (Array.isArray(touchedNotes) && touchedNotes.length && !(track && track.role === 'custom') && (block.turnaroundInterval || 0) > 0) {
    const num = block.numerator;
    // The turnaround only owns the groups the fill drives (its instruments, the Fill Kick, the closing crash). A plain note of any other
    // group inside the fill window, e.g. a hi-hat that keeps playing, is part of that group's own pattern.
    const driven = neRelevantEngineGroups(block.turnaroundInstruments || ['snare']).includes(groupId)
      || groupId === 'kick' || (groupId === 'cymbals' && block.turnaroundCrash !== false);
    const inArea = touchedNotes.map(n => !!n.turnaround || (driven && neNoteInTurnaroundArea(block, n, num)));
    lockTurnaround = inArea.some(Boolean);
    lockMain = inArea.some(x => !x);
    // A hand-placed note in the fill window belongs to the turnaround: flagged, it is kept while the turnaround is locked.
    touchedNotes.forEach((n, i) => { if (inArea[i]) n.turnaround = true; });
  }
  block.lockedGroups = block.lockedGroups || {};
  if (lockMain) block.lockedGroups[groupId] = true;
  if (lockTurnaround && !(track && track.role === 'custom')) block.lockedGroups[neTurnaroundLockKey(groupId)] = true;
  updateGroupLockUI();
}

function tracksInEngineGroup(engineGroupId) {
  return trackList.filter(t => UI_GROUP_TO_ENGINE_GROUP[t.group] === engineGroupId).map(t => t.id);
}

// Unlock a group: discard manual edits and regenerate from knobs.
async function unlockGroup(groupId) {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Hand back to knobs?',
      message: `This discards your manual edits to ${groupId} in this block and regenerates it from the current knob settings.`,
      confirmLabel: 'Discard & regenerate',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  block.lockedGroups = block.lockedGroups || {};
  block.lockedGroups[groupId] = false;
  block.lockedGroups[neTurnaroundLockKey(groupId)] = false;
  tracksInEngineGroup(groupId).forEach(tid => { if (block.data[tid]) block.data[tid] = []; });
  if (groupId.indexOf('custom:') === 0) {
    const tid = groupId.slice('custom:'.length);
    if (block.data[tid]) block.data[tid] = [];
    await regenerateCustomGroup();
    updateGroupLockUI();
    return;
  }
  if (groupId === 'kick') await regenerateKickGroup();
  else if (groupId === 'snare') await regenerateSnareGroup();
  else if (groupId === 'hihat') await regenerateHihatGroup();
  else if (groupId === 'ride') await regenerateRideGroup();
  else if (groupId === 'toms') await regenerateTomsGroup();
  else if (groupId === 'cymbals') await regenerateCymbalsGroup();
  updateGroupLockUI();
}

// Discard the hand edits of one group's turnaround only: its fill notes are generated again from the Fill controls, the plain notes stay.
async function unlockTurnaround(groupId) {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Hand the fill back to the dials?',
      message: `This discards your manual edits to the ${groupId} turnaround in this block and generates it again from the Fill settings. Notes outside the turnaround stay as they are.`,
      confirmLabel: 'Discard & regenerate',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  withBlockDataUndo(`Discard turnaround edits (${groupId})`, block, () => {
    block.lockedGroups = block.lockedGroups || {};
    block.lockedGroups[neTurnaroundLockKey(groupId)] = false;
    block._turnaroundFillCache = {};
    const tracks = tracksInEngineGroup(groupId);
    if (block.lockedGroups[groupId]) regenerateTurnaroundOverlay(block, readGrid(), tracks);
    else replicateGroupAcrossLength(block, readGrid(), tracks, getEnginePhrase(block, groupId));
    block.generated = false;
  });
}

// Check if a block has any discardable edits.
function blockHasDiscardableEdits(block) {
  if (!block) return false;
  if (isBreakBlock(block)) {
    if (block.breakUnlocked) return true;
    const mainGroups = neRelevantEngineGroups(block.breakInstruments || ['toms']);
    return mainGroups.some(g => block.lockedGroups && block.lockedGroups[g]) || !!(block.lockedGroups && block.lockedGroups.kick);
  }
  if (block.isFillBlock) return ALL_ENGINE_GROUPS.some(g => block.lockedGroups && block.lockedGroups[g]);
  return ALL_ENGINE_GROUPS.some(g => block.lockedGroups && (block.lockedGroups[g] || block.lockedGroups[neTurnaroundLockKey(g)]));
}

// Discard all edits on a block and regenerate it based on its type.
async function discardBlockBackToGenerator(block, g) {
  if (isBreakBlock(block)) {
    if (!blockHasDiscardableEdits(block)) return false;
    block.breakUnlocked = false;
    const res = await requestRegenerateBreakBlock(block, g, { forceAll: true });
    if (!res) return false;
    block.data = res.data;
    block.length = res.length;
    block.breakBeats = res.breakBeats;
    block.generated = false;
    return true;
  }
  if (block.isFillBlock) {
    const hadStrayLock = ALL_ENGINE_GROUPS.some(gid => block.lockedGroups && block.lockedGroups[gid]);
    if (!hadStrayLock) return false;
    block.lockedGroups = {};
    const res = await requestRegenerateFillBlock(block, g);
    if (!res) return false;
    block.data = res.data;
    block.length = res.length;
    block.fillBeats = res.fillBeats;
    block.feel = res.feel;
    block.generated = false;
    return true;
  }
  return discardLockedGroupsInActiveBlock(block);
}

// Discard locked groups in the active block (must be the active block).
async function discardLockedGroupsInActiveBlock(block) {
  const lockedNow = ALL_ENGINE_GROUPS.filter(g => block.lockedGroups && (block.lockedGroups[g] || block.lockedGroups[neTurnaroundLockKey(g)]));
  if (!lockedNow.length) return false;
  await Promise.all(lockedNow.map(groupId => {
    block.lockedGroups[groupId] = false;
    block.lockedGroups[neTurnaroundLockKey(groupId)] = false;
    tracksInEngineGroup(groupId).forEach(tid => { if (block.data[tid]) block.data[tid] = []; });
    if (groupId === 'kick') return regenerateKickGroup({ suppressUndo: true });
    else if (groupId === 'snare') return regenerateSnareGroup({ suppressUndo: true });
    else if (groupId === 'hihat') return regenerateHihatGroup({ suppressUndo: true });
    else if (groupId === 'ride') return regenerateRideGroup({ suppressUndo: true });
    else if (groupId === 'toms') return regenerateTomsGroup({ suppressUndo: true });
    else if (groupId === 'cymbals') return regenerateCymbalsGroup({ suppressUndo: true });
  }));
  return true;
}

// "Discard All Edits (Block)" – bulk discard for the active block.
async function unlockAllGroups() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (!blockHasDiscardableEdits(block)) {
    neMsg('Nothing to discard in this block');
    return;
  }
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Discard all edits in this block?',
      message: `This discards your manual edits in "${blockDisplayName(block)}" and regenerates it from its current knob/panel settings.`,
      confirmLabel: 'Discard all & regenerate',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  await withBlockDataUndoAsync(`Discard all edits (${blockDisplayName(block)})`, block, async () => {
    await discardBlockBackToGenerator(block, readGrid());
  });
  updateGroupLockUI();
}

// "Discard All Edits (Global)" – sweep all blocks.
async function unlockAllGroupsGlobal() {
  const targets = blocksList.filter(blockHasDiscardableEdits);
  if (!targets.length) {
    neMsg('Nothing to discard in any block');
    return;
  }
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Discard all edits in every block?',
      message: `This discards manual edits across ${targets.length} block${targets.length>1?'s':''} project-wide (${targets.map(blockDisplayName).join(', ')}) and regenerates each one from its own knob/panel settings.`,
      confirmLabel: 'Discard all & regenerate',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  stopPlayback();
  const originalActiveId = activeBlockId;
  const before = JSON.stringify(blocksList);
  const g = readGrid();
  for (const block of targets) {
    activeBlockId = block.id;
    syncKnobsToBlock(block);
    await discardBlockBackToGenerator(block, g);
  }
  activeBlockId = originalActiveId;
  const after = JSON.stringify(blocksList);
  const finish = () => {
    rebuildFullFromEditedBlocks();
    renderBlockTabs();
    renderNoteEditor();
    renderTimeline();
    updateGroupLockUI();
    syncKnobsToBlock(findBlock(activeBlockId));
  };
  finish();
  pushUndo(`Discard all edits (${targets.length} block${targets.length>1?'s':''}, global)`,
    () => { blocksList = JSON.parse(before); finish(); },
    () => { blocksList = JSON.parse(after); finish(); }
  );
}

// Per‑block, per‑group seed for deterministic variation.
function getGroupSeed(block, groupId) {
  block.seeds = block.seeds || {};
  if (block.seeds[groupId] === undefined) block.seeds[groupId] = seededHash(`${block.id}|${groupId}|seed`);
  return block.seeds[groupId];
}

// =========================================
// PHRASE LENGTH
// =========================================
function getPhraseLength(block, groupId) {
  const stored = block.phraseLength && block.phraseLength[groupId];
  return Math.max(1, Math.min(block.length || 1, stored || 1));
}

// The phrase belongs to the instrument (UI group, e.g. 'rackTom1', 'cymbal3'); Kick, Snare, Hi-Hat, Ride and
// Custom instruments are their own UI group, so for them it is the group phrase. Toms and Cymbals are generated
// as ONE rhythm from the longest instrument phrase; every shorter instrument is then cut to its own loop
// (applyInstrumentPhrases).
function engineGroupMembers(engineGroup) {
  return Object.keys(UI_GROUP_TO_ENGINE_GROUP).filter(u => UI_GROUP_TO_ENGINE_GROUP[u] === engineGroup);
}

function getEnginePhrase(block, engineGroup) {
  const members = engineGroupMembers(engineGroup);
  if (members.length <= 1) return getPhraseLength(block, engineGroup);
  return Math.max(...members.map(u => getPhraseLength(block, u)));
}

function resolvePhraseLength(block, groupId, ov) {
  const raw = (ov && ov.phraseLength != null) ? ov.phraseLength : getEnginePhrase(block, groupId);
  const phrase = Math.max(1, Math.min(block.length || 1, Math.round(raw) || 1));
  block.phraseLength = block.phraseLength || {};
  block.phraseLength[groupId] = phrase;
  return phrase;
}

// Repeat the first `phrase` bars of one instrument's notes over the whole block (fill notes stay as they are).
function tileInstrumentPhrase(block, g, trackId, phrase) {
  const num = g.numerator;
  const len = Math.max(1, block.length || 1);
  const build = Math.max(0, block.velocityBuild || 0) * 20;
  const buildAt = (bar) => (len > 1 ? bar / (len - 1) : 0) * build;
  const notes = block.data[trackId] || [];
  const out = notes.filter(n => n.turnaround);
  const source = notes.filter(n => !n.turnaround && n.beat < phrase * num);
  for (let bar = 0; bar < len; bar++) {
    const srcBar = bar % phrase;
    source.filter(n => n.beat >= srcBar * num && n.beat < (srcBar + 1) * num).forEach(n => {
      const vel = Math.max(1, Math.min(127, Math.round(n.vel - buildAt(srcBar) + buildAt(bar))));
      out.push({ beat: n.beat - srcBar * num + bar * num, vel });
    });
  }
  block.data[trackId] = out.sort((a, b) => a.beat - b.beat);
}

function applyInstrumentPhrases(block, g, engineGroup) {
  const members = engineGroupMembers(engineGroup);
  if (members.length <= 1) return;
  const source = getEnginePhrase(block, engineGroup);
  members.forEach(u => {
    const phrase = getPhraseLength(block, u);
    if (phrase >= source) return;
    trackList.filter(t => t.group === u).forEach(t => tileInstrumentPhrase(block, g, t.id, phrase));
  });
}

// Complete track -> engine group mapping using UI_GROUP_TO_ENGINE_GROUP.
function engineGroupForTrack(tid) {
  const track = trackList.find(t => t.id === tid);
  return track ? (UI_GROUP_TO_ENGINE_GROUP[track.group] || null) : null;
}

function setGroupKnobState(block, group, values) {
  block.knobs = block.knobs || {};
  block.knobs[group] = { ...values };
}

// =========================================
// ARTICULATION TIERS
// =========================================
const KICK_ARTICULATION_TIERS  = [ {id:'kick', tier:0}, {id:'kickOpen', tier:1} ];
const SNARE_ARTICULATION_TIERS = [ {id:'snare', tier:0}, {id:'snareOffCenter', tier:1}, {id:'snareEdge', tier:1}, {id:'snareRimShot', tier:2}, {id:'rim', tier:2}, {id:'snareRimOnly', tier:3} ];
const HIHAT_ARTICULATION_TIERS = [ {id:'chh', tier:0}, {id:'hihatTightEdge', tier:1}, {id:'hihatClosedTip', tier:1}, {id:'hihatTightTip', tier:1}, {id:'ohh', tier:2}, {id:'hihatOpenEdge4', tier:3} ];
const RIDE_ARTICULATION_TIERS  = [ {id:'ride', tier:0}, {id:'rideBowShank', tier:1}, {id:'rideEdge', tier:1}, {id:'rideBellTip', tier:2}, {id:'rideBell', tier:2}, {id:'rideMuteHit', tier:3} ];

// Distribute notes across articulations based on variation percentage and enabled articulations.
function applyArticulationVariation(block, g, defaultTrackId, tiers, variationPct, groupSeed, phrase, noteFilter) {
  const bias = variationPct/100;
  const turnaroundVaryPerOccurrence = !!block.turnaroundVaryPerOccurrence;
  const turnaroundPhraseBeats = (block.turnaroundInterval || 1) * g.numerator;
  const allNotes = block.data[defaultTrackId] || [];
  const notes = noteFilter ? allNotes.filter(noteFilter) : allNotes;
  const untouched = noteFilter ? allNotes.filter(n => !noteFilter(n)) : [];
  const trackNotes = {};
  tiers.forEach(t => trackNotes[t.id] = []);
  const pickableTiers = tiers.filter(t => isArticulationEnabled(block, t.id));
  const effectiveTiers = pickableTiers.length ? pickableTiers : tiers;
  const choiceCache = {};
  const phraseLen = phrase > 1 ? phrase : 1;
  notes.forEach(n => {
    const turnaroundOccIdx = turnaroundVaryPerOccurrence ? Math.floor(n.beat / turnaroundPhraseBeats) : 0;
    const phraseBarIdx = phraseLen > 1 ? Math.floor(n.beat / g.numerator) % phraseLen : 0;
    const beatWithinBar = n.beat % g.numerator;
    const cacheKey = turnaroundOccIdx + '|' + phraseBarIdx + '|' + beatWithinBar.toFixed(4);
    if (!(cacheKey in choiceCache)) {
      const rng = new RNG(groupSeed + seededHash(cacheKey));
      choiceCache[cacheKey] = pickWeightedArticulation(rng, bias, effectiveTiers);
    }
    trackNotes[choiceCache[cacheKey]].push(n);
  });
  trackNotes[defaultTrackId] = trackNotes[defaultTrackId].concat(untouched);
  tiers.forEach(t => { block.data[t.id] = trackNotes[t.id].sort((a,b) => a.beat-b.beat); });
}

// =========================================
// GROUP GENERATORS
// =========================================
function applyKickGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.kick) return;
  const feel = block.feel || 'straight';
  const subdivision = ov ? ov.subdivision : readSubdivisionKnob('kickSubdivision');
  const densityPct = ov ? ov.densityPct : getParamNum('kickDensity');
  if (!ov) document.getElementById('kickDensityVal').textContent = densityPct + '%';
  const anchorBeats = ov ? ov.anchorBeats : null;
  const phrase = resolvePhraseLength(block, 'kick', ov, 'kickPhrase');
  const groupSeed = getGroupSeed(block, 'kick');
  const tripletBeats = generateTripletLayerBeats(block.tripletLayers && block.tripletLayers.kick, densityPct, engineGroupTripletSubdivision('kick'));
  const notes = [];
  for (let barIdx = 0; barIdx < phrase; barIdx++) {
    const barSeed = phrase > 1 ? groupSeed + barIdx * 9973 : groupSeed;
    const beats = generateAnchoredPattern(g.numerator, subdivision, Math.round((densityPct/100)*g.numerator*subdivision), feel, barSeed, anchorBeats).map(x => x.beat);
    beats.concat(tripletBeats).forEach(b => notes.push({ beat: b + barIdx * g.numerator, vel: 110 }));
  }
  block.data['kick'] = notes;
  replicateGroupAcrossLength(block, g, ['kick'], phrase);
  const variationPct = ov ? ov.variationPct : getParamNum('kickVariation');
  if (!ov) document.getElementById('kickVariationVal').textContent = variationPct + '%';
  applyArticulationVariation(block, g, 'kick', KICK_ARTICULATION_TIERS, variationPct, groupSeed+3, phrase);
  setGroupKnobState(block, 'kick', { subdivision, density: densityPct, variation: variationPct, phrase });
  block.generated = false;
}

function applyAllUnlockedGroups(block, g) {
  if (!block) return;
  block._turnaroundFillCache = {};
  const groupTracks = {
    kick: ['kick'], snare: ['snare'], hihat: ['chh'], ride: ['ride'],
    toms: TOMS_ARTICULATION_TIERS.map(t => t.id), cymbals: CYMBALS_ARTICULATION_TIERS.map(t => t.id),
  };
  Object.entries(groupTracks).forEach(([groupId, tracks]) => {
    if (block.lockedGroups && block.lockedGroups[groupId]) {
      // The plain notes were edited by hand: only the turnaround follows the Fill controls (unless it was edited as well).
      if (!neTurnaroundLocked(block, groupId)) regenerateTurnaroundOverlay(block, g, tracks);
      return;
    }
    replicateGroupAcrossLength(block, g, tracks, getEnginePhrase(block, groupId));
    applyInstrumentPhrases(block, g, groupId);
  });
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
}

// Regeneration wrapper (bridge + local). Returns a Promise.
function regenerateGroup(groupId, applyFn, label, opts, sendType, buildPayload, applyResponse) {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (isFillGeneratedBlock(block)) { neMsg('Break/Intro/Outro blocks: use Length/Fill controls in the editor, not these knobs.'); return; }

  if (opts && opts.deferAudioSync) {
    block._turnaroundFillCache = {};
    block.lockedGroups = block.lockedGroups || {};
    block.lockedGroups[groupId] = false;
    applyFn(block, readGrid());
    rebuildFullFromEditedBlocks(false);
    // A batch reroll (all groups, or all blocks) redraws once at the end instead of after every group.
    if (!opts.skipRender) {
      renderNoteEditor();
      updateGroupLockUI();
    }
    return;
  }

  const mutateAsync = async () => {
    block._turnaroundFillCache = {};
    block.lockedGroups = block.lockedGroups || {};
    block.lockedGroups[groupId] = false;
    const g = readGrid();
    const p = buildPayload(block, g);
    // A hand-edited turnaround of this group survives the reroll of its plain notes.
    const keptFills = {};
    if (neTurnaroundLocked(block, groupId)) tracksInEngineGroup(groupId).forEach(tid => { keptFills[tid] = (block.data[tid] || []).filter(n => n.turnaround); });
    const res = await _sendBreakFillRequest(sendType, block.id, { block, grid: g, ...p });
    if (res) {
      applyResponse(block, res);
      Object.keys(keptFills).forEach(tid => {
        block.data[tid] = (block.data[tid] || []).filter(n => !n.turnaround).concat(keptFills[tid]).sort((x, y) => x.beat - y.beat);
      });
    }
  };

  if (opts && opts.suppressUndo) {
    return mutateAsync().then(() => {
      rebuildFullFromEditedBlocks(true);
      renderNoteEditor();
      updateGroupLockUI();
    });
  }
  return withBlockDataUndoAsync(`Reroll ${label} (${blockDisplayName(block)})`, block, mutateAsync);
}

function regenerateKickGroup(opts) { return regenerateGroup('kick', applyKickGroupToBlock, 'Kick', opts, 'regenerateKickGroup', buildKickCommitPayload, applyKickCommitResponse); }

// =========================================
// CUSTOM INSTRUMENT GROUP
// One track per custom instrument, single articulation — so no variation knob,
// no tiers, no ghost layer. Group id is namespaced ('custom:<trackId>') and used
// for locks, seeds, knob state and phrase length exactly like the six engine groups.
// =========================================
function customGroupId(trackId) { return 'custom:' + trackId; }

// Which custom instrument the toolbar's Custom cluster controls. Set explicitly by
// the cluster's dropdown, or implicitly when a custom track's row is clicked in the
// grid. Independent of focusedTrackId (velocity-lane focus / Rnd+Nudge scope).
let activeCustomTrackId = null;

function activeCustomTrack() {
  if (!activeCustomTrackId) return null;
  const t = trackList.find(x => x.id === activeCustomTrackId);
  return (t && t.role === 'custom') ? t : null;
}

// Select the instrument the Custom cluster controls (dropdown change or grid row click).
function neSetActiveCustomTrack(trackId) {
  const valid = !!(trackId && trackList.some(t => t.id === trackId && t.role === 'custom'));
  activeCustomTrackId = valid ? trackId : null;
}

// Single-track pattern: like the Kick generator without variation tiers and triplet overlay.
function applyCustomGroupToBlock(block, g, trackId, ov) {
  if (!block) return;
  const groupId = customGroupId(trackId);
  if (block.lockedGroups && block.lockedGroups[groupId]) return;
  const feel = block.feel || 'straight';
  const subdivision = ov ? ov.subdivision : readSubdivisionKnob('customSubdivision');
  const densityPct = ov ? ov.densityPct : getParamNum('customDensity');
  if (!ov) document.getElementById('customDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, groupId, ov, 'customPhrase');
  const groupSeed = getGroupSeed(block, groupId);
  const totalSteps = Math.round(g.numerator * subdivision);
  const hits = Math.round((densityPct/100) * totalSteps);
  const notes = [];
  for (let barIdx = 0; barIdx < phrase; barIdx++) {
    const barSeed = phrase > 1 ? groupSeed + barIdx * 9973 : groupSeed;
    generateAnchoredPattern(g.numerator, subdivision, hits, feel, barSeed, [])
      .forEach(x => notes.push({ beat: x.beat + barIdx * g.numerator, vel: 110 }));
  }
  block.data[trackId] = notes.sort((a,b) => a.beat - b.beat);
  replicateGroupAcrossLength(block, g, [trackId], phrase);
  setGroupKnobState(block, groupId, { subdivision, density: densityPct, phrase });
  block.generated = false;
}

function buildCustomCommitPayload(block, trackId) {
  const subdivisionIndex = getParamNum('customSubdivision');
  const densityPct = getParamNum('customDensity');
  document.getElementById('customDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, customGroupId(trackId), null, 'customPhrase');
  setGroupKnobState(block, customGroupId(trackId), { subdivision: readSubdivisionKnob('customSubdivision'), density: densityPct, phrase });
  return { trackId, densityPct, subdivisionIndex, phrase };
}
function applyCustomCommitResponse(block, res) {
  if (res && res.trackId) block.data[res.trackId] = res.notes;
}

function regenerateCustomGroup(opts) {
  const track = activeCustomTrack();
  if (!track) return Promise.resolve();
  const label = track.label || 'Custom';
  const applyFn = (block, g) => applyCustomGroupToBlock(block, g, track.id);
  return regenerateGroup(customGroupId(track.id), applyFn, label, opts, 'regenerateCustomGroup',
    (block, g) => buildCustomCommitPayload(block, track.id), applyCustomCommitResponse);
}

function applySnareGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.snare) return;
  const feel = block.feel || 'straight';
  const subdivision = ov ? ov.subdivision : readSubdivisionKnob('snareSubdivision');
  const densityPct = ov ? ov.densityPct : getParamNum('snareDensity');
  if (!ov) document.getElementById('snareDensityVal').textContent = densityPct + '%';
  const totalSteps = Math.round(g.numerator * subdivision);
  const mainHits = Math.round((densityPct/100) * totalSteps);
  const seed = getGroupSeed(block, 'snare');
  const anchorBeats = ov ? ov.anchorBeats : null;
  const ghostDensityPct = ov ? ov.ghostDensityPct : getParamNum('snareGhostDensity');
  if (!ov) document.getElementById('snareGhostDensityVal').textContent = ghostDensityPct + '%';
  const phrase = resolvePhraseLength(block, 'snare', ov, 'snarePhrase');

  const notes = [];
  for (let barIdx = 0; barIdx < phrase; barIdx++) {
    const barSeed = phrase > 1 ? seed + barIdx * 9973 : seed;
    const anchoredResult = generateAnchoredPattern(g.numerator, subdivision, mainHits, feel, barSeed, anchorBeats);
    const beats = anchoredResult.map(x => x.beat);

    let ghostBeats;
    if (anchorBeats && anchorBeats.length) {
      const occupiedSteps = new Set(beats.map(b => Math.round(b*subdivision) % totalSteps));
      const emptySteps = [];
      for (let i = 0; i < totalSteps; i++) if (!occupiedSteps.has(i)) emptySteps.push(i);
      const ghostHits = Math.round((ghostDensityPct/100) * emptySteps.length);
      const ghostMask = euclideanRhythm(ghostHits, emptySteps.length, barSeed+1);
      ghostBeats = [];
      ghostMask.forEach((on, i) => {
        if (!on) return;
        const step = emptySteps[i];
        const beatIdx = Math.floor(step/subdivision);
        const fracWithinBeat = (step%subdivision)/subdivision;
        ghostBeats.push(beatIdx + applyFeelWarp(fracWithinBeat, feel));
      });
    } else {
      ghostBeats = generateSnareGhostBeats(g.numerator, subdivision, mainHits, ghostDensityPct, feel, barSeed);
    }

    beats.forEach(b => notes.push({ beat: b + barIdx * g.numerator, vel: 110 }));
    ghostBeats.forEach(b => notes.push({ beat: b + barIdx * g.numerator, vel: 45 }));
  }
  block.data['snare'] = notes.sort((a,b) => a.beat-b.beat);
  replicateGroupAcrossLength(block, g, ['snare'], phrase);

  const variationPct = ov ? ov.rimPct : getParamNum('snareRim');
  if (!ov) document.getElementById('snareRimVal').textContent = variationPct + '%';
  applyArticulationVariation(block, g, 'snare', SNARE_ARTICULATION_TIERS, variationPct, seed+2, phrase, n => n.vel > 60);
  setGroupKnobState(block, 'snare', { subdivision, density: densityPct, ghost: ghostDensityPct, variation: variationPct, phrase });
  block.generated = false;
}

function applySnareGhostLayer(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.snare) return;
  const feel = block.feel || 'straight';
  const knobs = (block.knobs && block.knobs.snare) || {};
  const subdivision = knobs.subdivision || readSubdivisionKnob('snareSubdivision');
  const totalSteps = Math.round(g.numerator * subdivision);
  const seed = getGroupSeed(block, 'snare');
  const num = g.numerator;
  const phrase = getPhraseLength(block, 'snare');

  const ghostDensityPct = ov ? ov.ghostDensityPct : getParamNum('snareGhostDensity');
  if (!ov) document.getElementById('snareGhostDensityVal').textContent = ghostDensityPct + '%';

  const notes = [];
  for (let barIdx = 0; barIdx < phrase; barIdx++) {
    const lo = barIdx * num, hi = lo + num;
    const barSeed = phrase > 1 ? seed + barIdx * 9973 : seed;
    const mainBeatsThisBar = [];
    SNARE_ARTICULATION_TIERS.forEach(t => {
      (block.data[t.id] || []).forEach(n => {
        if (n.turnaround || n.beat < lo || n.beat >= hi) return;
        if (t.id === 'snare' && n.vel <= 60) return;
        mainBeatsThisBar.push(n.beat - lo);
      });
    });

    const occupiedSteps = new Set(mainBeatsThisBar.map(b => Math.round(b*subdivision) % totalSteps));
    const emptySteps = [];
    for (let i = 0; i < totalSteps; i++) if (!occupiedSteps.has(i)) emptySteps.push(i);
    const ghostHits = Math.round((ghostDensityPct/100) * emptySteps.length);
    const ghostMask = euclideanRhythm(ghostHits, emptySteps.length, barSeed+1);
    ghostMask.forEach((on, i) => {
      if (!on) return;
      const step = emptySteps[i];
      const beatIdx = Math.floor(step/subdivision);
      const fracWithinBeat = (step%subdivision)/subdivision;
      const beat = beatIdx + applyFeelWarp(fracWithinBeat, feel);
      notes.push({ beat: beat + lo, vel: 45 });
    });

    (block.data['snare'] || []).filter(n => n.vel > 60 && !n.turnaround && n.beat >= lo && n.beat < hi)
      .forEach(n => notes.push(n));
  }
  block.data['snare'] = notes.sort((a,b) => a.beat-b.beat);

  replicateGroupAcrossLength(block, g, ['snare'], phrase);

  block.knobs = block.knobs || {};
  block.knobs.snare = { ...knobs, ghost: ghostDensityPct };
  block.generated = false;
}

// Shared pulse group engine.
function applyPulseGroupToBlock(block, g, groupId, primaryTrack, tiers, mainVel, subdivision, densityPct, variationPct, phrase, variationSeedOffset, knobField) {
  const feel = block.feel || 'straight';
  const seed = getGroupSeed(block, groupId);
  const notes = [];
  for (let barIdx = 0; barIdx < phrase; barIdx++) {
    const barSeed = phrase > 1 ? seed + barIdx * 9973 : seed;
    const beats = generateGroupPattern(g.numerator, subdivision, Math.round((densityPct/100)*g.numerator*subdivision), feel, barSeed);
    beats.forEach(b => notes.push({ beat: b + barIdx * g.numerator, vel: mainVel }));
  }
  block.data[primaryTrack] = notes;
  replicateGroupAcrossLength(block, g, [primaryTrack], phrase);
  applyArticulationVariation(block, g, primaryTrack, tiers, variationPct, seed + variationSeedOffset, phrase);
  applyInstrumentPhrases(block, g, groupId);
  setGroupKnobState(block, groupId, { subdivision, density: densityPct, [knobField]: variationPct, phrase });
  block.generated = false;
}

// Hi‑Hat, Ride, Toms, Cymbals wrappers.
function applyHihatGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.hihat) return;
  const subdivision = ov ? ov.subdivision : readSubdivisionKnob('hihatSubdivision');
  const densityPct = ov ? ov.densityPct : getParamNum('hihatDensity');
  if (!ov) document.getElementById('hihatDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, 'hihat', ov, 'hihatPhrase');
  const variationPct = ov ? ov.openPct : getParamNum('hihatOpen');
  if (!ov) document.getElementById('hihatOpenVal').textContent = variationPct + '%';
  applyPulseGroupToBlock(block, g, 'hihat', 'chh', HIHAT_ARTICULATION_TIERS, 100, subdivision, densityPct, variationPct, phrase, 2, 'open');
}

function regenerateHihatGroup(opts) { return regenerateGroup('hihat', applyHihatGroupToBlock, 'Hi-Hat', opts, 'regenerateHihatGroup', buildHihatCommitPayload, applyPulseCommitResponse); }

function applyRideGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.ride) return;
  const subdivision = ov ? ov.subdivision : readSubdivisionKnob('rideSubdivision');
  const densityPct = ov ? ov.densityPct : getParamNum('rideDensity');
  if (!ov) document.getElementById('rideDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, 'ride', ov, 'ridePhrase');
  const variationPct = ov ? ov.variationPct : getParamNum('rideVariation');
  if (!ov) document.getElementById('rideVariationVal').textContent = variationPct + '%';
  applyPulseGroupToBlock(block, g, 'ride', 'ride', RIDE_ARTICULATION_TIERS, 100, subdivision, densityPct, variationPct, phrase, 3, 'variation');
}

function regenerateRideGroup(opts) { return regenerateGroup('ride', applyRideGroupToBlock, 'Ride', opts, 'regenerateRideGroup', buildRideCommitPayload, applyPulseCommitResponse); }

function regenerateSnareGroup(opts) { return regenerateGroup('snare', applySnareGroupToBlock, 'Snare', opts, 'regenerateSnareGroup', buildSnareCommitPayload, applySnareCommitResponse); }

async function regenerateSnareGhost(opts) {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (isFillGeneratedBlock(block)) { neMsg('Break/Intro/Outro blocks: use Length/Fill controls in the editor, not these knobs.'); return; }

  if (opts && opts.deferAudioSync) {
    applySnareGhostLayer(block, readGrid());
    rebuildFullFromEditedBlocks(false);
    renderNoteEditor();
    updateGroupLockUI();
    return;
  }

  const mutateAsync = async () => {
    const g = readGrid();
    const knobs = (block.knobs && block.knobs.snare) || {};
    const idxFromStored = knobs.subdivision != null ? SUBDIVISION_KNOB_VALUES.indexOf(knobs.subdivision) : -1;
    const subdivisionIndex = idxFromStored >= 0 ? idxFromStored : getParamNum('snareSubdivision');
    const phrase = getPhraseLength(block, 'snare'); // Ghost never changes the phrase itself, just reads it
    const ghostDensityPct = getParamNum('snareGhostDensity');
    document.getElementById('snareGhostDensityVal').textContent = ghostDensityPct + '%';
    const res = await _sendBreakFillRequest('regenerateSnareGhostLayer', block.id, { block, grid: g, ghostDensityPct, subdivisionIndex, phrase });
    if (res) block.data['snare'] = res.snare;
    block.knobs = block.knobs || {};
    block.knobs.snare = { ...knobs, ghost: ghostDensityPct };
  };

  if (opts && opts.suppressUndo) {
    await mutateAsync();
    rebuildFullFromEditedBlocks(true);
    renderNoteEditor();
    updateGroupLockUI();
  } else {
    await withBlockDataUndoAsync(`Adjust Snare Ghost (${blockDisplayName(block)})`, block, mutateAsync);
  }
}

const TOMS_ARTICULATION_TIERS = [
  { id: 'rackTom2Center', tier: 0 },
  { id: 'tomHi', tier: 1 }, { id: 'floorTom1Center', tier: 1 },
  { id: 'tomMid', tier: 2 },
  { id: 'tomLow', tier: 3 },
];

function applyTomsGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.toms) return;
  const subdivision = (ov && ov.subdivision != null) ? ov.subdivision : readSubdivisionKnob('tomsSubdivision');
  const densityPct = ov ? (ov.densityPct ?? ov.freqPct) : getParamNum('tomsDensity');
  if (!ov) document.getElementById('tomsDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, 'toms', ov, 'tomsPhrase');
  const variationPct = ov ? (ov.variationPct ?? ov.complexityPct) : getParamNum('tomsVariation');
  if (!ov) document.getElementById('tomsVariationVal').textContent = variationPct + '%';
  applyPulseGroupToBlock(block, g, 'toms', 'rackTom2Center', TOMS_ARTICULATION_TIERS, 100, subdivision, densityPct, variationPct, phrase, 3, 'variation');
}

function regenerateTomsGroup(opts) {
  return regenerateGroup('toms', applyTomsGroupToBlock, 'Toms', opts, 'regenerateTomsGroup', buildTomsCommitPayload,
    (b, res) => { applyPulseCommitResponse(b, res); applyInstrumentPhrases(b, readGrid(), 'toms'); });
}

const CYMBALS_ARTICULATION_TIERS = [
  { id: 'crash1', tier: 0 },
  { id: 'cymbal1Crash', tier: 1 }, { id: 'cymbal3Crash', tier: 1 },
  { id: 'crash2', tier: 2 },
  { id: 'cymbal5Crash', tier: 3 },
];

function applyCymbalsGroupToBlock(block, g, ov) {
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups.cymbals) return;
  const subdivision = (ov && ov.subdivision != null) ? ov.subdivision : readSubdivisionKnob('cymbalsSubdivision');
  const densityPct = ov ? (ov.densityPct ?? ov.freqPct) : getParamNum('cymbalsDensity');
  if (!ov) document.getElementById('cymbalsDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, 'cymbals', ov, 'cymbalsPhrase');
  const variationPct = ov ? (ov.variationPct ?? 0) : getParamNum('cymbalsVariation');
  if (!ov) document.getElementById('cymbalsVariationVal').textContent = variationPct + '%';
  applyPulseGroupToBlock(block, g, 'cymbals', 'crash1', CYMBALS_ARTICULATION_TIERS, 110, subdivision, densityPct, variationPct, phrase, 3, 'variation');
}

function regenerateCymbalsGroup(opts) {
  return regenerateGroup('cymbals', applyCymbalsGroupToBlock, 'Cymbals', opts, 'regenerateCymbalsGroup', buildCymbalsCommitPayload,
    (b, res) => { applyPulseCommitResponse(b, res); applyInstrumentPhrases(b, readGrid(), 'cymbals'); });
}

// =========================================
// SONG GENERATOR PRESETS
// =========================================
const GENRE_SECTION_PRESETS = {
  straight: {
    verse:  { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:100, hihatOpen:0,  rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    chorus: { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:0,   hihatOpen:0,  rideDensity:100, rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    bridge: { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:100, hihatOpen:30, rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
  },
  three_four: {
    verse:  { subdivision:2, kickDensity:17, kickVariation:0, kickAnchors:[0], snareDensity:17, snareGhost:0, snareRim:0, snareAnchors:[2], hihatDensity:100, hihatOpen:0,  rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    chorus: { subdivision:2, kickDensity:17, kickVariation:0, kickAnchors:[0], snareDensity:17, snareGhost:0, snareRim:0, snareAnchors:[2], hihatDensity:0,   hihatOpen:0,  rideDensity:100, rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    bridge: { subdivision:2, kickDensity:17, kickVariation:0, kickAnchors:[0], snareDensity:17, snareGhost:0, snareRim:0, snareAnchors:[2], hihatDensity:100, hihatOpen:30, rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
  },
  swing: {
    verse:  { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:100, hihatOpen:0,  rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    chorus: { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:0,   hihatOpen:0,  rideDensity:100, rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
    bridge: { subdivision:2, kickDensity:25, kickVariation:0, kickAnchors:[0,2], snareDensity:25, snareGhost:0, snareRim:0, snareAnchors:[1,3], hihatDensity:100, hihatOpen:30, rideDensity:0,   rideVariation:0, tomsDensity:0, tomsVariation:15, cymbalsDensity:0, cymbalsVariation:15 },
  },
};

function currentSectionPresets() {
  const gp = GENRE_SECTION_PRESETS[selectedGenre] || GENRE_SECTION_PRESETS.straight;
  return {
    verse: gp.verse, chorus: gp.chorus, bridge: gp.bridge,
  };
}

const SECTION_LABELS = { intro:'Intro', verse:'Verse', chorus:'Chorus', bridge:'Bridge', outro:'Outro', fill:'Fill', verseBreak:'Verse Break', chorusBreak:'Chorus Break', bridgeBreak:'Bridge Break' };

// Preset payload builders for the bridge.
function presetSubdivisionIndex(subdivisionValue) {
  const idx = SUBDIVISION_KNOB_VALUES.indexOf(subdivisionValue);
  return idx >= 0 ? idx : 1;
}
function buildKickPresetPayload(preset) {
  return { densityPct: preset.kickDensity, variationPct: preset.kickVariation || 0,
           subdivisionIndex: presetSubdivisionIndex(preset.subdivision), phrase: 1,
           anchorBeats: preset.kickAnchors || null };
}
function buildSnarePresetPayload(preset) {
  return { densityPct: preset.snareDensity, ghostDensityPct: preset.snareGhost,
           variationPct: preset.snareRim, subdivisionIndex: presetSubdivisionIndex(preset.subdivision),
           phrase: 1, anchorBeats: preset.snareAnchors || null };
}
function buildPulsePresetPayload(preset, densityKey, variationKey) {
  return { densityPct: preset[densityKey], variationPct: preset[variationKey] || 0,
           subdivisionIndex: presetSubdivisionIndex(preset.subdivision), phrase: 1 };
}

// Generate a typed block (section) using presets and C++ bridge.
async function generateTypedBlock(type, g, id, opts) {
  if (type === 'fill') {
    const data = buildFill(new RNG(Date.now() + Math.random()*1000), {}, g);
    return { id, name: SECTION_LABELS.fill, generated: true, numerator: g.numerator, data };
  }
  const preset = currentSectionPresets()[type];
  if (!preset) return null;
  const block = { id, name: SECTION_LABELS[type] || type, generated: true, numerator: g.numerator, data: emptyBlockData() };
  if (opts) { block.length = opts.length; block.turnaroundInterval = opts.turnaroundInterval; block.velocityBuild = opts.velocityBuild; }

  const [kickRes, snareRes, hihatRes, rideRes, tomsRes, cymbalsRes] = await Promise.all([
    _sendBreakFillRequest('regenerateKickGroup', block.id, { block, grid: g, ...buildKickPresetPayload(preset) }),
    _sendBreakFillRequest('regenerateSnareGroup', block.id, { block, grid: g, ...buildSnarePresetPayload(preset) }),
    _sendBreakFillRequest('regenerateHihatGroup', block.id, { block, grid: g, ...buildPulsePresetPayload(preset, 'hihatDensity', 'hihatOpen') }),
    _sendBreakFillRequest('regenerateRideGroup', block.id, { block, grid: g, ...buildPulsePresetPayload(preset, 'rideDensity', 'rideVariation') }),
    _sendBreakFillRequest('regenerateTomsGroup', block.id, { block, grid: g, ...buildPulsePresetPayload(preset, 'tomsDensity', 'tomsVariation') }),
    _sendBreakFillRequest('regenerateCymbalsGroup', block.id, { block, grid: g, ...buildPulsePresetPayload(preset, 'cymbalsDensity', 'cymbalsVariation') }),
  ]);
  if (kickRes) applyKickCommitResponse(block, kickRes);
  if (snareRes) applySnareCommitResponse(block, snareRes);
  if (hihatRes) applyPulseCommitResponse(block, hihatRes);
  if (rideRes) applyPulseCommitResponse(block, rideRes);
  if (tomsRes) applyPulseCommitResponse(block, tomsRes);
  if (cymbalsRes) applyPulseCommitResponse(block, cymbalsRes);
  return block;
}

// =========================================
// BREAK AND FILL GENERATION
// =========================================
const BREAK_HAND_PATTERNS = {
  toms:      [ { id: 'tomHi', count: 4 }, { id: 'rackTom2Center', count: 2 }, { id: 'tomMid', count: 2 } ],
  snareToms: [ { id: 'snare', count: 4 }, { id: 'tomHi', count: 2 }, { id: 'rackTom2Center', count: 2 } ],
};

function generateFixedBreakHandPattern(breakBeats, stepsPerBeat, patternSpec, anchor, feel, beatOffset) {
  feel = feel || 'straight';
  beatOffset = beatOffset || 0;
  const effectiveSpec = feel === 'halftime'
    ? patternSpec.map(p => ({ id: p.id, count: Math.max(1, Math.round(p.count / 2)) }))
    : patternSpec;
  const totalSteps = Math.max(1, Math.round(breakBeats * stepsPerBeat));
  const patternSteps = effectiveSpec.reduce((n, p) => n + p.count, 0);
  const startStep = anchor === 'start' ? 0 : Math.max(0, totalSteps - patternSteps);
  const dist = {};
  let step = startStep;
  effectiveSpec.forEach(p => {
    for (let i = 0; i < p.count && step < totalSteps; i++, step++) {
      const warpedAbs = warpAbsoluteBeat(beatOffset + step / stepsPerBeat, feel);
      (dist[p.id] = dist[p.id] || []).push(warpedAbs - beatOffset);
    }
  });
  return dist;
}

// The fixed hand pattern only plays the instruments that are checked: snare needs "snare", every tom needs "toms".
function filterHandPatternByInstruments(dist, instruments) {
  const out = {};
  Object.keys(dist).forEach(tid => {
    if (instruments.includes(tid === 'snare' ? 'snare' : 'toms')) out[tid] = dist[tid];
  });
  return out;
}

function genreBaselineFeel() {
  return (typeof selectedGenre !== 'undefined' && selectedGenre === 'swing') ? 'swing' : 'straight';
}

function generateFillBlock(g, id, name, totalLength, fillBeats, anchor, opts) {
  opts = opts || {};
  const num = g.numerator;
  const totalBeats = totalLength * num;
  fillBeats = Math.max(0.5, Math.min(totalBeats, fillBeats));
  const fillOffset = anchor === 'start' ? 0 : Math.max(0, totalBeats - fillBeats);
  const data = {};
  trackList.forEach(t => { data[t.id] = []; });

  const feel = opts.feel || genreBaselineFeel();
  const fillInstruments = (opts.fillInstruments || ['toms']).filter(i => i !== 'kick');
  const fillDensity = opts.fillDensity ?? 100;
  const fillVel = opts.fillVel ?? 112;
  const fillSubdivision = opts.fillSubdivision ?? 4;
  const fillSeed = opts.fillSeed;
  const fillDist = (fillSeed === undefined)
    ? filterHandPatternByInstruments(generateFixedBreakHandPattern(fillBeats, fillSubdivision, fillInstruments.includes('snare') ? BREAK_HAND_PATTERNS.snareToms : BREAK_HAND_PATTERNS.toms, anchor, feel, fillOffset), fillInstruments)
    : generateBreakFill(fillBeats, fillInstruments, fillDensity, fillSeed, fillSubdivision, feel, fillOffset);
  Object.keys(fillDist).forEach(tid => {
    if (!data[tid]) data[tid] = [];
    fillDist[tid].forEach(beat => data[tid].push({ beat: beat + fillOffset, vel: fillVel }));
  });

  const fillKickDensity = opts.fillKickDensity ?? 100;
  const fillKickSubdivision = opts.fillKickSubdivision ?? 1;
  const fillKickSeed = opts.fillKickSeed ?? 0;
  const kickSteps = Math.max(1, Math.round(fillBeats * fillKickSubdivision));
  const rawFillKickHits = Math.max(1, Math.round((fillKickDensity/100) * kickSteps));
  const fillKickHits = feel === 'halftime' ? Math.max(1, Math.round(rawFillKickHits/2)) : rawFillKickHits;
  const kickMask = buildEndAnchoredEuclidean(fillKickHits, kickSteps, fillKickSeed);
  if (!data.kick) data.kick = [];
  kickMask.forEach((on, i) => { if (on) data.kick.push({ beat: warpAbsoluteBeat(fillOffset + i/fillKickSubdivision, feel), vel: 115 }); });
  data.kick.sort((a,b) => a.beat-b.beat);

  return { id, name, generated:true, numerator:num, length: totalLength, fillBeats, fillAnchor: anchor, fillInstruments, fillDensity, fillVel, fillSubdivision, fillKickDensity, fillKickSubdivision, fillKickSeed: opts.fillKickSeed, fillSeed, feel, isFillBlock: true, turnaroundInterval:0, velocityBuild:0, data };
}

function generateBreakFill(breakBeats, instruments, densityPct, seed, stepsPerBeat, feel, beatOffset) {
  feel = feel || 'straight';
  beatOffset = beatOffset || 0;
  stepsPerBeat = stepsPerBeat ?? 4;
  const totalSteps = Math.max(2, Math.round(breakBeats * stepsPerBeat));
  const voicePool = [];
  (instruments || []).forEach(instKey => (TURNAROUND_INSTRUMENT_TRACKS[instKey] || []).forEach(t => voicePool.push(t)));
  const rawHits = Math.max(1, Math.round((densityPct/100) * totalSteps));
  const hits = feel === 'halftime' ? Math.max(1, Math.round(rawHits/2)) : rawHits;
  const mask = euclideanRhythm(hits, totalSteps, seed);
  const dist = {};
  if (voicePool.length) {
    const rng = new RNG(seed+1);
    const uniqueVoices = [...new Set(voicePool)];
    const shuffledVoices = uniqueVoices.map(v => ({ v, r: rng.next() })).sort((a,b) => a.r-b.r).map(x => x.v);
    let vi = 0;
    mask.forEach((on, i) => {
      if (!on) return;
      const voice = vi < shuffledVoices.length ? shuffledVoices[vi] : voicePool[rng.int(0, voicePool.length-1)];
      vi++;
      const warpedAbs = warpAbsoluteBeat(beatOffset + i/stepsPerBeat, feel);
      (dist[voice] = dist[voice] || []).push(warpedAbs - beatOffset);
    });
  }
  return dist;
}

function generateBreakBlock(parentType, g, id, totalLength, breakBeats, opts) {
  opts = opts || {};
  const parent = findBlock(parentType);
  if (!parent) return null;
  const num = g.numerator;
  const parentLen = Math.max(1, parent.length || 1);
  const totalBeats = totalLength * num;
  const grooveBeats = Math.max(0, totalBeats - breakBeats);
  const data = {};
  Object.keys(parent.data).forEach(tid => { data[tid] = []; });
  const barsNeeded = Math.ceil(totalBeats / num);
  for (let barIdx = 0; barIdx < barsNeeded; barIdx++) {
    const srcBar = barIdx % parentLen;
    Object.keys(parent.data).forEach(tid => {
      if (!data[tid]) data[tid] = [];
      (parent.data[tid]||[]).forEach(n => {
        const localBeat = n.beat - srcBar*num;
        if (localBeat < -0.0001 || localBeat >= num) return;
        const absBeat = localBeat + barIdx*num;
        if (absBeat < grooveBeats - 0.0001) data[tid].push({ beat: absBeat, vel: n.vel });
      });
    });
  }
  const feel = parent.feel || 'straight';
  const breakInstruments = (opts.breakInstruments || ['toms']).filter(i => i !== 'kick');
  const breakDensity = opts.breakDensity ?? 100;
  const breakFillVel = opts.breakFillVel ?? 112;
  const breakSubdivision = opts.breakSubdivision ?? 4;
  const breakSeed = opts.breakSeed; // stays undefined until Reroll is clicked — see getBreakSeed
  const fillDist = (breakSeed === undefined)
    ? filterHandPatternByInstruments(generateFixedBreakHandPattern(breakBeats, breakSubdivision, breakInstruments.includes('snare') ? BREAK_HAND_PATTERNS.snareToms : BREAK_HAND_PATTERNS.toms, undefined, feel, grooveBeats), breakInstruments)
    : generateBreakFill(breakBeats, breakInstruments, breakDensity, breakSeed, breakSubdivision, feel, grooveBeats);
  Object.keys(fillDist).forEach(tid => {
    if (!data[tid]) data[tid] = [];
    fillDist[tid].forEach(beat => data[tid].push({ beat: beat + grooveBeats, vel: breakFillVel }));
  });
  const breakKickDensity = opts.breakKickDensity ?? 100;
  const breakKickSubdivision = opts.breakKickSubdivision ?? 1;
  const breakKickSeed = opts.breakKickSeed ?? 0; // own seed — undefined-until-reroll resolves to a fixed, stable arrangement (see getBreakKickSeed)
  const kickSteps = Math.max(1, Math.round(breakBeats * breakKickSubdivision));
  const rawKickHits = Math.max(1, Math.round((breakKickDensity/100) * kickSteps));
  const kickHits = feel === 'halftime' ? Math.max(1, Math.round(rawKickHits/2)) : rawKickHits;
  const kickMask = buildEndAnchoredEuclidean(kickHits, kickSteps, breakKickSeed);
  if (!data.kick) data.kick = [];
  kickMask.forEach((on, i) => { if (on) data.kick.push({ beat: warpAbsoluteBeat(grooveBeats + i/breakKickSubdivision, feel), vel: 115 }); });
  data.kick.sort((a,b) => a.beat-b.beat);
  return { id, name: SECTION_LABELS[parentType+'Break'] || (parentType+'Break'), generated:true, numerator:num, length: totalLength, breakBeats, breakInstruments, breakDensity, breakFillVel, breakSubdivision, breakKickDensity, breakKickSubdivision, breakKickSeed: opts.breakKickSeed, breakSeed, breakParentId: parentType, turnaroundInterval:0, velocityBuild:0, data };
}

function breakFillTargetTracks() {
  return new Set([
    ...Object.values(TURNAROUND_INSTRUMENT_TRACKS).flat(),
    ...BREAK_HAND_PATTERNS.toms.map(t => t.id),
    ...BREAK_HAND_PATTERNS.snareToms.map(t => t.id),
  ]);
}

function computeFreshBreak(block, g) {
  const parentType = block.breakParentId;
  if (!parentType) return null;
  const totalLength = Math.max(1, block.length || 1);
  const totalBeats = totalLength * g.numerator;
  const breakBeats = Math.max(0.5, Math.min(totalBeats - 0.5, block.breakBeats ?? BREAK_DEFAULT_BEATS));
  return generateBreakBlock(parentType, g, block.id, totalLength, breakBeats, {
    breakInstruments: block.breakInstruments,
    breakDensity: block.breakDensity,
    breakFillVel: block.breakFillVel,
    breakSubdivision: block.breakSubdivision,
    breakKickDensity: block.breakKickDensity,
    breakKickSubdivision: block.breakKickSubdivision,
    breakKickSeed: getBreakKickSeed(block),
    breakSeed: getBreakSeed(block),
  });
}

function regenerateBreakBlockInPlace(block, g, opts) {
  const fresh = computeFreshBreak(block, g);
  if (!fresh) return false;
  const grooveBeats = Math.max(0, fresh.length * g.numerator - fresh.breakBeats);
  const forceAll = !!(opts && opts.forceAll);
  const mainGroups = neRelevantEngineGroups(block.breakInstruments || ['toms']);
  const mainLocked = !forceAll && mainGroups.some(gid => block.lockedGroups && block.lockedGroups[gid]);
  const kickLocked = !forceAll && !!(block.lockedGroups && block.lockedGroups.kick);
  if (forceAll) {
    block.lockedGroups = block.lockedGroups || {};
    mainGroups.forEach(gid => { block.lockedGroups[gid] = false; });
    block.lockedGroups.kick = false;
  }
  const mainTracks = breakFillTargetTracks();
  const allTracks = new Set([...Object.keys(block.data || {}), ...Object.keys(fresh.data || {})]);
  allTracks.forEach(tid => {
    const isKick = tid === 'kick';
    const isMain = !isKick && mainTracks.has(tid);
    const tailLocked = isKick ? kickLocked : (isMain ? mainLocked : false);
    const freshBefore = (fresh.data[tid] || []).filter(n => n.beat < grooveBeats - 0.0001);
    const tailSource = tailLocked
      ? (block.data[tid] || []).filter(n => n.beat >= grooveBeats - 0.0001)
      : (fresh.data[tid] || []).filter(n => n.beat >= grooveBeats - 0.0001);
    block.data[tid] = freshBefore.concat(tailSource).sort((a, b) => a.beat - b.beat);
  });
  block.length = fresh.length;
  block.breakBeats = fresh.breakBeats;
  return true;
}

// =========================================
// C++ BRIDGE GLUE FOR BREAK/FILL
// =========================================

const _breakFillQueues = new Map();

function _breakFillQueueKey(sendType, blockId) { return sendType + '::' + blockId; }

function _sendBreakFillRequest(sendType, blockId, payload) {
  return new Promise(resolve => {
    const key = _breakFillQueueKey(sendType, blockId);
    if (!_breakFillQueues.has(key)) _breakFillQueues.set(key, []);
    _breakFillQueues.get(key).push(resolve);
    hostBridge.send(sendType, payload);
  });
}

const BREAK_FILL_RESPONSE_TYPES = {
  regenerateBreakBlock: ['breakBlockRegenerated', 'breakBlockRegenerateFailed'],
  regenerateBreakMain: ['breakMainRegenerated', 'breakMainRegenerateFailed'],
  regenerateBreakKick: ['breakKickRegenerated', 'breakKickRegenerateFailed'],
  regenerateFillBlock: ['fillBlockRegenerated', 'fillBlockRegenerateFailed'],
  regenerateKickGroup: ['kickGroupRegenerated', 'kickGroupRegenerateFailed'],
  regenerateSnareGroup: ['snareGroupRegenerated', 'snareGroupRegenerateFailed'],
  regenerateSnareGhostLayer: ['snareGhostLayerRegenerated', 'snareGhostLayerRegenerateFailed'],
  regenerateHihatGroup: ['hihatGroupRegenerated', 'hihatGroupRegenerateFailed'],
  regenerateRideGroup: ['rideGroupRegenerated', 'rideGroupRegenerateFailed'],
  regenerateTomsGroup: ['tomsGroupRegenerated', 'tomsGroupRegenerateFailed'],
  regenerateCymbalsGroup: ['cymbalsGroupRegenerated', 'cymbalsGroupRegenerateFailed'],
  regenerateCustomGroup: ['customGroupRegenerated', 'customGroupRegenerateFailed'],
};
Object.entries(BREAK_FILL_RESPONSE_TYPES).forEach(([sendType, [successType, failType]]) => {
  const handle = (msg, ok) => {
    const queue = _breakFillQueues.get(_breakFillQueueKey(sendType, msg.blockId));
    if (!queue || !queue.length) return;
    queue.shift()(ok ? msg : null);
  };
  hostBridge.on(successType, msg => handle(msg, true));
  hostBridge.on(failType, msg => handle(msg, false));
});

function requestRegenerateBreakBlock(block, g, opts) {
  const forceAll = !!(opts && opts.forceAll);
  const mainGroups = neRelevantEngineGroups(block.breakInstruments || ['toms']);
  const mainLocked = !forceAll && mainGroups.some(gid => block.lockedGroups && block.lockedGroups[gid]);
  const kickLocked = !forceAll && !!(block.lockedGroups && block.lockedGroups.kick);
  if (forceAll) {
    block.lockedGroups = block.lockedGroups || {};
    mainGroups.forEach(gid => { block.lockedGroups[gid] = false; });
    block.lockedGroups.kick = false;
  }
  const parent = findBlock(block.breakParentId);
  if (!parent) return Promise.resolve(null);
  return _sendBreakFillRequest('regenerateBreakBlock', block.id, { block, parent, grid: g, mainLocked, kickLocked });
}

function requestRegenerateBreakMain(block, g) {
  const parent = findBlock(block.breakParentId);
  if (!parent) return Promise.resolve(null);
  return _sendBreakFillRequest('regenerateBreakMain', block.id, { block, parent, grid: g });
}

function requestRegenerateBreakKick(block, g) {
  const parent = findBlock(block.breakParentId);
  if (!parent) return Promise.resolve(null);
  return _sendBreakFillRequest('regenerateBreakKick', block.id, { block, parent, grid: g });
}

function requestRegenerateFillBlock(block, g) {
  return _sendBreakFillRequest('regenerateFillBlock', block.id, { block, grid: g });
}

// Commit payload builders for the six engine groups.
function buildKickCommitPayload(block) {
  const subdivisionIndex = getParamNum('kickSubdivision');
  const densityPct = getParamNum('kickDensity');
  document.getElementById('kickDensityVal').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, 'kick', null, 'kickPhrase');
  const variationPct = getParamNum('kickVariation');
  document.getElementById('kickVariationVal').textContent = variationPct + '%';
  setGroupKnobState(block, 'kick', { subdivision: readSubdivisionKnob('kickSubdivision'), density: densityPct, variation: variationPct, phrase });
  return { densityPct, variationPct, subdivisionIndex, phrase };
}
function applyKickCommitResponse(block, res) {
  block.data['kick'] = res.kick;
  block.data['kickOpen'] = res.kickOpen;
}

function buildSnareCommitPayload(block) {
  const subdivisionIndex = getParamNum('snareSubdivision');
  const densityPct = getParamNum('snareDensity');
  document.getElementById('snareDensityVal').textContent = densityPct + '%';
  const ghostDensityPct = getParamNum('snareGhostDensity');
  document.getElementById('snareGhostDensityVal').textContent = ghostDensityPct + '%';
  const phrase = resolvePhraseLength(block, 'snare', null, 'snarePhrase');
  const variationPct = getParamNum('snareRim');
  document.getElementById('snareRimVal').textContent = variationPct + '%';
  setGroupKnobState(block, 'snare', { subdivision: readSubdivisionKnob('snareSubdivision'), density: densityPct, ghost: ghostDensityPct, variation: variationPct, phrase });
  return { densityPct, ghostDensityPct, variationPct, subdivisionIndex, phrase };
}
function applySnareCommitResponse(block, res) {
  Object.assign(block.data, res.tracks);
}

function buildPulseCommitPayload(block, groupId, subdivisionElId, densityElId, variationElId, phraseElId, knobField) {
  const subdivisionIndex = getParamNum(subdivisionElId);
  const densityPct = getParamNum(densityElId);
  document.getElementById(densityElId + 'Val').textContent = densityPct + '%';
  const phrase = resolvePhraseLength(block, groupId, null, phraseElId);
  const variationPct = getParamNum(variationElId);
  document.getElementById(variationElId + 'Val').textContent = variationPct + '%';
  setGroupKnobState(block, groupId, { subdivision: readSubdivisionKnob(subdivisionElId), density: densityPct, [knobField]: variationPct, phrase });
  return { densityPct, variationPct, subdivisionIndex, phrase };
}
function applyPulseCommitResponse(block, res) {
  Object.assign(block.data, res.tracks);
}
function buildHihatCommitPayload(block) { return buildPulseCommitPayload(block, 'hihat', 'hihatSubdivision', 'hihatDensity', 'hihatOpen', 'hihatPhrase', 'open'); }
function buildRideCommitPayload(block) { return buildPulseCommitPayload(block, 'ride', 'rideSubdivision', 'rideDensity', 'rideVariation', 'ridePhrase', 'variation'); }
function buildTomsCommitPayload(block) { return buildPulseCommitPayload(block, 'toms', 'tomsSubdivision', 'tomsDensity', 'tomsVariation', 'tomsPhrase', 'variation'); }
function buildCymbalsCommitPayload(block) { return buildPulseCommitPayload(block, 'cymbals', 'cymbalsSubdivision', 'cymbalsDensity', 'cymbalsVariation', 'cymbalsPhrase', 'variation'); }

function isBreakMainLocked(block) {
  const groups = neRelevantEngineGroups(block.breakInstruments || ['toms']);
  return groups.some(gid => block.lockedGroups && block.lockedGroups[gid]);
}
function isBreakKickLocked(block) {
  return !!(block.lockedGroups && block.lockedGroups.kick);
}

async function rerollBreakBlock(block, g) {
  const mainLocked = isBreakMainLocked(block);
  const kickLocked = isBreakKickLocked(block);
  if (mainLocked && kickLocked) { neMsg('Fill and Kick are both locked — discard via 🔒 in the editor first.'); return; }
  await withBlockDataUndoAsync(`Reroll Break (${blockDisplayName(block)})`, block, async () => {
    if (!mainLocked) block.breakSeed = Math.floor(Math.random()*1e9);
    if (!kickLocked) block.breakKickSeed = Math.floor(Math.random()*1e9);
    const [mainRes, kickRes] = await Promise.all([
      mainLocked ? Promise.resolve(null) : requestRegenerateBreakMain(block, g),
      kickLocked ? Promise.resolve(null) : requestRegenerateBreakKick(block, g),
    ]);
    if (mainRes) Object.assign(block.data, mainRes.tracks);
    if (kickRes) block.data.kick = kickRes.kick;
    block.generated = false;
  });
}

async function rerollFillBlock(block, g) {
  await withBlockDataUndoAsync(`Reroll ${blockDisplayName(block)}`, block, async () => {
    block.fillSeed = Math.floor(Math.random()*1e9);
    block.fillKickSeed = Math.floor(Math.random()*1e9);
    const res = await requestRegenerateFillBlock(block, g);
    if (!res) return;
    block.data = res.data;
    block.length = res.length;
    block.fillBeats = res.fillBeats;
    block.feel = res.feel;
    block.generated = false;
  });
}

function rerollActiveBlock() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  const g = readGrid();
  if (isBreakBlock(block)) { rerollBreakBlock(block, g); return; }
  if (block.isFillBlock) { rerollFillBlock(block, g); return; }
  const unlockedGroups = ALL_ENGINE_GROUPS.filter(gid => !(block.lockedGroups && block.lockedGroups[gid]));
  if (!unlockedGroups.length) { neMsg('Everything in this block is locked — discard an edit first.'); return; }
  withBlockDataUndo(`Reroll All (${blockDisplayName(block)})`, block, () => {
    unlockedGroups.forEach(gid => {
      block.seeds = block.seeds || {};
      block.seeds[gid] = Math.floor(Math.random()*1e9);
      REROLL_FNS[gid]({ suppressUndo: true, deferAudioSync: true, skipRender: true });
    });
  });
}

async function rerollAllBlocksGlobal() {
  const targets = blocksList.filter(b => !isFillGeneratedBlock(b));
  if (!targets.length) {
    neMsg('No regular blocks to reroll (Break/Fill blocks are skipped — reroll those individually)');
    return;
  }
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Reroll every block?',
      message: `This rerolls all unlocked instrument groups across ${targets.length} block${targets.length>1?'s':''} project-wide (${targets.map(blockDisplayName).join(', ')}). Locked groups, and any Break/Fill blocks, are left untouched.`,
      confirmLabel: 'Reroll all',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  stopPlayback();
  const originalActiveId = activeBlockId;
  const before = JSON.stringify(blocksList);
  targets.forEach(block => {
    activeBlockId = block.id;
    syncKnobsToBlock(block);
    ALL_ENGINE_GROUPS.filter(gid => !(block.lockedGroups && block.lockedGroups[gid])).forEach(gid => {
      block.seeds = block.seeds || {};
      block.seeds[gid] = Math.floor(Math.random()*1e9);
      REROLL_FNS[gid]({ suppressUndo: true, deferAudioSync: true, skipRender: true });
    });
  });
  activeBlockId = originalActiveId;
  const after = JSON.stringify(blocksList);
  const finish = () => {
    rebuildFullFromEditedBlocks();
    renderBlockTabs();
    renderNoteEditor();
    renderTimeline();
    updateGroupLockUI();
    syncKnobsToBlock(findBlock(activeBlockId));
  };
  finish();
  pushUndo(`Reroll all blocks (${targets.length}, global)`,
    () => { blocksList = JSON.parse(before); finish(); },
    () => { blocksList = JSON.parse(after); finish(); }
  );
}

function syncBreakLengthsToParent(parent, g) {
  const targets = blocksList.filter(b => b.breakParentId === parent.id && (b.length || 1) !== (parent.length || 1));
  if (!targets.length) return Promise.resolve();
  const jobs = targets.map(breakBlock => {
    if (breakBlock.breakUnlocked) {
      resizeBlockLength(breakBlock, g, parent.length);
      return Promise.resolve();
    }
    breakBlock.length = parent.length;
    return requestRegenerateBreakBlock(breakBlock, g).then(res => {
      if (!res) return;
      breakBlock.data = res.data;
      breakBlock.length = res.length;
      breakBlock.breakBeats = res.breakBeats;
    });
  });
  return Promise.all(jobs).then(() => {
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  });
}

// =========================================
// BLOCK LENGTH AND TURNAROUND
// =========================================
const BLOCK_MAX_LENGTH = 4096;
const BLOCK_DEFAULT_LENGTH = { intro:1, verse:8, chorus:8, bridge:8, outro:1, fill:8, verseBreak:8, chorusBreak:8, bridgeBreak:8 };
const BREAK_DEFAULT_BEATS = 2;
const DEFAULT_BREAK_PARENT = { verseBreak:'verse', chorusBreak:'chorus', bridgeBreak:'bridge' };

const TURNAROUND_INSTRUMENT_TRACKS = { toms: ['tomHi','rackTom2Center','tomMid'], snare: ['snare'], hihat: ['chh'], floorTom: ['tomLow'], crash: ['crash1'] }; // hihat: Break/Intro/Outro fills only; the turnaround's hi-hat is the "Has Hi-Hat" setting

function getTurnaroundSeed(block) {
  return block.turnaroundSeed;
}
function getBreakSeed(block) {
  return block.breakSeed;
}
function getBreakKickSeed(block) {
  return block.breakKickSeed;
}

function buildEndAnchoredEuclidean(hits, steps, seed) {
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
  const lastHit = raw.lastIndexOf(true);
  const shift = steps - 1 - lastHit;
  const mask = new Array(steps);
  for (let i = 0; i < steps; i++) mask[(i + shift + steps) % steps] = raw[i];
  return seed ? seededShuffleMask(mask, seed) : mask;
}

function resolveTurnaroundLengthBeats(block, num) {
  return Math.max(0.25, Math.min(num, block.turnaroundLengthBeats ?? 1));
}

function getTurnaroundBarDistribution(block, barIdx, num, seed) {
  block._turnaroundFillCache = block._turnaroundFillCache || {};
  const cacheKey = barIdx;
  if (block._turnaroundFillCache[cacheKey]) return block._turnaroundFillCache[cacheKey];
  const instruments = block.turnaroundInstruments || ['snare'];
  const voicePool = [];
  instruments.forEach(instKey => (TURNAROUND_INSTRUMENT_TRACKS[instKey] || []).forEach(t => voicePool.push(t)));
  const dist = {};
  if (voicePool.length) {
    const feel = block.feel || 'straight';
    const lengthBeats = resolveTurnaroundLengthBeats(block, num);
    const densityPct = block.turnaroundDensity ?? 50;
    // The fill needs at least two steps inside its window: a window shorter than two steps of the chosen Subdiv gets a finer grid,
    // so Fill Length is what it says.
    const stepsPerBeat = Math.max(block.turnaroundSubdivision ?? 4, Math.ceil(2 / lengthBeats - 1e-9));
    const totalSteps = num * stepsPerBeat;
    const zoneSteps = Math.max(2, Math.round(lengthBeats * stepsPerBeat));
    const rawHits = Math.max(1, Math.round((densityPct/100) * zoneSteps));
    const hits = feel === 'halftime' ? Math.max(1, Math.round(rawHits/2)) : rawHits;
    const mask = buildEndAnchoredEuclidean(hits, zoneSteps, seed);
    const slotBeats = [];
    mask.forEach((on, i) => {
      if (!on) return;
      const step = totalSteps - zoneSteps + i;
      const beatIdx = Math.floor(step / stepsPerBeat);
      const fracWithinBeat = (step % stepsPerBeat) / stepsPerBeat;
      slotBeats.push(beatIdx + applyFeelWarp(fracWithinBeat, feel));
    });
    const rng = new RNG(seed+1);
    const uniqueVoices = [...new Set(voicePool)];
    const shuffledVoices = uniqueVoices.map(v => ({ v, r: rng.next() })).sort((a,b) => a.r-b.r).map(x => x.v);
    slotBeats.forEach((beat, i) => {
      const voice = i < shuffledVoices.length ? shuffledVoices[i] : voicePool[rng.int(0, voicePool.length-1)];
      (dist[voice] = dist[voice] || []).push(beat);
    });
  }
  block._turnaroundFillCache[cacheKey] = dist;
  return dist;
}

// A turnaround bar is every Nth bar (N = block.turnaroundInterval); its fill sits in the last `Fill Length` beats of the bar.
function neIsTurnaroundBar(block, barIdx) {
  const interval = block.turnaroundInterval || 0;
  return interval > 0 && ((barIdx + 1) % interval === 0);
}

// Is this note part of the turnaround: a generated fill note, or any note in the fill window of a turnaround bar?
function neNoteInTurnaroundArea(block, note, num) {
  if (note.turnaround) return true;
  if (!((block.turnaroundInterval || 0) > 0)) return false;
  const barIdx = Math.floor(note.beat / num + 1e-9);
  if (!neIsTurnaroundBar(block, barIdx)) return false;
  return note.beat - barIdx * num >= num - resolveTurnaroundLengthBeats(block, num) - 1e-6;
}

// The generated turnaround notes of one track for one bar (fill notes, Fill Kick, closing crash), absolute beats. `sourceBar` are the
// plain notes of that bar (bar-relative beats): fill notes never land on one of them.
function neTurnaroundNotesForBar(block, tid, barIdx, len, num, sourceBar) {
  const out = [];
  if (!neIsTurnaroundBar(block, barIdx)) return out;
  const userSeed = getTurnaroundSeed(block);
  const varyPerBar = !!block.turnaroundVaryPerOccurrence;
  const distSeed = userSeed !== undefined ? seededHash(`${block.id}|${varyPerBar ? barIdx : 0}|turnaroundfill|${userSeed}`) : 0;
  const dist = getTurnaroundBarDistribution(block, barIdx, num, distSeed);
  const fillVel = block.turnaroundFillVel ?? 112;
  (dist[tid] || []).forEach(b => { if (!hasNoteNear(sourceBar, b)) out.push({ beat: b + barIdx * num, vel: fillVel, turnaround: true }); });
  if (tid === 'kick') {
    const kickDensity = block.turnaroundKickDensity ?? 100;
    const kickSeed = block.turnaroundKickSeed ?? 0;
    const lengthBeats = resolveTurnaroundLengthBeats(block, num);
    const kickSubdivision = Math.max(block.turnaroundKickSubdivision ?? 1, Math.ceil(2 / lengthBeats - 1e-9)); // same rule as the fill grid
    const kickTotalSteps = num * kickSubdivision;
    const kickZoneSteps = Math.max(2, Math.round(lengthBeats * kickSubdivision));
    const kickHits = Math.max(1, Math.round((kickDensity/100) * kickZoneSteps));
    const kickMask = buildEndAnchoredEuclidean(kickHits, kickZoneSteps, kickSeed);
    kickMask.forEach((on, i) => {
      if (!on) return;
      const beat = (kickTotalSteps - kickZoneSteps + i) / kickSubdivision;
      if (!hasNoteNear(sourceBar, beat)) out.push({ beat: beat + barIdx * num, vel: 115, turnaround: true });
    });
  }
  if ((block.turnaroundCrash !== false) && tid === 'crash1' && !hasNoteNear(sourceBar, 0)) {
    const targetBarIdx = (barIdx + 1 < len) ? barIdx + 1 : 0;
    out.push({ beat: targetBarIdx * num, vel: 115, turnaround: true });
  }
  return out;
}

// The turnaround of an engine group has its own lock (key 't:<group>' in block.lockedGroups): set when the user edits a note in the
// turnaround area, independent of the group's own lock (edits elsewhere in the block).
function neTurnaroundLockKey(groupId) { return 't:' + groupId; }
function neTurnaroundLocked(block, groupId) { return !!(block.lockedGroups && block.lockedGroups[neTurnaroundLockKey(groupId)]); }

function replicateGroupAcrossLength(block, g, trackIds, phraseLen) {
  const len = Math.max(1, block.length || 1);
  const num = g.numerator;
  const phrase = Math.max(1, Math.min(len, Math.round(phraseLen) || 1));
  const velocityBuild = block.velocityBuild || 0;
  trackIds.forEach(tid => {
    // A hand-edited turnaround stays as it is: its notes are kept and no new fill is generated for this track.
    const groupId = engineGroupForTrack(tid);
    const keepFills = !!(groupId && neTurnaroundLocked(block, groupId));
    const keptFills = keepFills ? (block.data[tid] || []).filter(n => n.turnaround) : [];
    const phraseSource = (block.data[tid] || []).filter(n => n.beat < phrase * num && !n.turnaround);
    // "Has Hi-Hat" off: the hi-hat is silent in the fill window.
    const silenceWindow = groupId === 'hihat' && block.turnaroundHiHat === false;
    const windowStart = num - resolveTurnaroundLengthBeats(block, num) - 1e-6;
    const all = [];
    for (let barIdx = 0; barIdx < len; barIdx++) {
      const sourceBarIdx = barIdx % phrase;
      const srcLo = sourceBarIdx * num;
      const sourceBar = phraseSource
        .filter(n => n.beat >= srcLo && n.beat < srcLo + num)
        .map(n => ({ beat: n.beat - srcLo, vel: n.vel }));
      const buildFrac = len > 1 ? barIdx / (len - 1) : 0;
      const barSilent = silenceWindow && neIsTurnaroundBar(block, barIdx);
      sourceBar.forEach(n => {
        if (barSilent && n.beat >= windowStart) return;
        let beat = n.beat, vel = n.vel;
        if (velocityBuild > 0) vel = Math.max(1, Math.min(127, Math.round(vel + buildFrac * 20 * velocityBuild)));
        all.push({ beat: beat + barIdx * num, vel });
      });
      if (!keepFills) neTurnaroundNotesForBar(block, tid, barIdx, len, num, sourceBar).forEach(n => all.push(n));
    }
    keptFills.forEach(n => all.push(n));
    block.data[tid] = all.sort((a, b) => a.beat - b.beat);
  });
}

// Lays out only the turnaround of the given tracks again, leaving every other note as it is: for a group whose plain notes were
// edited by hand (locked) while its turnaround is still driven by the Fill controls.
function regenerateTurnaroundOverlay(block, g, trackIds) {
  const len = Math.max(1, block.length || 1);
  const num = g.numerator;
  trackIds.forEach(tid => {
    const base = (block.data[tid] || []).filter(n => !n.turnaround);
    const byBar = new Map();
    base.forEach(n => { const b = Math.floor(n.beat / num + 1e-9); (byBar.get(b) || byBar.set(b, []).get(b)).push(n); });
    const fills = [];
    for (let barIdx = 0; barIdx < len; barIdx++) {
      if (!neIsTurnaroundBar(block, barIdx)) continue;
      const sourceBar = (byBar.get(barIdx) || []).map(n => ({ beat: n.beat - barIdx * num, vel: n.vel }));
      neTurnaroundNotesForBar(block, tid, barIdx, len, num, sourceBar).forEach(n => fills.push(n));
    }
    block.data[tid] = base.concat(fills).sort((a, b) => a.beat - b.beat);
  });
}

function resizeBlockLength(block, g, newLength) {
  const num = g.numerator;
  const oldLen = Math.max(1, block.length || 1);
  const len = Math.max(1, Math.min(BLOCK_MAX_LENGTH, Math.round(newLength)));
  if (len === oldLen) { block.length = len; return; }
  Object.keys(block.data).forEach(tid => {
    const notes = block.data[tid] || [];
    if (len < oldLen) {
      block.data[tid] = notes.filter(n => n.beat < len * num);
    } else {
      const groupId = engineGroupForTrack(tid);
      const isLocked = !!(groupId && block.lockedGroups && block.lockedGroups[groupId]);
      if (isLocked) {
        block.data[tid] = notes;
        return;
      }
      const uiGroup = (trackList.find(x => x.id === tid) || {}).group;
      const stored = block.phraseLength && ((uiGroup && block.phraseLength[uiGroup]) || block.phraseLength[groupId]);
      const phrase = groupId ? Math.max(1, Math.min(oldLen, stored || 1)) : 1;
      const phraseSource = notes.filter(n => n.beat < phrase * num && !n.turnaround);
      const extra = [];
      for (let b = oldLen; b < len; b++) {
        const srcLo = (b % phrase) * num;
        phraseSource.filter(n => n.beat >= srcLo && n.beat < srcLo + num)
          .forEach(n => extra.push({ beat: (n.beat - srcLo) + b * num, vel: n.vel }));
      }
      block.data[tid] = notes.concat(extra).sort((a, b2) => a.beat - b2.beat);
    }
  });
  block.length = len;
}

// =========================================
// SONG STRUCTURE GENERATION
// =========================================
const STRUCTURE_CODES = {
  '12_bar_basic':  'AAAABBABBB',
  aaa:             'AAA',
  aaba_short:      'AABA',
  aaba_long:       'AABAAABA',
  abab_short:      'ABAB',
  abab_long:       'ABABABAB',
  abcabc_short:    'ABCABC',
  abcabc_long:     'ABCABCABCABC',
  chorus_first:    'BABCB',
  standard_pop:    'ABABCB',
  extended_pop:    'ABABCBB',
};

const LETTER_EXPANSION = {
  A: ['verse','verse','verse','verseBreak'],
  B: ['chorus','chorus','chorus','chorusBreak'],
  C: ['bridge','bridge','bridge','bridgeBreak'],
};

function generateSong(structureKey) {
  const code = STRUCTURE_CODES[structureKey];
  if (!code) return;
  const savedScrollY = window.scrollY;
  const ids = ['intro', ...[...code].flatMap(letter => LETTER_EXPANSION[letter] || []), 'outro'];
  withSongSequenceUndo('Generate song', () => {
    songEngine.songSequence = []; songEngine.currentSection = null;
    ids.forEach(id => { if (findBlock(id)) songEngine.addSlot(id); });
  });
  activeBlockId = 'verse';
  renderBlockTabs(); renderNoteEditor();
  fitTimelineZoom();
  rebuildFullFromEditedBlocks();
  renderStats(readParams(), readGrid());
  const label = document.getElementById('tlSongTypeLabel');
  if (label) label.textContent = '· ' + document.querySelector(`#generateSongMenu [data-structure="${structureKey}"]`)?.textContent;
  requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
}

async function clearSongSequence() {
  if (!songEngine.songSequence.length) { neMsg('Timeline is already empty'); return; }
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Clear the whole timeline?',
      message: `This removes all ${songEngine.songSequence.length} clips from the timeline. Blocks themselves are untouched — rebuild with Generate Song or by dragging blocks back in.`,
      confirmLabel: 'Clear all',
      danger: true,
      checkbox: { label: "Don't ask again (can be re-enabled in Note Editor settings)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  tlSetSelection([]);
  withSongSequenceUndo('Clear song', () => {
    songEngine.songSequence = [];
  });
  fitTimelineZoom();
  const label = document.getElementById('tlSongTypeLabel');
  if (label) label.textContent = '';
}

async function consolidateSong() {
  if (!songEngine.songSequence.length) { neMsg('Nothing to consolidate'); return; }
  const sorted = [...songEngine.songSequence].sort((a, b) => a.startBar - b.startBar);
  let cursor = 0;
  const alreadyTight = sorted.every(slot => {
    const tight = slot.startBar === cursor;
    cursor += songEngine.slotSpanBars(slot);
    return tight;
  });
  if (alreadyTight) { neMsg('Already consolidated — no gaps to close'); return; }
  tlSetSelection([]);
  withSongSequenceUndo('Consolidate song', () => {
    let bar = 0;
    songEngine.songSequence = sorted.map(slot => {
      const newSlot = { ...slot, startBar: bar };
      bar += songEngine.slotSpanBars(slot);
      return newSlot;
    });
  });
  fitTimelineZoom();
}
