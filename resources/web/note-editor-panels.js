// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// NOTE EDITOR — CONTROL PANELS
// Builds the sidebar/inline panels: block controls strip (length/turnaround/fill/zoom),
// dial widgets, Turnaround/Break/Fill knob panels. Each builder takes `ctx`
// and returns a detached DOM node; renderNoteEditor mounts it.
// =========================================

// Single source of truth for the Note Editor dial defaults. Both the initial
// value (`block.X ?? NE_DEFAULTS.x`) and each dial's reset target (`def:`) read
// from here, so changing a default in this one object updates both — no second
// place to edit. (subdivision / kickSubdivision are stored as VALUES; the dial
// index is derived via *_KNOB_VALUES.indexOf(...).)
const NE_DEFAULTS = {
  subdivision: 4,            // steps per beat → SUBDIVISION_KNOB_VALUES
  kickSubdivision: 1,        // → BREAK_KICK_SUBDIVISION_VALUES
  turnaroundDensity: 50,
  breakDensity: 100,
  fillDensity: 100,
  turnaroundKickDensity: 100,
  breakKickDensity: 100,
  fillKickDensity: 100,
};

// One undo step around a panel change; `fn` mutates the block and may return the promise of the regeneration it starts.
function neUndoable(label, fn) {
  return withProjectUndoAsync(label, async () => { await fn(); });
}

// Instrument-toggle checkbox rows (Turnaround/Break/Fill pickers)
// with drag-paint gesture across checkboxes. Shared builder with attachCheckboxGroupPaintDrag.
let instCbPaint = null; // state for a live paint-drag gesture

// `commit` is called after the gesture ends; must not run mid-drag to avoid re-render.
function attachCheckboxGroupPaintDrag(cb, applyMutation, commit) {
  cb._applyMutation = applyMutation;
  const applyTo = (el) => {
    if (!instCbPaint) return;
    if (!el || el.tagName !== 'INPUT' || el.type !== 'checkbox' || el.disabled) return;
    if (el.dataset.instGroup !== instCbPaint.groupTag) return;
    if (instCbPaint.touched.has(el)) return;
    instCbPaint.touched.add(el);
    el.checked = instCbPaint.target;
    el._applyMutation(instCbPaint.target);
  };
  cb.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    cb._pointerHandled = true;
    const target = !cb.checked;
    instCbPaint = { target, touched: new Set(), commit, groupTag: cb.dataset.instGroup, before: JSON.stringify(blocksList) };
    applyTo(cb);
    cb.setPointerCapture(e.pointerId);
  });
  cb.addEventListener('pointermove', (e) => {
    if (!instCbPaint) return;
    applyTo(document.elementFromPoint(e.clientX, e.clientY));
  });
  // The mouse click that follows pointerup would toggle the checkbox a second time (and fire `change`, undoing
  // the change) whenever the commit is asynchronous and the checkbox is still in the DOM.
  cb.addEventListener('click', (e) => {
    if (!cb._pointerHandled) return;
    cb._pointerHandled = false;
    e.preventDefault();
  });
  const finish = () => {
    setTimeout(() => { cb._pointerHandled = false; }, 0);
    if (!instCbPaint) return;
    const { touched, commit: doCommit, before } = instCbPaint;
    instCbPaint = null;
    if (touched.size) withProjectUndoAsync('Change instruments', async () => { await doCommit(); }, before);
  };
  cb.addEventListener('pointerup', finish);
  cb.addEventListener('pointercancel', finish);
  // Keyboard activation (Space/Enter) uses native change event.
  cb.addEventListener('change', () => {
    if (instCbPaint) return;
    const before = JSON.stringify(blocksList);
    cb._applyMutation(cb.checked);
    withProjectUndoAsync('Change instruments', async () => { await commit(); }, before);
  });
}

// Renders one row of instrument-toggle checkboxes into `container`.
// `getArr`/`setArr` read/write the backing array on the block.
// `groupTag` scopes paint-drag to this row.
function neInstrumentCheckboxRow(container, instrumentList, getArr, setArr, commit, groupTag, disabled, title) {
  instrumentList.forEach(([val, label]) => {
    const lab = document.createElement('label');
    lab.style.cssText = 'display:flex;align-items:center;gap:6px;font-size:10px;color:var(--text-dim);cursor:pointer;white-space:nowrap;';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = getArr().includes(val);
    cb.disabled = !!disabled;
    cb.dataset.instGroup = groupTag;
    if (title) { cb.dataset.tooltip = title; lab.dataset.tooltip = title; }
    attachCheckboxGroupPaintDrag(cb, (checked) => {
      const set = new Set(getArr());
      checked ? set.add(val) : set.delete(val);
      setArr([...set]);
    }, commit);
    lab.appendChild(cb); lab.appendChild(document.createTextNode(label));
    container.appendChild(lab);
  });
}

// Until a seed exists a Break/Intro/Outro generates its fixed hand pattern, which ignores the dials and all
// instruments except Snare/Toms. The first user change switches the block to seeded generation.
function neEnsureSeed(block, key) {
  if (block[key] === undefined) block[key] = Math.floor(Math.random() * 1e9);
}

// Name of a Break, Intro or Outro block, used for its toolbar clusters and sidebar labels.
function neFillKind(block) {
  if (isBreakBlock(block)) return 'Break';
  return block.fillAnchor === 'start' ? 'Outro' : 'Intro';
}

// Blue heading above the settings of a panel group.
function neSidebarHeading(text) {
  const heading = document.createElement('div');
  heading.className = 'ne-sidebar-heading';
  heading.textContent = text;
  return heading;
}

// Fill instrument picker: a label above the checkboxes, which are indented under it.
function neAppendFillInstruments(group, box, text) {
  const label = document.createElement('div');
  label.className = 'ne-ctl-label';
  label.textContent = text || 'Fill instruments:';
  label.style.marginBottom = '4px';
  box.style.marginLeft = '12px';
  group.appendChild(label);
  group.appendChild(box);
}

