// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// Drum Engine tab — MIDI mapping test keyboard
// =========================================
// Sends real MIDI note-on/off to the native sound engine, not to the browser preview.
// NATIVE_ENGINE_MAP: one entry per kit instance of the instrument table (bridge.js INSTRUMENT_TABLE), in the order of the table, filled by
// deApplyInstrumentTable(). `family` (colour family) and `implemented` are UI extras for color coding.
const NATIVE_ENGINE_MAP = [];

const DE_LEGEND_ITEMS = [
  { family: 'kick',         label: 'Kick' },
  { family: 'snare',        label: 'Snare' },
  { family: 'hihat-closed', label: 'Hi-Hat Closed' },
  { family: 'hihat-open',   label: 'Hi-Hat Open' },
  { family: 'tom',          label: 'Tom' },
  { family: 'cymbal',       label: 'Cymbal' },
  { family: 'ride',         label: 'Ride' },
];

// Color coding: hue identifies family, lightness varies per articulation.
const DE_FAMILY_HSL = {
  kick:           { h: 4,   s: 70 },
  snare:          { h: 32,  s: 68 },
  'hihat-closed': { h: 48,  s: 72 },
  'hihat-open':   { h: 48,  s: 32 },
  tom:            { h: 130, s: 45 },
  cymbal:         { h: 205, s: 62 },
  ride:           { h: 275, s: 48 },
};
const DE_WHITE_L_MIN = 55, DE_WHITE_L_MAX = 84;
const DE_BLACK_L_MIN = 20, DE_BLACK_L_MAX = 42;
const DE_LEGEND_L = 68;
const DE_UNIMPLEMENTED_HSL = { h: 220, s: 6 };

function deAssignFamilyIndices() {
  const totals = {};
  NATIVE_ENGINE_MAP.forEach(e => { totals[e.family] = (totals[e.family] || 0) + 1; });
  const seen = {};
  NATIVE_ENGINE_MAP.forEach(e => {
    e.artIndex = seen[e.family] || 0;
    seen[e.family] = e.artIndex + 1;
    e.familyTotal = totals[e.family];
  });
}

// =========================================
// Instrument table (bridge.js INSTRUMENT_TABLE, host: dsp/808InstrumentTable.h)
// =========================================
// Rows of a family in paramIndex order, and the views the cards are built from: identity, GM note, knob schema and defaults of an
// instance come from its row.
function deTableRows(family) {
  return INSTRUMENT_TABLE.instruments.filter((r) => r.family === family).sort((a, b) => a.paramIndex - b.paramIndex);
}

function deTableRow(family, index) {
  return INSTRUMENT_TABLE ? INSTRUMENT_TABLE.instruments.find((r) => r.family === family && r.paramIndex === index) : undefined;
}

function deInstanceDefaults(family, index) {
  const row = deTableRow(family, index);
  return row ? row.defaults : undefined;
}

// Knob groups of a row in the order of its controls: consecutive controls of the same group title form one group.
function deGroupsOfRow(row) {
  const groups = [];
  for (const c of row.controls) {
    let g = groups[groups.length - 1];
    if (!g || g.title !== c.group) { g = { title: c.group, params: [] }; groups.push(g); }
    g.params.push({ key: c.key, label: c.label, name: c.label, tooltip: c.tooltip, min: c.min, max: c.max, step: c.step });
  }
  return groups;
}

// A card of an instance of the table. Instances of the Rim Shot circuit have no morph pad.
function deInstanceOfRow(row) {
  return { index: row.paramIndex, note: row.gm, name: row.name, groups: deGroupsOfRow(row), defaults: row.defaults, pad: row.engine.kind !== 'RmRim' };
}

let KICK_INSTANCES = [];
let SNARE_INSTANCES = [];
let TOM_INSTANCES = [];
let deInstrumentTableApplied = false;

// Puts the table's rows into the structures built from static data before: the keyboard map and the instance lists.
function deApplyInstrumentTable() {
  if (deInstrumentTableApplied || !INSTRUMENT_TABLE) return;
  deInstrumentTableApplied = true;
  for (const r of INSTRUMENT_TABLE.instruments)
    if (r.gm !== null) NATIVE_ENGINE_MAP.push({ note: r.gm, name: r.name, family: r.uiFamily, implemented: true });
  deAssignFamilyIndices();
  KICK_INSTANCES = deTableRows('kick').map(deInstanceOfRow);
  DE_INSTANCED_FAMILIES.kick.instances = KICK_INSTANCES;
  SNARE_INSTANCES = deTableRows('snare').map(deInstanceOfRow);
  DE_INSTANCED_FAMILIES.snare.instances = SNARE_INSTANCES;
  TOM_INSTANCES = deTableRows('tom').map(deInstanceOfRow);
}

function deArticulationColor(entry, black) {
  if (entry.implemented === false) {
    const l = black ? (DE_BLACK_L_MIN + DE_BLACK_L_MAX) / 2 : (DE_WHITE_L_MIN + DE_WHITE_L_MAX) / 2;
    return `hsl(${DE_UNIMPLEMENTED_HSL.h}, ${DE_UNIMPLEMENTED_HSL.s}%, ${l}%)`;
  }
  const base = DE_FAMILY_HSL[entry.family] || DE_UNIMPLEMENTED_HSL;
  const t = entry.familyTotal > 1 ? entry.artIndex / (entry.familyTotal - 1) : 0.5;
  const lMin = black ? DE_BLACK_L_MIN : DE_WHITE_L_MIN;
  const lMax = black ? DE_BLACK_L_MAX : DE_WHITE_L_MAX;
  const l = lMin + (lMax - lMin) * t;
  return `hsl(${base.h}, ${base.s}%, ${l.toFixed(1)}%)`;
}

function deFamilyLegendColor(family) {
  const base = DE_FAMILY_HSL[family] || DE_UNIMPLEMENTED_HSL;
  return `hsl(${base.h}, ${base.s}%, ${DE_LEGEND_L}%)`;
}

const DE_BLACK_KEY_RATIO = 16 / 26;
const DE_BLACK_PITCH_CLASSES = new Set([1, 3, 6, 8, 10]);

function deIsBlackNote(note) {
  return DE_BLACK_PITCH_CLASSES.has(((note % 12) + 12) % 12);
}

// Sends a MIDI note event to the native engine via the bridge.
function deSendMidi(type, note) {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  // A custom instrument is previewed by its registry index (no track needed): the host plays it on its own MIDI preview channel.
  if (note !== null && typeof note === 'object') {
    if (type === 'midiNoteOn') hostBridge.send('customPreview', { index: note.custom });
    return;
  }
  hostBridge.send(type, { note });
}

// =========================================
// Sound parameter sub-tabs (Kick/Snare/Hi-Hat/Tom/Cymbal/Ride)
// =========================================
// Each schema entry defines a control. Defaults are taken from the
// respective dsp/*.h headers or per‑instrument defaults files.
// family/param/index map directly to the 'setVoiceParam' bridge message.

// HiHat: the circuit model determines tone and decay itself. The real hardware exposes exactly three pots
// (VR7 OH LEVEL, VR8 CH LEVEL, VR3 OH DECAY -- Closed has no decay knob on the real front panel either) plus the
// same Metal-Bank Pitch as Ride/Cymbal (digital transpose, no schematic component).
const HIHAT_PAD_AXIS = {
  level:       { axis: 'x', type: 'linear', spread: 0.5 },
  decayKnob01: { axis: 'y', type: 'linear', spread: 0.5, invert: true },
};


const HIHAT_PAD_LOG_PARAMS = new Set(
  Object.keys(HIHAT_PAD_AXIS).filter((k) => HIHAT_PAD_AXIS[k].type === 'log')
);

const deHiHatPadCornersCache = new Map();

function deHiHatInstanceDefaults(index) {
  return deInstanceDefaults('hihat', index);
}

// x=Tone/y=Decay, same schema as RIDE_PAD_AXIS below. Pitch is a Metal-Bank transpose knob, not a pad parameter.
const CYMBAL_PAD_AXIS = {
  toneKnob:    { axis: 'x', type: 'linear', spread: 0.5 },
  decayKnob01: { axis: 'y', type: 'linear', spread: 0.5, invert: true },
};
const CYMBAL_PAD_LOG_PARAMS = new Set(
  Object.keys(CYMBAL_PAD_AXIS).filter((k) => CYMBAL_PAD_AXIS[k].type === 'log')
);
const deCymbalPadCornersCache = new Map();

const RIDE_PAD_AXIS = {
  toneKnob:    { axis: 'x', type: 'linear', spread: 0.5 },
  decayKnob01: { axis: 'y', type: 'linear', spread: 0.5, invert: true },
};
const RIDE_PAD_LOG_PARAMS = new Set(
  Object.keys(RIDE_PAD_AXIS).filter((k) => RIDE_PAD_AXIS[k].type === 'log')
);
const deRidePadCornersCache = new Map();

function deCymbalInstanceDefaults(index) { return deInstanceDefaults('cymbal', index); }
function deRideInstanceDefaults(index)   { return deInstanceDefaults('ride', index); }

const DE_PARAM_SCHEMAS = {
  kick: {
    knobs: true,   // the knob groups and defaults of every instance come from the instrument table
  },
  snare: {
    knobs: true,   // the knob groups and defaults of every instance come from the instrument table
  },
  hihat: {
    articulationLevels: true,
    knobs: true,
    padAxis: HIHAT_PAD_AXIS,
    padLogParams: HIHAT_PAD_LOG_PARAMS,
    padCornersCache: deHiHatPadCornersCache,
    instanceDefaults: deHiHatInstanceDefaults,
    padTooltipCount: '4',   // the knob groups and defaults of every instance come from the instrument table
  },
  tom: {
    knobs: true,   // the knob groups and defaults of every instance come from the instrument table
  },
  cymbal: {
    articulationLevels: true,
    knobs: true,
    padAxis: CYMBAL_PAD_AXIS,
    padLogParams: CYMBAL_PAD_LOG_PARAMS,
    padCornersCache: deCymbalPadCornersCache,
    instanceDefaults: deCymbalInstanceDefaults,
    padTooltipCount: '4',
    // the knob groups and defaults of every instance come from the instrument table
  },
  ride: {
    articulationLevels: true,
    knobs: true,
    padAxis: RIDE_PAD_AXIS,
    padLogParams: RIDE_PAD_LOG_PARAMS,
    padCornersCache: deRidePadCornersCache,
    instanceDefaults: deRideInstanceDefaults,
    padTooltipCount: '4',   // the knob groups and defaults of every instance come from the instrument table
  },
};

// Caches for voice parameters and pad positions from the host.
let deVoiceParamStateCache = null;
let deVoiceParamStateConfigured = false;
let deInstanceSyncCallbacks = {};
let dePadPositionsCache = {};
let dePadResyncCallbacks = {};
let deUndoBaseline = null;   // sound state as of the last undo step or host load (see deInitUndo)

function deVoiceParamStateFor(family, index, key) {
  if (!deVoiceParamStateCache) return undefined;
  const idx = (index === undefined || index === null) ? -1 : index;
  const own = deVoiceParamStateCache[family + ':' + idx];
  if (own && key in own) return own[key];
  const fallback = deVoiceParamStateCache[family + ':0'];
  return fallback ? fallback[key] : undefined;
}

function deApplyVoiceParamStateToDom() {
  if (!deVoiceParamStateCache) return;
  deKickIsWritingSliders = true;
  deSuppressSend = true;
  document.querySelectorAll('[data-family][data-param-key]').forEach((el) => {
    const value = deVoiceParamStateFor(el.dataset.family, Number(el.dataset.index), el.dataset.paramKey);
    if (value === undefined) return;
    const input = el.slider || el;
    input.value = String(value);
    input.dispatchEvent(new Event('input', { bubbles: true }));
  });
  deKickIsWritingSliders = false;
  deSuppressSend = false;
}

