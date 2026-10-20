// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// MIXER TAB — port of the mixer.html mockup (layout/interaction prototype).
// Kept in an IIFE so its helpers (state, createKnob, renderStrip, ...) don't
// collide with the rest of the app.
// =========================================
(() => {
  const FAMILY_HSL = {
    kick: { h:4, s:70 }, snare: { h:32, s:68 }, hihat: { h:48, s:72 },
    tom: { h:130, s:45 }, cymbal: { h:205, s:62 }, ride: { h:275, s:48 },
    // 'custom' = the catch-all slot for every user-added track (role 'custom',
    // added via the visible "+ Add Track" button above the track list); they act as its articulations. Pink, matching
    // the custom track color in generator-core.js trackList.
    custom: { h:330, s:70 },
  };
  function familyColor(fam) {
    const c = FAMILY_HSL[fam];
    return c ? `hsl(${c.h} ${c.s}% 55%)` : '#64748b';
  }

  // Single source of truth for the mixer's default strip settings. Change a
  // value here and the initial state, the fader HTML + dB label, the Ctrl+click
  // reset, and the Pan/Rev/Del knob defaults all follow automatically — no
  // second place to edit.
  const MIXER_DEFAULTS = {
    gainDb: 0,         // fader position (dB) — unity, no built-in headroom cut
    pan: 0,            // center
    sendReverb: 0,     // off
    sendDelay: 0,      // off
  };

  const FIXED_CHANNELS = [
    { id:'kick', name:'Kick', family:'kick' },
    { id:'snare', name:'Snare', family:'snare' },
    { id:'hihat', name:'Hi-Hat', family:'hihat' },
    { id:'rackTom1', name:'Ra Tom 1', family:'tom' },
    { id:'rackTom2', name:'Ra Tom 2', family:'tom' },
    { id:'rackTom3', name:'Ra Tom 3', family:'tom' },
    { id:'floorTom1', name:'Fl Tom 1', family:'tom' },
    { id:'floorTom2', name:'Fl Tom 2', family:'tom' },
    { id:'cymbal1', name:'Cymbal 1', family:'cymbal' },
    { id:'cymbal2', name:'Cymbal 2', family:'cymbal' },
    { id:'cymbal3', name:'Cymbal 3', family:'cymbal' },
    { id:'cymbal4', name:'Cymbal 4', family:'cymbal' },
    { id:'cymbal5', name:'Cymbal 5', family:'cymbal' },
    { id:'ride', name:'Ride', family:'ride' },
    // Catch-all channel: ALL custom tracks (trackList entries with role
    // 'custom', added via the "+ Add Track" button above the track list) route through this one slot,
    // as if they were its articulations. Must stay LAST — the fixed kit
    // channels above keep their order.
    { id:'custom', name:'Custom', family:'custom' },
  ];

  const FX_DEFS = {
    eq:  { label:'EQ', title:'EQ (3-Band)', eq:true, params:[
            {k:'lowFreq',  label:'Low Freq',  min:20,   max:1000,  def:250,  unit:'Hz'},
            {k:'lowGain',  label:'Low Gain',  min:-18,  max:18,    def:0,    unit:'dB', type:'slider'},
            {k:'lowQ',     label:'Low Q',     min:0.3,  max:4,     def:3,    unit:''},
            {k:'midFreq',  label:'Mid Freq',  min:200,  max:8000,  def:1000, unit:'Hz'},
            {k:'midGain',  label:'Mid Gain',  min:-18,  max:18,    def:0,    unit:'dB', type:'slider'},
            {k:'midQ',     label:'Mid Q',     min:0.3,  max:12,    def:1,    unit:''},
            {k:'highFreq', label:'High Freq', min:1000, max:16000, def:4000, unit:'Hz'},
            {k:'highGain', label:'High Gain', min:-18,  max:18,    def:0,    unit:'dB', type:'slider'},
            {k:'highQ',    label:'High Q',    min:0.3,  max:4,     def:3,    unit:''} ] },
    comp:{ label:'Compressor', title:'Compressor', params:[
            {k:'threshold', label:'Threshold', min:-40, max:0, def:-18, unit:'dB'},
            {k:'ratio', label:'Ratio', min:1, max:20, def:4, unit:':1'},
            {k:'attack', label:'Attack', min:0.1, max:100, def:10, unit:'ms'},
            {k:'release', label:'Release', min:10, max:1000, def:150, unit:'ms'},
            {k:'makeup', label:'Makeup', min:0, max:24, def:0, unit:'dB'} ] },
    sat: { label:'Saturation',  title:'Saturation', params:[
            {k:'drive', label:'Drive', min:0, max:100, def:20, unit:'%'} ] },
    ts:  { label:'Transient Shaper',   title:'Transient Shaper', params:[
            {k:'attack', label:'Attack', min:-100, max:100, def:0, unit:''},
            {k:'sustain', label:'Sustain', min:-100, max:100, def:0, unit:''} ] },
    reverb: { label:'Reverb', title:'Reverb', params:[
            {k:'size', label:'Size', min:0, max:100, def:50, unit:'%'},
            {k:'decay', label:'Decay', min:0.1, max:10, def:2, unit:'s'},
            {k:'damping', label:'Damping', min:0, max:100, def:40, unit:'%'},
            {k:'mix', label:'Mix', min:0, max:100, def:100, unit:'%'} ] },
    delay: { label:'Delay', title:'Delay', params:[
            {k:'time', label:'Time', min:1, max:1000, def:250, unit:'ms'},
            {k:'feedback', label:'Feedback', min:0, max:95, def:30, unit:'%'},
            {k:'mix', label:'Mix', min:0, max:100, def:25, unit:'%'},
            {k:'mode', label:'Mode', min:0, max:3, def:1, step:1,
             fmt:(v)=>['Mono','Stereo','Ping-Pong','Off'][Math.round(v)]} ] },
    lim: { label:'Limiter', title:'Limiter', params:[
            {k:'ceiling', label:'Ceiling', min:-32, max:12, def:-0.3, step:0.1, unit:'dB'},
            {k:'attack',  label:'Attack',  min:0,  max:5,  def:0.5, unit:'ms'},
            {k:'release', label:'Release', min:1, max:500, def:50, unit:'ms'} ] },
  };

  // Shared tooltips for FX parameters — one text per parameter, used identically in the
  // bus knobs and the effect popovers (same function = same wording).
  const FX_PARAM_TIPS = {
    eq: { freq:'Frequency\nCenter or cutoff frequency of this band in Hz.',
          q:'Q\nBandwidth of this band — higher = narrower, more focused.',
          gain:'Gain\nBoosts or cuts this band by up to ±18 dB.' },
    comp:{ threshold:'Threshold\nLevel above which the compressor starts reducing gain.',
           ratio:'Ratio\nHow hard it compresses above the threshold.',
           attack:'Attack\nHow fast the compressor reacts once the signal crosses the threshold.',
           release:'Release\nHow long the compressor holds after the signal falls below the threshold.',
           makeup:'Makeup\nGain added after compression to bring the level back up.' },
    sat: { drive:'Drive\nAmount of saturation added to the signal.' },
    ts:  { attack:'Attack\nBoosts (positive) or cuts (negative) the transient onset of each hit.',
           sustain:'Sustain\nBoosts (positive) or cuts (negative) the body after the transient.' },
    reverb:{ size:'Size\nPerceived size of the reverb space — small room to large hall.',
             decay:'Decay\nLength of the reverb tail in seconds.',
             damping:'Damping\nDarkens the reverb tail — higher values roll off the high frequencies.',
             mix:'Mix\nBlend between dry and effected signal — 0% = dry, 100% = fully wet.' },
    delay:{ time:'Time\nDelay time in milliseconds.',
            feedback:'Feedback\nEcho repeats — higher = more and louder repeats.',
            mode:'Mode\nStereo behavior: Mono, Stereo, Ping-Pong or Off.',
            mix:'Mix\nBlend between dry and effected signal — 0% = dry, 100% = fully wet.' },
    lim: { ceiling:'Ceiling\nMaximum output level — nothing above this gets through.',
           attack:'Attack\nLook-ahead time: how long before a peak the limiter starts turning down. Longer = smoother, adds the same delay.',
           release:'Release\nHow quickly the limiter lets go after a peak.' },
  };
  const SLOT_COUNT = 4;
  // Fresh boot: instrument channels start with EMPTY FX slots — the user
  // builds their own chain. Only the master gets a sensible default (EQ ->
  // Limiter); its other slots stay empty too.
  const CHANNEL_FX_DEFAULT = [null,null,null,null];
  const MASTER_FX_DEFAULT = ['eq','lim',null,null];
  // Master FX chain: EQ, Limiter.
  const MASTER_FX = ['eq','lim'];

  const BUS_CHANNELS = [
    { id:'reverbBus', name:'Reverb', isBus:true, fxKeys:['reverb'] },
    { id:'delayBus', name:'Delay', isBus:true, fxKeys:['delay'] },
  ];

  // state[id] = { gainDb, pan, mute, solo, sendReverb, sendDelay, fx:{key:{...params}}, meter }
  const state = {};

  // Solo is exclusive: switching one channel on switches every other off.
  function setExclusiveSolo(id, on) {
    for (const k of Object.keys(state)) {
      const t = state[k];
      if (!t.soloBtn) continue;
      if (k === id) t.solo = on;
      else if (on) t.solo = false;
      t.soloBtn.classList.toggle('active', t.solo);
    }
  }
  // Kit Gain: one gain on the input of every instrument channel (before EQ / FX / fader), -24..+24 dB, saved with the mixer state.
  const KIT_GAIN_MIN = -24, KIT_GAIN_MAX = 24;
  const kitState = { gainDb: 0, autoTargetDb: 0, faderEl: null, dbVal: null };   // 808 default; the host pushes the state of the selected engine
  const presetState = { name: '', names: [], snapshot: null, modified: false };   // mixer presets (see the Presets block at the end)
  function kitGainDefault() { return 0; }

  function buildChannelList() {
    return FIXED_CHANNELS.map(c => ({ ...c, kind:'instrument' }));
  }

  function makeFxState(fxKeys, fxDefs) {
    const fx = {};
    for (const key of fxKeys) {
      const def = fxDefs[key];
      fx[key] = { on:true, params:{} };
      // Each channel gets its own params object to avoid shared references.
      for (const p of def.params) fx[key].params[p.k] = p.def;
    }
    return fx;
  }

  function panLabel(pan) {
    if (Math.abs(pan) < 0.005) return '0';
    const pct = Math.round(Math.abs(pan) * 100);
    return (pan < 0 ? 'L ' : 'R ') + pct;
  }

  // Short knob labels for the bus param knobs (fit the ~26px column width).
  function busKnobShort(k) {
    switch (k) {
      case 'size':     return 'Size';
      case 'decay':    return 'Decay';
      case 'damping':  return 'Damp';
      case 'time':     return 'Time';
      case 'feedback': return 'FB';
      case 'mode':     return 'Mode';
      default:         return k;
    }
  }
  function busKnobFmt(v, unit) {
    const r = Math.round(v * 100) / 100; // Float-Noise weg (Decay 0.2914… -> 0.29)
    if (unit === '%')  return Math.round(v) + '%';
    if (unit === 's')  return r + 's';
    if (unit === 'ms') return r + 'ms';
    return String(r);
  }

  // Rotary knob: vertical drag changes value, tooltip shows value while dragging. UI only, no DSP.
  let _knobTooltipEl = null;
  function knobTooltip() {
    if (!_knobTooltipEl) {
      _knobTooltipEl = document.createElement('div');
      _knobTooltipEl.className = 'knob-tooltip';
      document.body.appendChild(_knobTooltipEl);
    }
    return _knobTooltipEl;
  }
  function createKnob(container, opts) {
    const { label, title, min, max, def, format, onChange, step = 1 } = opts;
    container.classList.add('knob-wrap');
    // Layout like the rest of the VST (.ctrl): label on top, value below — always visible.
    container.innerHTML = `<div class="knob-label">${label}</div>`
      + `<div class="knob-visual"><div class="knob-marker"><div class="marker-dot"></div></div></div>`
      + `<div class="knob-value"></div>`;
    const knobEl = container.querySelector('.knob-visual');
    // Custom tooltip system (data-tooltip), not the native title attribute.
    knobEl.dataset.tooltip = title || label;
    const marker = container.querySelector('.knob-marker');
    const valEl  = container.querySelector('.knob-value');
    const range = max - min;
    let value = def;

    function render() {
      const t = (value - min) / range;
      // 270° sweep, common for audio knobs.
      const deg = -135 + t * 270;
      marker.style.transform = `rotate(${deg}deg)`;
      if (valEl) valEl.textContent = format(value);
    }
    function showTooltip() {
      const tip = knobTooltip();
      const r = knobEl.getBoundingClientRect();
      tip.style.left = (r.left + r.width / 2) + 'px';
      tip.style.top = (r.top - 6) + 'px';
      tip.textContent = format(value);
      tip.classList.add('show');
    }
    function hideTooltip() { knobTooltip().classList.remove('show'); }

    knobEl.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      // Ctrl+Click → Reset to default value.
      if (e.ctrlKey || e.metaKey) { value = def; render(); onChange(value); return; }
      knobEl.setPointerCapture(e.pointerId);
      knobEl.classList.add('active');
      const startY = e.clientY;
      const startVal = value;
      showTooltip();
      function onMove(ev) {
        const dy = startY - ev.clientY;
        // step=1 -> continuous; otherwise snap to whole steps (e.g. mode knob).
        const raw = startVal + (dy / 140) * range;
        value = Math.max(min, Math.min(max, Math.round((raw - min) / step) * step + min));
        render();
        showTooltip();
        onChange(value);
      }
      function onUp() {
        knobEl.removeEventListener('pointermove', onMove);
        knobEl.removeEventListener('pointerup', onUp);
        knobEl.classList.remove('active');
        hideTooltip();
      }
      knobEl.addEventListener('pointermove', onMove);
      knobEl.addEventListener('pointerup', onUp);
    });

    // Mouse wheel: one step per notch, like wireKnob() elsewhere in the UI.
    container.addEventListener('wheel', (e) => {
      e.preventDefault(); e.stopPropagation();
      const dir = e.deltaY < 0 ? 1 : -1;
      const nv = Math.max(min, Math.min(max, Math.round((value + dir * step - min) / step) * step + min));
      if (nv !== value) { value = nv; render(); onChange(value); }
    }, { passive: false });

    render();
    return { set(v) { value = v; render(); } };
  }

  function renderKitGainStrip(container) {
    // Same column layout as the channel strips (invisible fx / knob / button placeholders keep the fader at the same height).
    const ghostSlots = Array.from({length:SLOT_COUNT}, () => '<div class="fx-slot-top"><select class="fx-slot-select off" disabled tabindex="-1"></select></div>').join('');
    const strip = document.createElement('div');
    strip.className = 'strip is-kit';
    strip.innerHTML = `
      <div class="strip-name">KIT GAIN</div>
      <div class="strip-sep"></div>
      <div class="strip-fx" style="visibility:hidden">${ghostSlots}</div>
      <div class="strip-knobs" style="visibility:hidden">
        <div class="strip-knobs-row"></div>
        <div class="strip-knobs-row"></div>
      </div>
      <div class="strip-auto-row"><button class="auto-btn" data-auto>Auto</button><div class="auto-target" data-auto-target><span class="auto-target-val">+0.0</span><input class="strip-db-input" type="text" inputmode="decimal" spellcheck="false"></div></div>
      <div class="strip-readout-row"><div class="strip-db" data-db><span class="strip-db-val">+${kitState.gainDb.toFixed(1)}</span><input class="strip-db-input" type="text" inputmode="decimal" spellcheck="false"></div><div class="strip-peak-slot"><div class="strip-peak-readout"></div></div></div>
      <div class="strip-fader-row">
        <div class="fader-wrap"><input type="range" class="fader" min="${KIT_GAIN_MIN}" max="${KIT_GAIN_MAX}" step="0.1" value="${kitState.gainDb}"></div>
        <div class="strip-meter-host"></div>
      </div>
      <div class="strip-bottom"><div class="strip-btns" style="visibility:hidden"><button class="mute" tabindex="-1">Mute</button></div></div>
    `;
    container.appendChild(strip);
    // Invisible knobs in the two knob rows: they give the rows exactly the height of the channel strips' rows at any zoom.
    const ghostRows = strip.querySelectorAll('.strip-knobs-row');
    const ghostKnob = (row, label) => { const h = document.createElement('div'); row.appendChild(h); createKnob(h, { label, title: label, min: 0, max: 100, def: 0, format: v => String(v), onChange: () => {} }); };
    ghostKnob(ghostRows[0], 'Pan'); ghostKnob(ghostRows[1], 'Rev'); ghostKnob(ghostRows[1], 'Del');
    const dbLabel = strip.querySelector('[data-db]');
    const dbVal = strip.querySelector('.strip-db-val');
    const dbInput = strip.querySelector('[data-db] .strip-db-input');
    const faderEl = strip.querySelector('.fader');
    kitState.faderEl = faderEl; kitState.dbVal = dbVal;
    const kitPeakReadout = strip.querySelector('.strip-peak-readout');
    kitPeakReadout.dataset.tooltip = 'Peak (dBFS)\nLoudest instrument channel input after Kit Gain, held for the Meter Peak Hold time — visible only while it carries audio.';
    kitState.meter = createStereoDbMeter(strip.querySelector('.strip-meter-host'), { ticks:'none', readoutEl: kitPeakReadout });
    wireAutoControls(strip, 'kit', kitState);
    dbLabel.dataset.tooltip = 'Kit Gain (dB)\nDouble-click to type an exact value.';
    faderEl.dataset.tooltip = 'Kit Gain\nGain on the input of all instrument channels, before their faders and effects (−24 to +24 dB). Ctrl+click resets to the default of the selected engine.';
    function updateLabel() { dbVal.textContent = (kitState.gainDb >= 0 ? '+' : '') + kitState.gainDb.toFixed(1); }
    faderEl.addEventListener('input', () => { kitState.gainDb = +faderEl.value; updateLabel(); commitMixer(); });
    faderEl.addEventListener('pointerdown', (e) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      if (autoRuns.has('kit')) return;
      faderEl.value = String(kitGainDefault());
      faderEl.dispatchEvent(new Event('input'));
    });
    let editing = false;
    function openEdit() { if (editing || autoRuns.has('kit')) return; editing = true; dbInput.value = String(kitState.gainDb); dbVal.style.visibility = 'hidden'; dbInput.style.display = 'block'; dbInput.focus(); dbInput.select(); }
    function closeEdit(commit) {
      if (!editing) return;
      editing = false;
      if (commit) {
        const nv = parseFloat(dbInput.value.replace(',', '.'));
        if (!isNaN(nv)) { faderEl.value = Math.min(KIT_GAIN_MAX, Math.max(KIT_GAIN_MIN, nv)); faderEl.dispatchEvent(new Event('input')); }
      }
      dbInput.style.display = 'none';
      dbVal.style.visibility = 'visible';
    }
    dbLabel.addEventListener('dblclick', (e) => { e.stopPropagation(); openEdit(); });
    dbInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); closeEdit(true); }
      else if (e.key === 'Escape') { closeEdit(false); }
    });
    dbInput.addEventListener('blur', () => closeEdit(true));
  }

  function renderStrip(container, ch) {
    const isMaster = ch.kind === 'master';
    const isBus = !!ch.isBus;
    // Pan: instruments, master and buses. Rev/Del sends: instruments only.
    const hasPan = ch.kind === 'instrument' || isMaster || isBus;
    const hasSends = ch.kind === 'instrument';
    const hasSolo = !isMaster;
    const hasAuto = ch.kind === 'instrument' || isMaster;

    const defaultSlots = isMaster ? MASTER_FX_DEFAULT : CHANNEL_FX_DEFAULT;
    const fxSlots = defaultSlots.slice(0, SLOT_COUNT);
    while (fxSlots.length < SLOT_COUNT) fxSlots.push(null);

    state[ch.id] = {
      gainDb: MIXER_DEFAULTS.gainDb, pan: MIXER_DEFAULTS.pan, mute: false, solo: false,
      autoTargetDb: 0,
      sendReverb: hasSends ? MIXER_DEFAULTS.sendReverb : null,
      sendDelay: hasSends ? MIXER_DEFAULTS.sendDelay : null,
      fxSlots,
      fx: isBus ? makeFxState(ch.fxKeys, FX_DEFS) : makeFxState(fxSlots.filter(Boolean), FX_DEFS),
      meter: null,
    };
    const s = state[ch.id];

    const strip = document.createElement('div');
    strip.className = 'strip' + (isMaster ? ' is-master' : '') + (isBus ? ' is-bus' : '');
    if (!isMaster && !isBus) strip.style.setProperty('--strip-color', familyColor(ch.family));

    // Use short labels for collapsed display; full names in dropdown. OFF uses empty placeholder.
    const slotOptions = [
      { label:'EQ',               value:'eq',  short:'EQ' },
      { label:'Compressor',       value:'comp',short:'COMP' },
      { label:'Saturation',       value:'sat', short:'SAT' },
      { label:'Transient Shaper', value:'ts',  short:'TRANS' },
      { label:'Limiter',          value:'lim', short:'LIM' },
    ];
    const GEAR_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="12" cy="12" r="3"></circle><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"></path></svg>';
    // Bus strips expose their module's params as rotary knobs in the .strip-knobs band (2 per row). 'mix' is not a
    // knob: the bus fader is the wet level.
    const busKey = isBus ? ch.fxKeys[0] : null;
    // Bus module settings (Size/Decay/Damping or Time/Feedback) live in the
    // .strip-fx band ABOVE Pan, as their own accessible zoom panel. 'mix' is the
    // fader (wet level), so it stays out of the knob set. Pan keeps its own
    // .strip-knobs row below, like every other slot.
    let busSettingsHtml = '';
    if (isBus && busKey) {
      const keys = FX_DEFS[busKey].params.map(p => p.k).filter(k => k !== 'mix');
      let rows = '';
      for (let i = 0; i < keys.length; i += 2) {
        rows += '<div class="strip-knobs-row">' + keys.slice(i, i+2).map(k =>
          `<div class="bus-knob-host" data-busk="${k}"></div>`).join('') + '</div>';
      }
      busSettingsHtml = `<div class="strip-knobs bus-settings">${rows}</div>`;
    }
    const slotsHtml = isBus ? '' : Array.from({length:SLOT_COUNT}, (_,i) => {
      const val = s.fxSlots[i] || '';
      const isOff = !val;
      // One option per module (short label) + empty placeholder + OFF. No
      // duplicate values: a <select> with two options sharing one value can
      // fail to display the selected name reliably in WebView2, which is why
      // the loaded effect used not to show on boot.
      const opts = `<option value=""${isOff ? ' selected' : ''}></option>`
        + slotOptions.map(o => `<option value="${o.value}"${o.value===val ? ' selected' : ''}>${o.short}</option>`).join('')
        + `<option value="off">OFF</option>`;
      return `<div class="fx-slot-top">
          <select class="fx-slot-select${isOff ? ' off' : ''}" data-slot="${i}">${opts}</select>
          <button class="fx-settings-btn${isOff ? ' hidden' : ''}" data-fx-settings data-slot="${i}">${GEAR_SVG}</button>
        </div>`;
    }).join('');

    strip.innerHTML = `
      <div class="strip-name">${ch.name}</div>
      <div class="strip-sep"></div>
      <div class="strip-fx">${isBus ? busSettingsHtml : slotsHtml}</div>
      <div class="strip-knobs${hasPan && !hasSends ? ' pan-only' : ''}">
        <div class="strip-knobs-row">${hasPan ? '<div class="pan-knob-host"></div>' : ''}</div>
        <div class="strip-knobs-row">${hasSends ? '<div class="rev-knob-host"></div><div class="del-knob-host"></div>' : ''}</div>
      </div>
      <div class="strip-auto-row">${hasAuto ? `<button class="auto-btn" data-auto>Auto</button><div class="auto-target" data-auto-target><span class="auto-target-val">+0.0</span><input class="strip-db-input" type="text" inputmode="decimal" spellcheck="false"></div>` : ''}</div>
      <!-- Fader value above the fader, peak value above the meter, one row (same
           28px / 14px columns, gap and centering as .strip-fader-row below). The
           peak text/color/visibility are driven by the meter. -->
      <div class="strip-readout-row">
        <div class="strip-db" data-db><span class="strip-db-val">${MIXER_DEFAULTS.gainDb >= 0 ? '+' : ''}${MIXER_DEFAULTS.gainDb.toFixed(1)}</span><input class="strip-db-input" type="text" inputmode="decimal" spellcheck="false"></div>
        <div class="strip-peak-slot"><div class="strip-peak-readout"></div></div>
      </div>
      <div class="strip-fader-row">
        <div class="fader-wrap"><input type="range" class="fader" min="-60" max="18" step="0.1" value="${MIXER_DEFAULTS.gainDb}"></div>
        <div class="strip-meter-host"></div>
      </div>
      <div class="strip-bottom">
        <div class="strip-btns">
          <button class="mute" data-mute>Mute</button>
          ${hasSolo ? '<button class="solo" data-solo>Solo</button>' : ''}
        </div>
      </div>
    `;
    // Gear button tooltip via the custom system (set in JS so the line break survives).
    strip.querySelectorAll('.fx-settings-btn').forEach(b => { b.dataset.tooltip = 'Settings\nOpens the parameter panel for this effect.'; });
    container.appendChild(strip);

    const meterHost = strip.querySelector('.strip-meter-host');
    // All strips use clean level bars (no numbers on the meter itself) — the
    // single exception is the read-only peak value directly above the bar,
    // which the meter drives via readoutEl (see createStereoDbMeter).
    const peakReadout = strip.querySelector('.strip-peak-readout');
    peakReadout.dataset.tooltip = 'Peak (dBFS)\nLoudest level of this channel, held for the Meter Peak Hold time — visible only while it carries audio.';
    s.meter = createStereoDbMeter(meterHost, { ticks:'none', readoutEl: peakReadout });

    // Master only: a dB scale on the RIGHT of the meter, spanning its full
    // height, positioned by the meter's own range (-60..+18).
    if (isMaster) {
      const MMIN = -60, MMAX = 18;
      const SCALE_TICKS = [-60, -48, -36, -24, -12, 0, 6, 12, 18];
      const gainScale = document.createElement('div');
      gainScale.className = 'gain-scale';
      let ticksHtml = '';
      for (const v of SCALE_TICKS) {
        const pct = ((v - MMIN) / (MMAX - MMIN)) * 100;
        ticksHtml += '<div class="gain-tick' + (v >= 0 ? ' gain-tick-nosign' : '') + '" style="bottom:' + pct + '%"><span>' + v + '</span></div>';
      }
      gainScale.innerHTML = ticksHtml;
      strip.querySelector('.strip-fader-row').appendChild(gainScale);
    }

    const dbLabel = strip.querySelector('[data-db]');
    const dbVal = strip.querySelector('.strip-db-val');
    const dbInput = strip.querySelector('[data-db] .strip-db-input');
    const faderEl = strip.querySelector('.fader');
    const muteBtn = strip.querySelector('[data-mute]');
    const soloBtn = strip.querySelector('[data-solo]');

    dbLabel.dataset.tooltip = 'Volume (dB)\nDouble-click to type an exact value.';
    faderEl.dataset.tooltip = 'Volume\nLevel of this channel in dB (−60 to +18).';
    muteBtn.dataset.tooltip = 'Mute\nSilences this channel without touching its settings.';
    if (soloBtn) soloBtn.dataset.tooltip = 'Solo\nOnly soloed channels stay audible while any solo is active.';

    // Expose control refs on the state object so a host-pushed 'mixerState'
    // (initial load / project restore) can re-drive the DOM controls.
    s.faderEl = faderEl; s.dbVal = dbVal; s.muteBtn = muteBtn; s.soloBtn = soloBtn;
    s.stripName = ch.name;
    s.fxSelects = Array.from(strip.querySelectorAll('.fx-slot-select'));
    s.fxValEls = Array.from(strip.querySelectorAll('[data-fx-vals]'));

    function updateDbLabel() {
      dbVal.textContent = (s.gainDb >= 0 ? '+' : '') + s.gainDb.toFixed(1);
    }
    faderEl.addEventListener('input', () => {
      s.gainDb = +faderEl.value;
      updateDbLabel();
      commitMixer();
    });
    // Ctrl+Click on the fader → Reset to default (-6 dB).
    faderEl.addEventListener('pointerdown', (e) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      if (autoRuns.has(ch.id)) return;
      const DEF = MIXER_DEFAULTS.gainDb;
      faderEl.value = String(DEF);
      faderEl.dispatchEvent(new Event('input')); // aktualisiert s.gainDb + Label + Commit
    });
    updateDbLabel();

    // Double-click the dB value -> inline edit box, same behavior as the other
    // editable numbers in the UI (see wireKnob in ui-controller.js):
    // Enter confirms, Escape cancels, blur saves & closes. The entered value
    // is clamped to the fader's range; invalid input reverts silently.
    let dbEditing = false;
    function openDbEdit() {
      if (dbEditing || autoRuns.has(ch.id)) return;
      dbEditing = true;
      dbInput.value = String(s.gainDb);
      dbVal.style.visibility = 'hidden';
      dbInput.style.display = 'block';
      dbInput.focus();
      dbInput.select();
    }
    function closeDbEdit(commit) {
      if (!dbEditing) return;
      dbEditing = false;
      if (commit) {
        const nv = parseFloat(dbInput.value.replace(',', '.'));
        if (!isNaN(nv)) {
          const lo = parseFloat(faderEl.min), hi = parseFloat(faderEl.max);
          faderEl.value = Math.min(hi, Math.max(lo, nv));
          faderEl.dispatchEvent(new Event('input')); // updates s.gainDb + label
        } else {
          dbInput.value = String(s.gainDb);
        }
      } else {
        dbInput.value = String(s.gainDb);
      }
      dbInput.style.display = 'none';
      dbVal.style.visibility = 'visible';
    }
    dbLabel.addEventListener('dblclick', (e) => { e.stopPropagation(); openDbEdit(); });
    dbInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); closeDbEdit(true); }
      else if (e.key === 'Escape') { closeDbEdit(false); }
    });
    dbInput.addEventListener('blur', () => closeDbEdit(true));

    if (hasAuto) wireAutoControls(strip, ch.id, s);

    if (hasPan) {
      s.panKnob = createKnob(strip.querySelector('.pan-knob-host'), {
        label: 'Pan', title: 'Pan\nMoves the channel left or right in the stereo field.', min: -100, max: 100, def: MIXER_DEFAULTS.pan,
        format: v => panLabel(v / 100),
        onChange: v => { s.pan = v / 100; commitMixer(); },
      });
    }
    // Rev/Del sends only for instruments - the master has no send knobs
    // (and in the DOM also no .rev-knob-host/.del-knob-host, otherwise
    // createKnob would hit null).
    if (hasSends) {
      s.revKnob = createKnob(strip.querySelector('.rev-knob-host'), {
        label: 'Rev', title: 'Reverb Send\nSends part of this channel to the Reverb bus. 0% = dry.', min: 0, max: 100, def: MIXER_DEFAULTS.sendReverb,
        format: v => Math.round(v) + '%',
        onChange: v => { s.sendReverb = v / 100; commitMixer(); },
      });
      s.delKnob = createKnob(strip.querySelector('.del-knob-host'), {
        label: 'Del', title: 'Delay Send\nSends part of this channel to the Delay bus. 0% = dry.', min: 0, max: 100, def: MIXER_DEFAULTS.sendDelay,
        format: v => Math.round(v) + '%',
        onChange: v => { s.sendDelay = v / 100; commitMixer(); },
      });
    }
    // Bus param knobs (Size/Decay/Damping etc.) — direct rotary controls in the
    // .strip-knobs band, alongside Pan.
    if (isBus && busKey) {
      const def = FX_DEFS[busKey];
      strip.querySelectorAll('.bus-knob-host').forEach(host => {
        const k = host.dataset.busk;
        const pdef = def.params.find(p => p.k === k);
        if (!pdef) return;
        s['busKnob_' + k] = createKnob(host, {
          label: busKnobShort(k), title: (FX_PARAM_TIPS[busKey] || {})[k] || pdef.label, min: pdef.min, max: pdef.max, def: pdef.def, step: pdef.step || 1,
          format: pdef.fmt || (v => busKnobFmt(v, pdef.unit)),
          onChange: v => { if (!s.fx[busKey]) s.fx[busKey] = { on:true, params:{} }; s.fx[busKey].params[k] = (pdef.step ? Math.round(v) : v); commitMixer(); },
        });
      });
    }

    muteBtn.addEventListener('click', () => {
      s.mute = !s.mute;
      muteBtn.classList.toggle('active', s.mute);
      if (s.mute && s.solo) { s.solo = false; if (soloBtn) soloBtn.classList.remove('active'); }
      commitMixer();
    });
    if (soloBtn) {
      soloBtn.addEventListener('click', () => {
        setExclusiveSolo(ch.id, !s.solo);
        if (s.solo && s.mute) { s.mute = false; muteBtn.classList.remove('active'); }
        commitMixer();
      });
    }

    function settingsBtn(slotIdx) {
      return strip.querySelector(`.fx-settings-btn[data-slot="${slotIdx}"]`);
    }
    function syncSettingsBtn(slotIdx) {
      const btn = settingsBtn(slotIdx);
      if (!btn) return;
      btn.classList.toggle('hidden', !s.fxSlots[slotIdx]);
    }

    strip.querySelectorAll('.fx-slot-select').forEach(sel => {
      sel.addEventListener('change', (e) => {
        const slotIdx = +sel.dataset.slot;
        const raw = sel.value;
        const prevKey = s.fxSlots[slotIdx];
        const newVal = (raw === 'off') ? null : (raw || null);
        s.fxSlots[slotIdx] = newVal;
        sel.value = newVal || '';
        if (prevKey && prevKey !== newVal) closeFxPanel(ch.id + '::' + prevKey); // drop stale panel
        sel.classList.toggle('off', !sel.value);
        syncSettingsBtn(slotIdx);
        updateFxSlotValues(ch.id);
        commitMixer();
      });
    });

    strip.querySelectorAll('[data-fx-settings]').forEach(btn => {
      btn.addEventListener('click', () => {
        if (btn.classList.contains('hidden')) return;
        const key = s.fxSlots[+btn.dataset.slot];
        if (!key) return;
        const fxDef = FX_DEFS[key];
        if (!s.fx[key]) Object.assign(s.fx, makeFxState([key], FX_DEFS));
        makeFxPanel(ch.id, key, fxDef, btn);
      });
    });

    // Initial value display (default params are already set in makeFxState)
    updateFxSlotValues(ch.id);
  }

  // ---- Multiple floating, draggable effect panels -------------------------
  // Each open effect gets its OWN panel appended to <body> (outside .app), so it
  // is NOT clipped by the rack's overflow and can float over the whole UI - even
  // past the strip that opened it. A CSS transform on .app (UI scale) would make
  // position:fixed children clip inside it, hence body-level + matching scale.
  // Panels stay open until closed explicitly: their X button, or Options > "Close
  // all effect panels". Several may be open at once and overlap; drag by the
  // header to reposition, click a panel to bring it to front.
  const openFxPanels = new Map();   // key chId::fxKey -> { el, chId, fxKey, def, widget }
  let fxPanelZTop = 60;             // base .fx-pop z-index is 50

  function channelNameFromAnchor(anchorEl) {
    const strip = anchorEl && anchorEl.closest ? anchorEl.closest('.strip') : null;
    const nm = strip && strip.querySelector('.strip-name');
    return (nm && nm.textContent.trim()) || 'Channel';
  }
  function bringToFront(panelEl) { panelEl.style.zIndex = String(++fxPanelZTop); }

  function closeFxPanel(key) {
    const rec = openFxPanels.get(key);
    if (!rec) return;
    if (rec.widget) { try { rec.widget.destroy(); } catch (e) {} }
    rec.el.remove();
    openFxPanels.delete(key);
  }
  function closeAllFxPanels() { for (const key of Array.from(openFxPanels.keys())) closeFxPanel(key); }
  // Exposed to the Options menu ("Close all effect panels").
  window.closeAllMixerFxPanels = closeAllFxPanels;

  function positionPanelNear(panelEl, anchorEl) {
    const s = (typeof currentUIScale === 'function') ? currentUIScale() : 1;
    const rect = anchorEl.getBoundingClientRect();
    const pw = panelEl.offsetWidth * s, ph = panelEl.offsetHeight * s, m = 8;
    let left = rect.right + 6;
    if (left + pw > window.innerWidth - m) left = rect.left - pw - 6; // no room on the right -> open left
    left = Math.max(m, Math.min(left, window.innerWidth - pw - m));
    const top = Math.max(m, Math.min(rect.top, window.innerHeight - ph - m));
    panelEl.style.left = left + 'px';
    panelEl.style.top = top + 'px';
  }

  function wirePanelDrag(panelEl, handle) {
    let dragging = false, sx = 0, sy = 0, sl = 0, st = 0;
    const onDown = (e) => {
      if (e.button !== 0 || e.target.closest('.fx-panel-close-btn')) return;
      dragging = true; sx = e.clientX; sy = e.clientY;
      sl = panelEl.getBoundingClientRect().left; st = panelEl.getBoundingClientRect().top;
      bringToFront(panelEl); document.body.style.cursor = 'grabbing'; e.preventDefault();
    };
    const onMove = (e) => {
      if (!dragging) return;
      const s = (typeof currentUIScale === 'function') ? currentUIScale() : 1;
      const pw = panelEl.offsetWidth * s, ph = panelEl.offsetHeight * s, keep = 48;
      let nl = sl + (e.clientX - sx), nt = st + (e.clientY - sy);
      nl = Math.max(-(pw - keep), Math.min(nl, window.innerWidth - keep));
      nt = Math.max(0, Math.min(nt, window.innerHeight - keep));
      panelEl.style.left = nl + 'px'; panelEl.style.top = nt + 'px';
    };
    const onUp = () => { if (!dragging) return; dragging = false; document.body.style.cursor = ''; };
    handle.addEventListener('mousedown', onDown);
    window.addEventListener('mousemove', onMove, { passive: true });
    window.addEventListener('mouseup', onUp);
  }

  function makeFxPanel(chId, fxKey, def, anchorEl) {
    const key = chId + '::' + fxKey;
    const existing = openFxPanels.get(key);
    if (existing) { bringToFront(existing.el); return existing.el; } // already open -> just focus it

    const s = state[chId];
    if (!s || !s.fx[fxKey]) return null;
    const chName = channelNameFromAnchor(anchorEl);

    const el = document.createElement('div');
    el.className = 'fx-pop fx-panel';
    el.dataset.chid = chId; el.dataset.fxkey = fxKey;

    // Header: title (effect + channel) + close button. Doubles as the drag handle.
    const head = document.createElement('div'); head.className = 'fx-panel-head';
    const title = document.createElement('span'); title.className = 'fx-panel-title';
    title.textContent = def.title + ' - ' + chName;
    const closeBtn = document.createElement('button');
    closeBtn.type = 'button'; closeBtn.className = 'fx-panel-close-btn';
    closeBtn.textContent = String.fromCharCode(215); // multiplication sign (x)
    closeBtn.setAttribute('aria-label', 'Close ' + def.title + ' (' + chName + ')');
    closeBtn.dataset.tooltip = 'Close\nCloses this effect panel.';
    head.appendChild(title); head.appendChild(closeBtn);

    const body = document.createElement('div'); body.className = 'fx-panel-body';
    const rec = { el, chId, fxKey, def, widget: null };
    openFxPanels.set(key, rec); // register before building (the limiter builder stores its widget on it)

    if (def.eq) {
      body.appendChild(buildEqPanel(chId, def, s.fx[fxKey]));
    } else if (fxKey === 'lim') {
      el.classList.add('fx-pop-lim');
      body.appendChild(buildLimiterPanel(chId, def, s.fx[fxKey], rec));
    } else {
      const items = document.createElement('div'); items.className = 'ne-dial-row-items';
      def.params.forEach(p => {
        const step = (p.max - p.min) / 100;
        const fmt = v => (step >= 1 ? String(Math.round(v)) : v.toFixed(1)) + (p.unit || '');
        items.appendChild(neCreateDial({
          label: p.label, min: p.min, max: p.max, step, value: s.fx[fxKey].params[p.k],
          format: fmt, def: p.def, title: (FX_PARAM_TIPS[fxKey] || {})[p.k],
          onInput: v => { s.fx[fxKey].params[p.k] = v; updateFxSlotValues(chId); commitMixer(); },
        }));
      });
      const row = document.createElement('div'); row.className = 'ne-ctl-group ne-dial-row';
      row.appendChild(items); body.appendChild(row);
      if (fxKey === 'comp') el.classList.add('fx-pop-comp');
    }

    el.appendChild(head); el.appendChild(body);
    document.body.appendChild(el);

    el.addEventListener('mousedown', () => bringToFront(el));
    closeBtn.addEventListener('click', (e) => { e.stopPropagation(); closeFxPanel(key); });

    wirePanelDrag(el, head);
    positionPanelNear(el, anchorEl);
    bringToFront(el);
    return el;
  }

  // Options menu: "Close all effect panels".
  document.getElementById('closeAllFxBtn')?.addEventListener('click', () => {
    const menu = document.getElementById('mxOptionsMenu');
    if (menu) menu.style.display = 'none';
    closeAllFxPanels();
  });
  // ---- EQ panel: response curve + per-band gain faders + Freq/Q knobs. -------
  // The biquad math below mirrors source/dsp/Mixer.h (Biquad::SetPeak /
  // SetLowShelf / SetHighShelf) so the drawn curve matches the actual DSP.
  const EQ_SR = 48000;
  function eqClampFreq(f, sr) { return Math.max(20, Math.min(f, sr * 0.49)); }
  function eqPeak(f, gainDb, q, sr) {
    const A = Math.pow(10, gainDb / 40), w0 = 2 * Math.PI * eqClampFreq(f, sr) / sr;
    const cw = Math.cos(w0), sw = Math.sin(w0), alpha = sw / (2 * Math.max(0.01, q)), a0 = 1 + alpha / A;
    return { b0: (1 + alpha * A) / a0, b1: -2 * cw / a0, b2: (1 - alpha * A) / a0, a1: -2 * cw / a0, a2: (1 - alpha / A) / a0 };
  }
  // RBJ shelf structure (low/high). At gainDb==0 (A==1) the response is exactly
  // flat — and it's the same math as Biquad::SetLowShelf/SetHighShelf in Mixer.h,
  // so the drawn curve always matches the actual audio.
  function eqShelf(f, gainDb, q, sr, high) {
    const A = Math.pow(10, gainDb / 40), w0 = 2 * Math.PI * eqClampFreq(f, sr) / sr;
    const cw = Math.cos(w0), sw = Math.sin(w0), sA = Math.sqrt(A);
    // knob q [0.3,4] -> RBJ shelf-Q [0.3, 1/sqrt(2)]: higher knob = sharper transition.
    // Hard-capped at 1/sqrt(2) (~0.707): per the RBJ Audio EQ Cookbook that is the steepest
    // shelf that stays MONOTONIC — above it becomes a resonant peak + notch (mushy, and it
    // pushes the response past its +/-gain limits). Must stay byte-identical to
    // Biquad::SetLowShelf/SetHighShelf in Mixer.h so the drawn curve matches the audio.
    const t = Math.min(1, Math.max(0, (q - 0.3) / 3.7));
    const qs = 0.3 + (0.7071 - 0.3) * t;
    const alpha = sw / (2 * qs);
    if (high) {
      const a0 = (A + 1) - (A - 1) * cw + 2 * sA * alpha;
      return { b0: A * ((A + 1) + (A - 1) * cw + 2 * sA * alpha) / a0, b1: -2 * A * ((A - 1) + (A + 1) * cw) / a0,
               b2: A * ((A + 1) + (A - 1) * cw - 2 * sA * alpha) / a0, a1: 2 * ((A - 1) - (A + 1) * cw) / a0,
               a2: ((A + 1) - (A - 1) * cw - 2 * sA * alpha) / a0 };
    }
    const a0 = (A + 1) + (A - 1) * cw + 2 * sA * alpha;
    return { b0: A * ((A + 1) - (A - 1) * cw + 2 * sA * alpha) / a0, b1: 2 * A * ((A - 1) - (A + 1) * cw) / a0,
             b2: A * ((A + 1) - (A - 1) * cw - 2 * sA * alpha) / a0, a1: -2 * ((A - 1) + (A + 1) * cw) / a0,
             a2: ((A + 1) + (A - 1) * cw - 2 * sA * alpha) / a0 };
  }
  function eqDb(c, f, sr) {
    const w = 2 * Math.PI * f / sr, c1 = Math.cos(w), s1 = Math.sin(w);
    const c2 = Math.cos(2 * w), i2 = -Math.sin(2 * w);
    const nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -c.b1 * s1 + c.b2 * i2;
    const dr = 1 + c.a1 * c1 + c.a2 * c2, di = -c.a1 * s1 + c.a2 * i2;
    return 20 * Math.log10(Math.sqrt(nr * nr + ni * ni) / Math.sqrt(dr * dr + di * di));
  }
  function makeEqCurve() {
    const W = 190, H = 116, pad = 4, fMin = 20, fMax = 20000, dMax = 24, dMin = -24, N = 160;
    const NS = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(NS, 'svg');
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    svg.classList.add('eq-curve-svg');
    const yFor = db => pad + (H - 2 * pad) * ((dMax - Math.max(dMin, Math.min(dMax, db))) / (dMax - dMin));
    const xFor = f => pad + (W - 2 * pad) * (Math.log(f / fMin) / Math.log(fMax / fMin));
    [-12, 0, 12].forEach(db => { const l = document.createElementNS(NS, 'line'); l.setAttribute('x1', pad); l.setAttribute('x2', W - pad); l.setAttribute('y1', yFor(db)); l.setAttribute('y2', yFor(db)); l.setAttribute('class', 'eq-grid' + (db === 0 ? ' eq-grid-zero' : '')); svg.appendChild(l); });
    [100, 1000, 10000].forEach(f => { const l = document.createElementNS(NS, 'line'); l.setAttribute('y1', pad); l.setAttribute('y2', H - pad); l.setAttribute('x1', xFor(f)); l.setAttribute('x2', xFor(f)); l.setAttribute('class', 'eq-grid'); svg.appendChild(l); });
    const path = document.createElementNS(NS, 'path'); path.setAttribute('class', 'eq-curve-path'); svg.appendChild(path);
    function update(p) {
      let d = '';
      for (let i = 0; i < N; i++) {
        const f = fMin * Math.pow(fMax / fMin, i / (N - 1));
        let db = eqDb(eqShelf(p.lowFreq, p.lowGain, p.lowQ, EQ_SR, false), f, EQ_SR)
               + eqDb(eqPeak(p.midFreq, p.midGain, p.midQ, EQ_SR), f, EQ_SR)
               + eqDb(eqShelf(p.highFreq, p.highGain, p.highQ, EQ_SR, true), f, EQ_SR);
        db = Math.max(dMin, Math.min(dMax, db));
        d += (i === 0 ? 'M' : 'L') + xFor(f).toFixed(1) + ' ' + yFor(db).toFixed(1);
      }
      path.setAttribute('d', d);
    }
    return { el: svg, update };
  }
  function buildEqPanel(chId, def, s) {
    const p = s.params;
    const wrap = document.createElement('div'); wrap.className = 'eq-panel';
    const curve = makeEqCurve(); wrap.appendChild(curve.el);
    const refresh = () => { curve.update(p); updateFxSlotValues(chId); commitMixer(); };
    // Per-band gain faders (LOW / MID / HIGH) — vertical, like the strip fader.
    const gains = document.createElement('div'); gains.className = 'eq-gains';
    [['low', 'LOW'], ['mid', 'MID'], ['high', 'HIGH']].forEach(([band, name]) => {
      const col = document.createElement('div'); col.className = 'eq-gain-col';
      const lab = document.createElement('div'); lab.className = 'eq-band-label'; lab.textContent = name;
      const fw = document.createElement('div'); fw.className = 'fader-wrap eq-fader';
      const inp = document.createElement('input'); inp.type = 'range'; inp.min = '-18'; inp.max = '18'; inp.step = '0.1'; inp.value = String(p[band + 'Gain']);
      inp.dataset.tooltip = FX_PARAM_TIPS.eq.gain;
      const val = document.createElement('div'); val.className = 'eq-gain-val'; val.textContent = (p[band + 'Gain'] >= 0 ? '+' : '') + (+p[band + 'Gain']).toFixed(1);
      inp.addEventListener('input', () => { p[band + 'Gain'] = +inp.value; val.textContent = (+inp.value >= 0 ? '+' : '') + (+inp.value).toFixed(1); refresh(); });
      fw.appendChild(inp); col.appendChild(lab); col.appendChild(fw); col.appendChild(val); gains.appendChild(col);
    });
    wrap.appendChild(gains);
    // Freq + Q knobs, grouped per band (LOW/MID/HIGH) and stacked vertically —
    // frequency on top, Q below — aligned under the gain faders. No text labels,
    // only the numeric value (soll.png layout).
    const knobs = document.createElement('div'); knobs.className = 'eq-knobs';
    [['low', 'LOW'], ['mid', 'MID'], ['high', 'HIGH']].forEach(([band]) => {
      const col = document.createElement('div'); col.className = 'eq-band-col';
      [['Freq', v => String(Math.round(v))], ['Q', v => v.toFixed(2)]].forEach(([suffix, fmt]) => {
        const pr = def.params.find(x => x.k === band + suffix);
        const step = (pr.max - pr.min) / 100;
        col.appendChild(neCreateDial({ label: '', min: pr.min, max: pr.max, step, value: p[band + suffix], def: pr.def,
          title: FX_PARAM_TIPS.eq[suffix.toLowerCase()], format: fmt, onInput: v => { p[band + suffix] = v; refresh(); } }));
      });
      knobs.appendChild(col);
    });
    wrap.appendChild(knobs);
    curve.update(p);
    return wrap;
  }

  // Limiter real-time scope — FL-style dual-waveform display.
  // White filled wave + white line = output (post-limiter, what you hear).
  // Purple line at 50% alpha = input (pre-limiter), drawn only above the
  // ceiling: below it, output and input coincide (passthrough), so the purple
  // marks exactly where and how far the limiter clamps.
  // Green dashed line = ceiling. Orange curve hanging from the top edge = gain
  // reduction (depth in dB below +12).

  function createLimiterScope(host) {
    host.classList.add('lim-scope');
    host.innerHTML = `
      <div class="lim-scope-wrap"><canvas></canvas></div>
      <div class="lim-scope-status">
        <span class="lg out"><i></i> Output (audible)</span>
        <span class="lg in"><i></i> Input (pre-limiter)</span>
        <span class="lim-pk" data-tooltip="Peak In / Out\nHighest level since the panel opened or since your last click here. In = before the limiter, Out = after it. Click to reset.">Peak In <b class="pkIn">−∞</b> · Out <b class="pkOut">−∞</b> · Ceiling <b class="ceilVal">−6 dB</b></span>
      </div>`;
    const canvas = host.querySelector('canvas');
    const ctx = canvas.getContext('2d');
    const pkOutEl = host.querySelector('.pkOut');
    const pkInEl = host.querySelector('.pkIn');
    const fmtDb = db => db <= -99 ? '−∞' : (db < 0 ? '−' : '+') + Math.abs(db).toFixed(1) + ' dB';
    host.querySelector('.lim-pk').addEventListener('click', () => { peakIn = -100; peakOut = -100; showPeaks(); });
    function showPeaks() { pkInEl.textContent = fmtDb(peakIn); pkOutEl.textContent = fmtDb(peakOut); }
    const ceilValEl = host.querySelector('.ceilVal');

    let W = 0, H = 0, dpr = 1;
    function resize() {
      dpr = window.devicePixelRatio || 1;
      const r = canvas.getBoundingClientRect();
      if (r.width === 0) return;
      W = r.width; H = r.height;
      canvas.width = W * dpr; canvas.height = H * dpr;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    // Scale: +12 … −32 dB — bottom edge matches the ceiling minimum
    const TOP_DB = 12, BOT_DB = -32;
    // PAD keeps the +12 and bottom labels fully inside the canvas.
    const PAD = 8;
    const yOf = db => PAD + (H - 2 * PAD) * (1 - (Math.max(BOT_DB, Math.min(TOP_DB, db)) - BOT_DB) / (TOP_DB - BOT_DB));

    // Rolling buffers: one sample pushed per mixerLevels event (~30-60 Hz)
    const BUF_LEN = 240; // ~4-8 s of history
    const inBuf  = new Float32Array(BUF_LEN).fill(-70);
    const outBuf = new Float32Array(BUF_LEN).fill(-70);
    const grBuf = new Float32Array(BUF_LEN); // gain reduction in dB (>= 0) per sample
    let bufIdx = 0;

    let ceilingDb = -6;
    let peakIn = -100, peakOut = -100;
    let running = false, raf = null;

    function pushSample(inDb, outDb, gr) {
      inBuf[bufIdx]  = inDb;
      outBuf[bufIdx] = outDb;
      grBuf[bufIdx]  = gr;
      bufIdx = (bufIdx + 1) % BUF_LEN;
    }

    function draw() {
      ctx.clearRect(0, 0, W, H);

      // Grid lines + dB labels (every 6 dB, 0 dB bolder)
      ctx.font = '9px Consolas, monospace';
      ctx.textBaseline = 'middle';
      ctx.textAlign = 'left';
      for (let db = TOP_DB; db >= BOT_DB; db -= 6) {
        const yy = yOf(db);
        const isZero = db === 0;
        ctx.strokeStyle = isZero ? 'rgba(148,163,184,.22)' : 'rgba(148,163,184,.10)';
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, yy + .5); ctx.lineTo(W, yy + .5); ctx.stroke();
        ctx.fillStyle = 'rgba(148,163,184,.5)';
        ctx.fillText((db > 0 ? '+' : '') + db, 4, yy);
      }

      // Build waveform points (oldest → newest, left → right)
      const inPts = [], outPts = [];
      for (let i = 0; i < BUF_LEN; i++) {
        const bi = (bufIdx + i) % BUF_LEN;
        const x = (i / (BUF_LEN - 1)) * W;
        inPts.push([x, yOf(inBuf[bi])]);
        outPts.push([x, yOf(outBuf[bi])]);
      }

      // 1) White output fill (what you hear)
      ctx.beginPath();
      ctx.moveTo(0, H);
      outPts.forEach(p => ctx.lineTo(p[0], p[1]));
      ctx.lineTo(W, H); ctx.closePath();
      const g = ctx.createLinearGradient(0, 0, 0, H);
      g.addColorStop(0, 'rgba(248,250,252,.55)');
      g.addColorStop(1, 'rgba(248,250,252,.06)');
      ctx.fillStyle = g; ctx.fill();

      // 2) Purple input line (pre-limiter), clipped to the region above the
      // ceiling: below it, output and input coincide (passthrough), so only
      // the clamping excess is drawn — no overlap with the white output.
      const cy = yOf(ceilingDb);
      ctx.save();
      ctx.beginPath();
      ctx.rect(0, 0, W, cy);
      ctx.clip();
      ctx.beginPath();
      inPts.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.strokeStyle = '#c084fc'; ctx.lineWidth = 1.3;
      ctx.globalAlpha = .5; ctx.stroke(); ctx.globalAlpha = 1;
      ctx.restore();

      // 3) White output line (post-limiter, what you hear), on top of the input
      ctx.beginPath();
      outPts.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.strokeStyle = '#f8fafc'; ctx.lineWidth = 1.6; ctx.stroke();

      // 4) Green ceiling line (dashed, drawn on top)
      ctx.strokeStyle = '#10b981'; ctx.lineWidth = 1.4;
      ctx.setLineDash([5, 3]);
      ctx.beginPath(); ctx.moveTo(0, cy + .5); ctx.lineTo(W, cy + .5); ctx.stroke();
      ctx.setLineDash([]);

      // 5) Gain reduction: a curve hanging down from the top (+12) edge, as deep as the
      // limiter turns the signal down (1 dB reduction = 1 dB on the scale).
      const yTop = yOf(TOP_DB);
      const grPts = [];
      let anyGr = false;
      for (let i = 0; i < BUF_LEN; i++) {
        const gr = grBuf[(bufIdx + i) % BUF_LEN];
        if (gr > 0.05) anyGr = true;
        grPts.push([(i / (BUF_LEN - 1)) * W, gr > 0.05 ? yOf(TOP_DB - gr) : yTop]);
      }
      if (anyGr) {
        ctx.beginPath();
        ctx.moveTo(0, yTop);
        grPts.forEach(p => ctx.lineTo(p[0], p[1]));
        ctx.lineTo(W, yTop); ctx.closePath();
        const gg = ctx.createLinearGradient(0, yTop, 0, yOf(TOP_DB - 24));
        gg.addColorStop(0, 'rgba(251,146,60,.50)');
        gg.addColorStop(1, 'rgba(251,146,60,.08)');
        ctx.fillStyle = gg; ctx.fill();
        ctx.beginPath();
        grPts.forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
        ctx.strokeStyle = '#fb923c'; ctx.lineWidth = 1.3; ctx.stroke();
      }

    }

    function loop() { draw(); raf = requestAnimationFrame(loop); }

    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);

    return {
      setInput(inDbL, inDbR, outDbL, outDbR, grDbL, grDbR) {
        const inDb  = Math.max(inDbL  == null ? -100 : inDbL,  inDbR  == null ? -100 : inDbR);
        const outDb = Math.max(outDbL == null ? -100 : outDbL, outDbR == null ? -100 : outDbR);
        // Gain reduction (dB) comes from the limiter's own applied gain, so it lines up with the output peaks.
        const gr = Math.max(grDbL || 0, grDbR || 0);
        pushSample(isFinite(inDb) ? inDb : -70, isFinite(outDb) ? outDb : -70, gr);
        if (inDb > peakIn) peakIn = inDb;
        if (outDb > peakOut) peakOut = outDb;
        showPeaks();
        if (!running) { running = true; raf = requestAnimationFrame(loop); }
      },
      setCeiling(db) {
        ceilingDb = db;
        ceilValEl.textContent = (db < 0 ? '\u2212' : '+') + Math.abs(db).toFixed(1) + ' dB';
      },
      destroy() { running = false; if (raf) cancelAnimationFrame(raf); ro.disconnect(); },
    };
  }

  function buildLimiterPanel(chId, def, s, rec) {
    const p = s.params;
    const wrap = document.createElement('div'); wrap.className = 'lim-panel';

    // Scope on top (full width) - one self-contained widget per open panel.
    const scopeHost = document.createElement('div'); scopeHost.className = 'lim-scope-host';
    const widget = createLimiterScope(scopeHost);
    widget.setCeiling(p.ceiling);
    if (rec) rec.widget = widget;

    // Knobs below in a row (like the mockup)
    const knobsRow = document.createElement('div'); knobsRow.className = 'lim-knobs';
    const knobs = document.createElement('div'); knobs.className = 'lim-knobs-inner';   // the part that zooms on hover
    knobsRow.appendChild(knobs);
    const refresh = () => { updateFxSlotValues(chId); commitMixer(); };
    def.params.forEach(pr => {
      const step = pr.step != null ? pr.step : (pr.max - pr.min) / 100;
      const fmt = v => (step >= 1 ? String(Math.round(v)) : v.toFixed(1)) + (pr.unit || '');
      knobs.appendChild(neCreateDial({
        label: pr.label, min: pr.min, max: pr.max, step, value: p[pr.k], format: fmt, def: pr.def,
        title: FX_PARAM_TIPS.lim[pr.k],
        onInput: v => { p[pr.k] = v; if (pr.k === 'ceiling') widget.setCeiling(v); refresh(); },
      }));
    });

    wrap.appendChild(scopeHost); wrap.appendChild(knobsRow);
    return wrap;
  }

  // Effect panels are created on demand by makeFxPanel() above - one per open
  // effect, appended to <body>, draggable by the header, closed via its X or
  // Options > "Close all effect panels".

  // --- FX slot value summary (inline, at-a-glance) ---
  function fxSlotSummary(key, s) {
    if (!key || !s.fx[key]) return '';
    const p = s.fx[key].params;
    switch (key) {
      case 'eq':     return `L:${p.lowGain}dB  M:${p.midGain}dB@${Math.round(p.midFreq)}Hz  H:${p.highGain}dB`;
      case 'comp':   return `Thr:${p.threshold}dB  ${p.ratio}:1  Atk:${p.attack}ms  Rel:${p.release}ms  MU:+${p.makeup}dB`;
      case 'sat':    return `Drive: ${p.drive}%`;
      case 'ts':     return `Atk:${p.attack}  Sus:${p.sustain}`;
      case 'lim':    return `Ceil:${p.ceiling}dB  Atk:${p.attack}ms  Rel:${p.release}ms`;
      case 'reverb': return `Size:${p.size}%  Decay:${p.decay}s  Mix:${p.mix}%`;
      case 'delay':  return `Time:${p.time}ms  FB:${p.feedback}%  Mix:${p.mix}%`;
      default:       return '';
    }
  }

  function updateFxSlotValues(chId) {
    const s = state[chId];
    if (!s || !s.fxValEls) return;
    for (let i = 0; i < s.fxValEls.length; i++) {
      const key = s.fxSlots[i];
      s.fxValEls[i].textContent = key ? fxSlotSummary(key, s) : '';
      s.fxValEls[i].classList.toggle('empty', !key);
    }
  }

  // =========================================================================
  // AUTO GAIN — per instrument strip. The button starts a run: the fader stays
  // untouched while the loudest peak (the same value the meter shows, post
  // fader) is recorded for at least the minimum time (Options) and until the peak has
  // not grown for a moment; then the fader moves once by (target - recorded peak). The signal is linear in the
  // fader, so the result lands on the target without overshoot. A run that is
  // cancelled or loses the transport is discarded.
  // =========================================================================
  const AUTO_SILENCE_DB = -50;    // peaks below this count as no signal (tails, noise floor)
  const AUTO_SETTLE_MS = 2000;    // after the minimum time a run ends once the peak has not grown for this long
  const AUTO_CAP_MS = 60000;      // a run ends at the latest after this long (with the loudest peak seen so far)
  const AUTO_MIN_KEY = 'dggAutoGainMinSec';
  let autoMinSec = parseFloat(dggGetItem(AUTO_MIN_KEY));   // minimum measuring time, Options > Auto Gain Time
  if (!(autoMinSec > 0)) autoMinSec = 10;
  const autoMinSelect = document.getElementById('dggAutoGainMinSelect');
  if (autoMinSelect) {
    autoMinSelect.value = String(autoMinSec);
    if (autoMinSelect.value !== String(autoMinSec)) autoMinSelect.value = '10';
    autoMinSelect.addEventListener('change', () => { autoMinSec = parseFloat(autoMinSelect.value); dggSetItem(AUTO_MIN_KEY, String(autoMinSec)); });
  }
  const AUTO_RAMP_MS = 400;       // fader glide to the result
  const autoRuns = new Map();     // channel id -> run record
  let autoAll = null;             // Auto All in progress: { stage: 'kit' | 'channels' | 'master', pending: Set of channel ids, undoItems }
  const autoUndoStack = [];       // one entry per Auto / Auto All: { items: [{ id, gainDb }] } with the fader values before the run
  const autoAllBtn = document.getElementById('mxAutoAllBtn');
  let hostPlaying = false;

  function transportActive() {
    return hostPlaying || (typeof playing !== 'undefined' && playing);
  }
  function fmtDb(v) { return (v >= 0 ? '+' : '') + v.toFixed(1); }
  function updateAutoTargetLabel(s) { if (s.autoTgtVal) s.autoTgtVal.textContent = fmtDb(s.autoTargetDb); }

  // Button + target number of one strip (instrument channels, master, kit gain); `s` is that strip's state object.
  function wireAutoControls(strip, id, s) {
    const autoBtn = strip.querySelector('[data-auto]');
    const tgtWrap = strip.querySelector('[data-auto-target]');
    const tgtVal = tgtWrap.querySelector('.auto-target-val');
    const tgtInput = tgtWrap.querySelector('.strip-db-input');
    autoBtn.dataset.tooltip = id === 'kit'
      ? 'Auto\nSets Kit Gain so the loudest instrument channel reaches the target peak. Starts playback if it is not running; click again to cancel.'
      : 'Auto\nSets the fader so the loudest hit reaches the target peak. Starts playback if it is not running; click again to cancel.';
    tgtWrap.dataset.tooltip = 'Target peak\nPeak level that Auto aims for. Double-click to type a value.';
    s.autoBtn = autoBtn; s.autoTgtVal = tgtVal;
    autoBtn.addEventListener('click', () => autoToggle(id));
    let tgtEditing = false;
    tgtWrap.addEventListener('dblclick', (e) => {
      e.stopPropagation();
      if (tgtEditing) return;
      tgtEditing = true;
      tgtInput.value = String(s.autoTargetDb);
      tgtVal.style.visibility = 'hidden';
      tgtInput.style.display = 'block';
      tgtInput.focus(); tgtInput.select();
    });
    const closeTgtEdit = (commit) => {
      if (!tgtEditing) return;
      tgtEditing = false;
      if (commit) {
        const nv = parseFloat(tgtInput.value.replace(',', '.'));
        if (!isNaN(nv)) {
          s.autoTargetDb = Math.min(18, Math.max(-60, nv));
          updateAutoTargetLabel(s);
          commitMixer();
        }
      }
      tgtInput.style.display = 'none';
      tgtVal.style.visibility = 'visible';
    };
    tgtInput.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); closeTgtEdit(true); }
      else if (e.key === 'Escape') { closeTgtEdit(false); }
    });
    tgtInput.addEventListener('blur', () => closeTgtEdit(true));
    updateAutoTargetLabel(s);
  }

  // The state object of a strip: channels/master live in `state`, Kit Gain has its own.
  function autoStateOf(id) { return id === 'kit' ? kitState : state[id]; }
  // Peak (dB) a run on `id` measures: the value of the strip's own meter (Kit Gain: the loudest channel input after Kit Gain).
  function autoPeakOf(id, lv) {
    const v = lv[id];
    return v ? Math.max(v[0], v[1]) : null;
  }

  function autoSetLocked(chId, locked) {
    const s = autoStateOf(chId); if (!s) return;
    if (s.faderEl) s.faderEl.disabled = locked;
    if (s.autoBtn) s.autoBtn.classList.toggle('active', locked);
  }
  function autoRefreshButtons() {
    if (autoAllBtn) autoAllBtn.classList.toggle('active', !!autoAll);
  }
  function autoCancel(chId) {
    const run = autoRuns.get(chId); if (!run) return;
    if (run.rampTimer) clearTimeout(run.rampTimer);
    autoRuns.delete(chId);
    autoSetLocked(chId, false);
    if (autoAll && autoAll.pending.delete(chId) && autoAll.pending.size === 0) autoAllNextStage();
    autoRefreshButtons();
  }
  function autoCancelAll() {
    autoPushAllUndo();   // what Auto All had already applied stays undoable
    autoAll = null;
    for (const id of Array.from(autoRuns.keys())) autoCancel(id);
    autoRefreshButtons();
  }
  function autoStart(chId) {
    const t0 = performance.now();
    autoRuns.set(chId, { t0, maxDb: -100, maxAtMs: t0, ramping: false });
    autoSetLocked(chId, true);
  }
  // Auto starts the playback itself when it is not running (Play pressed from the Mixer tab plays the song).
  function autoEnsurePlayback() {
    if (!transportActive()) { const pb = document.getElementById('playBtn'); if (pb && !pb.disabled) pb.click(); }
    return transportActive();
  }
  function autoToggle(chId) {
    if (autoRuns.has(chId)) { autoCancel(chId); return; }
    if (!autoEnsurePlayback()) return;
    autoStart(chId);
  }
  // Auto All runs in three stages: Kit Gain first (it moves every channel), then all instrument channels
  // at the same time (their results do not depend on each other), then Master on the finished channel
  // levels. A channel that heard nothing during the measuring time gets nothing set.
  function autoAllNextStage() {
    if (autoAll.stage === 'kit') {
      const ids = Object.keys(state).filter(id => id !== 'master' && state[id].autoBtn);
      autoAll.stage = 'channels';
      autoAll.pending = new Set(ids);
      if (!ids.length) { autoAllNextStage(); return; }
      for (const id of ids) autoStart(id);
    } else if (autoAll.stage === 'channels') {
      autoAll.stage = 'master';
      autoAll.pending = new Set(['master']);
      autoStart('master');
    } else {
      autoPushAllUndo();
      autoAll = null;
    }
  }
  function autoPushAllUndo() {
    if (autoAll && autoAll.undoItems && autoAll.undoItems.length) autoUndoStack.push({ items: autoAll.undoItems });
  }
  // Ctrl+Z on the Mixer tab: a running measurement is cancelled, otherwise the last Auto / Auto All is
  // taken back. Returns false when there is nothing to undo here (the caller then does the normal undo).
  function mxUndoAuto() {
    if (autoRuns.size || autoAll) { autoCancelAll(); return true; }
    const entry = autoUndoStack.pop();
    if (!entry) return false;
    for (const it of entry.items) {
      const s = autoStateOf(it.id);
      if (!s || !s.faderEl) continue;
      s.faderEl.value = String(it.gainDb);
      s.faderEl.dispatchEvent(new Event('input'));
    }
    return true;
  }
  window.mxUndoAuto = mxUndoAuto;
  function autoAllToggle() {
    if (autoAll) { autoCancelAll(); return; }
    if (!autoEnsurePlayback()) return;
    for (const id of Array.from(autoRuns.keys())) autoCancel(id);
    autoAll = { stage: 'kit', pending: new Set(['kit']), undoItems: [] };
    autoStart('kit');
    autoRefreshButtons();
  }
  if (autoAllBtn) autoAllBtn.addEventListener('click', autoAllToggle);
  function autoFinish(chId, run) {
    const s = autoStateOf(chId);
    if (!s || run.maxDb <= AUTO_SILENCE_DB) { autoCancel(chId); return; }
    const lo = parseFloat(s.faderEl.min), hi = parseFloat(s.faderEl.max);
    const from = s.gainDb;
    const wanted = from + (s.autoTargetDb - run.maxDb);
    // Needing more than the fader can give means the 'peak' is not a real hit (a tail, noise): change nothing.
    if (wanted > hi) { autoCancel(chId); return; }
    const to = Math.round(Math.max(lo, wanted) * 10) / 10;
    if (autoAll) autoAll.undoItems.push({ id: chId, gainDb: from });
    else autoUndoStack.push({ items: [{ id: chId, gainDb: from }] });
    run.ramping = true;
    const t0 = performance.now();
    const step = (now) => {
      const k = Math.min(1, (now - t0) / AUTO_RAMP_MS);
      const g = k >= 1 ? to : from + (to - from) * k;
      s.faderEl.disabled = false;
      s.faderEl.value = String(g);
      s.faderEl.dispatchEvent(new Event('input'));
      s.faderEl.disabled = true;
      if (k < 1) run.rampTimer = setTimeout(() => step(performance.now()), 16);
      else { autoCancel(chId); mixerUndoRebase(); }   // Auto Gain has its own undo; it is not a mixer edit
    };
    run.rampTimer = setTimeout(() => step(performance.now()), 0);
  }
  function autoFeed(chId, run, peakDb, now) {
    if (run.ramping) return;
    if (peakDb > run.maxDb + 0.05) { run.maxDb = peakDb; run.maxAtMs = now; }
    const elapsed = now - run.t0;
    if (elapsed < autoMinSec * 1000) return;
    // Heard nothing during the measuring time: nothing is set for this channel.
    if (run.maxDb <= AUTO_SILENCE_DB) { autoCancel(chId); return; }
    if (now - run.maxAtMs >= AUTO_SETTLE_MS || elapsed >= AUTO_CAP_MS) autoFinish(chId, run);
  }
  // Status line (top right, where 'ready' / 'playing…' shows): what Auto is doing and how long it still measures.
  let autoStatusShown = false;
  function autoUpdateStatus() {
    const el = document.getElementById('statusText');
    if (!el) return;
    if (!autoRuns.size) {
      if (autoStatusShown) { autoStatusShown = false; el.textContent = transportActive() ? 'playing…' : 'ready'; }
      return;
    }
    const now = performance.now();
    let remain = 0;
    for (const r of autoRuns.values()) remain = Math.max(remain, autoMinSec * 1000 - (now - r.t0));
    const nameOf = (id) => id === 'kit' ? 'Kit Gain' : (id === 'master' ? 'Master' : (state[id] && state[id].stripName) || id);
    const what = autoAll ? (autoAll.stage === 'kit' ? 'Kit Gain' : (autoAll.stage === 'master' ? 'Master' : 'Instruments'))
                         : (autoRuns.size === 1 ? nameOf(Array.from(autoRuns.keys())[0]) : 'Channels');
    const count = remain > 0 ? Math.ceil(remain / 1000) + ' s' : 'finishing…';
    el.textContent = 'Auto gain: ' + what + ' · ' + count + (autoAll ? ' · click Auto All to cancel' : '');
    autoStatusShown = true;
  }
  setInterval(() => {
    if (!transportActive() && autoRuns.size && !Array.from(autoRuns.values()).some(r => r.ramping)) autoCancelAll();
    autoRefreshButtons();
    autoUpdateStatus();
  }, 150);

  // =========================================================================
  // MIXER TIMELINE — read-only view of the song timeline (the same songEngine data and the same
  // playhead, tlPlayheadBeat) in the transport row of the Mixer tab. It fits the whole song into the
  // available width; clicking or dragging sets where playback starts. Re-rendered by renderTimeline().
  // =========================================================================
  const mxTl = document.getElementById('mxTimeline');
  const MX_TL_PAD = 8;
  let mxTlPxPerBeat = 1, mxTlHead = null, mxTlLoopEl = null, mxTlTotalBeats = 1;
  function mxBeatAtEvent(e) {
    const rect = mxTl.getBoundingClientRect();
    const sc = rect.width / mxTl.offsetWidth || 1;   // UI Scale enlarges the rects but not the layout px
    const x = (e.clientX - rect.left) / sc - mxTl.clientLeft - MX_TL_PAD;
    return Math.max(0, Math.min(mxTlTotalBeats, x / mxTlPxPerBeat));
  }
  function mxTimelineShowLoop(lo, hi) {
    if (!mxTlLoopEl) return;
    if (lo == null) { mxTlLoopEl.style.display = 'none'; return; }
    mxTlLoopEl.style.display = 'block';
    mxTlLoopEl.style.left = (MX_TL_PAD + lo * mxTlPxPerBeat) + 'px';
    mxTlLoopEl.style.width = ((hi - lo) * mxTlPxPerBeat) + 'px';
  }
  // Loop strip along the bottom edge: drag sets the loop range (playback then loops it and starts at its
  // beginning), a right-click clears it. Same rules as the loop row of the Song Generator.
  function attachMxLoopBar(bar) {
    let drag = null;
    bar.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      e.stopPropagation();
      if (hostPlaying) return;   // the host's transport cannot be moved from here
      const handle = e.target.closest('.loop-handle');
      if (handle && tlLoopStart != null) {
        // Dragging an edge of the existing loop: only that edge moves (the loop stays at least one bar long).
        drag = { edge: handle.dataset.edge, moved: true };
        try { bar.setPointerCapture(e.pointerId); } catch (_) {}
        return;
      }
      drag = { start: mxBeatAtEvent(e), moved: false };
      try { bar.setPointerCapture(e.pointerId); } catch (_) {}
    });
    bar.addEventListener('pointermove', (e) => {
      if (!drag) return;
      const beat = mxBeatAtEvent(e);
      if (drag.edge) {
        const num = readGrid().numerator;
        const lo = drag.edge === 'start' ? Math.min(beat, tlLoopEnd - num) : tlLoopStart;
        const hi = drag.edge === 'end' ? Math.max(beat, tlLoopStart + num) : tlLoopEnd;
        drag.lo = Math.max(0, lo); drag.hi = hi;
        mxTimelineShowLoop(drag.lo, drag.hi);
        return;
      }
      if (Math.abs(beat - drag.start) * mxTlPxPerBeat > 4) drag.moved = true;
      if (drag.moved) mxTimelineShowLoop(Math.min(drag.start, beat), Math.max(drag.start, beat));
    });
    const finish = (e) => {
      if (!drag) return;
      const d = drag; drag = null;
      if (d.edge) {
        if (d.lo !== undefined) {
          tlLoopStart = d.lo; tlLoopEnd = d.hi;
          if (d.edge === 'start') tlPlayheadBeat = tlLoopStart;   // playback starts at the loop's start
        }
      } else if (d.moved) {
        const beat = mxBeatAtEvent(e);
        const lo = Math.min(d.start, beat), hi = Math.max(d.start, beat);
        if (hi - lo >= readGrid().numerator) { tlLoopStart = lo; tlLoopEnd = hi; tlPlayheadBeat = lo; }   // at least one bar
      }
      renderTimeline();   // redraws the Song Generator and this view
    };
    bar.addEventListener('pointerup', finish);
    bar.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      if (hostPlaying) return;
      tlLoopStart = null; tlLoopEnd = null;
      renderTimeline();
    });
    bar.addEventListener('pointercancel', () => { drag = null; renderMixerTimeline(); });
  }
  function renderMixerTimeline() {
    if (!mxTl || !mxTl.clientWidth) return;
    const g = readGrid(), num = g.numerator;
    const slots = songEngine.songSequence;
    const endBar = slots.length ? Math.max(...slots.map(sl => songEngine.slotEndBar(sl))) : 0;
    const bars = Math.max(endBar, 4);
    const pxPerBar = (mxTl.clientWidth - 2 * MX_TL_PAD) / bars;
    mxTlPxPerBeat = pxPerBar / num; mxTlTotalBeats = bars * num;
    mxTl.innerHTML = '';
    const ruler = document.createElement('div'); ruler.className = 'mx-tl-ruler'; mxTl.appendChild(ruler);
    let stride = 1; while (stride * pxPerBar < 26) stride *= 2;
    for (let b = 0; b < bars; b += stride) {
      const mk = document.createElement('div'); mk.className = 'mx-tl-mark';
      mk.style.left = (MX_TL_PAD + b * pxPerBar) + 'px'; mk.textContent = String(b + 1); ruler.appendChild(mk);
    }
    slots.forEach(slot => {
      const block = findBlock(slot.blockId); if (!block) return;
      const c = document.createElement('div'); c.className = 'mx-tl-clip';
      c.style.left = (MX_TL_PAD + slot.startBar * pxPerBar) + 'px';
      c.style.width = Math.max(3, songEngine.slotSpanBars(slot) * pxPerBar - 1) + 'px';
      c.style.background = blockColor(slot.blockId); c.textContent = blockDisplayName(block);
      mxTl.appendChild(c);
    });
    // Loop row directly under the ruler, same order and look as the Song Generator and the Note Editor.
    const bar = document.createElement('div'); bar.className = 'mx-tl-loopbar'; mxTl.appendChild(bar);
    mxTlLoopEl = document.createElement('div'); mxTlLoopEl.className = 'mx-tl-loop'; bar.appendChild(mxTlLoopEl);
    ['start', 'end'].forEach(edge => {
      const h = document.createElement('div');
      h.className = 'loop-handle ' + edge; h.dataset.edge = edge;
      mxTlLoopEl.appendChild(h);
    });
    mxTimelineShowLoop(tlLoopStart, tlLoopEnd);
    attachMxLoopBar(bar);
    mxTlHead = document.createElement('div'); mxTlHead.className = 'dg-playhead'; mxTl.appendChild(mxTlHead);
    mxTimelineSetPlayhead(-1);
  }
  // beatPos < 0: idle, parked where playback will start; otherwise the live position.
  function mxTimelineSetPlayhead(beatPos) {
    if (!mxTlHead) return;
    const beat = beatPos < 0 ? tlPlayheadBeat : beatPos;
    mxTlHead.style.left = (MX_TL_PAD + beat * mxTlPxPerBeat) + 'px';
  }
  window.renderMixerTimeline = renderMixerTimeline;
  window.mxTimelineSetPlayhead = mxTimelineSetPlayhead;
  if (mxTl) {
    let mxTlDrag = false;
    const setFromEvent = (e) => {
      tlPlayheadBeat = mxBeatAtEvent(e);
      mxTimelineSetPlayhead(-1);
    };
    mxTl.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || hostPlaying) return;   // the host's transport cannot be moved from here
      mxTlDrag = true; playheadScrubbing = performance.now();   // the live position does not move the playhead while it is dragged
      try { mxTl.setPointerCapture(e.pointerId); } catch (_) {} setFromEvent(e);
    });
    mxTl.addEventListener('pointermove', (e) => { if (mxTlDrag) setFromEvent(e); });
    const endDrag = () => {
      if (!mxTlDrag) return;
      mxTlDrag = false; playheadScrubbing = 0;
      requestPlaySeek('song', tlPlayheadBeat);   // while the song plays here, it continues from the new position (once, when the drag ends)
      if (!transportActive()) highlightTimelineBeat(-1);
    };
    mxTl.addEventListener('pointerup', endDrag);
    mxTl.addEventListener('pointercancel', endDrag);
    // Redraw whenever the available width changes (tab switch, window or UI scale, the Auto All button appearing).
    if (typeof ResizeObserver === 'function') {
      // Always redraw when the box changes size (drawing does not resize it, so this cannot loop).
      new ResizeObserver(() => { if (mxTl.clientWidth) renderMixerTimeline(); }).observe(mxTl);
    } else {
      window.addEventListener('resize', () => renderMixerTimeline());
    }
  }

  // =========================================================================
  // HOST BRIDGE WIRING — connect this UI to the iPlug2 C++ mixer DSP.
  //   JS -> C++ : hostBridge.send('mixerState', payload)  (on every change)
  //   C++ -> JS : 'mixerLevels'  (live L/R meter peaks, from OnIdle)
  //               'mixerState'   (initial load / project restore)
  // The JSON schema here must match DrumGroover.cpp ParseMixerChannel/
  // ParseMixerSlot and dsp/Mixer.h (channel ids in kMixerChannelNames,
  // FX names in FxTypeName, params POSITIONAL per the FxParam enums).
  // =========================================================================
  const HAS_HOST = (typeof hostBridge !== 'undefined') && !!(hostBridge.isEmbedded);
  let _pushTimer = null;

  function serializeSlot(key, s) {
    if (!key) return null;
    const def = FX_DEFS[key];
    if (!def) return null;
    const fs = (s.fx && s.fx[key]) || { on: false, params: {} };
    const params = [];
    for (const p of def.params) params.push((p.k in fs.params) ? fs.params[p.k] : p.def);
    // A module present in a slot is active; set the slot to OFF to remove it.
    return { type: key, on: true, params };
  }

  function serializeChannel(s) {
    const fx = [];
    for (let i = 0; i < SLOT_COUNT; i++) fx.push(serializeSlot(s.fxSlots[i], s));
    return {
      gainDb: s.gainDb,
      pan: (s.pan || 0),
      mute: !!s.mute,
      solo: !!s.solo,
      sendReverb: (s.sendReverb == null ? 0 : s.sendReverb),
      sendDelay:  (s.sendDelay  == null ? 0 : s.sendDelay),
      autoTargetDb: (s.autoTargetDb == null ? 0 : s.autoTargetDb),
      fx,
    };
  }

  // Buses host the reverb/delay UNITS — their params come from bus.fx[0].p[].
  // The mockup's per-bus EQ/COMP slots are cosmetic; the DSP only reads the
  // bus's own module, so always send valid Reverb/Delay params (defaults).
  function serializeBus(id, s) {
    const key = (id === 'delayBus') ? 'delay' : 'reverb';
    const def = FX_DEFS[key];
    const fs = (s.fx && s.fx[key]) || { on: false, params: {} };
    const params = [];
    for (const p of def.params) params.push((fs.params && (p.k in fs.params)) ? fs.params[p.k] : p.def);
    return { gainDb: s.gainDb, pan: (s.pan || 0), mute: !!s.mute, fx: [ { type: key, on: true, params } ] };
  }

  function buildPayload() {
    const channels = {};
    for (const ch of FIXED_CHANNELS) channels[ch.id] = serializeChannel(state[ch.id]);
    return {
      kitGainDb: kitState.gainDb,
      kitAutoTargetDb: kitState.autoTargetDb,
      channels,
      master: serializeChannel(state.master),
      reverbBus: serializeBus('reverbBus', state.reverbBus),
      delayBus: serializeBus('delayBus', state.delayBus),
    };
  }

  function pushNow() {
    if (!HAS_HOST) return;
    try { hostBridge.send('mixerState', buildPayload()); }
    catch (e) { console.error('mixer-tab: send mixerState failed', e); }
  }

  // Coalesce rapid changes (fader/knob drags fire many events) into one send.
  function commitMixer() {
    presetNoteChanged();
    if (!HAS_HOST) return;
    if (_pushTimer) return;
    _pushTimer = setTimeout(() => { _pushTimer = null; pushNow(); }, 25);
  }

  // --- Apply a host-pushed 'mixerState' to the DOM (load / restore). --------
  function applyHostChannel(id, cj) {
    const s = state[id]; if (!s || !cj) return;
    if (typeof cj.gainDb === 'number') {
      s.gainDb = cj.gainDb;
      if (s.faderEl) s.faderEl.value = String(cj.gainDb);
      if (s.dbVal) s.dbVal.textContent = (s.gainDb >= 0 ? '+' : '') + s.gainDb.toFixed(1);
    }
    if (typeof cj.autoTargetDb === 'number') { s.autoTargetDb = cj.autoTargetDb; updateAutoTargetLabel(s); }
    if (typeof cj.pan === 'number') { s.pan = cj.pan; if (s.panKnob) s.panKnob.set(cj.pan * 100); }
    if (typeof cj.sendReverb === 'number' && s.revKnob) { s.sendReverb = cj.sendReverb; s.revKnob.set(cj.sendReverb * 100); }
    if (typeof cj.sendDelay  === 'number' && s.delKnob) { s.sendDelay  = cj.sendDelay;  s.delKnob.set(cj.sendDelay  * 100); }
    if (typeof cj.mute === 'boolean') { s.mute = cj.mute; if (s.muteBtn) s.muteBtn.classList.toggle('active', s.mute); }
    if (typeof cj.solo === 'boolean' && s.soloBtn) setExclusiveSolo(id, cj.solo);
    if (Array.isArray(cj.fx)) {
      for (let i = 0; i < SLOT_COUNT; i++) {
        const slot = cj.fx[i];
        const key = (slot && slot.type) ? slot.type : null;
        s.fxSlots[i] = key;
        if (s.fxSelects && s.fxSelects[i]) { s.fxSelects[i].value = key || ''; s.fxSelects[i].classList.toggle('off', !key); }
        if (key && FX_DEFS[key]) {
          if (!s.fx[key]) Object.assign(s.fx, makeFxState([key], FX_DEFS));
          if (Array.isArray(slot.params)) FX_DEFS[key].params.forEach((p, idx) => {
            if (typeof slot.params[idx] === 'number') s.fx[key].params[p.k] = slot.params[idx];
          });
        }
      }
      updateFxSlotValues(id);
    }
  }

  function applyHostState(json) {
    if (!json) return;
    autoCancelAll(); // a loaded state replaces the fader the run was measuring against
    autoUndoStack.length = 0;
    if (typeof json.kitAutoTargetDb === 'number') { kitState.autoTargetDb = json.kitAutoTargetDb; updateAutoTargetLabel(kitState); }
    if (typeof json.kitGainDb === 'number') {
      kitState.gainDb = json.kitGainDb;
      if (kitState.faderEl) kitState.faderEl.value = String(json.kitGainDb);
      if (kitState.dbVal) kitState.dbVal.textContent = (kitState.gainDb >= 0 ? '+' : '') + kitState.gainDb.toFixed(1);
    }
    if (json.channels && typeof json.channels === 'object')
      for (const id in json.channels) applyHostChannel(id, json.channels[id]);
    if (json.master) applyHostChannel('master', json.master);
    const applyBus = (id, bj) => {
      const s = state[id]; if (!s || !bj) return;
      if (typeof bj.gainDb === 'number') { s.gainDb = bj.gainDb; if (s.faderEl) s.faderEl.value = String(bj.gainDb); if (s.dbVal) s.dbVal.textContent = (s.gainDb >= 0 ? '+' : '') + s.gainDb.toFixed(1); }
      if (typeof bj.pan === 'number') { s.pan = bj.pan; if (s.panKnob) s.panKnob.set(bj.pan * 100); }
      if (typeof bj.mute === 'boolean') { s.mute = bj.mute; if (s.muteBtn) s.muteBtn.classList.toggle('active', s.mute); }
      // Bus module params (reverb/delay knobs): positional per FX_DEFS[key].params.
      const key = (id === 'delayBus') ? 'delay' : 'reverb';
      const def = FX_DEFS[key];
      if (def && bj.fx && bj.fx[0] && Array.isArray(bj.fx[0].params)) {
        bj.fx[0].params.forEach((val, idx) => {
          if (typeof val !== 'number') return;
          const p = def.params[idx];
          if (!p) return;
          if (!s.fx[key]) s.fx[key] = { on:true, params:{} };
          s.fx[key].params[p.k] = val;
          const ref = s['busKnob_' + p.k];
          if (ref) ref.set(val);
        });
      }
    };
    applyBus('reverbBus', json.reverbBus);
    applyBus('delayBus', json.delayBus);
  }

  // --- Live meters + restore, from the host audio thread. -------------------
  if (typeof hostBridge !== 'undefined' && hostBridge.on) {
    hostBridge.on('mixerLevels', (payload) => {
      const lv = payload && payload.levels;
      if (lv) for (const id in state) {
        const v = lv[id];
        if (v && state[id].meter) state[id].meter.setDb(v[0], v[1]);
      }
      if (lv && lv.kit && kitState.meter) kitState.meter.setDb(lv.kit[0], lv.kit[1]);
      if (lv && autoRuns.size) {
        const now = performance.now();
        for (const [id, run] of Array.from(autoRuns)) {
          const p = autoPeakOf(id, lv);
          if (p !== null) autoFeed(id, run, p, now);
        }
      }
      // Limiter scopes: feed every open limiter panel (pre-limiter input +
      // post-limiter output = channel final level). Several may be open at once.
      if (payload.limIn) {
        for (const rec of openFxPanels.values()) {
          if (!rec.widget) continue;
          const inV  = payload.limIn[rec.chId];
          const outV = payload.limOut ? payload.limOut[rec.chId] : null;
          const grV  = payload.limGr  ? payload.limGr[rec.chId]  : null;
          if (inV || outV) rec.widget.setInput(
            inV  ? inV[0]  : -100, inV  ? inV[1]  : -100,
            outV ? outV[0] : -100, outV ? outV[1] : -100,
            grV  ? grV[0]  : 0,    grV  ? grV[1]  : 0
          );
        }
      }
    });
    hostBridge.on('songPosition', (msg) => { hostPlaying = !!(msg && msg.playing); });
    hostBridge.on('mixerState', (payload) => { applyHostState(payload); presetState.name = ''; presetBaseline(); mixerUndoRebase(); });
    hostBridge.on('mixerPresetList', (payload) => { presetState.names = (payload && payload.presets) || []; });
    hostBridge.on('mixerPresetLoaded', (payload) => { presetState.name = (payload && payload.name) || ''; presetBaseline(); mixerUndoRebase(); });
    hostBridge.on('mixerPresetRenamed', (payload) => { if (payload && presetState.name === payload.name) { presetState.name = payload.newName; updatePresetButton(); } });
  }

  // Layout: Master group left, instruments right.
  const rackMaster = document.getElementById('rack-master');
  const rackInstr = document.getElementById('rack-instruments');
  renderStrip(rackMaster, { id:'master', name:'Master', kind:'master' });
  for (const bus of BUS_CHANNELS) renderStrip(rackMaster, bus);
  for (const ch of buildChannelList()) renderStrip(rackInstr, ch);
  renderKitGainStrip(rackInstr);

  // --- Presets: a menu above the strips. Default = the mixer the selected engine starts with; the others are the user's own
  // presets of this engine (stored by the host). Switching the engine loads that engine's default (see HandleSetEngineMode).
  function hostSend(type, payload) {
    if (!HAS_HOST) return;
    try { hostBridge.send(type, payload); } catch (e) { console.error('mixer-tab: send ' + type + ' failed', e); }
  }
  function updatePresetButton() {
    const b = document.getElementById('mixerPresetBtn');
    if (b) b.textContent = 'Preset: ' + (presetState.name || 'Default') + (presetState.modified ? ' *' : '') + ' ▾';
  }
  function presetBaseline() {
    presetState.snapshot = JSON.stringify(buildPayload());
    presetState.modified = false;
    updatePresetButton();
  }
  function presetNoteChanged() {
    if (presetState.snapshot === null) return;
    const m = JSON.stringify(buildPayload()) !== presetState.snapshot;
    if (m !== presetState.modified) { presetState.modified = m; updatePresetButton(); }
  }
  async function presetConfirmDiscard() {
    if (!presetState.modified) return true;
    return await showModal({ title: 'Discard mixer changes?', message: 'The mixer has changes that are not saved as a preset.', confirmLabel: 'Discard', danger: true });
  }
  async function presetAskName(title, value) {
    const v = await showModal({ title, fields: [{ label: 'Name', value: value || '', placeholder: 'My mix' }], confirmLabel: 'Save' });
    const nm = v && v[0] ? v[0].trim() : '';
    return nm || null;
  }
  // Resets the mixer to zero values: every fader 0 dB, pan centre, sends 0, mute / solo off, kit gain 0 dB. The effect slots stay as they are.
  function resetMixerToZero() {
    const beforeJson = mixerUndoJson();
    const zero = { gainDb: 0, pan: 0, mute: false, solo: false, sendReverb: 0, sendDelay: 0, autoTargetDb: 0 };
    const channels = {};
    for (const c of FIXED_CHANNELS) channels[c.id] = { ...zero };
    applyHostState({
      kitGainDb: 0,
      kitAutoTargetDb: 0,
      channels,
      master: { gainDb: 0, pan: 0, mute: false },
      reverbBus: { gainDb: 0, pan: 0, mute: false },
      delayBus: { gainDb: 0, pan: 0, mute: false },
    });
    commitMixer();
    mixerUndoStep('Reset mixer', beforeJson);
  }
  function renderPresetMenu() {
    const menu = document.getElementById('mixerPresetMenu');
    if (!menu) return;
    menu.innerHTML = '';
    const item = (label, mark, enabled, action) => {
      const b = document.createElement('button');
      b.className = 'add-block-menu-item';
      b.disabled = !enabled;
      b.innerHTML = '<span></span><span class="menu-hotkey"></span>';
      b.children[0].textContent = label;
      b.children[1].textContent = mark || '';
      b.addEventListener('click', (e) => { e.stopPropagation(); closeAllToolbarMenus(); action(); });
      menu.appendChild(b);
    };
    const sep = () => { const d = document.createElement('div'); d.className = 'add-block-menu-sep'; menu.appendChild(d); };
    item('Default', presetState.name === '' ? '✓' : '', true, async () => { if (await presetConfirmDiscard()) hostSend('mixerPresetDefault', {}); });
    if (presetState.names.length) sep();
    for (const nm of presetState.names)
      item(nm, nm === presetState.name ? '✓' : '', true, async () => { if (await presetConfirmDiscard()) hostSend('mixerPresetLoad', { name: nm }); });
    sep();
    const hasName = presetState.name !== '';
    item('Save', '', hasName, () => hostSend('mixerPresetSave', { name: presetState.name }));
    item('Save as…', '', true, async () => {
      const nm = await presetAskName('Save mixer preset', presetState.name);
      if (!nm) return;
      if (presetState.names.includes(nm) && nm !== presetState.name) {
        const ok = await showModal({ title: 'Replace preset?', message: 'A preset named "' + nm + '" already exists.', confirmLabel: 'Replace', danger: true });
        if (!ok) return;
      }
      hostSend('mixerPresetSave', { name: nm });
    });
    item('Rename…', '', hasName, async () => {
      const nm = await presetAskName('Rename mixer preset', presetState.name);
      if (nm && nm !== presetState.name) hostSend('mixerPresetRename', { name: presetState.name, newName: nm });
    });
    item('Delete…', '', hasName, async () => {
      const ok = await showModal({ title: 'Delete preset?', message: 'Delete "' + presetState.name + '"?', confirmLabel: 'Delete', danger: true });
      if (!ok) return;
      hostSend('mixerPresetDelete', { name: presetState.name });
      presetState.name = '';
      presetState.modified = true;
      updatePresetButton();
    });
    sep();
    item('Reset mixer (all zero)', '', true, resetMixerToZero);
  }
  if (typeof registerToolbarMenu === 'function') registerToolbarMenu('mixerPresetBtn', 'mixerPresetMenu', renderPresetMenu);
  presetBaseline();
  hostSend('mixerPresetList', {});

  // Switching the engine replaces the mixer with that engine's default: ask first when there are unsaved changes.
  const engineSel = document.getElementById('deEngineSelect');
  if (engineSel) {
    let enginePrev = engineSel.value, engineBypass = false;
    const rememberEngine = () => { enginePrev = engineSel.value; };
    engineSel.addEventListener('pointerdown', rememberEngine, true);
    engineSel.addEventListener('keydown', rememberEngine, true);
    engineSel.addEventListener('change', (e) => {
      if (engineBypass || !presetState.modified) { enginePrev = engineSel.value; return; }
      e.stopImmediatePropagation();
      const next = engineSel.value;
      engineSel.value = enginePrev;
      presetConfirmDiscard().then((ok) => {
        if (!ok) return;
        engineBypass = true;
        engineSel.value = next;
        engineSel.dispatchEvent(new Event('change'));
        engineBypass = false;
      });
    }, true);
  }

  // Undo for the mixer: the serialized state is compared with the last known one after every change, whatever caused it (mouse, wheel,
  // keyboard, mute / solo, effect slots, kit gain). A difference becomes one undo step that restores the values like a load does.
  let mixerUndoBaseline = null;   // serialized mixer as of the last undo step or host load
  // Solo is a monitoring state and the Auto Gain targets belong to the Auto function (own undo): neither is part of a mixer undo step.
  function mixerUndoJson() {
    const p = buildPayload();
    delete p.kitAutoTargetDb;
    const strip = (c) => { if (c) { delete c.solo; delete c.autoTargetDb; } };
    Object.values(p.channels).forEach(strip);
    strip(p.master);
    return JSON.stringify(p);
  }
  function mixerUndoRebase() { mixerUndoBaseline = mixerUndoJson(); }
  function mixerUndoRestore(s) { autoCancelAll(); applyHostState(JSON.parse(s)); commitMixer(); mixerUndoRebase(); }
  function mixerUndoStep(label, beforeJson) {
    const afterJson = mixerUndoJson();
    mixerUndoBaseline = afterJson;
    if (afterJson === beforeJson || typeof pushUndo !== 'function') return;
    pushUndo(label, () => mixerUndoRestore(beforeJson), () => mixerUndoRestore(afterJson), 'mixer');
  }
  (function wireMixerUndo() {
    const panel = document.querySelector('.tab-panel.mixer-panel');
    if (!panel) return;
    mixerUndoRebase();
    let timer = null;
    const commit = () => {
      const now = mixerUndoJson();
      const base = mixerUndoBaseline === null ? now : mixerUndoBaseline;
      mixerUndoBaseline = now;
      if (now === base) return;
      mixerUndoStep('Mixer edit', base);
    };
    // The delay lets the control's own handlers finish before the state is compared.
    const check = (delay) => { clearTimeout(timer); timer = setTimeout(commit, delay); };
    panel.addEventListener('change', () => check(80), true);
    panel.addEventListener('pointerup', () => check(80), true);
    panel.addEventListener('keyup', () => check(80), true);
    panel.addEventListener('wheel', () => check(350), { capture: true, passive: true });
  })();

  // Accessible hover-zoom for the per-slot knob panels (Pan/Rev/Del): one panel
  // per strip that pops out + scales on hover, like the drum-engine clusters.
  // The rack scrolls/clips horizontally, so this uses the pop-out path (a plain
  // :hover scale would get sliced off at the rack edge). Gated on the Accessible
  // toggle inside mixerWireKnobZoom. Buses have no knobs -> skipped.
  if (typeof mixerWireKnobZoom === 'function') {
    document.querySelectorAll('.mixer-rack .strip-knobs').forEach((panel) => {
      if (panel.querySelector('.knob-visual')) mixerWireKnobZoom(panel);
    });
  }

  // Fader travel length: the fader is a horizontally-rotated range input, so
  // its WIDTH is the travel. All fader rows have the same height (rack
  // stretch), so measure ONE row and publish its content height (10px row
  // padding already excluded by contentRect) as --fader-len on the panel.
  // The ResizeObserver also re-fires on tab/window resizes.
  const mixerPanel = document.querySelector('.tab-panel.mixer-panel');
  const faderRow = mixerPanel && mixerPanel.querySelector('.strip-fader-row');
  if (mixerPanel && faderRow && typeof ResizeObserver !== 'undefined') {
    new ResizeObserver((entries) => {
      const cr = entries.length && entries[entries.length - 1].contentRect;
      if (cr && cr.height > 0) mixerPanel.style.setProperty('--fader-len', Math.floor(cr.height) + 'px');
    }).observe(faderRow);
  }
})();