function neShowBreakLockPill(ctx) {
  const { block } = ctx;
  const parent = breakParentOf(block);
  neSetStatOverride((pills) => {
    pills.innerHTML = `<span class="pill ne-lock-pill">🔒 The copied bars are synced with <b>${parent ? parent.name : '?'}</b> and locked here. Edit ${parent ? parent.name : 'the parent block'} directly, or turn on "Edit whole block" in the Break tab.</span>`;
  }, 10000);
}

// Control strip: block length, Break Length (for Break blocks), Turnaround/Fill/Build, Zoom.
function neBuildControlsPanel(ctx) {
  const { block, g, blockLen, numerator, isBreak } = ctx;

  const controls = document.createElement('div');
  controls.className = 'ne-controls';

  if (block.isFillBlock) controls.appendChild(neSidebarHeading(neFillKind(block)));


  // Length (Bars)
  const lenGroup = document.createElement('div');
  lenGroup.className = 'ne-ctl-group ne-ctl-inline';
  lenGroup.innerHTML = `<span class="ne-ctl-label">${block.isFillBlock ? neFillKind(block) + ' Length' : 'Length (Bars)'}</span>`;

  if (isBreak) {
    const parent = breakParentOf(block);
    const lenReadout = document.createElement('span');
    lenReadout.className = 'ne-len-input ne-len-readonly';
    lenReadout.textContent = String(blockLen);
    lenReadout.dataset.tooltip = 'Length\nInherited from the parent block — change it there, not here.';
    lenGroup.appendChild(lenReadout);
  } else {
    const lenInput = document.createElement('input');
    lenInput.type = 'number'; lenInput.className = 'ne-len-input';
    lenInput.min = '1'; lenInput.max = String(BLOCK_MAX_LENGTH); lenInput.step = '1';
    lenInput.value = String(blockLen);
    lenInput.dataset.tooltip = `Length\nThis block's length in bars (1–${BLOCK_MAX_LENGTH}). Shrinking discards trailing bars; growing continues the existing patterns.`;
    const commitLen = async () => {
      let v = parseInt(lenInput.value);
      if (isNaN(v)) { lenInput.value = String(block.length || 1); return; }
      v = Math.max(1, Math.min(BLOCK_MAX_LENGTH, v));
      if (v === (block.length || 1)) { lenInput.value = String(v); return; }
      const label = `Length: ${v} (${blockDisplayName(block)})`;
      if (block.isFillBlock) {
        await withProjectUndoAsync(label, async () => {
          block.length = v;
          const res = await requestRegenerateFillBlock(block, g);
          if (!res) return;
          block.data = res.data;
          block.length = res.length;
          block.fillBeats = res.fillBeats;
          block.feel = res.feel;
          block.generated = false;
          rebuildFullFromEditedBlocks();
          renderNoteEditor();
        });
        return;
      }
      await withProjectUndoAsync(label, async () => {
        resizeBlockLength(block, g, v);
        block.generated = false;
        // Growing only copies the plain notes into the new bars; the turnaround fills of the unlocked groups are laid out over the whole new length.
        if ((block.turnaroundInterval || 0) > 0) applyAllUnlockedGroups(block, g);
        suppressKnobRegen = true;
        syncPhraseKnobRanges(block);
        suppressKnobRegen = false;
        await syncBreakLengthsToParent(block, g);
        rebuildFullFromEditedBlocks();
        renderNoteEditor();
      });
    };
    // Leaving the field after a commit lets Ctrl+Z reach the app instead of the text box.
    lenInput.addEventListener('change', async () => { await commitLen(); lenInput.blur(); });
    lenInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); lenInput.blur(); } });
    lenGroup.appendChild(lenInput);
  }
  controls.appendChild(lenGroup);
  if (!isBreak) lenGroup.classList.add('ne-ctl-nosep');

  // Feel
  if (!isBreak) {
    controls.appendChild(neBuildFeelGroup(ctx));
  }

  // Turnaround (regular), Break (Break), or Fill (standalone Intro/Outro)
  if (isBreak) {
    neBuildBreakPanel(controls, ctx);
  } else if (block.isFillBlock) {
    neBuildFillPanel(controls, ctx);
  } else {
    neBuildTurnaroundPanel(controls, ctx);
  }

  return controls;
}

// Feel select: block-wide groove flavor. For ordinary blocks, regenerates unlocked groups;
// for Fill blocks, uses bridge-based regeneration.
function neBuildFeelGroup(ctx) {
  const { block, g } = ctx;
  const wrap = document.createElement('div');
  wrap.className = 'ne-ctl-group ne-ctl-inline';
  wrap.innerHTML = '<span class="ne-ctl-label">Feel</span>';
  const sel = document.createElement('select');
  sel.className = 'ne-select';
  sel.innerHTML = `<option value="straight">Straight</option><option value="swing">Swing</option><option value="halftime">Half-Time</option>`;
  sel.value = block.feel || 'straight';
  sel.dataset.tooltip = 'Feel\nThis block\'s groove feel — Straight, Swing or Half-Time. Applies only to this block.';
  wireCustomSelect(sel);
  sel.addEventListener('change', async () => {
    if (sel.value === (block.feel || 'straight')) return;
    const newFeel = sel.value;
    if (block.isFillBlock) {
      await withBlockDataUndoAsync(`Feel: ${sel.selectedOptions[0].text} (${blockDisplayName(block)})`, block, async () => {
        block.feel = newFeel;
        const res = await requestRegenerateFillBlock(block, g);
        if (!res) return;
        block.data = res.data;
        block.length = res.length;
        block.fillBeats = res.fillBeats;
        block.feel = res.feel;
        block.generated = false;
      });
      return;
    }
    withBlockDataUndo(`Feel: ${sel.selectedOptions[0].text} (${blockDisplayName(block)})`, block, () => {
      block.feel = sel.value;
      block._turnaroundFillCache = {};
      applyKickGroupToBlock(block, g);
      applySnareGroupToBlock(block, g);
      applyHihatGroupToBlock(block, g);
      applyRideGroupToBlock(block, g);
      applyTomsGroupToBlock(block, g);
      applyCymbalsGroupToBlock(block, g);
    });
  });
  wrap.appendChild(sel);
  return wrap;
}