if (typeof hostBridge !== 'undefined' && hostBridge.on) {
  hostBridge.on('voiceParamState', (payload) => {
    deVoiceParamStateCache = payload || {};
    deVoiceParamStateConfigured = !!(payload && payload.__configured);
    deApplyVoiceParamStateToDom();
    for (const key in dePadResyncCallbacks) dePadResyncCallbacks[key]();
    for (const key in deInstanceSyncCallbacks) deInstanceSyncCallbacks[key]();
    deUndoRebase();   // a host load is not an edit
  });
  hostBridge.on('padPositionsState', (payload) => {
    dePadPositionsCache = payload || {};
    for (const key in dePadResyncCallbacks) {
      const cb = dePadResyncCallbacks[key];
      if (typeof cb === 'function') cb();
    }
    deUndoRebase();
  });
}

function deSendVoiceParam(family, param, value, index) {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  const payload = { family, param, value };
  if (index !== undefined) payload.index = index;
  hostBridge.send('setVoiceParam', payload);
}

// Fired once a knob's drag ends (not on every 'input' tick during the
// drag) -- for Ride/HiHat/Crash this is the signal that starts a lazy
// background bake of fresh render-cache variants for the instrument's
// new parameter combination (see DrumGroover.cpp HandleCommitVoiceParam/
// EnqueueVariantBake).
function deSendVoiceParamCommit(family, param, value, index) {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  const payload = { family, param, value };
  if (index !== undefined) payload.index = index;
  hostBridge.send('commitVoiceParam', payload);
}

// =========================================
// Per-articulation volume knobs
// =========================================
// Keyed by MIDI note. Gain is applied at trigger time in ProcessMidiMsg.
function deSendArticulationLevel(note, value) {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  hostBridge.send('setArticulationLevel', { note, value });
}

// Default Articulations- / Custom-Instrument-Vol (unity gain = no change).
const DE_LEVEL_DEFAULT = 1.0;
let deArticulationGainCache = {};

function deApplyArticulationGain(note, value) {
  deArticulationGainCache[note] = value;
  const dial = document.querySelector(`.de-artic-vol-knob[data-note="${note}"]`);
  if (dial && dial.slider) {
    deKickIsWritingSliders = true;
    dial.slider.value = String(value);
    dial.slider.dispatchEvent(new Event('input'));
    deKickIsWritingSliders = false;
  }
}

if (typeof hostBridge !== 'undefined' && hostBridge.on) {
  hostBridge.on('articulationLevelState', (payload) => {
    if (!payload) return;
    for (const noteStr in payload) {
      deApplyArticulationGain(Number(noteStr), payload[noteStr]);
    }
    deUndoRebase();
  });
}

function deArticulationsForFamily(family) {
  if (family === 'hihat') {
    return NATIVE_ENGINE_MAP.filter(e => e.family === 'hihat-closed' || e.family === 'hihat-open');
  }
  return NATIVE_ENGINE_MAP.filter(e => e.family === family);
}

// The four closed Hi-Hat articulations (Closed Edge, Tight Edge, Closed Tip, Tight Tip) have no pad (their table
// rows only have Level and Pitch), so their knobs do not carry the blue "pad-linked" ring.
function deIsHiHatClosedNote(note) {
  if (note == null) return false;
  const entry = NATIVE_ENGINE_MAP.find((e) => e.note === note);
  return !!entry && entry.family === 'hihat-closed';
}

// The default of a custom instrument's Volume knob is the "volume" of its row in the instrument table (1.0 for every other instrument).
function deVolumeDefaultOf(inst) {
  return (inst && inst.defaults && typeof inst.defaults.volume === 'number') ? inst.defaults.volume : DE_LEVEL_DEFAULT;
}

function deBuildArticulationLevelKnob(note, name, defVal) {
  const defaultVolume = (typeof defVal === 'number') ? defVal : DE_LEVEL_DEFAULT;
  const dial = neCreateDial({
    label: 'Volume',
    min: 0, max: 3, step: 0.01,
    value: (note in deArticulationGainCache) ? deArticulationGainCache[note] : defaultVolume,
    def: defaultVolume,
    format: deFormatKnobVal({ step: 0.01 }),
    title: 'Volume\nSets the volume of this articulation.',
    onInput: (v) => { deArticulationGainCache[note] = v; deSendArticulationLevel(note, v); },
  });
  dial.dataset.note = String(note);
  dial.classList.add('de-artic-vol-knob');
  deWireKnobPreview(dial.slider, note);
  return dial;
}

// Volume of a custom-instrument card whose instrument is not assigned to a track (no GM note yet): scales the preview of the card; once a
// track is assigned the knob is the note gain again and takes over this value.
const deCustomEngineGainCache = {};
function deBuildCustomEngineLevelKnob(inst) {
  const dial = neCreateDial({
    label: 'Volume',
    min: 0, max: 3, step: 0.01,
    value: (inst.customId in deCustomEngineGainCache) ? deCustomEngineGainCache[inst.customId] : deVolumeDefaultOf(inst),
    def: deVolumeDefaultOf(inst),
    format: deFormatKnobVal({ step: 0.01 }),
    title: 'Volume\nSets the volume of this instrument.',
    onInput: (v) => {
      deCustomEngineGainCache[inst.customId] = v;
      if (typeof hostBridge !== 'undefined' && hostBridge.send) hostBridge.send('setCustomPreviewGain', { index: inst.index, value: v });
    },
  });
  dial.classList.add('de-artic-vol-knob');
  dial.dataset.customId = inst.customId;
  deWireKnobPreview(dial.slider, inst.previewNote);
  return dial;
}

function deBuildParamRow(family, def, index) {
  const row = document.createElement('div');
  row.className = 'de-param-instance-row';

  const label = document.createElement('div');
  label.className = 'lbl';
  label.textContent = def.label;

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = String(def.min);
  slider.max = String(def.max);
  slider.step = String(def.step);
  const schemaDefault = Array.isArray(def.defaults) ? def.defaults[index] : def.default;
  const cached = deVoiceParamStateFor(family, index, def.key);
  const startVal = cached !== undefined ? cached : schemaDefault;
  slider.value = String(startVal);
  slider.dataset.family = family;
  slider.dataset.paramKey = def.key;
  slider.dataset.index = String(index === undefined ? -1 : index);

  const valEl = document.createElement('div');
  valEl.className = 'val';
  valEl.textContent = String(startVal);

  slider.addEventListener('input', () => {
    const v = Number(slider.value);
    valEl.textContent = String(v);
    if (deSuppressSend) return;
    deSendVoiceParam(family, def.key, v, index);
  });
  slider.addEventListener('change', () => {
    if (deSuppressSend) return;
    deSendVoiceParamCommit(family, def.key, Number(slider.value), index);
  });
  // Ctrl+Click → Reset to default.
  slider.addEventListener('pointerdown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    e.preventDefault();
    if (slider.disabled) return;
    const dv = Number(schemaDefault);
    if (!Number.isFinite(dv)) return;
    slider.value = String(dv);
    valEl.textContent = String(dv);
    if (!deSuppressSend) deSendVoiceParam(family, def.key, dv, index);
  });

  row.appendChild(label);
  row.appendChild(slider);
  row.appendChild(valEl);
  return row;
}

function deFormatKnobVal(def) {
  const step = def.step || 1;
  const decimals = step < 0.001 ? 3 : step < 1 ? 2 : 0;
  return (v) => decimals ? v.toFixed(decimals) : String(Math.round(v));
}

// Preview sound on knob turn: starts a retrigger loop on pointerdown.
function deWireKnobPreview(slider, note) {
  if (note == null) return;
  let previewTimer = null;
  function tick() {
    if (!deKnobPreviewEnabled()) { stopPreviewLoop(); return; }
    deSendMidi('midiNoteOn', note);
  }
  function startPreviewLoop() {
    if (previewTimer) return;
    tick();
    previewTimer = setInterval(tick, deKickRetriggerMs);
  }
  function stopPreviewLoop() {
    if (!previewTimer) return;
    clearInterval(previewTimer);
    previewTimer = null;
    deSendMidi('midiNoteOff', note);
  }
  const ctrlBox = slider.closest('.ctrl');
  const visual = ctrlBox ? ctrlBox.querySelector('.knob-visual') : null;
  (visual || slider).addEventListener('pointerdown', (e) => {
    if (slider.disabled || deKickIsWritingSliders || !deKnobPreviewEnabled()) return;
    // Ctrl+Click is a reset gesture, not a turn -> do not start a retrigger loop.
    // wireKnob fires its `change` (stop) *before* this handler on Ctrl+Click;
    // if the loop were started here, it would run forever (no second change event).
    if (e && (e.ctrlKey || e.metaKey)) return;
    startPreviewLoop();
  });
  slider.addEventListener('input', () => {
    if (deKickIsWritingSliders || !deKnobPreviewEnabled()) return;
    startPreviewLoop();
  });
  slider.addEventListener('change', stopPreviewLoop);
}

function deBuildParamKnob(family, def, index, note) {
  const tipName = def.label || def.name;
  const tipText = def.tooltip ? (tipName + '\n' + def.tooltip) : tipName;
  const schemaDefault = family === 'kick'
    ? deInstanceDefaults('kick', index)[def.key]
    : family === 'snare'
    ? deInstanceDefaults('snare', index)[def.key]
    : family === 'tom'
    ? deInstanceDefaults('tom', index)[def.key]
    : family === 'custom'
    ? deInstanceDefaults('custom', index)[def.key]
    : family === 'hihat'
    ? deHiHatInstanceDefaults(index)[def.key]
    : family === 'cymbal'
    ? deCymbalInstanceDefaults(index)[def.key]
    : family === 'ride'
    ? deRideInstanceDefaults(index)[def.key]
    : (Array.isArray(def.defaults) ? def.defaults[index] : def.default);
  const cached = deVoiceParamStateFor(family, index, def.key);
  const startVal = cached !== undefined ? cached : schemaDefault;
  let committedVal = startVal;
  const dial = neCreateDial({
    label: def.label,
    min: def.min, max: def.max, step: def.step, value: startVal,
    def: schemaDefault,
    format: deFormatKnobVal(def),
    title: tipText,
    onInput: (v) => {
      if (deSuppressSend || deKickIsWritingSliders) { committedVal = v; }   // set by the host / Reset, not by the user: nothing to commit
      if (deSuppressSend) return;
      deSendVoiceParam(family, def.key, v, index);
    },
    // A click on the knob without a change ends with a 'change' event too; committing it would (re)bake an unchanged key.
    onChange: (v) => {
      if (deSuppressSend || v === committedVal) return;
      committedVal = v;
      deSendVoiceParamCommit(family, def.key, v, index);
    },
  });
  dial.dataset.paramKey = def.key;
  dial.dataset.family = family;
  dial.dataset.index = String(index === undefined ? -1 : index);
  const padAxis = DE_FAMILY_PAD_AXIS[family];
  // Closed Hi-Hat cards have no pad -> no blue "pad-linked" ring,
  // even if the key is in HIHAT_PAD_AXIS (e.g. 'level').
  if (padAxis && padAxis[def.key] && !deIsHiHatClosedNote(note)) {
    const visual = dial.querySelector('.knob-visual');
    if (visual) visual.classList.add('pad-linked');
  }
  // A bypass/on-off switch should not start a retrigger
  // preview loop when toggled (makes no conceptual sense for a
  // pure toggle).
  if (!def.noPreview) deWireKnobPreview(dial.slider, note);
  return dial;
}