// Mapping from fill/turnaround instrument option to engine group.
const FILL_INSTRUMENT_TO_ENGINE_GROUP = { toms:'toms', snare:'snare', hihat:'hihat', floorTom:'toms', crash:'cymbals', kick:'kick' };
const ALL_ENGINE_GROUPS = ['kick','snare','hihat','ride','toms','cymbals'];

function neRelevantEngineGroups(instruments) {
  return [...new Set((instruments||[]).map(i => FILL_INSTRUMENT_TO_ENGINE_GROUP[i]).filter(Boolean))];
}

// Displays lock icons and disables controls if all relevant groups are locked.
function neApplyGroupLockState(labelEl, inputEl, groups, block, opts) {
  const showIcons = !opts || opts.showIcons !== false;
  const lockedGroups = groups.filter(g => block.lockedGroups && block.lockedGroups[g]);
  if (showIcons) {
    lockedGroups.forEach(g => {
      const icon = document.createElement('span');
      icon.className = 'group-lock-icon';
      icon.textContent = '🔒';
      icon.dataset.tooltip = 'Locked\nThis group was manually edited in the note editor. Click to discard those edits and let the knobs control it again.';
      icon.addEventListener('click', (e) => { e.stopPropagation(); unlockGroup(g); });
      labelEl.appendChild(icon);
    });
  }
  const fullyLocked = groups.length > 0 && lockedGroups.length === groups.length;
  inputEl.disabled = fullyLocked;
  inputEl.closest('.ne-ctl-group').classList.toggle('ne-ctl-locked', fullyLocked);
}

// ── Compact sidebar dials (rotary knobs) ──
// Built fresh every render; uses wireKnob from ui-controller.js.
// opts: { label, min, max, step, value, format(v)->string, title, onInput(v), onChange(v) }
function neCreateDial(opts) {
  const ctrlBox = document.createElement('div');
  ctrlBox.className = 'ctrl';
  if (opts.def != null) ctrlBox.dataset.def = String(opts.def); // Ctrl+Click → Reset to default
  const format = opts.format || (v => String(v));
  const labelEl = document.createElement('div');
  labelEl.className = 'ctrl-label';
  labelEl.appendChild(document.createTextNode(opts.label + ' '));
  const valSpan = document.createElement('span');
  valSpan.className = 'val';
  valSpan.textContent = format(opts.value);
  labelEl.appendChild(valSpan);
  ctrlBox.appendChild(labelEl);

  const slider = document.createElement('input');
  slider.type = 'range';
  slider.min = String(opts.min); slider.max = String(opts.max); slider.step = String(opts.step ?? 1);
  slider.value = String(opts.value);
  if (opts.title) ctrlBox.dataset.tooltip = opts.title;
  ctrlBox.appendChild(slider);

  slider.addEventListener('input', () => { valSpan.textContent = format(+slider.value); if (opts.onInput) opts.onInput(+slider.value); });
  if (opts.onChange) slider.addEventListener('change', () => opts.onChange(+slider.value));

  wireKnob(slider);
  ctrlBox.slider = slider;
  return ctrlBox;
}

// A Note Editor toolbar cluster built from dials, in the look of the static group clusters. It carries
// `titleLabel` and takes the lock class like a dial row, so the lock/reroll helpers work on it unchanged.
function neToolbarCluster(titleText, dials) {
  const cluster = document.createElement('div');
  cluster.className = 'knob-cluster ne-fill-cluster';
  const title = document.createElement('div');
  title.className = 'cluster-title';
  title.appendChild(document.createTextNode(titleText + ' '));
  const knobs = document.createElement('div');
  knobs.className = 'cluster-knobs';
  dials.forEach(d => knobs.appendChild(d));
  cluster.appendChild(title);
  cluster.appendChild(knobs);
  cluster.titleLabel = title;
  cluster.addEventListener('mouseenter', () => { if (!cluster.classList.contains('ne-hover-hold')) setClusterZoomOrigin(cluster); });
  document.getElementById('neFillClusters').appendChild(cluster);
  return cluster;
}

// Lays out a row of dials with an optional heading.
function neDialRow(titleText, dials) {
  const group = document.createElement('div');
  group.className = 'ne-ctl-group ne-dial-row';
  let titleLabel = null;
  if (titleText) {
    titleLabel = document.createElement('span');
    titleLabel.className = 'ne-ctl-label';
    titleLabel.textContent = titleText;
    group.appendChild(titleLabel);
  }
  const items = document.createElement('div');
  items.className = 'ne-dial-row-items';
  dials.forEach(d => items.appendChild(d));
  group.appendChild(items);
  group.titleLabel = titleLabel;
  return group;
}

// Adds a reroll dice icon to the row's title or first dial label.
function neAddRerollIcon(row, dials, title, onClick) {
  const iconHost = row.titleLabel || (dials[0] && dials[0].querySelector('.ctrl-label'));
  if (!iconHost) return;
  const icon = document.createElement('span');
  icon.className = 'group-lock-icon';
  icon.style.cursor = 'pointer';
  icon.textContent = '🎲';
  icon.dataset.tooltip = title;
  icon.addEventListener('click', (e) => { e.stopPropagation(); onClick(); });
  iconHost.appendChild(icon);
}

// Applies lock state to a whole dial row (one icon per locked group, disables all dials if fully locked).
function neApplyDialRowLock(row, dials, groups, block) {
  const iconHost = row.titleLabel || (dials[0] && dials[0].querySelector('.ctrl-label'));
  const lockedGroups = groups.filter(g => block.lockedGroups && block.lockedGroups[g]);
  if (iconHost) {
    lockedGroups.forEach(g => {
      const icon = document.createElement('span');
      icon.className = 'group-lock-icon';
      icon.textContent = '🔒';
      icon.dataset.tooltip = 'Locked\nThis group was manually edited in the note editor. Click to discard those edits and let the knobs control it again.';
      icon.addEventListener('click', (e) => { e.stopPropagation(); unlockGroup(g); });
      iconHost.appendChild(icon);
    });
  }
  const fullyLocked = groups.length > 0 && lockedGroups.length === groups.length;
  dials.forEach(d => { if (d.slider) d.slider.disabled = fullyLocked; });
  row.classList.toggle('ne-ctl-locked', fullyLocked);
  return fullyLocked;
}

function neBuildTurnaroundPanel(panel, ctx) {
  const { block, g } = ctx;
  const fillGroups = neRelevantEngineGroups(block.turnaroundInstruments || ['snare']);

  panel.appendChild(neSidebarHeading('Turnaround (Fill)'));
  const taGroup = document.createElement('div');
  taGroup.className = 'ne-ctl-group ne-ctl-inline';
  taGroup.innerHTML = '<span class="ne-ctl-label">Fill Every</span>';
  const taInput = document.createElement('input');
  taInput.type = 'number'; taInput.className = 'ne-len-input';
  taInput.min = '0'; taInput.max = String(BLOCK_MAX_LENGTH); taInput.step = '1';
  taInput.value = String(block.turnaroundInterval || 0);
  taInput.dataset.tooltip = `Fill Every\nHow often a turnaround (fill) occurs: every Nth bar, any whole number up to ${BLOCK_MAX_LENGTH} (the last bar of each phrase). 0 turns it off.`;
  const commitTa = () => {
    const current = block.turnaroundInterval || 0;
    let v = parseInt(taInput.value, 10);
    if (isNaN(v)) { taInput.value = String(current); return; }
    v = Math.max(0, Math.min(BLOCK_MAX_LENGTH, v));
    taInput.value = String(v);
    if (v === current) return;
    neUndoable('Fill every', () => {
      block.turnaroundInterval = v;
      block.generated = false;
      applyAllUnlockedGroups(block, g);
    });
  };
  // Leaving the field after a commit lets Ctrl+Z reach the app instead of the text box.
  taInput.addEventListener('change', () => { commitTa(); taInput.blur(); });
  taInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); taInput.blur(); } });
  taGroup.appendChild(taInput);
  panel.appendChild(taGroup);
  taGroup.classList.add('ne-ctl-nosep');

  const applyTurnaroundChange = () => {
    block.generated = false;
    applyAllUnlockedGroups(block, g);
  };
  const turnaroundUndoable = (label, mutate) => neUndoable(label, () => { mutate(); applyTurnaroundChange(); });

  // Turnaround Length (Beats)
  const tlGroup = document.createElement('div');
  tlGroup.className = 'ne-ctl-group ne-ctl-inline';
  const tlMaxBeats = g.numerator;
  tlGroup.innerHTML = '<span class="ne-ctl-label">Fill Length (Beats)</span>';
  const tlInput = document.createElement('input');
  tlInput.type = 'number'; tlInput.className = 'ne-len-input';
  tlInput.min = '0.25'; tlInput.max = String(tlMaxBeats); tlInput.step = '0.25';
  tlInput.value = String(resolveTurnaroundLengthBeats(block, g.numerator));
  tlInput.dataset.tooltip = `Fill Length\nHow much of the bar-end the turnaround covers, in beats (max ${tlMaxBeats} = one full bar).`;
  const commitTL = () => {
    let v = parseFloat(tlInput.value);
    const current = resolveTurnaroundLengthBeats(block, g.numerator);
    if (isNaN(v)) { tlInput.value = String(current); return; }
    v = Math.max(0.25, Math.min(tlMaxBeats, Math.round(v*4)/4));
    if (v === current) { tlInput.value = String(v); return; }
    turnaroundUndoable('Fill length', () => { block.turnaroundLengthBeats = v; });
  };
  tlInput.addEventListener('change', commitTL);
  tlInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); commitTL(); } });
  tlGroup.appendChild(tlInput);
  panel.appendChild(tlGroup);
  tlGroup.classList.add('ne-ctl-nosep');

  // Closing Crash
  const tcGroup = document.createElement('div');
  tcGroup.className = 'ne-ctl-group';
  const tcLabel = document.createElement('label');
  tcLabel.style.cssText = 'display:flex;align-items:center;gap:6px;cursor:pointer;';
  tcLabel.dataset.tooltip = 'Closing Crash\nAdds a crash hit on the downbeat of the bar after the turnaround.';
  const tcCheck = document.createElement('input');
  tcCheck.type = 'checkbox'; tcCheck.checked = block.turnaroundCrash !== false;
  tcCheck.dataset.tooltip = tcLabel.dataset.tooltip;
  tcCheck.addEventListener('change', () => turnaroundUndoable('Closing crash', () => { block.turnaroundCrash = tcCheck.checked; }));
  const tcText = document.createElement('span');
  tcText.className = 'ne-ctl-label';
  tcText.style.textTransform = 'none';
  tcText.textContent = 'Closing Crash';
  tcLabel.appendChild(tcCheck); tcLabel.appendChild(tcText);
  tcGroup.appendChild(tcLabel);
  panel.appendChild(tcGroup);
  tcGroup.classList.add('ne-ctl-nosep');
  neApplyGroupLockState(tcText, tcCheck, ['cymbals'], block);

  // Vary per Occurrence
  const tvpGroup = document.createElement('div');
  tvpGroup.className = 'ne-ctl-group';
  const tvpLabel = document.createElement('label');
  tvpLabel.style.cssText = 'display:flex;align-items:center;gap:6px;cursor:pointer;';
  tvpLabel.dataset.tooltip = 'Vary per Occurrence\nOff: every turnaround in this block plays the identical fill. On: each occurrence gets its own variation.';
  const tvpCheck = document.createElement('input');
  tvpCheck.type = 'checkbox'; tvpCheck.checked = !!block.turnaroundVaryPerOccurrence;
  tvpCheck.dataset.tooltip = tvpLabel.dataset.tooltip;
  tvpCheck.addEventListener('change', () => turnaroundUndoable('Vary per occurrence', () => { block.turnaroundVaryPerOccurrence = tvpCheck.checked; }));
  const tvpText = document.createElement('span');
  tvpText.className = 'ne-ctl-label';
  tvpText.style.textTransform = 'none';
  tvpText.textContent = 'Vary per Occurrence';
  tvpLabel.appendChild(tvpCheck); tvpLabel.appendChild(tvpText);
  tvpGroup.appendChild(tvpLabel);
  panel.appendChild(tvpGroup);

  // Fill Subdivision/Amount
  const tsIdx = Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(block.turnaroundSubdivision ?? NE_DEFAULTS.subdivision));
  const tsDial = neCreateDial({
    label: 'Subdiv', min: 0, max: SUBDIVISION_KNOB_VALUES.length - 1, step: 1, value: tsIdx,
    def: Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(NE_DEFAULTS.subdivision)), // Default: subdivision
    format: v => SUBDIVISION_KNOB_LABELS[v],
    title: 'Subdivision\nResolution of the fill in steps per beat.',
    onChange: v => turnaroundUndoable('Fill subdivision', () => { block.turnaroundSubdivision = SUBDIVISION_KNOB_VALUES[v]; }),
  });
  const tdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.turnaroundDensity ?? NE_DEFAULTS.turnaroundDensity,
    def: NE_DEFAULTS.turnaroundDensity,
    format: v => v + '%',
    title: 'Amount\nHow many hits land within the fill window — independent of its length.',
    onChange: v => turnaroundUndoable('Fill amount', () => { block.turnaroundDensity = v; }),
  });
  const fillDialRow = neToolbarCluster('Fill', [tsDial, tdDial]);
  const fillLocked = neApplyDialRowLock(fillDialRow, [tsDial, tdDial], fillGroups, block);
  fillDialRow.classList.add('ne-ctl-nosep');
  neAddRerollIcon(fillDialRow, [tsDial, tdDial], 'Reroll\nGenerates a new fill with a fresh random seed — Length, Amount and instruments stay unchanged.', () => {
    turnaroundUndoable('Reroll fill', () => { block.turnaroundSeed = Math.floor(Math.random()*1e9); });
  });

  // Instrument picker
  const tiGroup = document.createElement('div');
  tiGroup.className = 'ne-ctl-group';
  const tiBox = document.createElement('div');
  tiBox.className = 'ne-ctl-row-items';
  tiBox.style.cssText = 'display:flex;flex-direction:column;align-items:flex-start;gap:4px;';
  neInstrumentCheckboxRow(tiBox, [['snare','Snare'],['toms','Toms'],['hihat','Hi-Hat'],['floorTom','Floor Tom'],['crash','2x Crash']],
    () => block.turnaroundInstruments || ['snare'],
    (arr) => { block.turnaroundInstruments = arr; },
    applyTurnaroundChange, 'turnaround-fill', fillLocked,
    'Fill Instruments\nWhich instruments the fill can use. Drag across checkboxes to set several at once.');
  neAppendFillInstruments(tiGroup, tiBox);
  panel.appendChild(tiGroup);
  tiGroup.classList.toggle('ne-ctl-locked', fillLocked);

  // Kick layer
  const tksIdx = Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(block.turnaroundKickSubdivision ?? NE_DEFAULTS.kickSubdivision));
  const tksDial = neCreateDial({
    label: 'Subdiv', min: 0, max: BREAK_KICK_SUBDIVISION_VALUES.length - 1, step: 1, value: tksIdx,
    def: Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(NE_DEFAULTS.kickSubdivision)), // Default: kick subdivision
    format: v => BREAK_KICK_SUBDIVISION_LABELS[v],
    title: 'Subdivision\nResolution of the Fill Kick pulse in steps per beat.',
    onChange: v => turnaroundUndoable('Fill kick subdivision', () => { block.turnaroundKickSubdivision = BREAK_KICK_SUBDIVISION_VALUES[v]; }),
  });
  const tkdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.turnaroundKickDensity ?? NE_DEFAULTS.turnaroundKickDensity,
    def: NE_DEFAULTS.turnaroundKickDensity,
    format: v => v + '%',
    title: 'Amount\nHow full the Fill Kick pulse is — 100% = a hit on every step.',
    onChange: v => turnaroundUndoable('Fill kick amount', () => { block.turnaroundKickDensity = v; }),
  });
  const kickDialRow = neToolbarCluster('Fill Kick', [tksDial, tkdDial]);
  neApplyDialRowLock(kickDialRow, [tksDial, tkdDial], ['kick'], block);
  neAddRerollIcon(kickDialRow, [tksDial, tkdDial], 'Reroll\nGenerates a new Fill Kick pulse with a fresh random seed. Only audible below 100% Amount.', () => {
    turnaroundUndoable('Reroll fill kick', () => { block.turnaroundKickSeed = Math.floor(Math.random()*1e9); });
  });
}