function deBuildPitchReadoutRow(mainKnobRow, cfg) {
  const pitchDial = mainKnobRow.querySelector(`[data-param-key="${cfg.pitchKey}"]`);
  const freqDials = cfg.freqKeys.map((k) => mainKnobRow.querySelector(`[data-param-key="${k}"]`));
  if (!pitchDial || !pitchDial.slider || freqDials.some((d) => !d || !d.slider)) return null;

  const row = document.createElement('div');
  row.className = 'de-pitch-readout';
  row.style.fontSize = '10px';
  row.style.opacity = '0.7';
  row.style.marginTop = '3px';
  row.style.whiteSpace = 'nowrap';

  function update() {
    const pitch = Number(pitchDial.slider.value);
    if (pitch === 1) { row.style.display = 'none'; return; }
    row.style.display = '';
    const actual = freqDials.map((d) => Math.round(Number(d.slider.value) * pitch)).join(' · ');
    row.textContent = `Pitch ×${pitch.toFixed(2)} → ${actual} Hz`;
  }

  pitchDial.slider.addEventListener('input', update);
  for (const d of freqDials) d.slider.addEventListener('input', update);
  update();
  return row;
}

function deBuildKnobGroupRow(family, group, index, titleMap, note) {
  const cluster = document.createElement('div');
  cluster.className = 'knob-cluster de-cluster';

  const knobsContainer = document.createElement('div');
  knobsContainer.className = 'cluster-knobs';

  const mainUnit = document.createElement('div');
  mainUnit.className = 'subgroup-unit-wrapper';

  const mainVerticalBox = document.createElement('div');
  mainVerticalBox.className = 'subgroup-vertical-stack';

  const mainLabel = document.createElement('div');
  mainLabel.className = 'cluster-title';
  mainLabel.textContent = (titleMap && titleMap[group.title]) || group.title;
  mainVerticalBox.appendChild(mainLabel);

  const mainKnobRow = document.createElement('div');
  mainKnobRow.style.display = 'flex';
  mainKnobRow.style.gap = '6px';
  for (const def of group.params) {
    mainKnobRow.appendChild(deBuildParamKnob(family, def, index, note));
  }
  mainVerticalBox.appendChild(mainKnobRow);

  if (group.pitchReadout) {
    const readoutRow = deBuildPitchReadoutRow(mainKnobRow, group.pitchReadout);
    if (readoutRow) mainVerticalBox.appendChild(readoutRow);
  }

  mainUnit.appendChild(mainVerticalBox);
  knobsContainer.appendChild(mainUnit);

  if (group.subgroups && group.subgroups.length > 0) {
    for (const sub of group.subgroups) {
      const spacer = document.createElement('div');
      spacer.style.width = '24px';
      knobsContainer.appendChild(spacer);

      const subWrapper = document.createElement('div');
      subWrapper.className = 'subgroup-unit-wrapper';

      const subVerticalBox = document.createElement('div');
      subVerticalBox.className = 'subgroup-vertical-stack';

      const subLabel = document.createElement('div');
      subLabel.className = 'cluster-title';
      subLabel.textContent = (titleMap && titleMap[sub.title]) || sub.title;
      subVerticalBox.appendChild(subLabel);

      const subKnobRow = document.createElement('div');
      subKnobRow.style.display = 'flex';
      subKnobRow.style.gap = '6px';
      for (const def of sub.params) {
        subKnobRow.appendChild(deBuildParamKnob(family, def, index, note));
      }
      subVerticalBox.appendChild(subKnobRow);

      subWrapper.appendChild(subVerticalBox);
      knobsContainer.appendChild(subWrapper);
    }
  }

  cluster.appendChild(knobsContainer);
  deWireClusterZoom(cluster);

  return cluster;
}

function deBuildParamPanel(family, schema) {
  const panel = document.createElement('div');
  panel.className = 'de-param-panel';
  panel.dataset.family = family;

  if (DE_INSTANCED_FAMILIES[family]) {
    const { instances, padCfg } = DE_INSTANCED_FAMILIES[family];
    const cardsWrap = document.createElement('div');
    cardsWrap.className = 'de-instance-cards';
    for (const inst of instances) {
      // An instance without a morph pad (Rim Shot circuit): its knob groups and Volume only.
      if (inst.pad === false)
        cardsWrap.appendChild(deBuildTomInstanceCard({ groups: inst.groups }, { family, index: inst.index, note: inst.note, name: inst.name, defaults: inst.defaults }));
      else cardsWrap.appendChild(deBuildInstanceCard(family, inst, padCfg));
    }
    panel.appendChild(cardsWrap);
    return panel;
  }

  if (family === 'tom') return deBuildTomPanel(panel, schema);

  if (schema.articulationLevels) {
    const cardsWrap = document.createElement('div');
    cardsWrap.className = 'de-instance-cards';

    const arts = deArticulationsForFamily(family);

    arts.forEach((art, idx) => {
      const inst = { index: idx, note: art.note, name: art.name };

      const container = document.createElement('div');
      container.className = 'de-instance-card';

      const main = document.createElement('div');
      main.className = 'de-instance-card-main';

      const titleRow = document.createElement('div');
      titleRow.className = 'de-instance-card-title-row';
      const title = document.createElement('div');
      title.className = 'de-instance-card-title';
      title.textContent = art.name;
      titleRow.appendChild(title);
      const resetSlot = document.createElement('div');
      resetSlot.className = 'de-instance-card-reset';
      titleRow.appendChild(resetSlot);
      main.appendChild(titleRow);

      // Closed Hi-Hat: no pad (the real hardware only has ONE
      // knob there, LEVEL -- an X/Y pad needs two meaningful axes).
      const isHiHatClosed = family === 'hihat' && art.family === 'hihat-closed';
      const tableRow = deTableRow(family, idx);
      const groupsForCard = tableRow ? deGroupsOfRow(tableRow) : schema.groups;

      const knobRow = document.createElement('div');
      knobRow.className = 'de-knob-groups-row de-instance-card-knobs';
      for (const group of groupsForCard) {
        knobRow.appendChild(deBuildKnobGroupRow(family, group, idx, undefined, art.note));
      }
      main.appendChild(knobRow);
      container.appendChild(main);

      if (isHiHatClosed) {
        // Reset button like on the cards with a pad (deBuildInstancePadUnit): resets the card's voice
        // parameters to their defaults and sends them to the engine.
        const resetBtn = document.createElement('button');
        resetBtn.type = 'button';
        resetBtn.className = 'btn btn-refresh';
        resetBtn.textContent = 'Reset';
        resetBtn.dataset.tooltip = 'Reset\nRestores all parameters to the default sound.';
        resetSlot.appendChild(resetBtn);
        resetBtn.addEventListener('click', () => {
          deApplyVoiceValues(deHiHatInstanceDefaults(idx), container);
        });

        // Only the per-note volume knob (same note-gain function as
        // embedded in the pad elsewhere), no pad/morph.
        const volSlot = document.createElement('div');
        volSlot.className = 'de-instance-card-vol';
        const volCluster = document.createElement('div');
        volCluster.className = 'knob-cluster de-cluster';
        const volKnobsContainer = document.createElement('div');
        volKnobsContainer.className = 'cluster-knobs';
        const volUnit = document.createElement('div');
        volUnit.className = 'subgroup-unit-wrapper';
        const volStack = document.createElement('div');
        volStack.className = 'subgroup-vertical-stack';
        const volKnobRow = document.createElement('div');
        volKnobRow.style.display = 'flex';
        volKnobRow.style.gap = '6px';
        volKnobRow.appendChild(deBuildArticulationLevelKnob(art.note, art.name));
        volStack.appendChild(volKnobRow);
        volUnit.appendChild(volStack);
        volKnobsContainer.appendChild(volUnit);
        volCluster.appendChild(volKnobsContainer);
        deWireClusterZoom(volCluster);
        volSlot.appendChild(volCluster);
        container.appendChild(volSlot);
      } else {
        const padUnit = deBuildInstancePadUnit({
          inst, container,
          titleRowEl: resetSlot,
          axisConfig: schema.padAxis,
          logParams: schema.padLogParams,
          cornersCache: schema.padCornersCache,
          instanceDefaults: schema.instanceDefaults,
          padTooltipCount: schema.padTooltipCount || '4',
          family,
        });

        const volWrapEl = padUnit.querySelector('.de-kick-pad-vol');
        if (volWrapEl) {
          const volSlot = document.createElement('div');
          volSlot.className = 'de-instance-card-vol';
          volSlot.appendChild(volWrapEl);
          container.appendChild(volSlot);
        }

        const padEl = padUnit.querySelector('.de-kick-pad');
        if (padEl) {
          const zoomShell = document.createElement('div');
          zoomShell.className = 'knob-cluster de-instance-card-pad-zoom';
          padEl.replaceWith(zoomShell);
          zoomShell.appendChild(padEl);
          deWireClusterZoom(zoomShell);
          container.appendChild(zoomShell);
        }
      }

      cardsWrap.appendChild(container);
    });

    panel.appendChild(cardsWrap);
    return panel;
  }

  if (schema.perInstance) {
    const pi = schema.perInstance;
    const title = document.createElement('div');
    title.className = 'de-param-group-title';
    title.textContent = pi.label + ' (per position/cymbal)';
    panel.appendChild(title);
    for (let i = 0; i < pi.instanceLabels.length; i++) {
      panel.appendChild(deBuildParamRow(family, { ...pi, label: pi.instanceLabels[i] }, i));
    }
  }

  if (schema.shared) {
    if (schema.perInstance) {
      const title = document.createElement('div');
      title.className = 'de-param-group-title';
      title.textContent = 'Shared (all positions/cymbals)';
      panel.appendChild(title);
    }
    for (const def of schema.shared) panel.appendChild(deBuildParamRow(family, def, undefined));
  }

  if (schema.groups) {
    if (schema.knobs) {
      const row = document.createElement('div');
      row.className = 'de-knob-groups-row';
      for (const group of schema.groups) row.appendChild(deBuildKnobGroupRow(family, group));
      panel.appendChild(row);
    } else {
      for (const group of schema.groups) {
        const title = document.createElement('div');
        title.className = 'de-param-group-title';
        title.textContent = group.title;
        panel.appendChild(title);
        for (const def of group.params) panel.appendChild(deBuildParamRow(family, def, undefined));
      }
    }
  }

  return panel;
}

// =========================================
// Tom panel
// =========================================
// Fifteen instances (the pools of the engine), in the order of the Note Editor: every tom articulation (Center, Rimshot,
// Rim Only of Rack Tom 1..3 and Floor Tom 1/2) is a card with its own circuit knobs, Reset and volume. The rim instances
// play the circuit under a very short decay; the circuit has no decay control, so they are shaped with tuning and level.