// Break panel — fill inside a Break block's trailing breakBeats window.
function neBuildBreakPanel(panel, ctx) {
  const { block, g, blockLen, blockBeats } = ctx;
  const parent = breakParentOf(block);

  const mainGroups = () => neRelevantEngineGroups(block.breakInstruments || ['toms']);
  const mainIsLocked = () => mainGroups().some(gid => block.lockedGroups && block.lockedGroups[gid]);
  const kickIsLocked = () => !!(block.lockedGroups && block.lockedGroups.kick);

  const regenerateMain = async () => {
    if (mainIsLocked()) return;
    const res = await requestRegenerateBreakMain(block, g);
    if (!res) return;
    Object.assign(block.data, res.tracks);
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  };
  const regenerateKickOnly = async () => {
    if (kickIsLocked()) return;
    const res = await requestRegenerateBreakKick(block, g);
    if (!res) return;
    block.data.kick = res.kick;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  };
  const regenerateLength = async () => {
    const res = await requestRegenerateBreakBlock(block, g, { forceAll: true });
    if (!res) return;
    block.data = res.data;
    block.length = res.length;
    block.breakBeats = res.breakBeats;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  };
  const regenerateMainAlways = async () => {
    const res = await requestRegenerateBreakMain(block, g);
    if (!res) return;
    Object.assign(block.data, res.tracks);
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  };

  // Edit whole block toggle
  const ubGroup = document.createElement('div');
  ubGroup.className = 'ne-ctl-group';
  const ubLabel = document.createElement('label');
  ubLabel.style.cssText = 'display:flex;align-items:center;gap:6px;cursor:pointer;';
  const ubCheck = document.createElement('input');
  ubCheck.type = 'checkbox'; ubCheck.checked = !!block.breakUnlocked;
  ubCheck.dataset.tooltip = 'Edit whole block\nAllows direct editing of the copied bars — they then stop following the parent block. Turning this off discards hand-edits and re-syncs them with the parent.';
  ubLabel.dataset.tooltip = ubCheck.dataset.tooltip;
  ubCheck.addEventListener('change', () => neUndoable('Edit whole block', async () => {
    block.breakUnlocked = ubCheck.checked;
    if (!block.breakUnlocked) {
      const res = await requestRegenerateBreakBlock(block, g, { forceAll: true });
      if (!res) return;
      block.data = res.data;
      block.length = res.length;
      block.breakBeats = res.breakBeats;
      block.generated = false;
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
    } else {
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
    }
  }));
  const ubText = document.createElement('span');
  ubText.className = 'ne-ctl-label';
  ubText.style.textTransform = 'none';
  ubText.textContent = 'Edit whole block';
  ubLabel.appendChild(ubCheck); ubLabel.appendChild(ubText);
  ubGroup.appendChild(ubLabel);
  panel.appendChild(ubGroup);

  // Lock display for Break controls (uses specific regenerate functions)
  const lockBreakControlRow = (row, dials, groups, regenerateFn) => {
    const iconHost = row.titleLabel || (dials[0] && dials[0].querySelector('.ctrl-label'));
    const lockedNow = groups.filter(gid => block.lockedGroups && block.lockedGroups[gid]);
    if (iconHost) {
      lockedNow.forEach(gid => {
        const icon = document.createElement('span');
        icon.className = 'group-lock-icon';
        icon.textContent = '🔒';
        icon.dataset.tooltip = 'Locked\nThis group was manually edited in the note editor. Click to discard those edits and let the knobs control it again.';
        icon.addEventListener('click', (e) => {
          e.stopPropagation();
          neUndoable('Discard edits', async () => {
            block.lockedGroups[gid] = false;
            await regenerateFn();
            updateGroupLockUI();
            renderNoteEditor();
          });
        });
        iconHost.appendChild(icon);
      });
    }
    const fullyLocked = lockedNow.length > 0;
    dials.forEach(d => { if (d.slider) d.slider.disabled = fullyLocked; });
    row.classList.toggle('ne-ctl-locked', fullyLocked);
  };

  panel.appendChild(neSidebarHeading(neFillKind(block)));
  // Break Length (Beats)
  const bbGroup = document.createElement('div');
  bbGroup.className = 'ne-ctl-group ne-ctl-inline';
  const bbMaxBeats = Math.max(0.5, blockLen*g.numerator - 0.5);
  bbGroup.innerHTML = '<span class="ne-ctl-label">Length (Beats)</span>';
  const bbInput = document.createElement('input');
  bbInput.type = 'number'; bbInput.className = 'ne-len-input';
  bbInput.min = '0.5'; bbInput.max = String(bbMaxBeats); bbInput.step = '0.25';
  bbInput.value = String(block.breakBeats ?? BREAK_DEFAULT_BEATS);
  bbInput.dataset.tooltip = `Break Length\nThe break's own length in beats, not whole bars (max ${bbMaxBeats}). The rest of the block stays a copy of the parent section.`;
  const commitBB = () => {
    let v = parseFloat(bbInput.value);
    if (isNaN(v)) { bbInput.value = String(block.breakBeats ?? BREAK_DEFAULT_BEATS); return; }
    const maxBeats = Math.max(0.5, blockLen*g.numerator - 0.5);
    v = Math.max(0.5, Math.min(maxBeats, Math.round(v*4)/4));
    if (v === (block.breakBeats ?? BREAK_DEFAULT_BEATS)) { bbInput.value = String(v); return; }
    neUndoable('Break length', () => {
      block.breakBeats = v;
      if (neLoopFollowsBreak) {
        const newGrooveBeats = Math.max(0, blockBeats - v);
        neLoopStart = newGrooveBeats;
        neLoopEnd = blockBeats;
        nePlayheadBeat = neLoopStart;
      }
      return regenerateLength();
    });
  };
  bbInput.addEventListener('change', commitBB);
  bbInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); commitBB(); } });
  bbGroup.appendChild(bbInput);
  panel.appendChild(bbGroup);

  // Fill Subdivision/Amount
  const bsIdx = Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(block.breakSubdivision ?? NE_DEFAULTS.subdivision));
  const bsDial = neCreateDial({
    label: 'Subdiv', min: 0, max: SUBDIVISION_KNOB_VALUES.length - 1, step: 1, value: bsIdx,
    def: Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(NE_DEFAULTS.subdivision)), // Default: subdivision
    format: v => SUBDIVISION_KNOB_LABELS[v],
    title: 'Subdivision\nResolution of the ' + neFillKind(block).toLowerCase() + ' in steps per beat.',
    onChange: v => neUndoable(neFillKind(block) + ' subdivision', () => { neEnsureSeed(block, 'breakSeed'); block.breakSubdivision = SUBDIVISION_KNOB_VALUES[v]; return regenerateMain(); }),
  });
  const bdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.breakDensity ?? NE_DEFAULTS.breakDensity,
    def: NE_DEFAULTS.breakDensity,
    format: v => v + '%',
    title: 'Amount\nHow many hits land within the ' + neFillKind(block).toLowerCase() + ' window — independent of its length.',
    onChange: v => neUndoable(neFillKind(block) + ' amount', () => { neEnsureSeed(block, 'breakSeed'); block.breakDensity = v; return regenerateMain(); }),
  });
  const breakFillDialRow = neToolbarCluster(neFillKind(block), [bsDial, bdDial]);
  lockBreakControlRow(breakFillDialRow, [bsDial, bdDial], mainGroups(), regenerateMain);
  breakFillDialRow.classList.add('ne-ctl-nosep');
  neAddRerollIcon(breakFillDialRow, [bsDial, bdDial], 'Reroll\nGenerates a new ' + neFillKind(block).toLowerCase() + ' with a fresh random seed — Length, Amount and instruments stay unchanged.', () => {
    if (mainIsLocked()) { neMsg('Manually edited — click the 🔒 above to discard first.'); return; }
    neUndoable('Reroll ' + neFillKind(block).toLowerCase(), () => { block.breakSeed = Math.floor(Math.random()*1e9); return regenerateMain(); });
  });

  // Instrument picker
  const biGroup = document.createElement('div');
  biGroup.className = 'ne-ctl-group';
  const biBox = document.createElement('div');
  biBox.className = 'ne-ctl-row-items';
  biBox.style.cssText = 'display:flex;flex-direction:column;align-items:flex-start;gap:4px;';
  neInstrumentCheckboxRow(biBox, [['toms','Toms'],['snare','Snare'],['hihat','Hi-Hat'],['floorTom','Floor Tom'],['crash','2x Crash']],
    () => block.breakInstruments || ['toms'],
    (arr) => { block.breakInstruments = arr; },
    () => { neEnsureSeed(block, 'breakSeed'); return regenerateMainAlways(); }, 'break-fill', mainIsLocked(),
    neFillKind(block) + ' Instruments\nWhich instruments the ' + neFillKind(block).toLowerCase() + ' can use. Drag across checkboxes to set several at once.');
  neAppendFillInstruments(biGroup, biBox, neFillKind(block) + ' instruments:');
  panel.appendChild(biGroup);
  biGroup.classList.toggle('ne-ctl-locked', mainIsLocked());

  // Kick layer
  const bksIdx = Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(block.breakKickSubdivision ?? NE_DEFAULTS.kickSubdivision));
  const bksDial = neCreateDial({
    label: 'Subdiv', min: 0, max: BREAK_KICK_SUBDIVISION_VALUES.length - 1, step: 1, value: bksIdx,
    def: Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(NE_DEFAULTS.kickSubdivision)), // Default: kick subdivision
    format: v => BREAK_KICK_SUBDIVISION_LABELS[v],
    title: 'Subdivision\nResolution of the ' + neFillKind(block) + ' Kick pulse in steps per beat.',
    onChange: v => neUndoable(neFillKind(block) + ' kick subdivision', () => { block.breakKickSubdivision = BREAK_KICK_SUBDIVISION_VALUES[v]; return regenerateKickOnly(); }),
  });
  const bkdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.breakKickDensity ?? NE_DEFAULTS.breakKickDensity,
    def: NE_DEFAULTS.breakKickDensity,
    format: v => v + '%',
    title: 'Amount\nHow full the ' + neFillKind(block) + ' Kick pulse is — 100% = a hit on every step.',
    onChange: v => neUndoable(neFillKind(block) + ' kick amount', () => { block.breakKickDensity = v; return regenerateKickOnly(); }),
  });
  const breakKickDialRow = neToolbarCluster(neFillKind(block) + ' Kick', [bksDial, bkdDial]);
  lockBreakControlRow(breakKickDialRow, [bksDial, bkdDial], ['kick'], regenerateKickOnly);
  neAddRerollIcon(breakKickDialRow, [bksDial, bkdDial], 'Reroll\nGenerates a new ' + neFillKind(block) + ' Kick pulse with a fresh random seed. Only audible below 100% Amount.', () => {
    if (kickIsLocked()) { neMsg('Manually edited — click the 🔒 above to discard first.'); return; }
    neUndoable('Reroll ' + neFillKind(block).toLowerCase() + ' kick', () => { block.breakKickSeed = Math.floor(Math.random()*1e9); return regenerateKickOnly(); });
  });
}