function deBuildTomInstanceCard(schema, inst) {
  const container = document.createElement('div');
  container.className = 'de-instance-card';
  if (inst.customId) container.dataset.customEngine = inst.customId;

  const main = document.createElement('div');
  main.className = 'de-instance-card-main';

  const titleRow = document.createElement('div');
  titleRow.className = 'de-instance-card-title-row';
  const title = document.createElement('div');
  title.className = 'de-instance-card-title';
  title.textContent = inst.name;
  titleRow.appendChild(title);
  const resetSlot = document.createElement('div');
  resetSlot.className = 'de-instance-card-reset';
  titleRow.appendChild(resetSlot);
  main.appendChild(titleRow);

  const knobRow = document.createElement('div');
  knobRow.className = 'de-knob-groups-row de-instance-card-knobs';
  for (const group of schema.groups) {
    knobRow.appendChild(deBuildKnobGroupRow(inst.family || 'tom', group, inst.index, undefined, inst.previewNote != null ? inst.previewNote : inst.note));
  }
  main.appendChild(knobRow);
  container.appendChild(main);

  const resetBtn = document.createElement('button');
  resetBtn.type = 'button';
  resetBtn.className = 'btn btn-refresh';
  resetBtn.textContent = 'Reset';
  resetBtn.dataset.tooltip = 'Reset\nRestores all parameters to the default sound.';
  resetSlot.appendChild(resetBtn);
  resetBtn.addEventListener('click', () => {
    deApplyVoiceValues(inst.defaults, container);
  });

  // Volume of this articulation (same note-gain function the pad cards embed).
  const volSlot = document.createElement('div');
  volSlot.className = 'de-instance-card-vol';
  const volCluster = document.createElement('div');
  volCluster.className = 'knob-cluster de-cluster';
  const volKnobsContainer = document.createElement('div');
  volKnobsContainer.className = 'cluster-knobs';
  const volUnit = document.createElement('div');
  volUnit.className = 'subgroup-unit-wrapper';
  const volStack = document.createElement('div');
  volStack.className = 'subgroup-vertical-stack';
  const volKnobRow = document.createElement('div');
  volKnobRow.style.display = 'flex';
  volKnobRow.style.gap = '6px';
  if (inst.note != null)
  {
    if (inst.customId && !(inst.note in deArticulationGainCache) && (inst.customId in deCustomEngineGainCache))
    {
      deArticulationGainCache[inst.note] = deCustomEngineGainCache[inst.customId];
      deSendArticulationLevel(inst.note, deCustomEngineGainCache[inst.customId]);
    }
    volKnobRow.appendChild(deBuildArticulationLevelKnob(inst.note, inst.name, inst.customId ? deVolumeDefaultOf(inst) : undefined));
  }
  else if (inst.customId) volKnobRow.appendChild(deBuildCustomEngineLevelKnob(inst));
  volStack.appendChild(volKnobRow);
  volUnit.appendChild(volStack);
  volKnobsContainer.appendChild(volUnit);
  volCluster.appendChild(volKnobsContainer);
  deWireClusterZoom(volCluster);
  volSlot.appendChild(volCluster);
  container.appendChild(volSlot);
  return container;
}

// Card of a custom instrument with an engine (row k of family 'custom' of the instrument table): the circuit's knobs (Tuning/Level/Pitch of a
// tom sheet, Level/Pitch of the RM sheet), previewed by its index (works without a track); the volume knob needs the GM note of the track it is
// assigned to.
function deBuildCustomEngineCard(k) {
  const row = deTableRow('custom', k);
  const track = (typeof trackList !== 'undefined') ? trackList.find((t) => t.customInstrument === row.id) : null;
  return deBuildTomInstanceCard({ groups: deGroupsOfRow(row) }, { family: 'custom', index: k, note: track ? track.gm : null, previewNote: { custom: k }, name: row.name, defaults: row.defaults, customId: row.id });
}

// The assignment of a track changed: rebuild the cards so their volume knob follows the note of the track.
function deRebuildCustomEngineCards() {
  document.querySelectorAll('.de-instance-card[data-custom-engine]').forEach((card) => {
    const inst = dggCustomInstruments().find((d) => d.id === card.dataset.customEngine);
    if (inst) card.replaceWith(deBuildCustomEngineCard(inst.index));
  });
}

function deBuildTomPanel(panel, schema) {
  const cardsWrap = document.createElement('div');
  cardsWrap.className = 'de-instance-cards';
  for (const inst of TOM_INSTANCES) cardsWrap.appendChild(deBuildTomInstanceCard({ groups: inst.groups }, inst));
  panel.appendChild(cardsWrap);
  return panel;
}

// =========================================
// XY pad math (Kick/Snare/Hi-Hat/Cymbal/Ride)
// =========================================
// Generic bilinear/log interpolation for pad-driven parameters.
function dePadCornerValue(axisConfig, param, ref, sx, sy) {
  const cfg = axisConfig[param];
  let s = cfg.axis === 'x' ? sx : sy;
  if (cfg.invert) s = -s;
  if (cfg.type === 'log') return ref * Math.pow(cfg.spread, s);
  return ref + cfg.spread * s;
}

function deGetPadCorners(cache, axisConfig, refByIndex, index) {
  if (cache.has(index)) return cache.get(index);
  const ref = refByIndex(index);
  const corners = { bl: {}, br: {}, tl: {}, tr: {} };
  for (const param in axisConfig) {
    corners.bl[param] = dePadCornerValue(axisConfig, param, ref[param], -1, -1);
    corners.br[param] = dePadCornerValue(axisConfig, param, ref[param],  1, -1);
    corners.tl[param] = dePadCornerValue(axisConfig, param, ref[param], -1,  1);
    corners.tr[param] = dePadCornerValue(axisConfig, param, ref[param],  1,  1);
  }
  cache.set(index, corners);
  return corners;
}

function dePadInterp(logParams, corners, param, x, y) {
  if (logParams.has(param)) {
    const l = Math.log(corners.bl[param]) * (1 - x) * (1 - y)
            + Math.log(corners.br[param]) * x * (1 - y)
            + Math.log(corners.tl[param]) * (1 - x) * y
            + Math.log(corners.tr[param]) * x * y;
    return Math.exp(l);
  }
  return corners.bl[param] * (1 - x) * (1 - y)
       + corners.br[param] * x * (1 - y)
       + corners.tl[param] * (1 - x) * y
       + corners.tr[param] * x * y;
}

function dePadFracForParam(logParams, corners, axisConfig, param, val) {
  const cfg = axisConfig[param];
  const lo = corners.bl[param];
  const hi = cfg.axis === 'x' ? corners.br[param] : corners.tl[param];
  let frac;
  if (logParams.has(param)) {
    frac = (Math.log(val) - Math.log(lo)) / (Math.log(hi) - Math.log(lo));
  } else {
    frac = (val - lo) / (hi - lo);
  }
  return Math.min(Math.max(frac, 0), 1);
}

const KICK_PAD_AXIS = {
  toneKnob:    { axis: 'x', type: 'linear', spread: 0.5 },
  decayKnob01: { axis: 'y', type: 'linear', spread: 0.5, invert: true },
};

const KICK_PAD_LOG_PARAMS = new Set(
  Object.keys(KICK_PAD_AXIS).filter((k) => KICK_PAD_AXIS[k].type === 'log')
);

const deKickPadCornersCache = new Map();

function deGetKickSliders(root) {
  const sliders = {};
  if (!root) return sliders;
  root.querySelectorAll('.ctrl[data-param-key]').forEach((ctrl) => {
    if (ctrl.slider) sliders[ctrl.dataset.paramKey] = ctrl.slider;
  });
  return sliders;
}

let deKickIsWritingSliders = false;
let deSuppressSend = false;

const DE_KNOB_PREVIEW_KEY = 'dggKnobPreviewSound';
function deKnobPreviewEnabled() {
  return dggGetItem(DE_KNOB_PREVIEW_KEY) !== '0';
}

let deKickRetriggerMs = 450;

function deApplyVoiceValues(values, root) {
  const sliders = deGetKickSliders(root);
  deKickIsWritingSliders = true;
  for (const key in values) {
    const slider = sliders[key];
    if (!slider) continue;
    slider.value = String(values[key]);
    slider.dispatchEvent(new Event('input', { bubbles: true }));
  }
  deKickIsWritingSliders = false;
}

function deBuildInstancePadUnit(cfg) {
  const { inst, container, titleRowEl, axisConfig, logParams, cornersCache, instanceDefaults, padTooltipCount, family } = cfg;

  function corners() { return deGetPadCorners(cornersCache, axisConfig, instanceDefaults, inst.index); }

  const wrap = document.createElement('div');
  wrap.className = 'de-kick-pad-unit';

  const body = document.createElement('div');
  body.className = 'de-kick-pad-body';

  const pad = document.createElement('div');
  pad.className = 'de-kick-pad';
  pad.dataset.tooltip = 'Morph Pad\nDrag to blend the sound between the four corner presets. Keeps retriggering while held.\nA hollow dot means a knob below was changed by hand and no longer matches — drag again to resync.';
  for (const [cls, text] of [['tl', 'low\nshort'], ['tr', 'high\nshort'], ['bl', 'low\nlong'], ['br', 'high\nlong']]) {
    const l = document.createElement('div');
    l.className = 'de-kick-pad-label ' + cls;
    l.textContent = text;
    pad.appendChild(l);
  }
  const dot = document.createElement('div');
  dot.className = 'de-kick-pad-dot';
  pad.appendChild(dot);

  const volWrap = document.createElement('div');
  volWrap.className = 'de-kick-pad-vol';

  const volCluster = document.createElement('div');
  volCluster.className = 'knob-cluster de-cluster';
  const volKnobsContainer = document.createElement('div');
  volKnobsContainer.className = 'cluster-knobs';
  const volUnit = document.createElement('div');
  volUnit.className = 'subgroup-unit-wrapper';
  const volStack = document.createElement('div');
  volStack.className = 'subgroup-vertical-stack';
  const volKnobRow = document.createElement('div');
  volKnobRow.style.display = 'flex';
  volKnobRow.style.gap = '6px';
  volKnobRow.appendChild(deBuildArticulationLevelKnob(inst.note, inst.name));
  volStack.appendChild(volKnobRow);
  volUnit.appendChild(volStack);
  volKnobsContainer.appendChild(volUnit);
  volCluster.appendChild(volKnobsContainer);
  deWireClusterZoom(volCluster);

  volWrap.appendChild(volCluster);

  body.append(pad, volWrap);
  wrap.appendChild(body);

  const padKey = family ? family + ':' + inst.index : null;
  let x = 0.5, y = 0.5, timer = null, dragging = false;

  function setSynced(synced) { dot.classList.toggle('unsynced', !synced); }

  const sliders = deGetKickSliders(container);
  for (const key in sliders) {
    sliders[key].addEventListener('input', () => {
      if (deKickIsWritingSliders) return;
      const cfg = axisConfig[key];
      if (cfg) {
        const val = parseFloat(sliders[key].value);
        if (!isNaN(val)) {
          const frac = dePadFracForParam(logParams, corners(), axisConfig, key, val);
          if (cfg.axis === 'x') x = frac; else y = frac;
          updateDot();
        }
      }
      setSynced(false);
    });
    sliders[key].addEventListener('change', () => {
      if (deKickIsWritingSliders) return;
      if (!axisConfig[key]) return;
      if (padKey) {
        if (!dePadPositionsCache) dePadPositionsCache = {};
        dePadPositionsCache[padKey] = { x, y, synced: false };
        if (typeof hostBridge !== 'undefined' && hostBridge.send) {
          hostBridge.send('setPadPos', { family: family, index: inst.index, x, y, synced: false });
        }
      }
    });
  }

  function applyToSliders(px, py) {
    const values = {};
    const c = corners();
    for (const key in c.bl) values[key] = dePadInterp(logParams, c, key, px, py);
    deApplyVoiceValues(values, container);
    setSynced(true);
  }
  function updateDot() {
    dot.style.left = (x * 100) + '%';
    dot.style.top = ((1 - y) * 100) + '%';
  }
  function setFromEvent(e) {
    const r = pad.getBoundingClientRect();
    x = Math.min(Math.max((e.clientX - r.left) / r.width, 0), 1);
    y = 1 - Math.min(Math.max((e.clientY - r.top) / r.height, 0), 1);
    updateDot();
  }

  function currentKnownValues() {
    const defaults = instanceDefaults(inst.index);
    const merged = { ...defaults };
    if (family) {
      for (const key in merged) {
        const hostValue = deVoiceParamStateFor(family, inst.index, key);
        if (hostValue !== undefined) merged[key] = hostValue;
      }
    }
    return merged;
  }

  function resyncPad() {
    if (padKey && dePadPositionsCache && dePadPositionsCache[padKey]) {
      const pos = dePadPositionsCache[padKey];
      x = typeof pos.x === 'number' ? pos.x : 0.5;
      y = typeof pos.y === 'number' ? pos.y : 0.5;
      updateDot();
      setSynced(pos.synced !== false);
      return;
    }
    x = 0.5;
    y = 0.5;
    updateDot();
    setSynced(true);
  }
  if (family) {
    dePadResyncCallbacks[family + ':' + inst.index] = resyncPad;
  }
  resyncPad();

  function trigger() {
    applyToSliders(x, y);
    deSendMidi('midiNoteOn', inst.note);
  }
  function startLoop() {
    if (timer) clearInterval(timer);
    trigger();
    timer = setInterval(trigger, deKickRetriggerMs);
  }
  function stopLoop() {
    if (timer) { clearInterval(timer); timer = null; }
    deSendMidi('midiNoteOff', inst.note);
  }

  pad.addEventListener('pointerdown', (e) => {
    dragging = true;
    pad.setPointerCapture(e.pointerId);
    setFromEvent(e);
    startLoop();
  });
  pad.addEventListener('pointermove', (e) => { if (dragging) setFromEvent(e); });
  pad.addEventListener('pointerup', () => {
    dragging = false;
    stopLoop();
    if (padKey) {
      if (!dePadPositionsCache) dePadPositionsCache = {};
      dePadPositionsCache[padKey] = { x, y, synced: true };
      if (typeof hostBridge !== 'undefined' && hostBridge.send) {
        hostBridge.send('setPadPos', { family: family, index: inst.index, x, y, synced: true });
      }
    }
  });
  pad.addEventListener('pointercancel', () => { dragging = false; stopLoop(); });

  const resetBtn = document.createElement('button');
  resetBtn.type = 'button';
  resetBtn.className = 'btn btn-refresh';
  resetBtn.textContent = 'Reset';
  resetBtn.dataset.tooltip = 'Reset\nRestores all parameters to the default sound.';
  titleRowEl.appendChild(resetBtn);
  resetBtn.addEventListener('click', () => {
    deApplyVoiceValues(instanceDefaults(inst.index), container);
    x = 0.5;
    y = 0.5;
    updateDot();
    setSynced(true);
    if (padKey) {
      if (!dePadPositionsCache) dePadPositionsCache = {};
      dePadPositionsCache[padKey] = { x, y, synced: true };
      if (typeof hostBridge !== 'undefined' && hostBridge.send) {
        hostBridge.send('setPadPos', { family: family, index: inst.index, x, y, synced: true });
      }
    }
  });

  function syncInstanceIfFresh() {
    if (!deVoiceParamStateCache || deVoiceParamStateConfigured) return;
    deApplyVoiceValues(instanceDefaults(inst.index), container);
  }
  if (family) {
    deInstanceSyncCallbacks[family + ':' + inst.index] = syncInstanceIfFresh;
  }
  syncInstanceIfFresh();
  setSynced(true);

  updateDot();
  return wrap;
}

function deBuildInstanceCard(family, inst, padCfg) {
  const container = document.createElement('div');
  container.className = 'de-instance-card';

  const main = document.createElement('div');
  main.className = 'de-instance-card-main';
  const titleRow = document.createElement('div');
  titleRow.className = 'de-instance-card-title-row';
  const title = document.createElement('div');
  title.className = 'de-instance-card-title';
  title.textContent = inst.name;
  titleRow.appendChild(title);
  const resetSlot = document.createElement('div');
  resetSlot.className = 'de-instance-card-reset';
  titleRow.appendChild(resetSlot);
  main.appendChild(titleRow);

  const knobRow = document.createElement('div');
  knobRow.className = 'de-knob-groups-row de-instance-card-knobs';
  const titleMap = undefined;
  for (const group of (inst.groups || DE_PARAM_SCHEMAS[family].groups)) {
    knobRow.appendChild(deBuildKnobGroupRow(family, group, inst.index, titleMap, inst.note));
  }
  main.appendChild(knobRow);
  container.appendChild(main);

  const padUnit = deBuildInstancePadUnit({ inst, container, titleRowEl: resetSlot, family, ...padCfg });

  const volWrapEl = padUnit.querySelector('.de-kick-pad-vol');
  if (volWrapEl) {
    const volSlot = document.createElement('div');
    volSlot.className = 'de-instance-card-vol';
    volSlot.appendChild(volWrapEl);
    container.appendChild(volSlot);
  }

  const padEl = padUnit.querySelector('.de-kick-pad');
  if (padEl) {
    const zoomShell = document.createElement('div');
    zoomShell.className = 'knob-cluster de-instance-card-pad-zoom';
    padEl.replaceWith(zoomShell);
    zoomShell.appendChild(padEl);
    deWireClusterZoom(zoomShell);
    container.appendChild(zoomShell);
  }

  return container;
}

const SNARE_PAD_AXIS = {
  toneKnob:   { axis: 'x', type: 'linear', spread: 0.5 },
  snappyKnob: { axis: 'y', type: 'linear', spread: 0.5, invert: true },
};

const SNARE_PAD_LOG_PARAMS = new Set(
  Object.keys(SNARE_PAD_AXIS).filter((k) => SNARE_PAD_AXIS[k].type === 'log')
);

const DE_FAMILY_PAD_AXIS = {
  kick: KICK_PAD_AXIS,
  snare: SNARE_PAD_AXIS,
  hihat: HIHAT_PAD_AXIS,
  cymbal: CYMBAL_PAD_AXIS,
  ride: RIDE_PAD_AXIS,
};

const deSnarePadCornersCache = new Map();


const DE_INSTANCED_FAMILIES = {
  kick: {
    instances: KICK_INSTANCES,   // replaced by the table's instances in deApplyInstrumentTable()
    padCfg: {
      axisConfig: KICK_PAD_AXIS, logParams: KICK_PAD_LOG_PARAMS,
      cornersCache: deKickPadCornersCache, instanceDefaults: (index) => deInstanceDefaults('kick', index),
      padTooltipCount: 'all 19',
    },
  },
  snare: {
    instances: SNARE_INSTANCES,   // replaced by the table's instances in deApplyInstrumentTable()
    padCfg: {
      axisConfig: SNARE_PAD_AXIS, logParams: SNARE_PAD_LOG_PARAMS,
      cornersCache: deSnarePadCornersCache, instanceDefaults: (index) => deInstanceDefaults('snare', index),
      padTooltipCount: 'all 4',
    },
  },
};

// =========================================
// Right-pinned dB meter (sidebar)
// =========================================
// Meter-only, follows the currently focused instrument family.
let deFocusFamily = 'kick';
const DE_FOCUS_FAMILY_KEY = 'dggDeFocusFamily';
let deSideMeter = null;

function deSetFocusFamily(family) {
  deFocusFamily = family;
  dggSetItem(DE_FOCUS_FAMILY_KEY, family);
  if (deSideMeter) deSideMeter.reset();
  deRefreshEngineSelectDisplay();
  deRefreshParamPanelForFamily(family);
}

let deFocusSidebarBuilt = false;

function deInitFocusSidebar() {
  if (deFocusSidebarBuilt) return;
  deFocusSidebarBuilt = true;

  const meterEl = document.getElementById('deSideMeter');
  // Read-only peak value above the meter — same behavior as the mixer strips:
  // held peak in its zone color, visible only while audio is playing. The
  // meter drives it via readoutEl (see db-meter.js).
  const dePeakReadout = document.getElementById('dePeakReadout');
  if (dePeakReadout) dePeakReadout.dataset.tooltip = 'Peak (dBFS)\nLoudest output level, held for the Meter Peak Hold time — visible only while audio is playing.';
  if (meterEl) deSideMeter = createDbMeter(meterEl, { ticks: 'master', readoutEl: dePeakReadout || null });

  if (typeof hostBridge !== 'undefined' && hostBridge.on) {
    hostBridge.on('levelMeter', (payload) => {
      const db = payload && payload.levels ? payload.levels[deFocusFamily] : undefined;
      if (db === undefined || !deSideMeter) return;
      deSideMeter.setDb(db);
    });
  }
}

let deParamTabsBuilt = false;