// Fill panel for standalone Intro/Outro blocks.
function neBuildFillPanel(panel, ctx) {
  const { block, g, blockLen } = ctx;

  const regenerate = async () => {
    const res = await requestRegenerateFillBlock(block, g);
    if (!res) return;
    block.data = res.data;
    block.length = res.length;
    block.fillBeats = res.fillBeats;
    block.feel = res.feel;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
  };

  // Fill Length (Beats)
  const fbGroup = document.createElement('div');
  fbGroup.className = 'ne-ctl-group ne-ctl-inline';
  const fbMaxBeats = Math.max(0.5, blockLen*g.numerator);
  fbGroup.innerHTML = '<span class="ne-ctl-label">Length (Beats)</span>';
  const fbInput = document.createElement('input');
  fbInput.type = 'number'; fbInput.className = 'ne-len-input';
  fbInput.min = '0.5'; fbInput.max = String(fbMaxBeats); fbInput.step = '0.25';
  fbInput.value = String(block.fillBeats ?? BREAK_DEFAULT_BEATS);
  fbInput.dataset.tooltip = `${neFillKind(block)} Length\nThe ${neFillKind(block).toLowerCase()}'s length in beats (max ${fbMaxBeats} = the whole block).`;
  const commitFB = () => {
    let v = parseFloat(fbInput.value);
    if (isNaN(v)) { fbInput.value = String(block.fillBeats ?? BREAK_DEFAULT_BEATS); return; }
    const maxBeats = Math.max(0.5, blockLen*g.numerator);
    v = Math.max(0.5, Math.min(maxBeats, Math.round(v*4)/4));
    if (v === (block.fillBeats ?? BREAK_DEFAULT_BEATS)) { fbInput.value = String(v); return; }
    neUndoable(neFillKind(block) + ' length', () => { block.fillBeats = v; return regenerate(); });
  };
  fbInput.addEventListener('change', commitFB);
  fbInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); commitFB(); } });
  fbGroup.appendChild(fbInput);
  panel.appendChild(fbGroup);

  // Fill Subdivision/Amount
  const fsIdx = Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(block.fillSubdivision ?? NE_DEFAULTS.subdivision));
  const fsDial = neCreateDial({
    label: 'Subdiv', min: 0, max: SUBDIVISION_KNOB_VALUES.length - 1, step: 1, value: fsIdx,
    def: Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(NE_DEFAULTS.subdivision)), // Default: subdivision
    format: v => SUBDIVISION_KNOB_LABELS[v],
    title: 'Subdivision\nResolution of the ' + neFillKind(block).toLowerCase() + ' in steps per beat.',
    onChange: v => neUndoable(neFillKind(block) + ' subdivision', () => { neEnsureSeed(block, 'fillSeed'); block.fillSubdivision = SUBDIVISION_KNOB_VALUES[v]; return regenerate(); }),
  });
  const fdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.fillDensity ?? NE_DEFAULTS.fillDensity,
    def: NE_DEFAULTS.fillDensity,
    format: v => v + '%',
    title: 'Amount\nHow many hits land within the ' + neFillKind(block).toLowerCase() + ' window — independent of its length.',
    onChange: v => neUndoable(neFillKind(block) + ' amount', () => { neEnsureSeed(block, 'fillSeed'); block.fillDensity = v; return regenerate(); }),
  });
  const fillMainDialRow = neToolbarCluster(neFillKind(block), [fsDial, fdDial]);
  fillMainDialRow.classList.add('ne-ctl-nosep');
  neAddRerollIcon(fillMainDialRow, [fsDial, fdDial], 'Reroll\nGenerates a new ' + neFillKind(block).toLowerCase() + ' with a fresh random seed — Length, Amount and instruments stay unchanged.', () => {
    neUndoable('Reroll ' + neFillKind(block).toLowerCase(), () => { block.fillSeed = Math.floor(Math.random()*1e9); return regenerate(); });
  });

  // Instrument picker
  const fiGroup = document.createElement('div');
  fiGroup.className = 'ne-ctl-group';
  const fiBox = document.createElement('div');
  fiBox.className = 'ne-ctl-row-items';
  fiBox.style.cssText = 'display:flex;flex-direction:column;align-items:flex-start;gap:4px;';
  neInstrumentCheckboxRow(fiBox, [['toms','Toms'],['snare','Snare'],['hihat','Hi-Hat'],['floorTom','Floor Tom'],['crash','2x Crash']],
    () => block.fillInstruments || ['toms'],
    (arr) => { block.fillInstruments = arr; },
    () => { neEnsureSeed(block, 'fillSeed'); return regenerate(); }, 'fill-fill', undefined,
    neFillKind(block) + ' Instruments\nWhich instruments the ' + neFillKind(block).toLowerCase() + ' can use. Drag across checkboxes to set several at once.');
  neAppendFillInstruments(fiGroup, fiBox, neFillKind(block) + ' instruments:');
  panel.appendChild(fiGroup);

  // Kick layer
  const fksIdx = Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(block.fillKickSubdivision ?? NE_DEFAULTS.kickSubdivision));
  const fksDial = neCreateDial({
    label: 'Subdiv', min: 0, max: BREAK_KICK_SUBDIVISION_VALUES.length - 1, step: 1, value: fksIdx,
    def: Math.max(0, BREAK_KICK_SUBDIVISION_VALUES.indexOf(NE_DEFAULTS.kickSubdivision)), // Default: kick subdivision
    format: v => BREAK_KICK_SUBDIVISION_LABELS[v],
    title: 'Subdivision\nResolution of the ' + neFillKind(block) + ' Kick pulse in steps per beat.',
    onChange: v => neUndoable(neFillKind(block) + ' kick subdivision', () => { block.fillKickSubdivision = BREAK_KICK_SUBDIVISION_VALUES[v]; return regenerate(); }),
  });
  const fkdDial = neCreateDial({
    label: 'Amnt', min: 0, max: 100, step: 5, value: block.fillKickDensity ?? NE_DEFAULTS.fillKickDensity,
    def: NE_DEFAULTS.fillKickDensity,
    format: v => v + '%',
    title: 'Amount\nHow full the ' + neFillKind(block) + ' Kick pulse is — 100% = a hit on every step.',
    onChange: v => neUndoable(neFillKind(block) + ' kick amount', () => { block.fillKickDensity = v; return regenerate(); }),
  });
  const fillKickDialRow = neToolbarCluster(neFillKind(block) + ' Kick', [fksDial, fkdDial]);
  neAddRerollIcon(fillKickDialRow, [fksDial, fkdDial], 'Reroll\nGenerates a new ' + neFillKind(block) + ' Kick pulse with a fresh random seed. Only audible below 100% Amount.', () => {
    neUndoable('Reroll ' + neFillKind(block).toLowerCase() + ' kick', () => { block.fillKickSeed = Math.floor(Math.random()*1e9); return regenerate(); });
  });
}