function deInitVoiceParamTabs() {
  if (deParamTabsBuilt) return;
  deParamTabsBuilt = true;

  const panelsWrap = document.getElementById('deParamPanels');
  const tabBar = document.getElementById('deParamTabs');
  if (!panelsWrap || !tabBar) return;

  panelsWrap.innerHTML = '';
  for (const family of Object.keys(DE_PARAM_SCHEMAS)) {
    panelsWrap.appendChild(deBuildParamPanel(family, DE_PARAM_SCHEMAS[family]));
  }
  const customPanel = document.createElement('div');
  customPanel.className = 'de-param-panel';
  customPanel.dataset.family = 'custom';
  const customCards = document.createElement('div');
  customCards.className = 'de-instance-cards';
  for (const inst of dggCustomInstruments()) customCards.appendChild(deBuildCustomEngineCard(inst.index));
  customPanel.appendChild(customCards);
  panelsWrap.appendChild(customPanel);
  const savedFamily = dggGetItem(DE_FOCUS_FAMILY_KEY);
  const initialFamily = (savedFamily && DE_PARAM_SCHEMAS[savedFamily]) ? savedFamily : 'kick';
  tabBar.querySelectorAll('.de-param-tab-btn').forEach(b => b.classList.toggle('active', b.dataset.family === initialFamily));
  panelsWrap.querySelectorAll('.de-param-panel').forEach(p => p.classList.toggle('active', p.dataset.family === initialFamily));
  deInitFocusSidebar();
  deSetFocusFamily(initialFamily);

  tabBar.addEventListener('click', (e) => {
    const btn = e.target.closest('.de-param-tab-btn');
    if (!btn) return;
    tabBar.querySelectorAll('.de-param-tab-btn').forEach(b => b.classList.remove('active'));
    panelsWrap.querySelectorAll('.de-param-panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    panelsWrap.querySelector(`.de-param-panel[data-family="${btn.dataset.family}"]`).classList.add('active');
    deSetFocusFamily(btn.dataset.family);
  });
}

const DE_POLY_FAMILIES = ['kick', 'snare', 'tom', 'ride'];

// =========================================
// Undo for the Drum Engine tab
// =========================================
// One generic layer: the sound state (voice parameters, articulation / custom volumes, pad positions, polyphony) is captured
// when a gesture starts and compared when it ends; a difference becomes one undo step. Restoring writes the values to the
// host again (and to the controls, if they exist), so it does not depend on which panels are currently built.

function dePolySlider(family) {
  return document.getElementById('dePoly' + family[0].toUpperCase() + family.slice(1));
}

function deUndoCapture() {
  const panel = document.querySelector('.tab-panel[data-tab-panel="drumEngine"]');
  const voice = {};
  if (panel) {
    panel.querySelectorAll('[data-family][data-param-key]').forEach((el) => {
      const input = el.slider || el;
      voice[el.dataset.family + '|' + el.dataset.index + '|' + el.dataset.paramKey] = String(input.value);
    });
  }
  const poly = {};
  for (const family of DE_POLY_FAMILIES) {
    const slider = dePolySlider(family);
    if (slider) poly[family] = String(slider.value);
  }
  return {
    voice,
    gains: { ...deArticulationGainCache },
    customGains: { ...deCustomEngineGainCache },
    pads: JSON.parse(JSON.stringify(dePadPositionsCache || {})),
    poly,
  };
}

function deUndoSetDial(dial, value) {
  if (!dial || !dial.slider) return;
  deKickIsWritingSliders = true;
  dial.slider.value = String(value);
  dial.slider.dispatchEvent(new Event('input'));
  deKickIsWritingSliders = false;
}

// Moves the sound state from `current` to `target`; only entries that differ are written.
function deUndoApply(target, current) {
  for (const k in target.voice) {
    if (current.voice[k] === target.voice[k]) continue;
    const [family, idxStr, key] = k.split('|');
    const index = Number(idxStr) < 0 ? undefined : Number(idxStr);
    const value = Number(target.voice[k]);
    deSendVoiceParam(family, key, value, index);
    deSendVoiceParamCommit(family, key, value, index);
    if (deVoiceParamStateCache) {
      const cacheKey = family + ':' + (index === undefined ? -1 : index);
      (deVoiceParamStateCache[cacheKey] = deVoiceParamStateCache[cacheKey] || {})[key] = value;
    }
    const el = document.querySelector(`[data-family="${family}"][data-index="${idxStr}"][data-param-key="${key}"]`);
    if (el) {
      const input = el.slider || el;
      deSuppressSend = true; deKickIsWritingSliders = true;
      input.value = String(value);
      input.dispatchEvent(new Event('input', { bubbles: true }));
      deKickIsWritingSliders = false; deSuppressSend = false;
    }
  }

  const notes = new Set([...Object.keys(target.gains), ...Object.keys(current.gains)]);
  notes.forEach((note) => {
    if (current.gains[note] === target.gains[note]) return;
    const dial = document.querySelector(`.de-artic-vol-knob[data-note="${note}"]`);
    const value = (note in target.gains) ? target.gains[note] : Number(dial && dial.dataset.def != null ? dial.dataset.def : DE_LEVEL_DEFAULT);
    deApplyArticulationGain(Number(note), value);
    deSendArticulationLevel(Number(note), value);
  });

  const customIds = new Set([...Object.keys(target.customGains), ...Object.keys(current.customGains)]);
  customIds.forEach((id) => {
    if (current.customGains[id] === target.customGains[id]) return;
    const dial = document.querySelector(`.de-artic-vol-knob[data-custom-id="${id}"]`);
    if (id in target.customGains) deCustomEngineGainCache[id] = target.customGains[id]; else delete deCustomEngineGainCache[id];
    if (dial) {
      const value = (id in target.customGains) ? target.customGains[id] : Number(dial.dataset.def);
      deUndoSetDial(dial, value);   // its input handler sends the preview gain
    }
  });

  const padKeys = new Set([...Object.keys(target.pads), ...Object.keys(current.pads)]);
  let padsChanged = false;
  padKeys.forEach((padKey) => {
    if (JSON.stringify(current.pads[padKey]) === JSON.stringify(target.pads[padKey])) return;
    padsChanged = true;
    if (!dePadPositionsCache) dePadPositionsCache = {};
    const pos = target.pads[padKey];
    if (pos === undefined) { delete dePadPositionsCache[padKey]; return; }
    dePadPositionsCache[padKey] = pos;
    const [family, index] = padKey.split(':');
    if (typeof hostBridge !== 'undefined' && hostBridge.send) {
      hostBridge.send('setPadPos', { family, index: Number(index), x: pos.x, y: pos.y, synced: pos.synced !== false });
    }
  });
  if (padsChanged) for (const key in dePadResyncCallbacks) dePadResyncCallbacks[key]();

  for (const family of DE_POLY_FAMILIES) {
    const slider = dePolySlider(family);
    if (!slider || target.poly[family] === undefined || slider.value === target.poly[family]) continue;
    slider.value = target.poly[family];
    slider.dispatchEvent(new Event('input'));
  }
}

function deUndoLabel(before, after) {
  const changed = Object.keys(after.voice).filter((k) => before.voice[k] !== after.voice[k]);
  if (changed.length === 1) {
    const [family, , key] = changed[0].split('|');
    return `Drum Engine: ${family} ${key}`;
  }
  return 'Drum Engine edit';
}

// Makes the current state the reference for the next change (after a host load or an undo/redo).
function deUndoRebase() { deUndoBaseline = deUndoCapture(); }

function deInitUndo() {
  const panel = document.querySelector('.tab-panel[data-tab-panel="drumEngine"]');
  if (!panel || panel._deUndoWired) return;
  panel._deUndoWired = true;
  deUndoRebase();
  let timer = null;
  // Whatever caused a change (mouse, wheel, keyboard, Reset, pad), the undo step is the difference between the last known
  // state and the current one; the events below only say "look now".
  const commit = () => {
    const now = deUndoCapture();
    const base = deUndoBaseline || now;
    for (const k in now.voice) if (!(k in base.voice)) base.voice[k] = now.voice[k];   // control built after the baseline was taken
    deUndoBaseline = now;
    if (JSON.stringify(now) === JSON.stringify(base)) return;
    pushUndo(deUndoLabel(base, now),
      () => { deUndoApply(base, deUndoCapture()); deUndoRebase(); },
      () => { deUndoApply(now, deUndoCapture()); deUndoRebase(); },
      'drumEngine');
  };
  // The delay lets the control's own handlers (reset buttons, pad release) finish before the state is compared.
  const check = (delay) => { clearTimeout(timer); timer = setTimeout(commit, delay); };
  panel.addEventListener('change', () => check(60), true);
  panel.addEventListener('pointerup', () => check(60), true);
  panel.addEventListener('keyup', () => check(60), true);
  panel.addEventListener('wheel', () => check(350), { capture: true, passive: true });
}

function deInitPolyphonyControls() {
  for (const family of DE_POLY_FAMILIES) {
    const slider = document.getElementById('dePoly' + family[0].toUpperCase() + family.slice(1));
    const valEl = document.getElementById('dePoly' + family[0].toUpperCase() + family.slice(1) + 'Val');
    if (!slider) continue;

    wireKnob(slider);

    // Ctrl+Click → Reset to default. defaultValue = initial value from index.html and
    // stays stable after host push/changes → automatically follows when defaults change.
    const polyCb = slider.closest('.ctrl');
    if (polyCb && slider.defaultValue != null) polyCb.dataset.def = String(slider.defaultValue);

    slider.addEventListener('input', () => {
      const value = Number(slider.value);
      if (valEl) valEl.textContent = String(value);
      if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
      hostBridge.send('setPolyphony', { family, value });
    });
  }

  const retrigInput = document.getElementById('deRetrigger');
  if (retrigInput) {
    const clamp = (v) => Math.min(1000, Math.max(150, Math.round(v / 50) * 50));
    retrigInput.addEventListener('input', () => {
      const v = Number(retrigInput.value);
      if (!Number.isFinite(v)) return;
      deKickRetriggerMs = v;
    });
    retrigInput.addEventListener('change', () => {
      const v = clamp(Number(retrigInput.value) || deKickRetriggerMs);
      retrigInput.value = String(v);
      deKickRetriggerMs = v;
    });
  }

  const knobPreviewToggle = document.getElementById('deKnobPreviewToggle');
  if (knobPreviewToggle) {
    knobPreviewToggle.checked = deKnobPreviewEnabled();
    knobPreviewToggle.addEventListener('change', (e) => {
      dggSetItem(DE_KNOB_PREVIEW_KEY, e.target.checked ? '1' : '0');
    });
  }

  // Meter Peak Hold is global state — accessible as its own select (seconds)
  // in the options menus of both Drum Engine AND Mixer, both synchronized to the same value.
  const peakHoldSelects = ['dggMeterPeakHoldSelect', 'dggMeterPeakHoldSelectMixer'].map((id) => document.getElementById(id)).filter(Boolean);
  peakHoldSelects.forEach((sel) => {
    sel.value = String(dbMeterPeakHoldSec);
    if (sel.value !== String(dbMeterPeakHoldSec)) sel.value = '2';
    sel.addEventListener('change', (e) => {
      setDbMeterPeakHold(parseFloat(e.target.value));
      peakHoldSelects.forEach((o) => { o.value = e.target.value; });
    });
  });

  if (typeof hostBridge !== 'undefined' && hostBridge.on) {
    hostBridge.on('polyphonyState', (payload) => {
      if (!payload) return;
      for (const family of DE_POLY_FAMILIES) {
        if (!(family in payload)) continue;
        const slider = document.getElementById('dePoly' + family[0].toUpperCase() + family.slice(1));
        const valEl = document.getElementById('dePoly' + family[0].toUpperCase() + family.slice(1) + 'Val');
        const value = payload[family];
        if (slider) {
          slider.value = String(value);
          slider.dispatchEvent(new Event('input'));
        }
        if (valEl) valEl.textContent = String(value);
      }
      deUndoRebase();
    });
  }

  deInitCacheVariantControls();
}

function deFormatBakeDuration(seconds) {
  if (seconds < 90) return Math.max(1, Math.round(seconds)) + ' s';
  const min = seconds / 60;
  if (min < 90) return Math.round(min) + ' min';
  return (min / 60).toFixed(1) + ' h';
}

// Cache panel (toolbar, next to Options): variant count per sound plus the
// two clear actions. The count is only a pending value until "Apply…" --
// the host first reports how many samples the new count would bake, and a
// large job needs an explicit confirmation (it keeps most CPU cores busy
// for as long as it takes). See DrumGroover.cpp HandleApplyCacheVariantCount.
function deInitCacheVariantControls() {
  // `slider` is the "Change to" edit box (pending value); `currentEl` shows
  // the count actually in effect, which the host only changes once every
  // sample for a new count has been baked.
  const slider = document.getElementById('deCacheVariantInput');
  const currentEl = document.getElementById('deCacheVariantCurrent');
  const applyBtn = document.getElementById('deCacheApplyBtn');
  let appliedCount = currentEl ? Number(currentEl.textContent) || 7 : 7;

  const pendingValue = () => {
    const n = Math.round(Number(slider.value));
    return Number.isFinite(n) ? Math.min(20, Math.max(1, n)) : appliedCount;
  };
  const showAppliedCount = () => {
    if (slider) slider.value = String(appliedCount);
    if (currentEl) currentEl.textContent = String(appliedCount);
  };

  if (slider) {
    slider.addEventListener('change', () => { slider.value = String(pendingValue()); });
    slider.addEventListener('keydown', (e) => { if (e.key === 'Enter' && applyBtn) applyBtn.click(); });
  }
  if (applyBtn) {
    applyBtn.addEventListener('click', () => {
      if (typeof hostBridge === 'undefined' || !hostBridge.send || !slider) return;
      slider.value = String(pendingValue());
      hostBridge.send('queryCacheBakeEstimate', { value: pendingValue() });
    });
  }

  if (typeof hostBridge === 'undefined' || !hostBridge.on) return;

  // Cache size limit (machine-wide, stored by the host). Lowering it below
  // the current cache size deletes sets immediately, so that asks first.
  const kGiB = 1024 * 1024 * 1024;
  const limitSelect = document.getElementById('deCacheLimitSelect');
  const usageLabel = document.getElementById('deCacheUsageLabel');
  let cacheInfo = { usedBytes: 0, files: 0, limitGiB: limitSelect ? Number(limitSelect.value) : 5 };
  const formatGiB = (bytes) => (bytes / kGiB).toFixed(bytes < 10 * kGiB ? 2 : 1) + ' GB';
  const requestCacheInfo = () => hostBridge.send('queryCacheInfo', {});

  const showCacheInfo = () => {
    if (limitSelect) {
      if (![...limitSelect.options].some(o => Number(o.value) === cacheInfo.limitGiB)) {
        const opt = document.createElement('option');
        opt.value = String(cacheInfo.limitGiB);
        opt.textContent = cacheInfo.limitGiB + ' GB';
        limitSelect.appendChild(opt);
      }
      limitSelect.value = String(cacheInfo.limitGiB);
    }
    if (usageLabel) usageLabel.textContent = 'Using ' + formatGiB(cacheInfo.usedBytes) + ' (' + cacheInfo.files + ' files)';
    if (rebakeCheck) rebakeCheck.checked = !!cacheInfo.rebakeOnChange;
    if (cacheOffHiHat) cacheOffHiHat.checked = !!cacheInfo.cacheOffHiHat;
    if (cacheOffRide) cacheOffRide.checked = !!cacheInfo.cacheOffRide;
  };

  const cacheOffHiHat = document.getElementById('deCacheOffHiHatCheck');
  const cacheOffRide = document.getElementById('deCacheOffRideCheck');
  if (cacheOffHiHat) cacheOffHiHat.addEventListener('change', () => hostBridge.send('setCacheDisabled', { instrument: 'hihat', disabled: cacheOffHiHat.checked }));
  if (cacheOffRide) cacheOffRide.addEventListener('change', () => hostBridge.send('setCacheDisabled', { instrument: 'ride', disabled: cacheOffRide.checked }));
  const rebakeCheck = document.getElementById('deRebakeOnChangeCheck');
  if (rebakeCheck) {
    rebakeCheck.addEventListener('change', () => {
      hostBridge.send('setRebakeOnChange', { on: rebakeCheck.checked });
    });
  }

  hostBridge.on('cacheInfo', (payload) => {
    if (!payload) return;
    cacheInfo = payload;
    showCacheInfo();
  });

  if (limitSelect) {
    limitSelect.addEventListener('change', async () => {
      const gib = Number(limitSelect.value);
      if (gib * kGiB < cacheInfo.usedBytes) {
        const ok = await showModal({
          title: 'Lower Cache Limit',
          message: 'The cache currently uses <b>' + formatGiB(cacheInfo.usedBytes) + '</b>. A limit of <b>' + gib
            + ' GB</b> deletes the least recently used sets right now (factory defaults are kept). Continue?',
          confirmLabel: 'Delete and Apply', cancelLabel: 'Cancel', danger: true,
        });
        if (!ok) { showCacheInfo(); return; }
      }
      hostBridge.send('setCacheLimit', { gib });
    });
  }

  const cacheMenuBtn = document.getElementById('deCacheMenuBtn');
  if (cacheMenuBtn) cacheMenuBtn.addEventListener('click', requestCacheInfo);
  requestCacheInfo();

  hostBridge.on('cacheVariantCount', (payload) => {
    if (!payload || typeof payload.value !== 'number') return;
    appliedCount = payload.value;
    showAppliedCount();
  });

  hostBridge.on('cacheBakeEstimate', async (payload) => {
    if (!payload || typeof payload.value !== 'number') return;
    const { value, missing } = payload;
    let ok = true;
    if (missing > 0) {
      const perFile = payload.avgBakeMs > 0 ? payload.avgBakeMs / Math.max(1, payload.threads) / 1000 : 0;
      const seconds = missing * perFile;
      const timeLine = perFile > 0
        ? 'Estimated time: about ' + deFormatBakeDuration(seconds) + ' (based on this session’s bakes).'
        : 'Time can’t be estimated yet — no sample has been baked in this session.';
      const big = missing >= 100 || seconds >= 300;
      const overLimit = payload.projectedBytes > payload.limitBytes;
      ok = await showModal({
        title: 'Apply ' + value + ' Variants',
        message: 'This bakes <b>' + missing + '</b> new sample file' + (missing === 1 ? '' : 's')
          + ' in the background, using most CPU cores.<br>' + timeLine
          + (big ? '<br><br><b>This is a large job.</b> Playback may stutter and new sounds stay unavailable until baking finishes.' : '')
          + (overLimit ? '<br><br><b>The cache would grow to about ' + (payload.projectedBytes / (1024 * 1024 * 1024)).toFixed(1)
            + ' GB, above your ' + (payload.limitBytes / (1024 * 1024 * 1024)).toFixed(0) + ' GB limit.</b> '
            + 'Baking would delete older sets again and cannot finish. Raise the size limit first.' : '')
          + '<br><br>Existing variants are kept. Continue?',
        confirmLabel: 'Bake',
        cancelLabel: 'Cancel',
        danger: big || overLimit,
      });
    }
    if (!ok) { showAppliedCount(); return; }
    if (missing > 0) openBakePopup(missing, appliedCount);
    else {
      const statusEl = document.getElementById('statusText');
      if (statusEl) {
        statusEl.textContent = 'Nothing to bake — all files for ' + value + ' variants already exist';
        setTimeout(() => { if (statusEl.textContent.startsWith('Nothing to bake')) statusEl.textContent = 'ready'; }, 5000);
      }
    }
    hostBridge.send('applyCacheVariantCount', { value });
  });

  // Modal progress popup for an applied variant count: stays until the host
  // has verified that every file for the confirmed count exists (or reports
  // that it could not get them all), or the user cancels (which drops the
  // queued jobs and restores the previous variant count). Files baked so far
  // are kept, so applying again resumes where it stopped.
  let bakePopup = null;
  function closeBakePopup() {
    if (!bakePopup) return;
    bakePopup.overlay.remove();
    bakePopup = null;
  }
  function openBakePopup(total, previousCount) {
    closeBakePopup();
    const overlay = document.createElement('div');
    overlay.className = 'app-modal-overlay';
    const box = document.createElement('div');
    box.className = 'app-modal-box';
    box.innerHTML = '<div class="app-modal-title">Baking Cache Variants</div>'
      + '<div class="app-modal-message"><span class="de-bake-popup-text">Starting…</span></div>'
      + '<div style="width:100%; height:8px; background:rgba(255,255,255,0.15); border-radius:3px; overflow:hidden; margin-top:10px;">'
      + '<div class="de-bake-popup-bar" style="height:100%; width:0%; background:var(--accent, #6ac); transition:width 0.15s linear;"></div></div>'
      + '<div class="app-modal-actions"><button class="btn btn-sm btn-danger de-bake-popup-cancel">Cancel</button></div>';
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    const cancelBtn = box.querySelector('.de-bake-popup-cancel');
    cancelBtn.addEventListener('click', () => {
      cancelBtn.disabled = true;
      cancelBtn.textContent = 'Cancelling…';
      hostBridge.send('cancelCacheBake', { value: previousCount });
    });
    bakePopup = {
      overlay, total,
      textEl: box.querySelector('.de-bake-popup-text'),
      barEl: box.querySelector('.de-bake-popup-bar'),
    };
  }

  hostBridge.on('cacheBakeCancelled', () => {
    closeBakePopup();
    const statusEl = document.getElementById('statusText');
    if (statusEl) {
      statusEl.textContent = 'Bake cancelled';
      setTimeout(() => { if (statusEl.textContent === 'Bake cancelled') statusEl.textContent = 'ready'; }, 5000);
    }
  });

  hostBridge.on('cacheApplyProgress', async (payload) => {
    if (!bakePopup || !payload) return;
    const p = bakePopup;
    const total = Math.max(p.total, payload.total || 0, 1);
    const finished = Math.min(total, Math.max(0, total - (payload.remaining || 0)));
    p.textEl.textContent = finished + ' / ' + total + ' files verified';
    p.barEl.style.width = Math.round(100 * finished / total) + '%';
    if (payload.active) return;
    closeBakePopup();
    hostBridge.send('queryCacheInfo', {});
    const statusEl = document.getElementById('statusText');
    if (payload.verified) {
      if (statusEl) {
        statusEl.textContent = 'Cache variants ready';
        setTimeout(() => { if (statusEl.textContent === 'Cache variants ready') statusEl.textContent = 'ready'; }, 5000);
      }
    } else {
      await showModal({
        title: 'Baking Incomplete',
        message: payload.remaining + ' file' + (payload.remaining === 1 ? ' is' : 's are') + ' still missing and could not be baked '
          + '(disk error, or the cache size limit removed sets). Apply again to retry the missing ones.',
        confirmLabel: 'OK', cancelLabel: 'Close', danger: true,
      });
    }
  });

  // Clear actions: "cache" deletes every baked set that is not a
  // factory-default set and rebakes what's needed on demand; "default"
  // deletes the factory-default sets and bakes them again from the
  // 808-*-defaults.js tables.
  const kClearCacheTiers = [
    { btnId: 'deClearCacheAllBtn', tier: 'cache', label: 'Cache',
      message: 'Deletes all baked sound variants except the factory defaults.<br>They’ll rebake automatically as needed — continue?' },
    { btnId: 'deClearDefaultCacheBtn', tier: 'default', label: 'Default Cache',
      message: 'Deletes the factory-default sound variants and bakes them again.<br>Only do this if you suspect they’re corrupted — continue?' },
  ];
  for (const { btnId, tier, label, message } of kClearCacheTiers) {
    const btn = document.getElementById(btnId);
    if (!btn) continue;
    btn.addEventListener('click', async () => {
      const ok = await showModal({
        title: 'Clear ' + label,
        message,
        confirmLabel: 'Clear',
        cancelLabel: 'Cancel',
        danger: true,
      });
      if (!ok) return;
      const statusEl = document.getElementById('statusText');
      if (statusEl) statusEl.textContent = 'Clearing ' + label + '…';
      if (typeof hostBridge !== 'undefined' && hostBridge.send) hostBridge.send('clearCache', { tier });
    });
  }

  hostBridge.on('cacheCleared', (payload) => {
    hostBridge.send('queryCacheInfo', {});
    const statusEl = document.getElementById('statusText');
    if (!statusEl) return;
    const tierInfo = kClearCacheTiers.find(t => t.tier === (payload && payload.tier));
    const mb = payload && typeof payload.bytesFreed === 'number' ? (payload.bytesFreed / (1024 * 1024)) : 0;
    statusEl.textContent = `${tierInfo ? tierInfo.label : 'Cache'} cleared (${mb.toFixed(1)} MB freed)`;
    setTimeout(() => { if (statusEl.textContent.endsWith('MB freed)')) statusEl.textContent = 'ready'; }, 5000);
  });

  hostBridge.on('cacheBakeProgress', (payload) => {
    deUpdateCacheBakeDialog(payload);
    // Uses the shared transport status text (visible on every tab) and only when it is not already showing
    // something else (playing/export/...).
    const statusEl = document.getElementById('statusText');
    if (statusEl) {
      // The text follows the message, not the previous text: a cache bake ("Initializing cache…") wins over a temporary preview render ("Rendering preview…"),
      // neither touches a text that something else put there (playing, export, ...).
      const ours = (t) => t === 'ready' || t === 'Initializing cache…' || t === 'Rendering preview…';
      const want = (payload && payload.active) ? 'Initializing cache…' : (payload && payload.preview) ? 'Rendering preview…' : 'ready';
      if (ours(statusEl.textContent) && statusEl.textContent !== want) statusEl.textContent = want;
    }

    const row = document.getElementById('deCacheBakeProgressRow');
    const bar = document.getElementById('deCacheBakeProgressBar');
    const label = document.getElementById('deCacheBakeProgressLabel');
    if (!row || !bar || !label) return;
    if (!payload || !payload.active || !payload.total) {
      row.style.display = 'none';
      return;
    }
    row.style.display = 'block';
    const pct = Math.round(100 * (payload.done / payload.total));
    bar.style.width = pct + '%';
    label.textContent = 'Baking variants: ' + payload.done + '/' + payload.total;
  });
}

let deEngineMode = 'internal';

function deInitEngineModeControls() {
  const select = document.getElementById('deEngineSelect');
  if (!select) return;

  select.addEventListener('change', () => {
    deEngineMode = select.value;
    // The values in these histories belong to the previous engine; undoing them would write them into the new one.
    clearUndoHistory('drumEngine');
    clearUndoHistory('mixer');
    deRefreshParamPanelForFamily(deFocusFamily);
    if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
    hostBridge.send('setEngineMode', { engine: deEngineMode });
  });

  if (typeof hostBridge !== 'undefined' && hostBridge.on) {
    hostBridge.on('engineModeState', (payload) => {
      if (!payload) return;
      deEngineMode = payload.engine || 'internal';
      select.value = deEngineMode;
      deRefreshParamPanelForFamily(deFocusFamily);
    });
  }
}

function deRefreshEngineSelectDisplay() {
  const select = document.getElementById('deEngineSelect');
  if (select) select.value = deEngineMode;
}

// The Cache menu only applies to the internal engine; Lo-Fi plays samples and bakes nothing.
function deCacheMenuShouldShow(activeTab) {
  return activeTab === 'drumEngine' && deEngineMode !== 'lofi';
}
window.deCacheMenuShouldShow = deCacheMenuShouldShow;

function deApplyCacheMenuVisibility() {
  const wrap = document.getElementById('deCacheMenuWrap');
  if (!wrap) return;
  const show = deCacheMenuShouldShow('drumEngine');
  const tabActive = document.querySelector('.tab-panel.active')?.dataset.tabPanel === 'drumEngine';
  wrap.style.display = (show && tabActive) ? '' : 'none';
  if (!show) {
    const menu = document.getElementById('deCacheMenu');
    if (menu) menu.style.display = 'none';
  }
}

function deRefreshParamPanelForFamily(family) {
  deApplyCacheMenuVisibility();
  if (family === 'custom') return; // engine-agnostic roster for now
  const panel = document.querySelector(`.de-param-panel[data-family="${family}"]`);
  if (!panel) return;
  const isLoFi = deEngineMode === 'lofi';

  // Lo-Fi has no per-parameter synthesis, so the adjustable controls (knob groups, XY pads, slider rows) are
  // hidden and each voice keeps its chrome (name + Reset + Volume). The pad wrapper .de-instance-card-pad-zoom
  // is hidden as well, otherwise it is an empty flex item adding dead space at the card's right edge.
  panel.querySelectorAll('.de-knob-groups-row, .de-kick-pad, .de-instance-card-pad-zoom, .de-param-instance-row').forEach((el) => {
    el.style.display = isLoFi ? 'none' : '';
  });
}

let deEngineParamsBuilt = false;

// =========================================
// Sound-cache dialog
// =========================================
// The host bakes the sound of every instrument once (first start, cleared cache, Cache panel "Apply") and until a sound is baked its
// instrument is silent, so a larger bake gets a dialog: what is going on, which instruments are being worked on, and how far it is.
// Fed by the host's 'cacheBakeProgress' message; shown for the first-start bake (also after the cache was cleared). Any later bake,
// e.g. of the sounds of a knob edit, only shows in the status line.
const DE_CACHE_NAMES = { ride: 'Ride', hihat: 'Hi-Hat', cymbal: 'Cymbals', kick: 'Kick', snare: 'Snare', tom: 'Toms', rimshot: 'Rim Shot', claves: 'Claves', handclap: 'Hand Clap', maracas: 'Maracas', hc: 'Hand Clap / Maracas', cowbell: 'Cowbell', cb: 'Cowbell', pc2: 'PC-2' };
let deCacheDialog = null;

function deCacheNameList(keys) {
  return keys.map((k) => DE_CACHE_NAMES[k] || k).join(', ');
}

function deBuildCacheDialog() {
  const overlay = document.createElement('div');
  overlay.className = 'app-modal-overlay';
  overlay.id = 'deCacheDialog';
  const box = document.createElement('div');
  box.className = 'app-modal-box';
  box.style.width = '440px';
  const el = (cls, text) => { const d = document.createElement('div'); d.className = cls; if (text) d.textContent = text; return d; };
  const title = el('app-modal-title');
  const message = el('app-modal-message');
  const activity = el('app-modal-message');
  activity.style.color = 'var(--text)';
  const barOuter = document.createElement('div');
  barOuter.style.cssText = 'width:100%; height:6px; background:rgba(255,255,255,0.15); border-radius:3px; overflow:hidden;';
  const bar = document.createElement('div');
  bar.style.cssText = 'height:100%; width:0%; background:var(--accent, #6ac); transition:width 0.2s linear;';
  barOuter.appendChild(bar);
  const count = el('app-modal-message');
  count.style.margin = '6px 0 0';
  const actions = el('app-modal-actions');
  // Cancel instead of Hide: while the bake runs there is nothing to do behind the dialog, but letting it run is a
  // choice -- running jobs finish and write their file, queued ones are dropped, and the next bake resumes where this
  // one left off (C++ HandleCancelCacheBake).
  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'btn';
  cancel.textContent = 'Cancel';
  cancel.dataset.tooltip = 'Cancel\nStops the calculation now. Finished sounds stay on disk; the rest is picked up where it left off the next time a bake runs.';
  cancel.addEventListener('click', () => {
    if (deCacheDialog) { deCacheDialog.overlay.remove(); deCacheDialog = null; }
    const statusEl = document.getElementById('statusText');
    if (statusEl) statusEl.textContent = 'Cache bake cancelled';
    if (typeof hostBridge !== 'undefined' && hostBridge.send) hostBridge.send('cancelCacheBake', {});
  });
  actions.appendChild(cancel);
  box.append(title, message, activity, barOuter, count, actions);
  overlay.appendChild(box);
  document.body.appendChild(overlay);
  return { overlay, title, message, activity, bar, count };
}

function deUpdateCacheBakeDialog(p) {
  const busy = !!p && (p.pendingJobs || 0) > 0;
  const show = busy && !!p.firstRun;
  if (!show) {
    if (deCacheDialog) { deCacheDialog.overlay.remove(); deCacheDialog = null; }
    return;
  }
  if (!deCacheDialog) deCacheDialog = deBuildCacheDialog();
  const d = deCacheDialog;
  const labels = p.labels || [];
  d.title.textContent = p.firstRun ? 'Preparing the sound cache' : 'Updating the sound cache';
  d.message.textContent = p.firstRun
    ? 'First start: the sounds of all instruments are calculated and stored once. An instrument stays silent until its sound is ready. This only happens on the first start, and again after the cache was cleared.'
    : 'Sounds are calculated and stored. An instrument stays silent until its sound is ready.';
  d.activity.textContent = labels.length ? 'Calculating: ' + deCacheNameList(labels) : 'Calculating the next sounds…';
  const total = p.unitsTotal || 0;
  const frac = total > 0 ? (p.unitsDone || 0) / total : 0;
  d.bar.style.width = Math.round(100 * Math.min(1, Math.max(0, frac))) + '%';
  d.count.textContent = total > 0 ? (p.unitsDone || 0) + ' of ' + total + ' sounds ready' : '';
}

// Tells the host the UI is up: it writes default.index and bakes the factory-default sets. The defaults themselves are the host's
// instrument table's, nothing is sent with the message. app.js calls this once the UI has finished loading, so on first start the
// bake dialog is visible from the beginning instead of most of the cache already being written by then.
function deSendFactoryDefaults() {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  hostBridge.send('setFactoryDefaults', {});
}

function deInitEngineParams() {
  if (deEngineParamsBuilt || !INSTRUMENT_TABLE) return;
  deEngineParamsBuilt = true;
  deApplyInstrumentTable();

  deInitPolyphonyControls();
  deInitEngineModeControls();
  deInitVoiceParamTabs();

  const legend = document.getElementById('deLegend');
  if (legend) {
    legend.innerHTML = '';
    for (const item of DE_LEGEND_ITEMS) {
      const el = document.createElement('div');
      el.className = 'de-legend-item';
      el.innerHTML = `<span class="de-legend-swatch" style="background:${deFamilyLegendColor(item.family)}"></span><span>${item.label}</span>`;
      legend.appendChild(el);
    }
  }
}

function runDeInitEngineParamsWhenReady() {
  const start = () => dggOnInstrumentTable(deInitEngineParams);
  if (typeof dggOnReady === 'function') {
    dggOnReady(start);
  } else {
    start();
  }
}
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', runDeInitEngineParamsWhenReady);
} else {
  runDeInitEngineParamsWhenReady();
}

let deKeyboardBuilt = false;

function initDrumEngineTab() {
  if (deKeyboardBuilt || !INSTRUMENT_TABLE) return;
  deKeyboardBuilt = true;

  deInitEngineParams();
  deInitUndo();

  const keyboard = document.getElementById('deKeyboard');
  if (!keyboard) return;
  keyboard.innerHTML = '';

  const mapByNote = new Map(NATIVE_ENGINE_MAP.map(e => [e.note, e]));
  const notes = NATIVE_ENGINE_MAP.map(e => e.note);
  const minNote = Math.min(...notes);
  const maxNote = Math.max(...notes);

  let rangeStart = minNote - 2;
  while (deIsBlackNote(rangeStart)) rangeStart--;
  let rangeEnd = maxNote + 2;
  while (deIsBlackNote(rangeEnd)) rangeEnd++;

  let whiteCount = 0;
  const keyEntries = [];
  const blackEls = [];

  for (let note = rangeStart; note <= rangeEnd; note++) {
    const mapped = mapByNote.get(note);
    const black = deIsBlackNote(note);
    const key = document.createElement('div');
    key.className = 'de-key ' + (black ? 'de-key-black' : 'de-key-white');
    key.dataset.note = String(note);
    key.dataset.tooltip = mapped ? `${mapped.name}\nNote ${note}. Click and hold to audition it.` : `Note ${note}\nNot mapped to an articulation yet.`;

    if (mapped) {
      key.dataset.family = mapped.family;
      key.style.background = deArticulationColor(mapped, black);
      if (!black) key.textContent = mapped.name;
    }

    keyEntries.push({ el: key, black, whiteIndex: whiteCount });
    if (black) {
      blackEls.push(key);
    } else {
      whiteCount++;
    }

    keyboard.appendChild(key);
  }

  blackEls.forEach(el => keyboard.appendChild(el));

  function deLayoutKeyboard() {
    const wrapW = keyboard.parentElement.clientWidth;
    if (!wrapW || whiteCount === 0) return;
    const whiteW = wrapW / whiteCount;
    const blackW = whiteW * DE_BLACK_KEY_RATIO;
    for (const entry of keyEntries) {
      if (entry.black) {
        entry.el.style.left = (entry.whiteIndex * whiteW - blackW / 2) + 'px';
        entry.el.style.width = blackW + 'px';
      } else {
        entry.el.style.left = (entry.whiteIndex * whiteW) + 'px';
        entry.el.style.width = whiteW + 'px';
      }
    }
  }

  deLayoutKeyboard();
  window.addEventListener('resize', deLayoutKeyboard);
  window.deLayoutDrumEngineKeyboard = deLayoutKeyboard;

  let activeNote = null;

  function pressKey(note, el) {
    if (activeNote !== null) releaseKey();
    activeNote = note;
    if (el) el.classList.add('active');
    deSendMidi('midiNoteOn', note);
  }

  function releaseKey() {
    if (activeNote === null) return;
    const el = keyboard.querySelector(`.de-key[data-note="${activeNote}"]`);
    if (el) el.classList.remove('active');
    deSendMidi('midiNoteOff', activeNote);
    activeNote = null;
  }

  keyboard.addEventListener('mousedown', (e) => {
    const el = e.target.closest('.de-key');
    if (!el) return;
    pressKey(Number(el.dataset.note), el);
  });
  window.addEventListener('mouseup', releaseKey);
  keyboard.addEventListener('mouseleave', releaseKey);

  if (typeof hostBridge !== 'undefined' && hostBridge.on) {
    hostBridge.on('nativeNoteOn', (payload) => {
      const el = keyboard.querySelector(`.de-key[data-note="${payload && payload.note}"]`);
      if (el) el.classList.add('active');
    });
    hostBridge.on('nativeNoteOff', (payload) => {
      const el = keyboard.querySelector(`.de-key[data-note="${payload && payload.note}"]`);
      if (el) el.classList.remove('active');
    });
  }
}

window.initDrumEngineTab = initDrumEngineTab;