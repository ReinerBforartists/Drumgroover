// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// checkbox (optional): { label, id } — renders an extra checkbox row (e.g.
// "Don't ask again"). ONLY changes the resolved shape when passed, so every
// existing call site (which doesn't pass it) keeps getting back a plain
// boolean/values array exactly as before.
function showModal({ title, message, fields, confirmLabel='OK', cancelLabel='Cancel', danger=false, checkbox=null }) {
  return new Promise((resolve) => {
    const overlay = document.createElement('div');
    overlay.className = 'app-modal-overlay';
    const box = document.createElement('div');
    box.className = 'app-modal-box';
    let html = `<div class="app-modal-title">${title}</div>`;
    if (message) html += `<div class="app-modal-message">${message}</div>`;
    (fields||[]).forEach((f,i) => {
      if (f.options) {
        html += `<label class="app-modal-label">${f.label}</label><select class="app-modal-input" data-i="${i}">` +
          f.options.map(o => `<option value="${o.value}"${o.value === f.value ? ' selected' : ''}>${o.label}</option>`).join('') + `</select>`;
        return;
      }
      html += `<label class="app-modal-label">${f.label}</label><input type="text" class="app-modal-input" data-i="${i}" value="${(f.value||'').replace(/"/g,'&quot;')}" placeholder="${f.placeholder||''}">`;
    });
    if (checkbox) {
      html += `<label class="app-modal-label app-modal-checkbox-label"><input type="checkbox" class="app-modal-checkbox"> ${checkbox.label}</label>`;
    }
    html += `<div class="app-modal-actions"><button class="btn btn-sm app-modal-cancel">${cancelLabel}</button><button class="btn btn-sm ${danger?'btn-danger':'btn-refresh'} app-modal-confirm">${confirmLabel}</button></div>`;
    box.innerHTML = html;
    overlay.appendChild(box);
    document.body.appendChild(overlay);

    const inputs = [...box.querySelectorAll('.app-modal-input')];
    const confirmBtn = box.querySelector('.app-modal-confirm');
    const cancelBtn = box.querySelector('.app-modal-cancel');
    if (inputs[0]) {
      inputs[0].focus(); if (inputs[0].select) inputs[0].select();   // a <select> field has no select()
    } else {
      cancelBtn.focus();
    }
    const checkboxEl = checkbox ? box.querySelector('.app-modal-checkbox') : null;

    const cleanup = () => overlay.remove();
    const doConfirm = () => {
      const values = inputs.map(inp => inp.value);
      const dontShowAgain = checkboxEl ? checkboxEl.checked : false;
      cleanup();
      resolve(checkbox ? { confirmed: true, dontShowAgain } : (fields ? values : true));
    };
    const doCancel = () => {
      cleanup();
      resolve(checkbox ? { confirmed: false, dontShowAgain: false } : (fields ? null : false));
    };

    box.querySelector('.app-modal-confirm').addEventListener('click', doConfirm);
    box.querySelector('.app-modal-cancel').addEventListener('click', doCancel);
    overlay.addEventListener('click', (e) => { if (e.target === overlay) doCancel(); });
    box.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doConfirm(); }
      else if (e.key === 'Escape') { doCancel(); }
      else if (!inputs.length && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
        e.preventDefault();
        (document.activeElement === cancelBtn ? confirmBtn : cancelBtn).focus();
      }
    });
  });
}

function isTimelineTabActive() {
  return document.querySelector('.tab-panel[data-tab-panel="timeline"]')?.classList.contains('active');
}

function paletteDisplayOrder() {
  const seen = new Set();
  const ordered = [];
  blocksList.forEach(b => {
    if (b.breakParentId || seen.has(b.id)) return;
    ordered.push(b); seen.add(b.id);
    blocksList.filter(x => x.breakParentId === b.id).forEach(child => {
      if (!seen.has(child.id)) { ordered.push(child); seen.add(child.id); }
    });
  });
  blocksList.forEach(b => { if (!seen.has(b.id)) { ordered.push(b); seen.add(b.id); } });
  return ordered;
}

function renderBlockTabs() {
  const savedScrollY = window.scrollY;
  const wrap = document.getElementById('blockPalette');
  wrap.innerHTML = '';
  paletteDisplayOrder().forEach(b => {
    const item = document.createElement('div');
    item.className = 'palette-item' + (b.id===activeBlockId ? ' active' : '');
    item.style.background = blockColor(b.id);
    item.dataset.blockId = b.id;

    const nameStrip = document.createElement('div');
    nameStrip.className = 'palette-item-name';
    nameStrip.textContent = blockDisplayName(b);
    item.appendChild(nameStrip);
    const body = document.createElement('div');
    body.className = 'palette-item-body';
    item.appendChild(body);

    if (!isFillGeneratedBlock(b)) {
      const trigger = document.createElement('button');
      trigger.type = 'button';
      trigger.className = 'palette-item-trigger';
      trigger.textContent = '+';
      trigger.dataset.tooltip = 'Add Break\nCreates a break block for this song block.';
      trigger.addEventListener('pointerdown', (e) => e.stopPropagation());
      trigger.addEventListener('click', (e) => {
        e.stopPropagation();
        createBreakBlockFor(b);
      });
      item.appendChild(trigger);
    }

    item.addEventListener('dblclick', async () => {
      if (isBreakBlock(b)) { neMsg('Name comes from the parent block — rename it there.'); return; }
      const result = await showModal({ title:'Rename block', fields:[{label:'New name', value:b.name}], confirmLabel:'Rename' });
      const nn = result && result[0];
      if (nn && nn !== b.name) {
        withProjectUndoAsync(`Rename block (${b.name})`, async () => { b.name = nn; b.generated = false; renderBlockTabs(); renderTimeline(); });
      }
    });
    item.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      if (suppressNextContextMenu) { suppressNextContextMenu = false; return; }
      deleteBlockWithBreaks(b);
    });
    attachPaletteDrag(item, b);
    wrap.appendChild(item);
  });
  attachPalettePaintDeleteOnce();
  requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  updatePaletteScrollbar();
}

function createBreakBlockFor(parent) {
  if (isFillGeneratedBlock(parent)) { neMsg('Break/Intro/Outro blocks can\'t have their own Break — they\'re already a standalone fill.'); return; }
  const g = readGrid();
  const id = 'break_' + parent.id + '_' + Date.now();
  const totalLength = Math.max(1, parent.length || 1);
  const b = generateBreakBlock(parent.id, g, id, totalLength, BREAK_DEFAULT_BEATS);
  if (!b) { neMsg('Could not create Break — parent block not found.'); return; }
  blocksList.push(b);
  activeBlockId = id;
  renderBlockTabs(); renderNoteEditor();
}

function deleteBlockWithBreaks(block) {
  const cascadeIds = new Set([block.id, ...blocksList.filter(x => x.breakParentId === block.id).map(x => x.id)]);
  const removedBlocks = blocksList.filter(x => cascadeIds.has(x.id));
  const removedSlots = songEngine.songSequence.filter(slot => cascadeIds.has(slot.blockId));
  const wasActive = cascadeIds.has(activeBlockId);

  const doRemove = () => {
    const savedScrollY = window.scrollY;
    blocksList = blocksList.filter(x => !cascadeIds.has(x.id));
    songEngine.songSequence = songEngine.songSequence.filter(slot => !cascadeIds.has(slot.blockId));
    if (wasActive) activeBlockId = blocksList[0]?.id || null;
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };
  const doRestore = () => {
    const savedScrollY = window.scrollY;
    removedBlocks.forEach(x => blocksList.push(x));
    removedSlots.forEach(slot => songEngine.songSequence.push(slot));
    songEngine.songSequence.sort((a,b) => a.startBar - b.startBar);
    if (wasActive) activeBlockId = block.id;
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };

  doRemove();

  const extraBreaks = removedBlocks.length - 1;
  const label = extraBreaks > 0
    ? `Deleted "${blockDisplayName(block)}" + ${extraBreaks} Break${extraBreaks>1?'s':''}`
    : `Deleted "${blockDisplayName(block)}"`;

  pushUndo(label, doRestore, doRemove);
}

async function clearAllBlocks() {
  if (!blocksList.length) { neMsg('There are no blocks to clear'); return; }
  if (shouldShowConfirmDialogs()) {
    const res = await showModal({
      title: 'Clear all blocks?',
      message: `This deletes all ${blocksList.length} block${blocksList.length>1?'s':''} and removes them from the timeline. Undo (Ctrl+Z) brings it all back if needed.`,
      confirmLabel: 'Clear all',
      danger: true,
      checkbox: { label: "Don't ask again (re-enable in Note Editor Options → Show confirmation dialogs)" }
    });
    if (!res.confirmed) return;
    if (res.dontShowAgain) setShowConfirmDialogs(false);
  }
  const savedScrollY = window.scrollY;
  const beforeBlocks = JSON.stringify(blocksList);
  const beforeSeq = JSON.stringify(songEngine.songSequence);
  const beforeActive = activeBlockId;
  const doClear = () => {
    blocksList = [];
    songEngine.songSequence = [];
    activeBlockId = null;
    tlSetSelection([]);
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };
  const doRestore = () => {
    blocksList = JSON.parse(beforeBlocks);
    songEngine.songSequence = JSON.parse(beforeSeq);
    activeBlockId = beforeActive;
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };
  doClear();
  pushUndo('Clear all blocks', doRestore, doClear);
}

function deleteBlocksWithBreaks(blocks) {
  const rootIds = new Set(blocks.map(b => b.id));
  const cascadeIds = new Set([...rootIds, ...blocksList.filter(x => rootIds.has(x.breakParentId)).map(x => x.id)]);
  if (!cascadeIds.size) return;
  const removedBlocks = blocksList.filter(x => cascadeIds.has(x.id));
  const removedSlots = songEngine.songSequence.filter(slot => cascadeIds.has(slot.blockId));
  const wasActive = cascadeIds.has(activeBlockId);

  const doRemove = () => {
    const savedScrollY = window.scrollY;
    blocksList = blocksList.filter(x => !cascadeIds.has(x.id));
    songEngine.songSequence = songEngine.songSequence.filter(slot => !cascadeIds.has(slot.blockId));
    if (wasActive) activeBlockId = blocksList[0]?.id || null;
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };
  const doRestore = () => {
    const savedScrollY = window.scrollY;
    removedBlocks.forEach(x => blocksList.push(x));
    removedSlots.forEach(slot => songEngine.songSequence.push(slot));
    songEngine.songSequence.sort((a,b) => a.startBar - b.startBar);
    if (wasActive) activeBlockId = removedBlocks[0]?.id || null;
    renderBlockTabs(); renderNoteEditor(); renderTimeline();
    rebuildFullFromEditedBlocks();
    requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  };

  doRemove();
  const label = removedBlocks.length > 1 ? `Deleted ${removedBlocks.length} blocks` : `Deleted "${blockDisplayName(removedBlocks[0])}"`;
  pushUndo(label, doRestore, doRemove);
}

let palettePaintDeleteAttached = false;
function attachPalettePaintDeleteOnce() {
  if (palettePaintDeleteAttached) return;
  palettePaintDeleteAttached = true;
  const wrap = document.getElementById('blockPalette');
  let painting = null;

  const itemAtPoint = (clientX, clientY) => {
    const el = document.elementFromPoint(clientX, clientY);
    return el && el.closest('.palette-item');
  };

  wrap.addEventListener('pointerdown', (e) => {
    if (e.button !== 2) return;
    e.preventDefault();
    painting = { touchedIds: new Set(), moved: false };
    const item = e.target.closest('.palette-item');
    if (item) painting.touchedIds.add(item.dataset.blockId);
    wrap.setPointerCapture(e.pointerId);
  });

  wrap.addEventListener('pointermove', (e) => {
    if (!painting) return;
    const item = itemAtPoint(e.clientX, e.clientY);
    if (!item || !item.dataset.blockId) return;
    const id = item.dataset.blockId;
    if (!painting.touchedIds.has(id)) {
      painting.moved = true;
      painting.touchedIds.add(id);
      item.classList.add('paint-marked');
    }
  });

  const finishPalettePaint = () => {
    if (!painting) return;
    const { touchedIds, moved } = painting;
    painting = null;
    if (moved && touchedIds.size) {
      armSuppressNextContextMenu();
      const targets = [...touchedIds].map(id => findBlock(id)).filter(Boolean);
      deleteBlocksWithBreaks(targets);
    }
  };
  wrap.addEventListener('pointerup', finishPalettePaint);
  wrap.addEventListener('pointercancel', () => { painting = null; });
}

const UNDO_HISTORY_MAX = 40;
// One undo history per tab (Note Editor, Song Generator, Mixer, Drum Engine): Ctrl+Z only touches the tab you are looking at.
const undoStacks = {};

function undoTabKey() {
  const btn = document.querySelector('#mainTabBar .tab-btn.active');
  return (btn && btn.dataset.tab) || 'noteEditor';
}

function undoStack(tab) {
  const key = tab || undoTabKey();
  if (!undoStacks[key]) undoStacks[key] = { undo: [], redo: [] };
  return undoStacks[key];
}

// Without a tab, every history is cleared (project load / new project).
function clearUndoHistory(tab) {
  if (tab) delete undoStacks[tab];
  else Object.keys(undoStacks).forEach(k => delete undoStacks[k]);
  refreshUndoRedoButtons();
}

// `tab` is optional: the entry goes to the history of the tab that is active when it is pushed.
// Many steps keep a JSON snapshot of the whole block list, so the number of steps shrinks with the total length of the blocks
// (a 4000-bar block is several megabytes per snapshot).
function undoHistoryLimit() {
  let bars = 0;
  for (const b of blocksList) bars += (b.length || 1);
  return bars > 2000 ? 6 : bars > 800 ? 12 : bars > 300 ? 24 : UNDO_HISTORY_MAX;
}

function pushUndo(label, undoFn, redoFn, tab) {
  const st = undoStack(tab);
  st.undo.push({ label, undo: undoFn, redo: redoFn });
  const limit = undoHistoryLimit();
  while (st.undo.length > limit) st.undo.shift();
  st.redo.length = 0;
  refreshUndoRedoButtons();
}

function performUndo() {
  const st = undoStack();
  const entry = st.undo.pop();
  if (!entry) return;
  entry.undo();
  st.redo.push(entry);
  if (st.redo.length > UNDO_HISTORY_MAX) st.redo.shift();
  refreshUndoRedoButtons();
}

function performRedo() {
  const st = undoStack();
  const entry = st.redo.pop();
  if (!entry) return;
  entry.redo();
  st.undo.push(entry);
  if (st.undo.length > UNDO_HISTORY_MAX) st.undo.shift();
  refreshUndoRedoButtons();
}

function refreshUndoRedoButtons() {
  const undoBtn = document.getElementById('undoBtn');
  const redoBtn = document.getElementById('redoBtn');
  const st = undoStack();
  const topUndo = st.undo[st.undo.length - 1];
  const topRedo = st.redo[st.redo.length - 1];
  if (undoBtn) {
    undoBtn.disabled = !topUndo;
    undoBtn.dataset.tooltip = topUndo ? `Undo\n${topUndo.label}` : 'Undo\nNothing to undo';
    undoBtn.onclick = topUndo ? performUndo : null;
  }
  if (redoBtn) {
    redoBtn.disabled = !topRedo;
    redoBtn.dataset.tooltip = topRedo ? `Redo\n${topRedo.label}` : 'Redo\nNothing to redo';
    redoBtn.onclick = topRedo ? performRedo : null;
  }
}

function snapshotLocked(block) {
  return JSON.stringify(block.lockedGroups || {});
}
function restoreLocked(block, snapshot) {
  block.lockedGroups = JSON.parse(snapshot);
  updateGroupLockUI();
}

function withBlockDataUndo(label, block, mutateFn) {
  const beforeData = JSON.stringify(block.data);
  const beforeLocked = JSON.stringify(block.lockedGroups || {});
  const beforeKnobs = JSON.stringify(block.knobs || {});
  const beforeFeel = block.feel || 'straight';
  const beforeModes = JSON.stringify(block.patternModes || {});
  const beforeShape = { length: block.length, breakBeats: block.breakBeats, fillBeats: block.fillBeats, breakUnlocked: !!block.breakUnlocked };
  mutateFn();
  const afterData = JSON.stringify(block.data);
  const afterLocked = JSON.stringify(block.lockedGroups || {});
  const afterKnobs = JSON.stringify(block.knobs || {});
  const afterFeel = block.feel || 'straight';
  const afterModes = JSON.stringify(block.patternModes || {});
  const afterShape = { length: block.length, breakBeats: block.breakBeats, fillBeats: block.fillBeats, breakUnlocked: !!block.breakUnlocked };
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
  updateGroupLockUI();
  const restore = (dataStr, lockedStr, knobsStr, feelVal, shape, modesStr) => {
    block.data = JSON.parse(dataStr);
    block.lockedGroups = JSON.parse(lockedStr);
    block.knobs = JSON.parse(knobsStr);
    block.feel = feelVal;
    block.patternModes = JSON.parse(modesStr);
    block.length = shape.length; block.breakBeats = shape.breakBeats; block.fillBeats = shape.fillBeats; block.breakUnlocked = shape.breakUnlocked;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    updateGroupLockUI();
    if (activeBlockId === block.id) syncKnobsToBlock(block);
  };
  pushUndo(label, () => restore(beforeData, beforeLocked, beforeKnobs, beforeFeel, beforeShape, beforeModes), () => restore(afterData, afterLocked, afterKnobs, afterFeel, afterShape, afterModes));
}

// Undo step over the whole block list, for changes that also touch other blocks (e.g. a length change resizing its Breaks).
// `snapshotBefore` is for gestures that change the blocks before the undoable work starts (e.g. a checkbox drag).
async function withProjectUndoAsync(label, asyncFn, snapshotBefore) {
  const before = snapshotBefore !== undefined ? snapshotBefore : JSON.stringify(blocksList);
  await asyncFn();
  const after = JSON.stringify(blocksList);
  if (before === after) return;
  const restore = (s) => {
    blocksList = JSON.parse(s);
    rebuildFullFromEditedBlocks();
    renderBlockTabs();
    renderNoteEditor();
    renderTimeline();
    updateGroupLockUI();
    syncKnobsToBlock(findBlock(activeBlockId));
  };
  pushUndo(label, () => restore(before), () => restore(after));
}

async function withBlockDataUndoAsync(label, block, asyncMutateFn) {
  const beforeData = JSON.stringify(block.data);
  const beforeLocked = JSON.stringify(block.lockedGroups || {});
  const beforeKnobs = JSON.stringify(block.knobs || {});
  const beforeFeel = block.feel || 'straight';
  const beforeModes = JSON.stringify(block.patternModes || {});
  const beforeShape = { length: block.length, breakBeats: block.breakBeats, fillBeats: block.fillBeats, breakUnlocked: !!block.breakUnlocked };
  await asyncMutateFn();
  const afterData = JSON.stringify(block.data);
  const afterLocked = JSON.stringify(block.lockedGroups || {});
  const afterKnobs = JSON.stringify(block.knobs || {});
  const afterFeel = block.feel || 'straight';
  const afterModes = JSON.stringify(block.patternModes || {});
  const afterShape = { length: block.length, breakBeats: block.breakBeats, fillBeats: block.fillBeats, breakUnlocked: !!block.breakUnlocked };
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
  updateGroupLockUI();
  const restore = (dataStr, lockedStr, knobsStr, feelVal, shape, modesStr) => {
    block.data = JSON.parse(dataStr);
    block.lockedGroups = JSON.parse(lockedStr);
    block.knobs = JSON.parse(knobsStr);
    block.feel = feelVal;
    block.patternModes = JSON.parse(modesStr);
    block.length = shape.length; block.breakBeats = shape.breakBeats; block.fillBeats = shape.fillBeats; block.breakUnlocked = shape.breakUnlocked;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    updateGroupLockUI();
    if (activeBlockId === block.id) syncKnobsToBlock(block);
  };
  pushUndo(label, () => restore(beforeData, beforeLocked, beforeKnobs, beforeFeel, beforeShape, beforeModes), () => restore(afterData, afterLocked, afterKnobs, afterFeel, afterShape, afterModes));
}

function withSongSequenceUndo(label, mutateFn) {
  const before = JSON.stringify(songEngine.songSequence);
  mutateFn();
  const after = JSON.stringify(songEngine.songSequence);
  renderTimeline();
  rebuildFullFromEditedBlocks();
  renderStats(readParams(), readGrid());
  const restore = (str) => {
    songEngine.songSequence = JSON.parse(str);
    renderTimeline();
    rebuildFullFromEditedBlocks();
    renderStats(readParams(), readGrid());
  };
  pushUndo(label, () => restore(before), () => restore(after));
}

// Dragging a palette chip inserts a copy into the timeline; leaving the plugin
// window turns it into a MIDI drag-out to the host instead.
function attachPaletteDrag(item, b) {
  let drag = null;
  item.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    drag = { sx:e.clientX, sy:e.clientY, moved:false, ghost:null };
    item.setPointerCapture(e.pointerId);
  });
  item.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const dx = e.clientX-drag.sx, dy = e.clientY-drag.sy;
    if (!drag.moved && (Math.abs(dx)>5 || Math.abs(dy)>5)) {
      drag.moved = true;
      const g = document.createElement('div');
      g.className = 'tl-drag-ghost';
      g.textContent = blockDisplayName(b);
      g.style.background = b.id ? blockColor(b.id) : 'transparent';
      if (!b.id) { g.style.border = '2px dashed var(--border2)'; g.style.color = 'var(--muted)'; }
      document.body.appendChild(g);
      drag.ghost = g;
    }
    if (drag.ghost && pointerLeftPluginWindow(e)) {
      drag.ghost.remove();
      hideDropMarker();
      try { item.releasePointerCapture(e.pointerId); } catch(_) {}
      drag = null;
      if (hostBridge.isEmbedded && b.id) hostBridge.send('startNativeDragExportBlock', { block: b, params: readParams(), grid: readGrid() });
      return;
    }
    if (drag.ghost) {
      drag.ghost.style.left = (e.clientX+10)+'px';
      drag.ghost.style.top = (e.clientY+10)+'px';
      const track = document.getElementById('tlTrack');
      const tr = track.getBoundingClientRect();
      const over = e.clientX>=tr.left-20 && e.clientY>=tr.top-15 && e.clientY<=tr.bottom+15;
      const pxPerBar = currentTlPxPerBar();
      if (over) {
        const desiredBar = computeDropBar(track, e.clientX, pxPerBar);
        showDropMarker(songEngine.nearestBoundary(Math.max(0, desiredBar), null), pxPerBar);
      }
      else hideDropMarker();
    }
  });
  const finish = (e) => {
    if (!drag) return;
    if (drag.moved) {
      if (drag.ghost) drag.ghost.remove();
      const track = document.getElementById('tlTrack');
      const tr = track.getBoundingClientRect();
      const over = e.clientX>=tr.left-20 && e.clientY>=tr.top-15 && e.clientY<=tr.bottom+15;
      if (over) {
        const desiredBar = computeDropBar(track, e.clientX, currentTlPxPerBar());
        withSongSequenceUndo(`Add clip (${b.name})`, () => {
          songEngine.placeSlotAtRipple(desiredBar, { blockId: b.id, loopCount: 1 });
        });
      }
      hideDropMarker();
    } else if (b.id) {
      activeBlockId = b.id;
      document.querySelectorAll('#blockPalette .palette-item').forEach(el => el.classList.remove('active'));
      item.classList.add('active');
      syncKnobsToBlock(b);
      renderNoteEditor();
    }
    drag = null;
  };
  item.addEventListener('pointerup', finish);
  item.addEventListener('lostpointercapture', () => { if (drag?.ghost) drag.ghost.remove(); hideDropMarker(); drag=null; });
}

function showDropMarker(bar, pxPerBar) {
  const track = document.getElementById('tlTrack');
  let marker = track.querySelector('.tl-drop-marker');
  if (!marker) {
    marker = document.createElement('div');
    marker.className = 'tl-drop-marker';
    track.appendChild(marker);
  }
  marker.style.left = (bar*pxPerBar + tlLeadInPx)+'px';
  marker.style.display = 'block';
}
function hideDropMarker() {
  const track = document.getElementById('tlTrack');
  const marker = track && track.querySelector('.tl-drop-marker');
  if (marker) marker.style.display = 'none';
}
function computeDropBarFromLocalX(localX, pxPerBar) {
  const x = localX - tlLeadInPx;
  return Math.max(0, Math.round(x / pxPerBar));
}
function computeDropBar(track, clientX, pxPerBar) {
  const tr = track.getBoundingClientRect();
  return computeDropBarFromLocalX((clientX - tr.left) / currentUIScale(), pxPerBar);
}
function currentTlPxPerBar() { return tlCurrentPxPerBar; }

// =========================================
// RENDERING — Note Editor (continuous time, like SD3's Grid Editor)
// =========================================
const PX_PER_BEAT = 50, NE_ROW_H = 26, NE_MERGE_EPS = 0.06, NE_LEFT_PAD = 28;
const NE_ROWS_WRAP_H = 440;
let NE_VEL_H = 80;
const NE_ENDLESS_EXTRA_BARS = 8;
// At 100% zoom the block fills the width minus this many beats, so the dimmed area behind the block's end is visible.
const NE_END_GAP_BEATS = 1;
const GROUPS = { kick: 'Kick', snare: 'Snare', hihat: 'Hi-Hat', rackTom1: 'Rack Tom 1', rackTom2: 'Rack Tom 2', rackTom3: 'Rack Tom 3', floorTom1: 'Floor Tom 1', floorTom2: 'Floor Tom 2', cymbal1: 'Cymbal 1', cymbal2: 'Cymbal 2', cymbal3: 'Cymbal 3', ride: 'Ride', cymbal4: 'Cymbal 4', cymbal5: 'Cymbal 5' };
let expandedGroups = new Set();
let selectedNotes = new Set();
let neNudgeSteps = 0;
// Set while an undo/redo step puts the selection tools' baselines back, so the render does not take the restored notes for a new selection.
let neKeepToolBaselines = false;
let neNudgeSelectedRefs = new Set();
let showGmNotes = false;
let selectedGenre = 'straight';
let suppressNextRowClick = false;
let suppressNextContextMenu = false;
document.addEventListener('contextmenu', (e) => {
  if (suppressNextContextMenu) { e.preventDefault(); suppressNextContextMenu = false; }
});
function armSuppressNextContextMenu() {
  suppressNextContextMenu = true;
  setTimeout(() => { suppressNextContextMenu = false; }, 400);
}
let neCurrentPxPerBeat = PX_PER_BEAT;
let neZoom = 1;
const neZoomByBlock = new Map();   // block id -> zoom the block had when it was left
const neScrollByBlock = new Map(); // block id -> horizontal scroll (px) it had when it was left
const NE_ZOOM_MIN = 0.25, NE_ZOOM_MAX_BASE = 4, NE_MAX_PX_PER_BEAT = 160;
let neFitPxPerBeat = 0;   // the fit-to-width size, set by renderNoteEditor
// The zoom is relative to the fit-to-width size, so a long block (tiny fit size) needs a much larger maximum to get close to its notes.
function neZoomMax() { return Math.max(NE_ZOOM_MAX_BASE, neFitPxPerBeat > 0 ? NE_MAX_PX_PER_BEAT / neFitPxPerBeat : NE_ZOOM_MAX_BASE); }
// The zoom slider is logarithmic (0..1), because the range spans two orders of magnitude for long blocks.
function neZoomToSlider(z) { const mx = neZoomMax(); return Math.log(Math.max(NE_ZOOM_MIN, Math.min(mx, z)) / NE_ZOOM_MIN) / Math.log(mx / NE_ZOOM_MIN); }
function neSliderToZoom(s) { const mx = neZoomMax(); return NE_ZOOM_MIN * Math.pow(mx / NE_ZOOM_MIN, Math.max(0, Math.min(1, s))); }

// =========================================
// RENDERING — Stats
// =========================================
function renderStats(p, g) {
  const block = findBlock(activeBlockId);
  let hits = 0; if (block) trackList.forEach(t => hits += (block.data[t.id]||[]).length);
  const seqBlocks = songEngine.songSequence.length;
  const pills = document.getElementById('statPills');
  const pill = (...t) => `<span class="pill">${t.join('')}</span>`;
  const genreLabel = document.getElementById('genreSelect').selectedOptions[0]?.text || selectedGenre;
  const FEEL_LABELS = { straight:'Straight', swing:'Swing', halftime:'Half-Time' };
  const feelLabel = FEEL_LABELS[block?.feel || 'straight'];
  pills.innerHTML = [
    pill(genreLabel, ' · ', feelLabel), pill(g.numerator,'/',g.denominator,' · Snap 1/',g.subdivision),
    pill(seqBlocks,' Blocks in Song Generator'), pill(hits,' Notes (',block?blockDisplayName(block):'–',')'), pill('Seed ',p.seed),
  ].join('');
}

// =========================================
// AUDIO ENGINE
// =========================================

function uniqueName(base, existingNames) {
  if (!existingNames.includes(base)) return base;
  let n = 2;
  while (existingNames.includes(`${base} ${n}`)) n++;
  return `${base} ${n}`;
}

function baseNameOf(name) {
  return name.replace(/(?: Copy(?: \d+)?)+$/, '');
}

function findUnusedGmNote() {
  const used = new Set(trackList.map(t => t.gm));
  for (let n = 27; n <= 127; n++) if (!used.has(n)) return n;
  for (let n = 0; n < 27; n++) if (!used.has(n)) return n;
  return 127;
}

// =========================================
// TOOLBAR DROPDOWN MENUS
// =========================================
const toolbarMenus = [];

function closeAllToolbarMenus() {
  toolbarMenus.forEach(m => {
    if (m.closeTimer) { clearTimeout(m.closeTimer); m.closeTimer = null; }
    m.menu.style.display = 'none';
  });
}

function registerToolbarMenu(btnId, menuId, onBeforeOpen) {
  const btn = document.getElementById(btnId);
  const menu = document.getElementById(menuId);
  if (!btn || !menu) return null;
  const wrap = btn.closest('.palette-add-buttons') || btn.parentElement;
  const entry = { btn, menu, wrap, closeTimer: null };
  toolbarMenus.push(entry);

  const openThis = () => {
    closeAllToolbarMenus();
    if (onBeforeOpen) onBeforeOpen();
    menu.style.display = 'block';
  };

  btn.addEventListener('click', (e) => {
    e.stopPropagation();
    const wasOpen = menu.style.display !== 'none';
    if (wasOpen) closeAllToolbarMenus(); else openThis();
  });
  btn.addEventListener('mouseenter', () => {
    if (menu.style.display !== 'none') return;
    const anotherOpen = toolbarMenus.some(m => m !== entry && m.menu.style.display !== 'none');
    if (anotherOpen) openThis();
  });
  wrap.addEventListener('mouseleave', () => {
    entry.closeTimer = setTimeout(() => { menu.style.display = 'none'; entry.closeTimer = null; }, 200);
  });
  wrap.addEventListener('mouseenter', () => {
    if (entry.closeTimer) { clearTimeout(entry.closeTimer); entry.closeTimer = null; }
  });

  return entry;
}

document.addEventListener('click', (e) => {
  const insideAny = toolbarMenus.some(m => m.wrap.contains(e.target));
  if (!insideAny) closeAllToolbarMenus();
});

// =========================================
// CUSTOM SELECT POPUP
// =========================================
function currentUIScale() {
  return parseFloat(document.documentElement.style.getPropertyValue('--ui-scale')) || 1;
}

let openCustomSelectPopup = null;

function closeCustomSelectPopup() {
  if (!openCustomSelectPopup) return;
  openCustomSelectPopup.list.remove();
  openCustomSelectPopup.selectEl.classList.remove('custom-select-open');
  openCustomSelectPopup = null;
}

function openCustomSelectPopupFor(selectEl) {
  if (selectEl.disabled) return;
  if (openCustomSelectPopup && openCustomSelectPopup.selectEl === selectEl) { closeCustomSelectPopup(); return; }
  closeCustomSelectPopup();

  const scale = currentUIScale();
  const rect = selectEl.getBoundingClientRect();

  const list = document.createElement('div');
  list.className = 'custom-select-list';
  list.style.left = rect.left + 'px';
  list.style.top = rect.bottom + 'px';
  list.style.minWidth = (rect.width / scale) + 'px';
  list.style.transform = `scale(${scale})`;
  list.style.transformOrigin = 'top left';

  Array.from(selectEl.options).forEach(opt => {
    const item = document.createElement('div');
    item.className = 'custom-select-item' + (opt.disabled ? ' disabled' : '') + (opt.value === selectEl.value ? ' selected' : '');
    item.textContent = opt.text;
    if (!opt.disabled) {
      item.addEventListener('click', () => {
        if (selectEl.value !== opt.value) {
          selectEl.value = opt.value;
          selectEl.dispatchEvent(new Event('change', { bubbles: true }));
          selectEl.dispatchEvent(new Event('input', { bubbles: true }));
        }
        closeCustomSelectPopup();
      });
    }
    list.appendChild(item);
  });

  document.body.appendChild(list);

  let listRect = list.getBoundingClientRect();
  if (listRect.bottom > window.innerHeight) {
    list.style.top = 'auto';
    list.style.bottom = (window.innerHeight - rect.top) + 'px';
    list.style.transformOrigin = 'bottom left';
    listRect = list.getBoundingClientRect();
  }
  if (listRect.right > window.innerWidth) {
    const overflowPx = listRect.right - window.innerWidth + 8;
    list.style.left = (rect.left - overflowPx) + 'px';
  }

  selectEl.classList.add('custom-select-open');
  openCustomSelectPopup = { list, selectEl };
}

function wireCustomSelect(selectEl) {
  if (!selectEl || selectEl._customSelectWired) return;
  selectEl._customSelectWired = true;

  selectEl.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    selectEl.focus();
    openCustomSelectPopupFor(selectEl);
  });
  selectEl.addEventListener('keydown', (e) => {
    const opensNativePopup = e.key === 'Enter' || e.key === ' ' || e.key === 'F4'
      || ((e.key === 'ArrowDown' || e.key === 'ArrowUp') && e.altKey);
    if (!opensNativePopup) return;
    e.preventDefault();
    openCustomSelectPopupFor(selectEl);
  });
}

document.addEventListener('click', (e) => {
  if (!openCustomSelectPopup) return;
  if (e.target === openCustomSelectPopup.selectEl || openCustomSelectPopup.list.contains(e.target)) return;
  closeCustomSelectPopup();
});
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeCustomSelectPopup(); });
window.addEventListener('resize', closeCustomSelectPopup);
document.addEventListener('scroll', closeCustomSelectPopup, true);

document.querySelectorAll('select').forEach(wireCustomSelect);

const addBlockMenu = document.getElementById('addBlockMenu');
registerToolbarMenu('addBlockBtn', 'addBlockMenu', () => {
  document.getElementById('clearAllBlocksBtn').disabled = blocksList.length === 0;
});
(function initMenuHotkeyLabels() {
  const isMac = /Mac|iPod|iPhone|iPad/.test(navigator.platform || navigator.userAgent || '');
  document.querySelectorAll('.menu-hotkey[data-hotkey]').forEach(el => {
    const combo = el.dataset.hotkey;
    el.textContent = isMac ? combo.replace('Ctrl', '⌘').replace('Shift', '⇧').replace(/\+/g, '') : combo;
  });
})();

const fileMenu = document.getElementById('fileMenu');
registerToolbarMenu('fileMenuBtn', 'fileMenu');

registerToolbarMenu('deOptionsMenuBtn', 'deOptionsMenu');
registerToolbarMenu('deCacheMenuBtn', 'deCacheMenu');

function doCopySelection() {
  const block = findBlock(activeBlockId);
  if (block) neCopySelection(block);
}
function doCutSelection() {
  const block = findBlock(activeBlockId);
  if (block && neCopySelection(block)) {
    const keys = [...selectedNotes];
    neDeleteNoteKeys(block, keys, keys.length > 1 ? `Cut ${keys.length} notes` : 'Cut note');
  }
}
function doPasteClipboard() {
  const block = findBlock(activeBlockId);
  if (block && noteClipboard) {
    const count = noteClipboard.notes.length;
    const label = count > 1 ? `Paste ${count} notes` : 'Paste note';
    withBlockDataUndo(label, block, () => nePasteClipboard(block, nePlayheadBeat));
  }
}

const editMenu = document.getElementById('editMenu');
registerToolbarMenu('editMenuBtn', 'editMenu', () => {
  const block = findBlock(activeBlockId);
  if (isTimelineTabActive()) {
    document.getElementById('editSelectAllBtn').disabled = songEngine.songSequence.length === 0;
    document.getElementById('editSelectNoneBtn').disabled = tlSelectedIndices.size === 0;
  } else {
    document.getElementById('editSelectAllBtn').disabled = !block || eligibleNoteKeys(block).length === 0;
    document.getElementById('editSelectNoneBtn').disabled = selectedNotes.size === 0;
  }
  document.getElementById('editInvertSelectionBtn').disabled = isTimelineTabActive()
    ? songEngine.songSequence.length === 0
    : (!block || eligibleNoteKeys(block).length === 0);
  document.getElementById('editCopyBtn').disabled = !block || selectedNotes.size === 0;
  document.getElementById('editCutBtn').disabled = !block || selectedNotes.size === 0;
  document.getElementById('editPasteBtn').disabled = !block || !noteClipboard;
  const blockLocked = blockHasDiscardableEdits(block);
  document.getElementById('discardAllEditsBlockBtn').disabled = !blockLocked;
  const anyBlockLocked = blocksList.some(blockHasDiscardableEdits);
  document.getElementById('discardAllEditsGlobalBtn').disabled = !anyBlockLocked;
});
document.getElementById('editSelectAllBtn').addEventListener('click', () => { editMenu.style.display = 'none'; if (isTimelineTabActive()) tlSelectAll(); else selectAllNotes(); });
document.getElementById('editSelectNoneBtn').addEventListener('click', () => { editMenu.style.display = 'none'; if (isTimelineTabActive()) tlSelectNone(); else selectNoneNotes(); });
document.getElementById('editInvertSelectionBtn').addEventListener('click', () => { editMenu.style.display = 'none'; if (isTimelineTabActive()) tlInvertSelection(); else invertNoteSelection(); });
document.getElementById('editCopyBtn').addEventListener('click', () => { editMenu.style.display = 'none'; doCopySelection(); });
document.getElementById('editCutBtn').addEventListener('click', () => { editMenu.style.display = 'none'; doCutSelection(); });
document.getElementById('editPasteBtn').addEventListener('click', () => { editMenu.style.display = 'none'; doPasteClipboard(); });
document.getElementById('discardAllEditsBlockBtn').addEventListener('click', () => { editMenu.style.display = 'none'; unlockAllGroups(); });
document.getElementById('discardAllEditsGlobalBtn').addEventListener('click', () => { editMenu.style.display = 'none'; unlockAllGroupsGlobal(); });

const neSettingsPanel = document.getElementById('neSettingsPanel');
const neSettingsBtn = document.getElementById('neSettingsBtn');
registerToolbarMenu('neSettingsBtn', 'neSettingsPanel');
registerToolbarMenu('tlOptionsMenuBtn', 'tlOptionsMenu');
registerToolbarMenu('mxOptionsMenuBtn', 'mxOptionsMenu');
fileMenu.querySelectorAll('.add-block-menu-item').forEach(btn => {
  btn.addEventListener('click', () => { fileMenu.style.display = 'none'; });
});

function currentViewportScale() {
  return ((window.innerWidth / 1440) + (window.innerHeight / 810)) / 2;
}
// The CSS scale is always derived from the actual WebView viewport, so the fixed 1440x810 design fills whatever
// client area the host provides (window minus title bar/borders). The menu value is only used to tell the host
// which window size to request.
function fitToViewport() {
  document.documentElement.style.setProperty('--ui-scale', currentViewportScale());
}
document.querySelectorAll('#uiScaleSubmenu .add-block-menu-item').forEach(btn => {
  btn.addEventListener('click', () => {
    hostBridge.send('setUIScale', { scale: parseFloat(btn.dataset.scale) });
    fitToViewport(); // immediate consistency; the resize event re-fits once the host applied the new size
  });
});
window.addEventListener('resize', fitToViewport);
fitToViewport();

let muted = false;
const muteBtn = document.getElementById('muteBtn');
// Scrolling "MUTED" next to the status text: the track holds the same text twice, the CSS animation moves it by half.
const muteMarqueeTrack = document.getElementById('muteMarqueeTrack');
if (muteMarqueeTrack) muteMarqueeTrack.innerHTML = '<span>' + 'MUTED · '.repeat(4) + '</span><span>' + 'MUTED · '.repeat(4) + '</span>';
function applyMutedDisplay() {
  document.body.classList.toggle('plugin-muted', muted);
  if (!muteBtn) return;
  muteBtn.classList.toggle('active', muted);
  muteBtn.textContent = muted ? 'Muted' : 'Mute';
}
muteBtn?.addEventListener('click', () => {
  muted = !muted;
  applyMutedDisplay();
  hostBridge.send('setMuted', { muted });
});
hostBridge.on('mutedState', (payload) => {
  muted = !!(payload && payload.muted);
  applyMutedDisplay();
});

function createEmptyOrDuplicateBlock(duplicate) {
  const src = duplicate ? findBlock(activeBlockId) : null;
  const name = uniqueName(src ? baseNameOf(src.name) + ' Copy' : 'New Block', blocksList.map(b => b.name));
  const id = 'block_' + name.toLowerCase().replace(/[^a-z0-9]/g,'_') + '_' + Date.now();
  const g = readGrid();
  const data = {};
  trackList.forEach(t => { data[t.id] = src ? (src.data[t.id]||[]).map(s=>({...s})) : []; });
  blocksList.push({ id, name, generated:false, numerator:g.numerator, length: src ? (src.length||1) : 1, turnaroundInterval: src ? (src.turnaroundInterval||0) : 0, velocityBuild: src ? (src.velocityBuild||0) : 0, data });
  activeBlockId = id;
  renderBlockTabs(); renderNoteEditor();
}

function createFillStyleBlock(anchor) {
  const g = readGrid();
  const label = anchor === 'start' ? 'Outro-style' : 'Intro-style';
  const name = uniqueName(label, blocksList.map(b => b.name));
  const id = 'fill_' + name.toLowerCase().replace(/[^a-z0-9]/g,'_') + '_' + Date.now();
  const b = generateFillBlock(g, id, name, 1, BREAK_DEFAULT_BEATS, anchor, {});
  if (!b) { neMsg('Could not create block.'); return; }
  blocksList.push(b);
  activeBlockId = id;
  renderBlockTabs(); renderNoteEditor();
}

// Panel behavior (like the Options menus): clicking an item does NOT close
// the menu — it stays open so several blocks can be created in a row. It
// closes on mouse-leave or outside click via registerToolbarMenu().
addBlockMenu.querySelectorAll('.add-block-menu-item[data-kind]').forEach(btn => {
  btn.addEventListener('click', () => {
    const kind = btn.dataset.kind;
    if (kind === 'empty') createEmptyOrDuplicateBlock(false);
    else if (kind === 'duplicate') createEmptyOrDuplicateBlock(true);
    else if (kind === 'intro') createFillStyleBlock('end');
    else if (kind === 'outro') createFillStyleBlock('start');
  });
});
document.getElementById('clearAllBlocksBtn').addEventListener('click', () => {
  clearAllBlocks();
});

const GROUP_KNOB_NEUTRAL_DEFAULTS = {
  kick:    { subdivision: 4, density: 0, variation: 0, phrase: 1 },
  snare:   { subdivision: 4, density: 0, ghost: 0, variation: 0, phrase: 1 },
  hihat:   { subdivision: 4, density: 0, open: 0, phrase: 1 },
  ride:    { subdivision: 4, density: 0, variation: 0, phrase: 1 },
  toms:    { subdivision: 4, density: 0, variation: 0, phrase: 1 },
  cymbals: { subdivision: 4, density: 0, variation: 0, phrase: 1 },
};

function knobStateForGroup(block, group) {
  if (block && block.knobs && block.knobs[group]) return block.knobs[group];
  const preset = block && currentSectionPresets()[block.id];
  if (preset) {
    switch (group) {
      case 'kick':    return { subdivision: preset.subdivision, density: preset.kickDensity, variation: preset.kickVariation||0 };
      case 'snare':   return { subdivision: preset.subdivision, density: preset.snareDensity, ghost: preset.snareGhost, variation: preset.snareRim };
      case 'hihat':   return { subdivision: preset.subdivision, density: preset.hihatDensity, open: preset.hihatOpen };
      case 'ride':    return { subdivision: preset.subdivision, density: preset.rideDensity, variation: preset.rideVariation||0 };
      case 'toms':    return { subdivision: preset.subdivision, density: preset.tomsDensity, variation: preset.tomsVariation };
      case 'cymbals': return { subdivision: preset.subdivision, density: preset.cymbalsDensity, variation: preset.cymbalsVariation };
    }
  }
  return GROUP_KNOB_NEUTRAL_DEFAULTS[group];
}

const PHRASE_SLIDER_IDS = ['kickPhrase','snarePhrase','hihatPhrase','ridePhrase','tomsPhrase','cymbalsPhrase'];
function syncPhraseKnobRanges(block) {
  if (!block) return;
  const maxBars = Math.max(1, Math.min(BLOCK_MAX_LENGTH, block.length || 1));
  PHRASE_SLIDER_IDS.forEach(id => {
    const el = document.getElementById(id);
    if (!el) return;
    el.max = String(maxBars);
    if (+el.value > maxBars) {
      el.value = String(maxBars);
      const lab = document.getElementById(id + 'Val'); if (lab) lab.textContent = String(maxBars);
      const groupId = id.replace(/Phrase$/, '');
      block.phraseLength = block.phraseLength || {};
      if (block.phraseLength[groupId] != null) block.phraseLength[groupId] = maxBars;
    }
    el.dispatchEvent(new Event('input'));
  });
}

function syncKnobsToBlock(block) {
  if (!block) return;
  suppressKnobRegen = true;
  const set = (id, val, labelId, labelText) => {
    const el = document.getElementById(id);
    if (el) { el.value = val; el.dispatchEvent(new Event('input')); }
    const lab = labelId && document.getElementById(labelId); if (lab) lab.textContent = labelText;
  };
  const subIdxOf = (sub) => Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(sub));
  const syncSubdivision = (idPrefix, sub) => {
    const idx = subIdxOf(sub);
    set(idPrefix+'Subdivision', idx, idPrefix+'SubdivisionVal', SUBDIVISION_KNOB_LABELS[idx]);
  };
  const syncPhrase = (idPrefix, groupId, phraseVal) => {
    const phrase = Math.max(1, Math.min(block.length || 1, phraseVal || (block.phraseLength && block.phraseLength[groupId]) || 1));
    set(idPrefix+'Phrase', phrase, idPrefix+'PhraseVal', String(phrase));
  };
  syncPhraseKnobRanges(block);

  const kick = knobStateForGroup(block, 'kick');
  syncSubdivision('kick', kick.subdivision);
  set('kickDensity', kick.density, 'kickDensityVal', kick.density+'%');
  set('kickVariation', kick.variation||0, 'kickVariationVal', (kick.variation||0)+'%');
  syncPhrase('kick', 'kick', kick.phrase);

  const snare = knobStateForGroup(block, 'snare');
  syncSubdivision('snare', snare.subdivision);
  set('snareDensity', snare.density, 'snareDensityVal', snare.density+'%');
  set('snareGhostDensity', snare.ghost||0, 'snareGhostDensityVal', (snare.ghost||0)+'%');
  set('snareRim', snare.variation||0, 'snareRimVal', (snare.variation||0)+'%');
  syncPhrase('snare', 'snare', snare.phrase);

  const hihat = knobStateForGroup(block, 'hihat');
  syncSubdivision('hihat', hihat.subdivision);
  set('hihatDensity', hihat.density, 'hihatDensityVal', hihat.density+'%');
  set('hihatOpen', hihat.open||0, 'hihatOpenVal', (hihat.open||0)+'%');
  syncPhrase('hihat', 'hihat', hihat.phrase);

  const ride = knobStateForGroup(block, 'ride');
  syncSubdivision('ride', ride.subdivision);
  set('rideDensity', ride.density, 'rideDensityVal', ride.density+'%');
  set('rideVariation', ride.variation||0, 'rideVariationVal', (ride.variation||0)+'%');
  syncPhrase('ride', 'ride', ride.phrase);

  const toms = knobStateForGroup(block, 'toms');
  syncSubdivision('toms', toms.subdivision);
  set('tomsDensity', toms.density, 'tomsDensityVal', toms.density+'%');
  set('tomsVariation', toms.variation||0, 'tomsVariationVal', (toms.variation||0)+'%');
  syncPhrase('toms', 'toms', toms.phrase);

  const cymbals = knobStateForGroup(block, 'cymbals');
  syncSubdivision('cymbals', cymbals.subdivision);
  set('cymbalsDensity', cymbals.density, 'cymbalsDensityVal', cymbals.density+'%');
  set('cymbalsVariation', cymbals.variation||0, 'cymbalsVariationVal', (cymbals.variation||0)+'%');
  syncPhrase('cymbals', 'cymbals', cymbals.phrase);

  ['kick', 'snare', 'hihat', 'ride', 'toms', 'cymbals'].forEach(g => {
    const modeEl = document.getElementById(g + 'PatternMode');
    if (modeEl) paintPatternModeIcon(modeEl, getPatternModeName(block, g));
  });
  refreshMainPatternModeIcon();

  // Custom cluster: only while a custom track is focused (cluster visible).
  const customTrack = activeCustomTrack();
  if (customTrack) neApplyCustomKnobValues(block, customTrack);
  suppressKnobRegen = false;
}

// State a time signature / genre change replaces; the numerator comes from the blocks because the field has already changed.
function neTsCapture() {
  return {
    blocks: JSON.stringify(blocksList),
    numerator: (blocksList[0] && blocksList[0].numerator) || readGrid().numerator,
    genre: selectedGenre,
  };
}

function neTsRestore(st) {
  blocksList = JSON.parse(st.blocks);
  selectedGenre = st.genre;
  document.getElementById('genreSelect').value = st.genre;
  setParamValue('numerator', st.numerator);
  rebuildFullFromEditedBlocks();
  renderBlockTabs();
  renderNoteEditor();
  renderTimeline();
  renderStats(readParams(), readGrid());
  syncKnobsToBlock(findBlock(activeBlockId));
}

document.getElementById('genreSelect').addEventListener('change', (e) => {
  const prev = neTsCapture();
  selectedGenre = e.target.value;
  setParamValue('numerator', (selectedGenre === 'three_four') ? '3' : '4');
  neApplyTimeSignature((selectedGenre === 'swing') ? 'swing' : 'straight', prev);
});

document.getElementById('numerator').addEventListener('change', () => neApplyTimeSignature(null, neTsCapture()));

function neRegenerateMainBlocks(g, genreFeel) {
  const presets = currentSectionPresets();
  ['verse','chorus','bridge'].forEach(id => {
    const block = findBlock(id);
    if (!block) return;
    if (genreFeel) block.feel = genreFeel;
    const preset = presets[id];
    applyKickGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.kickDensity, variationPct: preset.kickVariation||0, anchorBeats: preset.kickAnchors||null });
    applySnareGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.snareDensity, ghostDensityPct: preset.snareGhost, rimPct: preset.snareRim, anchorBeats: preset.snareAnchors||null });
    applyHihatGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.hihatDensity, openPct: preset.hihatOpen });
    applyRideGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.rideDensity, variationPct: preset.rideVariation||0 });
    applyTomsGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.tomsDensity, variationPct: preset.tomsVariation });
    applyCymbalsGroupToBlock(block, g, { subdivision: preset.subdivision, densityPct: preset.cymbalsDensity, variationPct: preset.cymbalsVariation });
  });
}

// A new bar length touches every block: the main blocks are regenerated, Intro/Outro and unlocked Breaks are
// regenerated by the host, locked Breaks follow their parent, any other block only gets its notes trimmed.
async function neApplyTimeSignature(genreFeel, prev) {
  const g = readGrid();
  blocksList.forEach(b => { b.numerator = g.numerator; });
  neRegenerateMainBlocks(g, genreFeel);
  const jobs = [];
  blocksList.forEach(b => {
    if (['verse','chorus','bridge'].includes(b.id)) return;
    if (b.isFillBlock) {
      jobs.push(requestRegenerateFillBlock(b, g).then(res => {
        if (!res) return;
        b.data = res.data; b.length = res.length; b.fillBeats = res.fillBeats; b.feel = res.feel;
      }));
    } else if (isBreakBlock(b)) {
      if (!b.breakUnlocked) return;
      jobs.push(requestRegenerateBreakBlock(b, g).then(res => {
        if (!res) return;
        b.data = res.data; b.length = res.length; b.breakBeats = res.breakBeats;
      }));
    } else {
      const limit = (b.length || 1) * g.numerator;
      Object.keys(b.data || {}).forEach(tid => { b.data[tid] = (b.data[tid] || []).filter(n => n.beat < limit); });
    }
  });
  await Promise.all(jobs);
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
  renderStats(readParams(), readGrid());
  syncKnobsToBlock(findBlock(activeBlockId));
  const after = { blocks: JSON.stringify(blocksList), numerator: g.numerator, genre: selectedGenre };
  if (prev && (prev.blocks !== after.blocks || prev.numerator !== after.numerator || prev.genre !== after.genre)) {
    pushUndo('Change time signature', () => neTsRestore(prev), () => neTsRestore(after));
  }
}

function updateGroupLockUI() {
  const block = findBlock(activeBlockId);
  const clusters = [
    { group: 'kick',  knobs: ['kickSubdivision','kickDensity','kickVariation','kickPhrase'],   icon: 'kickLockIcon' },
    { group: 'snare', knobs: ['snareSubdivision','snareDensity','snareGhostDensity','snareRim','snarePhrase'], icon: 'snareLockIcon' },
    { group: 'hihat', knobs: ['hihatSubdivision','hihatDensity','hihatOpen','hihatPhrase'], icon: 'hihatLockIcon' },
    { group: 'ride',  knobs: ['rideSubdivision','rideDensity','rideVariation','ridePhrase'],   icon: 'rideLockIcon' },
    { group: 'toms',  knobs: ['tomsSubdivision','tomsDensity','tomsVariation','tomsPhrase'],  icon: 'tomsLockIcon' },
    { group: 'cymbals', knobs: ['cymbalsSubdivision','cymbalsDensity','cymbalsVariation','cymbalsPhrase'], icon: 'cymbalsLockIcon' },
  ];
  const fillBlockActive = isFillGeneratedBlock(block);
  clusters.forEach(({ group, knobs, icon }) => {
    const locked = fillBlockActive || !!(block && block.lockedGroups && block.lockedGroups[group]);
    knobs.forEach(id => {
      const el = document.getElementById(id);
      el.disabled = locked;
      el.closest('.ctrl').classList.toggle('knob-disabled', locked);
    });
    document.getElementById(icon).style.display = (!fillBlockActive && locked) ? 'inline' : 'none';
  });
  // Custom cluster: dynamic group id per track — only relevant while the cluster is visible
  // (a custom track is focused). Same lock semantics as the six engine groups.
  const customTrack = activeCustomTrack();
  const customClusterEl = document.getElementById('customKnobCluster');
  if (customTrack && customClusterEl && customClusterEl.style.display !== 'none') {
    const gid = customGroupId(customTrack.id);
    const locked = fillBlockActive || !!(block && block.lockedGroups && block.lockedGroups[gid]);
    ['customSubdivision','customDensity','customPhrase'].forEach(id => {
      const el = document.getElementById(id);
      el.disabled = locked;
      el.closest('.ctrl').classList.toggle('knob-disabled', locked);
    });
    document.getElementById('customLockIcon').style.display = (!fillBlockActive && locked) ? 'inline' : 'none';
  }

  const kickLocked = fillBlockActive || !!(block && block.lockedGroups && block.lockedGroups.kick);
  const ctrlRow = document.querySelector('.ne-control-row');
  if (ctrlRow) ctrlRow.classList.toggle('locked', kickLocked);

  const noSelection = !block || selectedNotes.size === 0;
  const rndSlider = document.getElementById('randomizeAmt');
  if (rndSlider) {
    rndSlider.disabled = noSelection;
    rndSlider.closest('.ne-vel-mini-ctrl').classList.toggle('knob-disabled', noSelection);
  }
  ['swingAmt', 'quantizeAmt'].forEach(id => {
    const slider = document.getElementById(id);
    if (!slider) return;
    slider.disabled = noSelection;
    slider.closest('.ne-vel-mini-ctrl').classList.toggle('knob-disabled', noSelection);
  });
  const nudgeCtrl = document.getElementById('nudgeCtrl');
  if (nudgeCtrl) nudgeCtrl.classList.toggle('knob-disabled', noSelection);
}

// Sets the Custom cluster's slider values from the focused custom track's per-block
// knob state (or neutral defaults). Caller is responsible for suppressKnobRegen.
function neApplyCustomKnobValues(block, track) {
  const gid = customGroupId(track.id);
  const st = (block.knobs && block.knobs[gid]) || { subdivision: 4, density: 50, phrase: 1 };
  const setv = (id, val, labId, labText) => {
    const el = document.getElementById(id);
    if (el && +el.value !== +val) { el.value = val; el.dispatchEvent(new Event('input')); }
    const lab = labId && document.getElementById(labId); if (lab) lab.textContent = labText;
  };
  const subIdx = Math.max(0, SUBDIVISION_KNOB_VALUES.indexOf(st.subdivision));
  setv('customSubdivision', subIdx, 'customSubdivisionVal', SUBDIVISION_KNOB_LABELS[subIdx]);
  setv('customDensity', st.density, 'customDensityVal', st.density + '%');
  const phrase = Math.max(1, Math.min(block.length || 1, st.phrase || 1));
  setv('customPhrase', phrase, 'customPhraseVal', String(phrase));
}

// Custom cluster lifecycle: visible while at least one custom track exists. The title's
// dropdown picks which instrument the knobs control (defaults to the first track / keeps
// the last selection); knob values reflect that track's per-block state.
function neSyncCustomCluster() {
  const cluster = document.getElementById('customKnobCluster');
  if (!cluster) return;
  const block = findBlock(activeBlockId);
  const customTracks = trackList.filter(t => t.role === 'custom');
  const sel = document.getElementById('customTrackSelect');
  if (sel) {
    // Rebuild options from the roster; keep the selection, fall back to the first track.
    let cur = activeCustomTrackId && customTracks.some(t => t.id === activeCustomTrackId) ? activeCustomTrackId : null;
    if (!cur && customTracks.length) cur = customTracks[0].id;
    sel.innerHTML = '';
    customTracks.forEach(t => { const o = document.createElement('option'); o.value = t.id; o.textContent = t.label; sel.appendChild(o); });
    sel.value = cur || '';
    if (sel.value !== (activeCustomTrackId || '')) activeCustomTrackId = sel.value || null;
  }
  const track = activeCustomTrack();
  if (!track || !block) { cluster.style.display = 'none'; return; }
  cluster.style.display = '';
  suppressKnobRegen = true;
  neApplyCustomKnobValues(block, track);
  suppressKnobRegen = false;
}

let suppressKnobRegen = false;
const KNOB_REGEN_DEBOUNCE_MS = 120;
const knobGestureStarters = {};
// Quant, Swing and Rnd keep the notes as they were when the selection was made (the gesture items) so that the slider value is the
// whole effect. toolBaselineApi reads that baseline into a plain snapshot for the undo entry and resolves it again in the restored notes.
function toolBaselineApi(read, write) {
  return {
    get() {
      const items = read();
      return items ? { unit: items.unit, items: items.map(({ note, ...rest }) => ({ ...rest })) } : null;
    },
    set(snap, block) {
      if (!snap) { write(null); return; }
      const items = snap.items.map(it => {
        const note = (block.data[it.trackId] || []).find(n => n.beat === it.appliedBeat && (it.appliedVel === undefined || n.vel === it.appliedVel));
        return note ? { ...it, note } : null;
      });
      if (items.some(it => !it)) { write(null); return; }
      items.unit = snap.unit;
      write(items);
    },
  };
}
// toolBaseline (optional): a toolBaselineApi; the undo entry then also restores the baseline and the selection.
function wireKnobGestureUndo(sliderIds, regenFn, label, toolBaseline) {
  let gesture = null;
  let pendingRegen = null;
  const captureControls = () => sliderIds.map(id => {
    const el = document.getElementById(id);
    const valEl = document.getElementById(id + 'Val');
    return { id, value: el.value, valText: valEl ? valEl.textContent : null };
  });
  const restoreControls = (controls) => {
    suppressKnobRegen = true;
    controls.forEach(c => {
      const el = document.getElementById(c.id);
      if (el.value !== c.value) { el.value = c.value; el.dispatchEvent(new Event('input')); }
      const valEl = document.getElementById(c.id + 'Val');
      if (valEl && c.valText !== null) valEl.textContent = c.valText;
    });
    suppressKnobRegen = false;
  };
  const beginGesture = () => {
    const block = findBlock(activeBlockId);
    if (block && !gesture) {
      gesture = {
        blockId: block.id,
        beforeData: JSON.stringify(block.data),
        beforeLocked: JSON.stringify(block.lockedGroups || {}),
        beforeKnobs: JSON.stringify(block.knobs || {}),
        beforeControls: captureControls(),
        beforeTool: toolBaseline ? { baseline: toolBaseline.get(), selection: [...selectedNotes] } : null
      };
    }
  };
  const applyTick = () => {
    clearTimeout(pendingRegen);
    pendingRegen = null;
    if (gesture && gesture.blockId !== activeBlockId) return;
    regenFn({ suppressUndo: true, deferAudioSync: true });
  };
  const applyFinal = () => {
    clearTimeout(pendingRegen);
    pendingRegen = null;
    if (gesture && gesture.blockId !== activeBlockId) return;
    return regenFn({ suppressUndo: true });
  };
  sliderIds.forEach(id => {
    knobGestureStarters[id] = beginGesture;
    document.getElementById(id).addEventListener('input', () => {
      if (suppressKnobRegen) return;
      beginGesture();
      clearTimeout(pendingRegen);
      pendingRegen = setTimeout(applyTick, KNOB_REGEN_DEBOUNCE_MS);
    });
    document.getElementById(id).addEventListener('change', async () => {
      await applyFinal();
      if (!gesture) return;
      const g = gesture; gesture = null;
      const block = findBlock(g.blockId);
      if (!block) return;
      const afterData = JSON.stringify(block.data);
      const afterLocked = JSON.stringify(block.lockedGroups || {});
      const afterKnobs = JSON.stringify(block.knobs || {});
      const afterControls = captureControls();
      const afterTool = toolBaseline ? { baseline: toolBaseline.get(), selection: [...selectedNotes] } : null;
      const dataChanged = afterData !== g.beforeData || afterLocked !== g.beforeLocked;
      const controlsChanged = JSON.stringify(afterControls) !== JSON.stringify(g.beforeControls);
      if (!dataChanged && !controlsChanged) return;
      const restore = (dataStr, lockedStr, knobsStr, controls, tool) => {
        block.data = JSON.parse(dataStr);
        block.lockedGroups = JSON.parse(lockedStr);
        block.knobs = JSON.parse(knobsStr);
        block.generated = false;
        restoreControls(controls);
        if (tool) {
          selectedNotes.clear();
          tool.selection.forEach(k => selectedNotes.add(k));
          toolBaseline.set(tool.baseline, block);
          neKeepToolBaselines = true;
        }
        rebuildFullFromEditedBlocks();
        renderNoteEditor();
        neKeepToolBaselines = false;
        updateGroupLockUI();
      };
      pushUndo(label,
        () => restore(g.beforeData, g.beforeLocked, g.beforeKnobs, g.beforeControls, g.beforeTool),
        () => restore(afterData, afterLocked, afterKnobs, afterControls, afterTool)
      );
    });
  });
}
document.addEventListener('pointerdown', (e) => {
  const visual = e.target.closest && e.target.closest('.knob-visual');
  if (!visual) return;
  const ctrlBox = visual.closest('.ctrl');
  const slider = ctrlBox && ctrlBox.querySelector('input[type="range"]');
  if (slider && knobGestureStarters[slider.id]) knobGestureStarters[slider.id]();
}, true);
document.addEventListener('wheel', (e) => {
  const ctrlBox = e.target.closest && e.target.closest('.ctrl');
  if (!ctrlBox) return;
  const slider = ctrlBox.querySelector('input[type="range"]');
  if (slider && knobGestureStarters[slider.id]) knobGestureStarters[slider.id]();
}, { capture: true, passive: true });

wireKnobGestureUndo(['kickSubdivision','kickDensity','kickVariation','kickPhrase'], regenerateKickGroup, 'Adjust Kick');
wireKnobGestureUndo(['snareSubdivision','snareDensity','snareRim','snarePhrase'], regenerateSnareGroup, 'Adjust Snare');
wireKnobGestureUndo(['snareGhostDensity'], regenerateSnareGhost, 'Adjust Snare Ghost');
wireKnobGestureUndo(['hihatSubdivision','hihatDensity','hihatOpen','hihatPhrase'], regenerateHihatGroup, 'Adjust Hi-Hat');
wireKnobGestureUndo(['rideSubdivision','rideDensity','rideVariation','ridePhrase'], regenerateRideGroup, 'Adjust Ride');
wireKnobGestureUndo(['tomsDensity','tomsVariation','tomsPhrase'], regenerateTomsGroup, 'Adjust Toms');
wireKnobGestureUndo(['cymbalsDensity','cymbalsVariation','cymbalsPhrase'], regenerateCymbalsGroup, 'Adjust Cymbals');

const generateSongMenu = document.getElementById('generateSongMenu');
registerToolbarMenu('generateSongBtn', 'generateSongMenu', () => {
  document.getElementById('clearSongBtn').disabled = songEngine.songSequence.length === 0;
  document.getElementById('consolidateSongBtn').disabled = songEngine.songSequence.length === 0;
});
generateSongMenu.querySelectorAll('.add-block-menu-item[data-structure]').forEach(btn => {
  btn.addEventListener('click', () => {
    generateSongMenu.style.display = 'none';
    generateSong(btn.dataset.structure);
  });
});
document.getElementById('clearSongBtn').addEventListener('click', () => {
  generateSongMenu.style.display = 'none';
  clearSongSequence();
});
document.getElementById('consolidateSongBtn').addEventListener('click', () => {
  generateSongMenu.style.display = 'none';
  consolidateSong();
});
document.getElementById('kickLockIcon').addEventListener('click', () => unlockGroup('kick'));
document.getElementById('snareLockIcon').addEventListener('click', () => unlockGroup('snare'));
document.getElementById('hihatLockIcon').addEventListener('click', () => unlockGroup('hihat'));
document.getElementById('rideLockIcon').addEventListener('click', () => unlockGroup('ride'));
document.getElementById('tomsLockIcon').addEventListener('click', () => unlockGroup('toms'));
document.getElementById('cymbalsLockIcon').addEventListener('click', () => unlockGroup('cymbals'));
document.getElementById('gmNotesToggle').addEventListener('change', (e) => {
  showGmNotes = e.target.checked;
  renderNoteEditor();
});
document.getElementById('infoStringToggle').addEventListener('change', (e) => {
  document.getElementById('statPills').style.visibility = e.target.checked ? '' : 'hidden';
});

const A11Y_ZOOM_KEY = 'dggA11yHoverZoom';
const A11Y_FACTOR_KEY = 'dggA11yZoomFactor';
const A11Y_CONTROL_IDS = [
  ['a11yHoverZoomToggle', 'a11yZoomFactorSelect'],
  ['a11yHoverZoomToggleDE', 'a11yZoomFactorSelectDE'],
  ['a11yHoverZoomToggleSG', 'a11yZoomFactorSelectSG'],
  ['a11yHoverZoomToggleMixer', 'a11yZoomFactorSelectMixer'],
];
const a11yControls = A11Y_CONTROL_IDS
  .map(([toggleId, selectId]) => ({ toggle: document.getElementById(toggleId), select: document.getElementById(selectId) }))
  .filter(c => c.toggle && c.select);

function applyA11yHoverZoom(enabled) {
  document.body.classList.toggle('a11y-hover-zoom', enabled);
}

function syncA11yControls() {
  const enabled = dggGetItem(A11Y_ZOOM_KEY) !== '0';
  const factor = dggGetItem(A11Y_FACTOR_KEY) || '1.5';
  a11yControls.forEach(({ toggle, select }) => {
    toggle.checked = enabled;
    select.value = factor;
    select.disabled = !enabled;
  });
  document.documentElement.style.setProperty('--a11y-zoom-factor', factor);
  applyA11yHoverZoom(enabled);
}
if (typeof dggOnReady === 'function') {
  dggOnReady(syncA11yControls);
} else {
  syncA11yControls();
}

a11yControls.forEach(({ toggle, select }) => {
  toggle.addEventListener('change', (e) => {
    dggSetItem(A11Y_ZOOM_KEY, e.target.checked ? '1' : '0');
    syncA11yControls();
  });
  select.addEventListener('change', (e) => {
    dggSetItem(A11Y_FACTOR_KEY, e.target.value);
    syncA11yControls();
  });
});

const showConfirmDialogsToggle = document.getElementById('showConfirmDialogsToggle');
function initShowConfirmDialogsToggle() {
  if (showConfirmDialogsToggle) showConfirmDialogsToggle.checked = dggGetItem(SHOW_CONFIRM_DIALOGS_KEY) !== '0';
}
if (typeof dggOnReady === 'function') {
  dggOnReady(initShowConfirmDialogsToggle);
} else {
  initShowConfirmDialogsToggle();
}
showConfirmDialogsToggle.addEventListener('change', (e) => {
  dggSetItem(SHOW_CONFIRM_DIALOGS_KEY, e.target.checked ? '1' : '0');
});
function syncShowConfirmDialogsToggle() {
  showConfirmDialogsToggle.checked = dggGetItem(SHOW_CONFIRM_DIALOGS_KEY) !== '0';
}

// ============================================================================
// Accessible hover zoom and clipping: a scaled .knob-cluster / .ne-dial-row is cut off by any ancestor with
// overflow:auto|hidden and a bounded size (z-index does not help). Clusters inside such a container therefore
// pop to position:fixed on hover (popOut*/popIn*) with a placeholder keeping their spot. Clusters without a
// clipping ancestor use the plain mouseenter -> setClusterZoomOrigin path. A new scrolling container around
// clusters needs the popOut*/popIn* treatment.
// ============================================================================

function clampedOrigin(near, far, size, grow) {
  const maxOrigin = near / grow;
  const minOrigin = size - (far / grow);
  let origin = size / 2;
  origin = Math.min(origin, maxOrigin);
  origin = Math.max(origin, minOrigin);
  return Math.max(0, Math.min(size, origin));
}
// Nearest ancestor that actually clips (overflow auto|hidden|scroll|clip). A
// scale-in-place zoom is sliced at the first such ancestor, so it must be the
// bounds we keep the scaled box inside. Returns null if there is none.
function getClipAncestor(el) {
  let n = el.parentElement;
  while (n && n !== document.documentElement) {
    const cs = getComputedStyle(n);
    if (/(auto|hidden|scroll|clip)/.test(cs.overflowX || 'visible') ||
        /(auto|hidden|scroll|clip)/.test(cs.overflowY || 'visible')) return n;
    n = n.parentElement;
  }
  return null;
}
function setClusterZoomOrigin(cluster, boundsOverride) {
  const factor = parseFloat(document.documentElement.style.getPropertyValue('--a11y-zoom-factor')) || 1.5;
  const grow = factor - 1;
  if (grow <= 0) { cluster.style.transformOrigin = '50% 50%'; return; }
  const bounds = boundsOverride || cluster.closest('#deParamPanels') || cluster.closest('.tab-panel') || cluster.closest('.app') || cluster.parentElement;
  const prevTransform = cluster.style.transform, prevTransition = cluster.style.transition;
  cluster.style.transition = 'none';
  cluster.style.transform = 'none';
  void cluster.offsetHeight;
  const cRect = cluster.getBoundingClientRect();
  const bRect = bounds.getBoundingClientRect();
  cluster.style.transform = prevTransform;
  cluster.style.transition = prevTransition;
  const originX = clampedOrigin(Math.max(0, cRect.left - bRect.left), Math.max(0, bRect.right - cRect.right), cRect.width, grow);
  const originY = clampedOrigin(Math.max(0, cRect.top - bRect.top), Math.max(0, bRect.bottom - cRect.bottom), cRect.height, grow);
  const scale = currentUIScale();
  cluster.style.transformOrigin = `${originX / scale}px ${originY / scale}px`;
}
document.querySelectorAll('.ne-editing-toolbar .knob-cluster').forEach(cluster => {
  cluster.addEventListener('mouseenter', () => setClusterZoomOrigin(cluster));
});
document.querySelector('#dePolyRow .knob-cluster')?.addEventListener('mouseenter', function() { setClusterZoomOrigin(this); });

// --- Fixed-position placement, containing-block aware ----------------------
// CSS: any ancestor with a non-none transform/perspective/filter (or will-change: transform|view / contain: paint)
// becomes the containing block for its position:fixed descendants. For rows inside .app that is .app; a dial row
// in a body-level FX panel (.fx-pop carries its own `transform: scale(var(--ui-scale))`) gets .fx-pop. Fixed
// coordinates are computed against that containing block's origin.
function getFixedContainingBlock(el) {
  let n = el.parentElement;
  while (n && n !== document.documentElement) {
    const cs = getComputedStyle(n);
    if ((cs.transform && cs.transform !== 'none') ||
        (cs.perspective && cs.perspective !== 'none') ||
        (cs.filter && cs.filter !== 'none') ||
        (cs.backdropFilter && cs.backdropFilter !== 'none') ||
        /(^|,\s*)(transform|view)(\s*,|$)/.test(cs.willChange || '') ||
        (cs.contain && /paint/.test(cs.contain))) return n;
    n = n.parentElement;
  }
  return null; // no transformed ancestor -> the viewport is the containing block
}
function getFixedContainingBlockScale(cb) {
  if (!cb) return { sx: 1, sy: 1 };
  const m = /^matrix\(([^)]+)\)/.exec(getComputedStyle(cb).transform || '');
  if (m) {
    const p = m[1].split(',').map(x => parseFloat(x.trim()));
    if (p.length === 6 && isFinite(p[0]) && isFinite(p[3])) return { sx: Math.abs(p[0]), sy: Math.abs(p[3]) };
  }
  return { sx: 1, sy: 1 }; // CB created by perspective/filter/will-change: unscaled
}
// Place `el` (position:fixed) at the exact screen spot it occupied before.
// The probe read-back makes this independent of which edge of the containing
// block (padding vs content) the browser treats as origin -- .fx-pop has a
// 1px border + 10px padding, so hardcoding that offset would be fragile.
function placeFixedAt(el, rect) {
  const cb = getFixedContainingBlock(el);
  const { sx, sy } = getFixedContainingBlockScale(cb);
  el.style.position = 'fixed';
  el.style.margin = '0';
  // Shrink-to-fit: a row with few knobs (Kick: 2, Fill: 3) must not keep the
  // full sidebar width as empty space to the right of the knobs. Measure the
  // max-content width while untransformed and cap at the original width, so
  // content-wider elements (EQ/Limiter rows) keep their previous layout.
  el.style.width = 'max-content';
  const contentW = el.getBoundingClientRect().width;
  const w = (isFinite(contentW) && contentW > 0) ? Math.min(rect.width, contentW) : rect.width;
  el.style.width = (w / sx) + 'px';
  el.style.top = '0px';
  el.style.left = '0px';
  const probe = el.getBoundingClientRect();
  el.style.top = ((rect.top - probe.top) / sy) + 'px';
  el.style.left = ((rect.left - probe.left) / sx) + 'px';
}

let poppedParamCluster = null;
function popOutParamCluster(cluster) {
  if (poppedParamCluster === cluster) return;
  if (poppedParamCluster) popInParamCluster(poppedParamCluster);
  const rect = cluster.getBoundingClientRect();
  const cs = getComputedStyle(cluster);
  const marginTop = parseFloat(cs.marginTop) || 0;
  const marginBottom = parseFloat(cs.marginBottom) || 0;
  const marginLeft = parseFloat(cs.marginLeft) || 0;
  const marginRight = parseFloat(cs.marginRight) || 0;
  const scale = currentUIScale();
  const placeholder = document.createElement('div');
  placeholder.className = 'de-param-cluster-placeholder';
  placeholder.style.width = (rect.width / scale + marginLeft + marginRight) + 'px';
  placeholder.style.height = (rect.height / scale + marginTop + marginBottom) + 'px';
  cluster.parentNode.insertBefore(placeholder, cluster);
  cluster._a11yPlaceholder = placeholder;
  placeFixedAt(cluster, rect); // containing-block aware (see helper above)
  cluster.classList.add('a11y-zoomed');
  poppedParamCluster = cluster;
  setClusterZoomOrigin(cluster);
}
function popInParamCluster(cluster) {
  if (!cluster) return;
  cluster.style.position = ''; cluster.style.top = ''; cluster.style.left = ''; cluster.style.width = ''; cluster.style.margin = '';
  cluster.style.removeProperty('--a11y-zoom-factor');
  cluster.classList.remove('a11y-zoomed');
  if (cluster._a11yPlaceholder) { cluster._a11yPlaceholder.remove(); cluster._a11yPlaceholder = null; }
  if (poppedParamCluster === cluster) poppedParamCluster = null;
}
function deWireClusterZoom(cluster) {
  // inside an editor card the controls block zooms (deWireCardZoom), not its single clusters; the pad cluster does not zoom
  cluster.addEventListener('mouseenter', () => { if (!cluster.closest('.de-instance-card')) popOutParamCluster(cluster); });
  cluster.addEventListener('mouseleave', () => popInParamCluster(cluster));
}
// Drum Engine editor: the knob groups and Volume zoom together as one block (.de-instance-card-ctrls); the pad does not zoom.
// While zoomed the block is an overlay; the factor is capped to what fits into the window.
function setCardZoomOrigin(card) {
  const factor = parseFloat(document.documentElement.style.getPropertyValue('--a11y-zoom-factor')) || 1.5;
  const bounds = card.closest('.app') || card.parentElement;
  const prevTransform = card.style.transform, prevTransition = card.style.transition;
  card.style.transition = 'none';
  card.style.transform = 'none';
  void card.offsetHeight;
  const cRect = card.getBoundingClientRect();
  const bRect = bounds.getBoundingClientRect();
  card.style.transform = prevTransform;
  card.style.transition = prevTransition;
  const eff = Math.max(1, Math.min(factor, bRect.height / cRect.height, bRect.width / cRect.width));
  card.style.setProperty('--a11y-zoom-factor', String(eff));
  const grow = eff - 1;
  if (grow <= 0) { card.style.transformOrigin = '50% 50%'; return; }
  const originX = clampedOrigin(Math.max(0, cRect.left - bRect.left), Math.max(0, bRect.right - cRect.right), cRect.width, grow);
  const originY = clampedOrigin(Math.max(0, cRect.top - bRect.top), Math.max(0, bRect.bottom - cRect.bottom), cRect.height, grow);
  const scale = currentUIScale();
  card.style.transformOrigin = `${originX / scale}px ${originY / scale}px`;
}
function deWireCardZoom(card) {
  if (card._cardZoomWired) return;
  card._cardZoomWired = true;
  card.addEventListener('mouseenter', () => { popOutParamCluster(card); setCardZoomOrigin(card); });
  card.addEventListener('mouseleave', () => popInParamCluster(card));
}
document.getElementById('deParamPanels')?.addEventListener('scroll', () => { if (poppedParamCluster) popInParamCluster(poppedParamCluster); });

// Mixer per-slot knob panels (.mixer-rack .strip-knobs, Pan/Rev/Del). In-place
// :hover scale (the CSS does the zoom); JS only sets a bounds-aware origin bound
// to .mixer-rack's clip so edge strips grow inward instead of being sliced off.
// Gated on the Accessible toggle so an OFF switch never causes a layout jump.
// Wired from mixer-tab.js after the strips are built.
function mixerWireKnobZoom(panel) {
  if (!panel || panel._mixerZoomWired) return;
  panel._mixerZoomWired = true;
  const a11yOn = () => document.body.classList.contains('a11y-hover-zoom');
  panel.addEventListener('mouseenter', () => { if (a11yOn()) setClusterZoomOrigin(panel, getClipAncestor(panel)); });
}

// Sidebar cards. The static cards (Main, Selected notes) sit outside the scrolling sidebar and zoom in place (CSS :hover); JS only sets
// a bounds-aware origin. The cards inside #neSidebar are clipped by it, so while hovered they pop out to position:fixed (placeholder keeps
// their spot), like the Drum Engine clusters. A re-render of the sidebar removes both the card and its placeholder.
const neA11yOn = () => document.body.classList.contains('a11y-hover-zoom');
document.querySelectorAll('.ne-sidebar-top').forEach(card => card.addEventListener('mouseenter', () => { if (neA11yOn()) setClusterZoomOrigin(card); }));
let poppedSidebarSection = null;
function popInSidebarSection(sec) {
  if (!sec) return;
  sec.style.position = ''; sec.style.top = ''; sec.style.left = ''; sec.style.width = ''; sec.style.margin = ''; sec.style.transformOrigin = '';
  sec.classList.remove('a11y-zoomed');
  if (sec._a11yPlaceholder) { sec._a11yPlaceholder.remove(); sec._a11yPlaceholder = null; }
  if (poppedSidebarSection === sec) poppedSidebarSection = null;
}
function popOutSidebarSection(sec) {
  if (poppedSidebarSection === sec) return;
  if (poppedSidebarSection) popInSidebarSection(poppedSidebarSection);
  const rect = sec.getBoundingClientRect();
  const scale = currentUIScale();
  const placeholder = document.createElement('div');
  placeholder.className = 'de-param-cluster-placeholder';
  placeholder.style.width = (rect.width / scale) + 'px';
  placeholder.style.height = (rect.height / scale) + 'px';
  sec.parentNode.insertBefore(placeholder, sec);
  sec._a11yPlaceholder = placeholder;
  placeFixedAt(sec, rect);
  sec.classList.add('a11y-zoomed');
  poppedSidebarSection = sec;
  setClusterZoomOrigin(sec);
  sec.addEventListener('mouseleave', () => popInSidebarSection(sec), { once: true });
}
document.getElementById('neSidebar')?.addEventListener('mouseover', (e) => {
  if (!neA11yOn()) return;
  const sec = e.target.closest && e.target.closest('.ne-section');
  if (!sec || sec === poppedSidebarSection || sec._a11yPlaceholder) return;
  popOutSidebarSection(sec);
});
document.getElementById('neSidebar')?.addEventListener('scroll', () => { if (poppedSidebarSection) popInSidebarSection(poppedSidebarSection); });

// Sidebar dial rows zoom in place (CSS :hover). JS only sets a bounds-aware
// origin, bound to .ne-sidebar's clip, so the scaled row stays inside instead of
// being sliced. No pop-out / placeholder / lock needed -- in-place scale has no
// persistent state to desync on re-render or scroll.
const neSidebarEl = document.getElementById('neSidebar');
neSidebarEl?.addEventListener('mouseover', (e) => {
  const row = e.target.closest('.ne-dial-row');
  if (!row || !(!row.contains(e.relatedTarget))) return;
  if (row.classList.contains('ne-hover-hold')) return;   // already zoomed; re-measuring would restart its zoom animation
  setClusterZoomOrigin(row, getClipAncestor(row));
  // The scaled row counts as scrollable overflow; without an existing scrollbar,
  // suppress it for the duration of the hover so none appears.
  clearTimeout(neZoomNoScrollTimer);
  if (neSidebarEl.classList.contains('ne-zoom-noscroll') || neSidebarEl.scrollHeight <= neSidebarEl.clientHeight) neSidebarEl.classList.add('ne-zoom-noscroll');
});
// Released only after the row's scale-down transition (.12s) has finished.
let neZoomNoScrollTimer = 0;
const neReleaseZoomNoScroll = () => {
  clearTimeout(neZoomNoScrollTimer);
  neZoomNoScrollTimer = setTimeout(() => neSidebarEl.classList.remove('ne-zoom-noscroll'), 200);
};
neSidebarEl?.addEventListener('mouseout', (e) => {
  const row = e.target.closest('.ne-dial-row');
  if (row && row.contains(e.relatedTarget)) return;
  neReleaseZoomNoScroll();
});
neSidebarEl?.addEventListener('mouseleave', neReleaseZoomNoScroll);

// A re-render replaces the sidebar rows. The pointer then sits over the zoomed area of a row that is
// no longer zoomed, outside the new row's unzoomed box, so it would never zoom again. The new row at the
// same index keeps the zoom until the pointer leaves its zoomed rect.
function neHoveredSidebarRowIndex(sidebar) {
  return [...sidebar.querySelectorAll('.ne-dial-row')].findIndex(r => r.matches(':hover'));
}
function neRestoreSidebarRowZoom(index) {
  const row = neSidebarEl && neSidebarEl.querySelectorAll('.ne-dial-row')[index];
  if (!row) return;
  if (neSidebarEl.classList.contains('ne-zoom-noscroll') || neSidebarEl.scrollHeight <= neSidebarEl.clientHeight) neSidebarEl.classList.add('ne-zoom-noscroll');
  neHoldZoom(row, getClipAncestor(row), neReleaseZoomNoScroll);
}
// Keeps `el` zoomed (class ne-hover-hold, styled like :hover) from the first paint of a rebuilt element
// until the pointer leaves its zoomed rect. bounds: clip ancestor for the zoom origin (null = default).
function neHoldZoom(el, bounds, onRelease) {
  if (!document.body.classList.contains('a11y-hover-zoom')) return;
  setClusterZoomOrigin(el, bounds || undefined);
  // Zoomed from the first paint: a transition would show the element shrunken and growing again.
  el.style.transition = 'none';
  el.classList.add('ne-hover-hold');
  void el.offsetHeight;
  requestAnimationFrame(() => { el.style.transition = ''; });
  const heldAt = performance.now();
  const release = (e) => {
    if (performance.now() - heldAt < 300) return;   // the rect is still mid-transition (.12s) and smaller than its final size
    if (el.isConnected) {
      const r = el.getBoundingClientRect();
      if (e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom) return;
    }
    el.classList.remove('ne-hover-hold');
    document.removeEventListener('mousemove', release, true);
    if (onRelease) onRelease();
  };
  document.addEventListener('mousemove', release, true);
}

function neExtendGridToBottom() {
  const rowsCol = document.querySelector('#drumGrid .ne-rows');
  if (!rowsCol) return;
  // The rows area now stretches to fill the Note Editor's height (.ne-rows-wrap),
  // so measure its real height instead of assuming a fixed one.
  const rowsWrap = rowsCol.closest('.ne-rows-wrap');
  const wrapH = (rowsWrap && rowsWrap.clientHeight > 0) ? rowsWrap.clientHeight : NE_ROWS_WRAP_H;
  rowsCol.querySelectorAll('.ne-grid-filler').forEach(el => el.remove());
  const rows = rowsCol.querySelectorAll('.ne-row');
  if (!rows.length) return;
  const last = rows[rows.length - 1];
  const remaining = wrapH - (last.offsetTop + last.offsetHeight);
  if (remaining <= 0) return;
  const template = rowsCol.querySelector('.ne-row:not(.ne-group-header-row):not(.ne-control-row)') || last;
  const filler = template.cloneNode(true);
  filler.className = 'ne-row ne-grid-filler';
  filler.style.height = remaining + 'px';
  [...filler.children].forEach(ch => {
    const keep = ch.classList && (ch.classList.contains('ne-row-grid') || ch.classList.contains('ne-tick') || ch.classList.contains('ne-snap-line') || ch.classList.contains('ne-bar-line'));
    if (!keep) ch.remove();
  });
  const rowsInner = rowsCol.querySelector(':scope > div');
  if (rowsInner) {
    rowsInner.querySelectorAll('.ne-endless-overlay, .ne-break-lock-overlay, .ne-turnaround-zone').forEach(src => {
      const clone = document.createElement('div');
      clone.className = src.className;
      clone.style.left = src.style.left;
      clone.style.width = src.style.width;
      clone.style.top = '0';
      clone.style.bottom = '0';
      filler.appendChild(clone);
    });
  }
  rowsCol.appendChild(filler);
}

function neResyncA11yHover() {
  // Sidebar dial rows now zoom in place (CSS :hover), so there is no popped-out
  // state to restore after a re-render -- only the grid filler needs extending.
  neExtendGridToBottom();
}
// Shared "+ Add Track" action — used by the visible button above the track list.
// The setup dialog comes first: on confirm the track is created with exactly
// those settings, so no unconfigured "Track N" lands in the list. The track
// takes the name of its instrument, so there is no separate name field and no
// "None" option — a custom track always plays an engine. Each instrument can
// carry at most one track: only instruments not assigned yet are offered.
async function neAddTrack() {
  const usedInstruments = new Set(trackList.filter(t => t.customInstrument).map(t => t.customInstrument));
  const instrumentOptions = dggCustomInstruments()
    .filter(d => !usedInstruments.has(d.id))
    .map(d => ({ value:d.id, label:d.name }));
  if (!instrumentOptions.length) return;   // the button is disabled in this state — belt and braces
  const result = await showModal({
    title:'New custom track',
    fields:[{label:'Instrument', value:instrumentOptions[0].value, options:instrumentOptions},
            {label:'GM note number', value:String(findUnusedGmNote())}],
    confirmLabel:'Create'
  });
  if (!result) return;
  const [instrumentId, gmStr] = result;
  const def = dggCustomInstruments().find(d => d.id === instrumentId);
  if (!def) return;
  const label = def.name;
  const gm = parseInt(gmStr);
  const id = 'custom_' + label.toLowerCase().replace(/[^a-z0-9]/g,'_') + '_' + Date.now();
  withTracksUndo(`Add track (${label})`, () => {
    trackList.push({ id, label, color: DGInstrument.trackHex('custom', 0, 1), gm: isNaN(gm) ? findUnusedGmNote() : gm, role:'custom', customInstrument: instrumentId });
    blocksList.forEach(b => { b.data[id] = []; });
    focusedTrackId = id;
    neSetActiveCustomTrack(id);   // the new track is what we set up next — cluster selects it
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    syncCustomInstruments();
  });
}

// Undo step over the track list and every block's data, for adding or deleting a custom track.
function withTracksUndo(label, fn) {
  const snap = () => ({ tracks: JSON.stringify(trackList), blocks: JSON.stringify(blocksList) });
  const before = snap();
  fn();
  const after = snap();
  const restore = (st) => {
    trackList = JSON.parse(st.tracks);
    blocksList = JSON.parse(st.blocks);
    if (focusedTrackId && !trackList.some(t => t.id === focusedTrackId)) focusedTrackId = null;
    if (activeCustomTrackId && !trackList.some(t => t.id === activeCustomTrackId)) activeCustomTrackId = null;
    selectedNotes.clear();
    rebuildFullFromEditedBlocks();
    renderBlockTabs();
    renderNoteEditor();
    syncCustomInstruments();
  };
  pushUndo(label, () => restore(before), () => restore(after));
}
document.getElementById('neAddTrackTopBtn').addEventListener('click', neAddTrack);

// The "+ Add Track" button is enabled only while at least one custom instrument
// is still unassigned (one track per instrument). Re-evaluated on every note
// editor render, which all assignment changes funnel through.
dggOnInstrumentTable(() => neRefreshAddTrackButton());   // the instruments come with the table
function neRefreshAddTrackButton() {
  const btn = document.getElementById('neAddTrackTopBtn');
  if (!btn) return;
  const usedInstruments = new Set(trackList.filter(t => t.customInstrument).map(t => t.customInstrument));
  const free = dggCustomInstruments().some(d => !usedInstruments.has(d.id));
  btn.disabled = !free;
  btn.dataset.tooltip = free
    ? 'Add Track\nOpens the setup dialog (instrument, GM note). On confirm the track is added under the instrument\'s name.'
    : 'Add Track\nAll custom instruments are already in use — one track per instrument.';
}

// Delete a custom track (subtab ✕): removes it from the roster and its notes from every block,
// freeing its instrument for a new track. Confirmed first; Undo brings the track and its notes back.
async function neDeleteCustomTrack(t) {
  let noteCount = 0;
  blocksList.forEach(b => { noteCount += (b.data[t.id] || []).length; });
  const ok = await showModal({
    title:'Delete track',
    message:`Remove "${t.label}" and its ${noteCount} note${noteCount === 1 ? '' : 's'} from all blocks?`,
    confirmLabel:'Delete',
    danger:true
  });
  if (!ok) return;
  withTracksUndo(`Delete track (${t.label})`, () => {
    const i = trackList.findIndex(x => x.id === t.id);
    if (i >= 0) trackList.splice(i, 1);
    blocksList.forEach(b => { delete b.data[t.id]; });
    mutedInstruments.delete(t.id);
    soloedInstruments.delete(t.id);
    if (focusedTrackId === t.id) focusedTrackId = null;
    if (activeCustomTrackId === t.id) activeCustomTrackId = null;   // cluster falls back to the first remaining track
    for (const k of [...selectedNotes]) if (k.startsWith(t.id + '|')) selectedNotes.delete(k);
    syncCustomInstruments();   // C++ drops the assignment (missing entry = unassigned)
    renderNoteEditor();        // also re-enables "+ Add Track" via neRefreshAddTrackButton
  });
}

// =========================================
// UI WIRING
// =========================================
const sliderMap = {
  randomizeAmt:['randomizeAmtVal', v=>v+'%'],
  swingAmt:['swingAmtVal', v=>(v>0?'+':'')+v+'%'],
  quantizeAmt:['quantizeAmtVal', v=>v+'%'],
};
Object.entries(sliderMap).forEach(([id,[valId,fmt]]) => {
  document.getElementById(id)?.addEventListener('input', e => { document.getElementById(valId).textContent = fmt(e.target.value); });
});

['numerator','subdivision'].forEach(id => document.getElementById(id).addEventListener('change', () => generateSequence(true)));
document.getElementById('denominator')?.addEventListener('change', () => renderStats(readParams(), readGrid()));


let randomizeGestureItems = null;
function randomizeRegen() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (randomizeGestureItems && !sliderBaselineValid(block, randomizeGestureItems)) randomizeGestureItems = null;
  if (!randomizeGestureItems) {
    if (!selectedNotes.size) return;
    const items = neResolveSelection(block);
    if (!items.length) return;
    randomizeGestureItems = items.map(it => ({
      trackId: it.trackId, note: it.note, appliedBeat: it.note.beat, appliedVel: it.note.vel,
      origBeat: it.note.beat, origVel: it.note.vel,
      velF: Math.random() - 0.5, beatF: Math.random() - 0.5
    }));
  }
  const amount = getParamNum('randomizeAmt') / 100;
  const g = readGrid();
  const totalBeats = (block.length || 1) * g.numerator;
  const touchedTracks = new Set();
  randomizeGestureItems.forEach(it => {
    it.note.vel = amount > 0 ? Math.max(1, Math.min(127, it.origVel + Math.round(it.velF * 30 * amount))) : it.origVel;
    it.note.beat = amount > 0 ? Math.max(0, Math.min(totalBeats - 0.0001, it.origBeat + it.beatF * g.snap * 0.7 * amount)) : it.origBeat;
    if (it.note.turnaround) delete it.note.turnaround;
    touchedTracks.add(it.trackId);
  });
  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));
  selectedNotes.clear();
  randomizeGestureItems.forEach(it => selectedNotes.add(it.trackId + '|' + it.note.beat));
  touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid));
  randomizeGestureItems.forEach(it => { it.appliedBeat = it.note.beat; it.appliedVel = it.note.vel; });
  block.generated = false;
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
}
wireKnobGestureUndo(['randomizeAmt'], randomizeRegen, 'Randomize', toolBaselineApi(() => randomizeGestureItems, v => { randomizeGestureItems = v; }));

// The selection tools (Quant, Swing, Rnd) work from the notes as they were when the selection was made, like the velocity lane's
// sliders: the slider value is the whole effect, and Ctrl+click puts the notes back. The baseline is dropped when the selection
// changes, or when something else moved or replaced the notes since the last slider move.
function sliderBaselineValid(block, items) {
  return items.every(it => (block.data[it.trackId] || []).includes(it.note)
    && it.note.beat === it.appliedBeat && (it.appliedVel === undefined || it.note.vel === it.appliedVel));
}
['randomizeAmt', 'swingAmt', 'quantizeAmt'].forEach(id => {
  const slider = document.getElementById(id);
  slider.addEventListener('pointerdown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    e.preventDefault();
    if (slider.disabled) return;
    slider.value = 0;
    slider.dispatchEvent(new Event('input'));
    slider.dispatchEvent(new Event('change'));
  });
});

// Swing: moves the off-beat notes of the selection by -50..+50% of the swing unit (positive = later); 0 leaves the notes alone.
// The unit is the Snap step, or the first multiple of it (up to an 8th) at which the selection has notes on odd positions: with a
// 16th Snap and hi-hats on 8ths the 8th off-beats swing, with hi-hats on 16ths the 16th off-beats do.
let swingGestureItems = null;
function swingRegen() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (swingGestureItems && !sliderBaselineValid(block, swingGestureItems)) swingGestureItems = null;
  if (!swingGestureItems) {
    if (!selectedNotes.size) return;
    const items = neResolveSelection(block);
    if (!items.length) return;
    const snap = readGrid().snap;
    const onOddStep = (beat, unit) => { const step = Math.round(beat / unit); return step % 2 !== 0 && Math.abs(beat - step * unit) <= unit * 0.3; };
    let unit = snap;
    for (let k = 0; k < 4; k++) {
      const candidate = snap * Math.pow(2, k);
      if (k > 0 && candidate > 0.5 + 1e-9) break;   // swing stays at 8th level or finer
      if (items.some(it => onOddStep(it.note.beat, candidate))) { unit = candidate; break; }
    }
    swingGestureItems = items.map(it => ({ trackId: it.trackId, note: it.note, origBeat: it.note.beat, appliedBeat: it.note.beat }));
    swingGestureItems.unit = unit;
  }
  const g = readGrid();
  const unit = swingGestureItems.unit;
  const shift = unit * getParamNum('swingAmt') / 100;
  const totalBeats = (block.length || 1) * g.numerator;
  const touchedTracks = new Set();
  swingGestureItems.forEach(it => {
    const step = Math.round(it.origBeat / unit);
    const offBeat = step % 2 !== 0 && Math.abs(it.origBeat - step * unit) <= unit * 0.3;
    it.note.beat = offBeat ? Math.max(0, Math.min(totalBeats - 0.0001, it.origBeat + shift)) : it.origBeat;
    if (shift !== 0 && it.note.turnaround) delete it.note.turnaround;
    touchedTracks.add(it.trackId);
  });
  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));
  selectedNotes.clear();
  swingGestureItems.forEach(it => selectedNotes.add(it.trackId + '|' + it.note.beat));
  if (shift !== 0) touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid));
  swingGestureItems.forEach(it => { it.appliedBeat = it.note.beat; });
  block.generated = false;
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
}
wireKnobGestureUndo(['swingAmt'], swingRegen, 'Swing', toolBaselineApi(() => swingGestureItems, v => { swingGestureItems = v; }));

// Quantize strength: moves each selected note from where it was toward its Snap grid position by the chosen share; 100% equals Snap to Grid.
let quantizeGestureItems = null;
function quantizeRegen() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  const g = readGrid();
  const totalBeats = (block.length || 1) * g.numerator;
  if (quantizeGestureItems && !sliderBaselineValid(block, quantizeGestureItems)) quantizeGestureItems = null;
  if (!quantizeGestureItems) {
    if (!selectedNotes.size) return;
    const items = neResolveSelection(block);
    if (!items.length) return;
    quantizeGestureItems = items.map(it => ({
      trackId: it.trackId, note: it.note, origBeat: it.note.beat, appliedBeat: it.note.beat,
      target: Math.max(0, Math.min(totalBeats - 0.0001, neQuantizeForTrack(it.note.beat, g, block, it.trackId)))
    }));
  }
  const strength = getParamNum('quantizeAmt') / 100;
  const touchedTracks = new Set();
  quantizeGestureItems.forEach(it => {
    it.note.beat = it.origBeat + (it.target - it.origBeat) * strength;
    if (strength > 0 && it.note.turnaround) delete it.note.turnaround;
    touchedTracks.add(it.trackId);
  });
  touchedTracks.forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));
  selectedNotes.clear();
  quantizeGestureItems.forEach(it => selectedNotes.add(it.trackId + '|' + it.note.beat));
  if (strength > 0) touchedTracks.forEach(tid => markTrackManuallyEdited(block, tid));
  quantizeGestureItems.forEach(it => { it.appliedBeat = it.note.beat; });
  block.generated = false;
  rebuildFullFromEditedBlocks();
  renderNoteEditor();
}
wireKnobGestureUndo(['quantizeAmt'], quantizeRegen, 'Quantize', toolBaselineApi(() => quantizeGestureItems, v => { quantizeGestureItems = v; }));
['randomizeAmt', 'swingAmt', 'quantizeAmt'].forEach(id => wireMiniSliderWheel(document.getElementById(id)));

const REROLL_FNS = { kick: regenerateKickGroup, snare: regenerateSnareGroup, hihat: regenerateHihatGroup, ride: regenerateRideGroup, toms: regenerateTomsGroup, cymbals: regenerateCymbalsGroup };
async function rerollGroup(groupId) {
  const block = findBlock(activeBlockId);
  if (!block) return;
  if (block.lockedGroups && block.lockedGroups[groupId]) return;
  block.seeds = block.seeds || {};
  block.seeds[groupId] = Math.floor(Math.random()*1e9);
  await REROLL_FNS[groupId]();
}
Object.keys(REROLL_FNS).forEach(g => document.getElementById(g+'RerollBtn').addEventListener('click', () => rerollGroup(g)));
// Pattern mode letter next to the dice: a popup picks the mode, the letter shows the current one.
function paintPatternModeIcon(el, mode) {
  const info = PATTERN_MODE_INFO[mode] || PATTERN_MODE_INFO.classic;
  el.textContent = info.letter;
  el.classList.toggle('mode-on', mode !== 'classic' && !!PATTERN_MODE_INFO[mode]);
  el.dataset.tooltip = `Pattern mode: ${info.label}\n${info.tip} Click to change. Density stays exact in every mode.`;
}
// The pattern mode panel stays open while you pick modes and edit a formula. It closes with Esc, a click outside or a second click on its letter.
let patternModePanel = null;
function closePatternModePanel() {
  if (!patternModePanel) return;
  document.removeEventListener('keydown', patternModePanel.onKey, true);
  document.removeEventListener('mousedown', patternModePanel.onDown, true);
  patternModePanel.el.remove();
  patternModePanel = null;
}
// opts: anchor (the letter button), title, block, getMode(), getFormula(), onApply(mode, formula)
function openPatternModePanel(opts) {
  if (patternModePanel && patternModePanel.anchor === opts.anchor) { closePatternModePanel(); return; }
  closePatternModePanel();
  let mode = opts.getMode();
  let formula = opts.getFormula();
  const el = document.createElement('div');
  el.className = 'pattern-mode-panel';

  const head = document.createElement('div');
  head.className = 'pattern-mode-head';
  const title = document.createElement('span');
  title.textContent = 'Pattern mode: ' + opts.title;
  head.appendChild(title);
  el.appendChild(head);

  const list = document.createElement('div');
  list.className = 'pattern-mode-list';
  el.appendChild(list);

  const formulaBox = document.createElement('div');
  formulaBox.className = 'pattern-mode-formula';
  const formulaLabel = document.createElement('div');
  formulaLabel.textContent = 'Formula';
  const presetSelect = document.createElement('select');
  presetSelect.className = 'pattern-mode-preset';
  const presetHint = document.createElement('option');
  presetHint.value = '';
  presetHint.textContent = 'Insert preset...';
  presetSelect.appendChild(presetHint);
  PATTERN_FORMULA_PRESETS.forEach((p, k) => {
    const o = document.createElement('option');
    o.value = String(k);
    o.textContent = p.name;
    presetSelect.appendChild(o);
  });
  wireCustomSelect(presetSelect);   // the native list would not scale with the UI
  const input = document.createElement('input');
  input.type = 'text';
  input.className = 'pattern-mode-input';
  input.spellcheck = false;
  input.maxLength = FORMULA_MAX_LENGTH;
  input.value = formula;
  const status = document.createElement('div');
  status.className = 'pattern-mode-status';
  const help = document.createElement('div');
  help.className = 'pattern-mode-help';
  help.textContent = 'Every step gets a score; the highest scores get the hits. Names: i (step), n (steps), hits, s1 s2 s3 (seed, 0 to 1), r (random per step, 0 to 1), pi, bar and bars (bar in the phrase, phrase length). Add "; count" for the number of hits per bar, e.g. ; hits*(bar+1)/bars builds up over the phrase. Functions: sin cos abs floor frac sqrt min max mod, mandel(x,y), julia(zx,zy,cx,cy), logi(r,x,k). Operators: + - * / % ^';
  formulaBox.appendChild(formulaLabel);
  formulaBox.appendChild(presetSelect);
  formulaBox.appendChild(input);
  formulaBox.appendChild(status);
  formulaBox.appendChild(help);
  el.appendChild(formulaBox);

  const optButtons = {};
  const refresh = () => {
    PATTERN_MODES.forEach(m => optButtons[m].classList.toggle('active', m === mode));
    formulaBox.style.display = mode === 'formula' ? '' : 'none';
    const err = compileFormula(input.value).error;
    status.textContent = err || 'Formula OK';
    status.classList.toggle('error', !!err);
  };
  const apply = () => {
    if (findBlock(opts.block.id) !== opts.block) { closePatternModePanel(); return; }   // undo or redo replaced the block
    opts.onApply(mode, formula);
  };
  PATTERN_MODES.forEach(m => {
    const info = PATTERN_MODE_INFO[m];
    const b = document.createElement('button');
    b.className = 'pattern-mode-opt';
    b.textContent = info.letter + '  ' + info.label;
    b.dataset.tooltip = info.label + '\n' + info.tip;
    b.addEventListener('click', () => {
      if (m === mode) return;
      mode = m;
      if (m === 'formula' && !formula.trim()) { formula = DEFAULT_PATTERN_FORMULA; input.value = formula; }
      refresh();
      apply();
      if (m === 'formula') input.focus();
    });
    optButtons[m] = b;
    list.appendChild(b);
  });
  presetSelect.addEventListener('change', () => {
    const preset = PATTERN_FORMULA_PRESETS[+presetSelect.value];
    presetSelect.value = '';
    if (!preset) return;
    input.value = preset.formula;
    formula = preset.formula;
    refresh();
    apply();
  });
  input.addEventListener('input', refresh);
  input.addEventListener('change', () => { formula = input.value; refresh(); apply(); });
  input.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter') input.blur();
  });
  refresh();

  const onKey = (e) => { if (e.key === 'Escape') closePatternModePanel(); };
  document.addEventListener('keydown', onKey, true);
  // A click outside closes the panel; the click itself still reaches its target. The letter button toggles the panel on its own.
  const onDown = (e) => {
    if (el.contains(e.target) || opts.anchor.contains(e.target) || e.target.closest('.custom-select-list')) return;
    closePatternModePanel();
  };
  document.addEventListener('mousedown', onDown, true);
  document.body.appendChild(el);
  const rect = opts.anchor.getBoundingClientRect();
  const scale = currentUIScale();
  const w = el.offsetWidth * scale, h = el.offsetHeight * scale;   // the panel scales with the UI (transform), so clamp its scaled size
  el.style.left = Math.max(4, Math.min(rect.left, window.innerWidth - w - 4)) + 'px';
  el.style.top = Math.max(4, Math.min(rect.bottom + 2, window.innerHeight - h - 4)) + 'px';
  patternModePanel = { el, anchor: opts.anchor, blockId: opts.block.id, onKey, onDown };
}
function paintPatternModeIcon(el, mode) {
  const info = PATTERN_MODE_INFO[mode] || PATTERN_MODE_INFO.classic;
  el.textContent = info.letter;
  el.classList.toggle('mode-on', mode !== 'classic' && !!PATTERN_MODE_INFO[mode]);
  el.dataset.tooltip = 'Pattern mode: ' + info.label + '\n' + info.tip + ' Click to change. Density stays exact in every mode.';
}
// A letter button that opens the mode panel. cfg: title, block, getMode(), getFormula(), onApply(mode, formula) which runs inside the caller's own undo step.
function makePatternModeIcon(cfg) {
  const el = document.createElement('span');
  el.className = 'group-lock-icon pattern-mode-icon';
  el.style.cursor = 'pointer';
  paintPatternModeIcon(el, cfg.getMode());
  el.addEventListener('click', (e) => {
    e.stopPropagation();
    openPatternModePanel({
      anchor: el, title: cfg.title, block: cfg.block, getMode: cfg.getMode, getFormula: cfg.getFormula,
      onApply: (m, f) => { paintPatternModeIcon(el, m); cfg.onApply(m, f); },
    });
  });
  return el;
}
// Static groups: keeps seed and knobs, regenerates with the new distribution (a locked group only stores the mode).
const PATTERN_GROUP_TITLES = { kick: 'Kick', snare: 'Snare', hihat: 'Hi-Hat', ride: 'Ride', toms: 'Toms', cymbals: 'Cymbals' };
document.querySelectorAll('.pattern-mode-icon[data-group]').forEach(icon => icon.addEventListener('click', (e) => {
  e.stopPropagation();
  const groupId = icon.dataset.group;
  const block = findBlock(activeBlockId);
  if (!block) return;
  openPatternModePanel({
    anchor: icon, title: PATTERN_GROUP_TITLES[groupId], block,
    getMode: () => getPatternModeName(block, groupId),
    getFormula: () => getPatternFormula(block, groupId),
    onApply: async (m, f) => {
      paintPatternModeIcon(icon, m);
      const locked = block.lockedGroups && block.lockedGroups[groupId];
      await withBlockDataUndoAsync('Pattern mode ' + PATTERN_MODE_INFO[m].label + ' (' + blockDisplayName(block) + ')', block, async () => {
        setPatternSpec(block, groupId, m, f);
        if (!locked) await REROLL_FNS[groupId]({ suppressUndo: true });
      });
    },
  });
}));
// Reroll panel letter: the method the Block and All dice reroll with. Picking it only stores the method.
function refreshMainPatternModeIcon() {
  if (patternModePanel && patternModePanel.blockId !== activeBlockId) closePatternModePanel();
  const el = document.getElementById('mainPatternMode');
  if (!el) return;
  const block = findBlock(activeBlockId);
  el.style.display = (!block || isFillGeneratedBlock(block)) ? 'none' : '';
  if (block && !isFillGeneratedBlock(block)) paintPatternModeIcon(el, getMainPatternMethod(block).mode);
}
document.getElementById('mainPatternMode').addEventListener('click', (e) => {
  e.stopPropagation();
  const icon = e.currentTarget;
  const block = findBlock(activeBlockId);
  if (!block || isFillGeneratedBlock(block)) return;
  openPatternModePanel({
    anchor: icon, title: 'Reroll method', block,
    getMode: () => getPatternModeName(block, 'main'),
    getFormula: () => getPatternFormula(block, 'main'),
    onApply: (m, f) => {
      paintPatternModeIcon(icon, m);
      withBlockDataUndo('Reroll method ' + PATTERN_MODE_INFO[m].label + ' (' + blockDisplayName(block) + ')', block, () => {
        setPatternSpec(block, 'main', m, f);
      });
    },
  });
});
// Options > Show toolbar: off hides the generator controls (toolbar clusters, Genre, Feel, Fill/Break/instrument settings) for people who set notes by hand;
// The selection tools (Rnd, Swing, Nudge) live in the sidebar and stay. The choice is remembered like the other options.
const SHOW_TOOLBAR_KEY = 'dggShowToolbar';
const showToolbarToggle = document.getElementById('showToolbarToggle');
function applyShowToolbar() {
  const show = dggGetItem(SHOW_TOOLBAR_KEY) !== '0';
  showToolbarToggle.checked = show;
  document.getElementById('neEditingToolbar').classList.toggle('collapsed', !show);
  document.body.classList.toggle('ne-hand-mode', !show);
  if (!show) closePatternModePanel();
  window.dispatchEvent(new Event('resize'));
  // The rows area takes the freed (or lost) height; the grid lines have to continue down to the velocity lane at the new size.
  requestAnimationFrame(neExtendGridToBottom);
}
if (typeof dggOnReady === 'function') dggOnReady(applyShowToolbar); else applyShowToolbar();
showToolbarToggle.addEventListener('change', (e) => {
  dggSetItem(SHOW_TOOLBAR_KEY, e.target.checked ? '1' : '0');
  applyShowToolbar();
});
// Click feedback for every dice (static, Main, Custom and the Break/Fill/Turnaround rows) and pattern mode letter.
document.addEventListener('click', (e) => {
  const icon = e.target.closest && e.target.closest('.group-lock-icon');
  if (!icon) return;
  const cls = icon.classList.contains('pattern-mode-icon') ? 'mode-flash' : (icon.textContent.trim() === '🎲' ? 'dice-roll' : '');
  if (!cls) return;
  icon.classList.remove(cls);
  void icon.offsetWidth;   // restart the animation on rapid clicks
  icon.classList.add(cls);
  icon.addEventListener('animationend', () => icon.classList.remove(cls), { once: true });
}, true);
// Custom cluster: same gesture-undo wiring as the engine groups, plus its own reroll dice.
wireKnobGestureUndo(['customSubdivision','customDensity','customPhrase'], regenerateCustomGroup, 'Adjust Custom');
document.getElementById('customTrackSelect').addEventListener('change', () => {
  neSetActiveCustomTrack(document.getElementById('customTrackSelect').value);
  const block = findBlock(activeBlockId);
  const track = activeCustomTrack();
  if (block && track) { suppressKnobRegen = true; neApplyCustomKnobValues(block, track); suppressKnobRegen = false; }
  updateGroupLockUI();
});
document.getElementById('customRerollBtn').addEventListener('click', async () => {
  const track = activeCustomTrack();
  if (!track) return;
  const block = findBlock(activeBlockId);
  if (!block) return;
  const gid = customGroupId(track.id);
  if (block.lockedGroups && block.lockedGroups[gid]) return;   // locked — discard via 🔒 first
  block.seeds = block.seeds || {};
  block.seeds[gid] = Math.floor(Math.random()*1e9);
  await regenerateCustomGroup();
});
document.getElementById('rerollAllBtn').addEventListener('click', rerollActiveBlock);
document.getElementById('rerollAllBlocksBtn').addEventListener('click', rerollAllBlocksGlobal);

document.getElementById('playBtn').addEventListener('click', () => {
  const activeTab = document.querySelector('#mainTabBar .tab-btn.active')?.dataset.tab;
  if (activeTab === 'timeline' || activeTab === 'mixer') playFullSong(); else playActiveBlock();
});
document.getElementById('stopBtn').addEventListener('click', stopPlayback);

const tlZoomSlider = document.getElementById('tlZoomSlider');
const tlZoomVal = document.getElementById('tlZoomVal');
tlZoomSlider.addEventListener('input', () => { tlZoomVal.textContent = Math.round(+tlZoomSlider.value * 100) + '%'; });
tlZoomSlider.addEventListener('change', () => { tlZoom = +tlZoomSlider.value; renderTimeline(); });
wireMiniSliderWheel(tlZoomSlider);
function fitTimelineZoom() {
  const songEndBar = songEngine.songSequence.length ? Math.max(...songEngine.songSequence.map(s => songEngine.slotEndBar(s))) : 0;
  const selectedSlot = (selectedSegmentIdx != null) ? songEngine.songSequence[selectedSegmentIdx] : null;
  tlFitReferencePxPerBar = tlComputeFitPxPerBar(songEndBar);

  if (!selectedSlot || songEndBar <= 0) {
    tlZoom = 1;
    tlZoomSlider.value = String(tlZoom);
    tlZoomVal.textContent = '100%';
    renderTimeline();
    tlMain.scrollLeft = 0;
    return;
  }

  const selStartBar = selectedSlot.startBar;
  const selSpanBars = songEngine.slotSpanBars(selectedSlot);
  const marginBars = Math.max(1, selSpanBars * (10/80));
  const viewStartBar = Math.max(0, selStartBar - marginBars);
  const viewEndBar = Math.min(songEndBar, selStartBar + selSpanBars + marginBars);
  const viewBars = Math.max(0.5, viewEndBar - viewStartBar);

  tlZoom = Math.max(TL_ZOOM_MIN, Math.min(TL_ZOOM_MAX, songEndBar / viewBars));
  tlZoomSlider.value = String(tlZoom);
  tlZoomVal.textContent = Math.round(tlZoom * 100) + '%';
  renderTimeline();
  tlMain.scrollLeft = Math.max(0, viewStartBar * tlCurrentPxPerBar + tlLeadInPx);
}
document.getElementById('tlZoomFitBtn').addEventListener('click', fitTimelineZoom);

const tlMain = document.getElementById('tlMain');
tlMain.addEventListener('wheel', (e) => {
  if (!e.ctrlKey && !e.metaKey) return;
  e.preventDefault();
  const rect = tlMain.getBoundingClientRect();
  const cursorOffsetInViewport = e.clientX - rect.left;
  const barUnit = tlCurrentPxPerBar;
  const barPosAtCursor = (cursorOffsetInViewport + tlMain.scrollLeft) / barUnit;
  const dir = e.deltaY < 0 ? 1 : -1;
  const newZoom = Math.max(TL_ZOOM_MIN, Math.min(TL_ZOOM_MAX, tlZoom * (1 + dir * 0.1)));
  if (newZoom === tlZoom) return;
  tlZoom = newZoom;
  tlZoomSlider.value = String(tlZoom);
  tlZoomVal.textContent = Math.round(tlZoom * 100) + '%';
  renderTimeline();
  const newBarUnit = tlCurrentPxPerBar;
  tlMain.scrollLeft = Math.max(0, barPosAtCursor * newBarUnit - cursorOffsetInViewport);
}, { passive: false });

const neZoomSlider = document.getElementById('neZoomSlider');
const neZoomVal = document.getElementById('neZoomVal');
neZoomSlider.addEventListener('input', () => { neZoomVal.textContent = Math.round(neSliderToZoom(+neZoomSlider.value) * 100) + '%'; });
neZoomSlider.addEventListener('change', () => {
  const rowsCol = document.querySelector('.ne-rows');
  const oldScrollLeft = rowsCol ? rowsCol.scrollLeft : 0;
  const beatAtLeftEdge = Math.max(0, (oldScrollLeft - NE_LEFT_PAD) / (neCurrentPxPerBeat || 1));
  neZoom = neSliderToZoom(+neZoomSlider.value);
  neAutoFitZoomBlockId = null;
  renderNoteEditor();
  const freshRowsCol = document.querySelector('.ne-rows');
  if (freshRowsCol) freshRowsCol.scrollLeft = Math.max(0, beatAtLeftEdge * neCurrentPxPerBeat + NE_LEFT_PAD);
});
wireMiniSliderWheel(neZoomSlider);
document.getElementById('neZoomFitBtn').addEventListener('click', fitNoteEditorZoom);

function fitNoteEditorZoom() {
  const block = findBlock(activeBlockId);
  const items = block ? neResolveSelection(block) : [];

  if (!block || items.length === 0) {
    neZoom = 1;
    neAutoFitZoomBlockId = null;
    renderNoteEditor();
    const freshRowsCol = document.querySelector('.ne-rows');
    if (freshRowsCol) freshRowsCol.scrollLeft = 0;
    return;
  }

  const numerator = block.numerator;
  const blockLen = Math.max(1, block.length || 1);
  const blockBeats = blockLen * numerator;

  const beats = items.map(it => it.note.beat);
  const selMinBeat = Math.min(...beats);
  const selMaxBeat = Math.max(...beats);
  const selSpanBeats = Math.max(0.01, selMaxBeat - selMinBeat);
  const marginBeats = Math.max(1, selSpanBeats * (10/80));
  const viewStartBeat = Math.max(0, selMinBeat - marginBeats);
  const viewEndBeat = Math.min(blockBeats, selMaxBeat + marginBeats);
  const viewBeats = Math.max(0.5, viewEndBeat - viewStartBeat);

  neZoom = Math.max(NE_ZOOM_MIN, Math.min(neZoomMax(), blockBeats / viewBeats));
  neAutoFitZoomBlockId = null;
  renderNoteEditor();
  const freshRowsCol = document.querySelector('.ne-rows');
  if (freshRowsCol) freshRowsCol.scrollLeft = Math.max(0, viewStartBeat * neCurrentPxPerBeat + NE_LEFT_PAD);
}

let tlFittedOnce = false;
let neSidebarCollapsed = false;
function setNeSidebarCollapsed(collapsed) {
  neSidebarCollapsed = collapsed;
  document.getElementById('neLayout')?.classList.toggle('ne-sidebar-collapsed', collapsed);
  const btn = document.getElementById('neSidebarToggle');
  if (btn) { btn.textContent = collapsed ? '‹' : '›'; btn.dataset.tooltip = 'Sidebar\nHotkey Tab\nToggles the note-editor sidebar.'; }
  setTimeout(renderNoteEditor, 160);
}
document.getElementById('neSidebarToggle')?.addEventListener('click', () => setNeSidebarCollapsed(!neSidebarCollapsed));

function initTabs() {
  const bar = document.getElementById('mainTabBar');
  if (!bar) return;
  const buttons = [...bar.querySelectorAll('.tab-btn')];
  const blockPaletteWrap = document.getElementById('blockPaletteWrap');
  const blockPalette = document.getElementById('blockPalette');
  const blockPaletteTrack = document.getElementById('blockPaletteScrollTrack');
  const blockPaletteSep = document.querySelector('#blockPaletteWrap .toolbar-sep');
  const addBlockMenuWrap = document.getElementById('addBlockMenuWrap');
  const generateSongMenuWrap = document.getElementById('generateSongMenuWrap');
  const deOptionsMenuWrap = document.getElementById('deOptionsMenuWrap');
  const deCacheMenuWrap = document.getElementById('deCacheMenuWrap');
  const deParamTabs = document.getElementById('deParamTabs');
  const neSettingsMenuWrap = document.getElementById('neSettingsMenuWrap');
  const tlOptionsMenuWrap = document.getElementById('tlOptionsMenuWrap');
  const mxOptionsMenuWrap = document.getElementById('mxOptionsMenuWrap');
  const mxPresetMenuWrap = document.getElementById('mxPresetMenuWrap');
  const fileMenuWrap = document.getElementById('fileMenuWrap');
  const editMenuWrap = document.getElementById('editMenuWrap');
  const ACTIVE_TAB_KEY = 'dggActiveTab';
  function switchToTab(target) {
    const btn = buttons.find(b => b.dataset.tab === target);
    if (!btn) return;
      // Playback keeps running across tab switches within its own mode (Note Editor = block loop; Song Generator and Mixer = song).
      // Entering a tab of the other mode stops it; Drum Engine and How To belong to neither and stop it too.
      const tabPlayMode = target === 'noteEditor' ? 'block' : (target === 'timeline' || target === 'mixer') ? 'song' : null;
      if (playbackMode && playbackMode !== tabPlayMode) stopPlayback();
      // The Drum Engine has no playback of its own: no Play/Stop there, so it is never unclear what would play.
      for (const id of ['playBtn', 'stopBtn']) { const el = document.getElementById(id); if (el) el.style.display = (target === 'drumEngine') ? 'none' : ''; }
      buttons.forEach(b => b.classList.toggle('active', b === btn));
      document.querySelectorAll('.tab-panel').forEach(p => {
        p.classList.toggle('active', p.dataset.tabPanel === target);
      });
      refreshUndoRedoButtons();   // the undo buttons follow the history of the tab that is now active
      if (blockPaletteWrap) blockPaletteWrap.style.display = (target === 'howto') ? 'none' : '';
      // Mixer & Drum Engine tabs: keep the transport row (Play/Stop/master vol),
      // but hide the block chips so the tab content gets the space.
      const hidePaletteChips = target === 'mixer' || target === 'drumEngine';
      if (blockPalette) blockPalette.style.display = hidePaletteChips ? 'none' : '';
      if (blockPaletteTrack) blockPaletteTrack.style.display = hidePaletteChips ? 'none' : '';
      if (blockPaletteSep) blockPaletteSep.style.display = hidePaletteChips ? 'none' : '';
      if (target !== 'howto' && target !== 'drumEngine' && target !== 'mixer') requestAnimationFrame(updatePaletteScrollbar);
      if (addBlockMenuWrap) addBlockMenuWrap.style.display = (target === 'noteEditor') ? '' : 'none';
      if (generateSongMenuWrap) generateSongMenuWrap.style.display = (target === 'timeline') ? '' : 'none';
      if (deOptionsMenuWrap) deOptionsMenuWrap.style.display = (target === 'drumEngine') ? '' : 'none';
      if (deCacheMenuWrap) {
        const showCache = window.deCacheMenuShouldShow ? window.deCacheMenuShouldShow(target) : (target === 'drumEngine');
        deCacheMenuWrap.style.display = showCache ? '' : 'none';
      }
      // Instrument sub-tabs live in the shared transport row; only show them on the Drum Engine tab.
      if (deParamTabs) deParamTabs.style.display = (target === 'drumEngine') ? '' : 'none';
      if (neSettingsMenuWrap) neSettingsMenuWrap.style.display = (target === 'noteEditor') ? '' : 'none';
      if (tlOptionsMenuWrap) tlOptionsMenuWrap.style.display = (target === 'timeline') ? '' : 'none';
      if (mxOptionsMenuWrap) mxOptionsMenuWrap.style.display = (target === 'mixer') ? '' : 'none';
      const mxTimelineEl = document.getElementById('mxTimeline');
      if (mxTimelineEl) mxTimelineEl.style.display = (target === 'mixer') ? '' : 'none';
      if (blockPaletteWrap) blockPaletteWrap.classList.toggle('mx-timeline-on', target === 'mixer');
      const mxAutoAllBtn = document.getElementById('mxAutoAllBtn');
      if (mxAutoAllBtn) mxAutoAllBtn.style.display = (target === 'mixer') ? '' : 'none';
      if (target === 'mixer' && window.renderMixerTimeline) window.renderMixerTimeline();
      if (mxPresetMenuWrap) mxPresetMenuWrap.style.display = (target === 'mixer') ? '' : 'none';
      if (fileMenuWrap) fileMenuWrap.style.display = (target === 'howto' || target === 'drumEngine') ? 'none' : '';
      if (editMenuWrap) editMenuWrap.style.display = (target === 'howto' || target === 'drumEngine' || target === 'mixer') ? 'none' : '';
      document.querySelectorAll('.edit-menu-ne-only').forEach(el => {
        el.style.display = (target === 'noteEditor') ? '' : 'none';
      });
      if (target === 'timeline' && !tlFittedOnce) {
        tlFittedOnce = true;
        requestAnimationFrame(() => fitTimelineZoom());
      }
      if (target === 'noteEditor') renderNoteEditor();
      if (target === 'drumEngine') {
        if (window.initDrumEngineTab) window.initDrumEngineTab();
        if (window.deLayoutDrumEngineKeyboard) window.deLayoutDrumEngineKeyboard();
      }
    dggSetItem(ACTIVE_TAB_KEY, target);
  }
  buttons.forEach(btn => {
    btn.addEventListener('click', () => switchToTab(btn.dataset.tab));
  });
  const savedTab = dggGetItem(ACTIVE_TAB_KEY);
  if (savedTab && buttons.some(b => b.dataset.tab === savedTab)) {
    switchToTab(savedTab);
  }
}

let tlPanDrag = null;
tlMain.addEventListener('pointerdown', (e) => {
  if (e.button !== 1) return;
  e.preventDefault();
  tlPanDrag = { startX: e.clientX, startScroll: tlMain.scrollLeft };
  tlMain.setPointerCapture(e.pointerId);
  tlMain.style.cursor = 'grabbing';
});
tlMain.addEventListener('pointermove', (e) => {
  if (!tlPanDrag) return;
  tlMain.scrollLeft = tlPanDrag.startScroll - (e.clientX - tlPanDrag.startX);
});
const endTlPan = (e) => { if (!tlPanDrag) return; tlPanDrag = null; tlMain.style.cursor = ''; try { tlMain.releasePointerCapture(e.pointerId); } catch(_){} };
tlMain.addEventListener('pointerup', endTlPan);
tlMain.addEventListener('pointercancel', endTlPan);
tlMain.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });

// =========================================
// UI WIRING — EXPORT BUTTONS
// =========================================
document.getElementById('exportSongBtn').addEventListener('click', async () => {
  if (!fullPattern) await generateSequence();
  midiExporter.exportSong(fullPattern, readParams(), readGrid());
});
document.getElementById('exportBlocksBtn').addEventListener('click', () => {
  midiExporter.exportAllBlocks(blocksList, readParams(), readGrid());
});

document.getElementById('snapToGridBtn').addEventListener('click', () => {
  const block = findBlock(activeBlockId);
  if (!block || !selectedNotes.size) return;
  const g = readGrid();
  const count = selectedNotes.size;
  const label = count > 1 ? `Snap ${count} notes to grid` : 'Snap note to grid';
  let changed = false;
  withBlockDataUndo(label, block, () => { changed = neSnapSelectionToGrid(block, g); });
  if (!changed) { undoStack().undo.pop(); refreshUndoRedoButtons(); }
});

function eligibleNoteKeys(block) {
  const keys = [];
  if (!block) return keys;
  const isBreak = isBreakBlock(block);
  const breakLocked = isBreak && !block.breakUnlocked;
  const blockBeats = Math.max(1, block.length || 1) * block.numerator;
  const grooveBeats = breakLocked ? (blockBeats - (block.breakBeats ?? BREAK_DEFAULT_BEATS)) : 0;
  trackList.forEach(t => (block.data[t.id]||[]).forEach(note => {
    if (breakLocked && note.beat < grooveBeats) return;
    keys.push(t.id+'|'+note.beat);
  }));
  return keys;
}
function selectAllNotes() {
  const block = findBlock(activeBlockId);
  selectedNotes.clear();
  eligibleNoteKeys(block).forEach(k => selectedNotes.add(k));
  renderNoteEditor();
}
function selectNoneNotes() {
  selectedNotes.clear();
  renderNoteEditor();
}
function invertNoteSelection() {
  const block = findBlock(activeBlockId);
  const eligible = eligibleNoteKeys(block);
  const wasSelected = new Set(selectedNotes);
  selectedNotes.clear();
  eligible.forEach(k => { if (!wasSelected.has(k)) selectedNotes.add(k); });
  renderNoteEditor();
}

document.addEventListener('keydown', e => {
  if (e.code==='Space' && e.target.tagName!=='INPUT' && e.target.tagName!=='SELECT' && document.querySelector('#mainTabBar .tab-btn.active')?.dataset.tab !== 'drumEngine') {
    e.preventDefault(); if (playing) stopPlayback(); else document.getElementById('playBtn').click();
  }
  const inField = e.target.tagName==='INPUT' || e.target.tagName==='TEXTAREA' || e.target.tagName==='SELECT';
  if (e.key === 'Tab' && !inField) {
    const noteEditorActive = document.querySelector('.tab-panel[data-tab-panel="noteEditor"]')?.classList.contains('active');
    if (noteEditorActive) {
      e.preventDefault();
      setNeSidebarCollapsed(!neSidebarCollapsed);
    }
  }
  if (e.key === 'f' && !inField) {
    const timelineActive = document.querySelector('.tab-panel[data-tab-panel="timeline"]')?.classList.contains('active');
    const noteEditorActiveForFit = document.querySelector('.tab-panel[data-tab-panel="noteEditor"]')?.classList.contains('active');
    if (timelineActive) {
      e.preventDefault();
      fitTimelineZoom();
    } else if (noteEditorActiveForFit) {
      e.preventDefault();
      fitNoteEditorZoom();
    }
  }
  if ((e.ctrlKey||e.metaKey) && e.code==='KeyA' && !inField) {
    e.preventDefault();
    if (isTimelineTabActive()) tlSelectAll();
    else selectAllNotes();
  }
  if (e.altKey && e.code==='KeyA' && !inField) {
    e.preventDefault();
    if (isTimelineTabActive()) tlSelectNone();
    else selectNoneNotes();
  }
  if ((e.ctrlKey||e.metaKey) && e.code==='KeyI' && !inField) {
    e.preventDefault();
    if (isTimelineTabActive()) tlInvertSelection();
    else invertNoteSelection();
  }
  if ((e.key === 'Delete' || e.key === 'Backspace') && !inField && selectedNotes.size > 0) {
    e.preventDefault();
    const block = findBlock(activeBlockId);
    const keys = [...selectedNotes];
    const label = keys.length > 1 ? `Delete ${keys.length} notes` : 'Delete note';
    neDeleteNoteKeys(block, keys, label);
  }
  if ((e.key === 'Delete' || e.key === 'Backspace') && !inField && tlSelectedIndices.size > 0) {
    const timelineActiveForDelete = document.querySelector('.tab-panel[data-tab-panel="timeline"]')?.classList.contains('active');
    if (timelineActiveForDelete) {
      e.preventDefault();
      tlDeleteSlots(tlSelectedIndices);
    }
  }
  if ((e.ctrlKey||e.metaKey) && e.code==='KeyC' && !inField && selectedNotes.size > 0) {
    e.preventDefault();
    doCopySelection();
  }
  if ((e.ctrlKey||e.metaKey) && e.code==='KeyX' && !inField && selectedNotes.size > 0) {
    e.preventDefault();
    doCutSelection();
  }
  if ((e.ctrlKey||e.metaKey) && e.code==='KeyV' && !inField && noteClipboard) {
    e.preventDefault();
    doPasteClipboard();
  }
  // A <select>, slider or checkbox keeps focus after use but has no text to undo, so the shortcut must still reach the app.
  const inTextField = inField && e.target.tagName !== 'SELECT'
    && !(e.target.tagName === 'INPUT' && ['range', 'checkbox', 'radio', 'button'].includes(e.target.type));
  if ((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='z' && !inTextField) {
    e.preventDefault();
    const onMixer = document.querySelector('#mainTabBar .tab-btn.active')?.dataset.tab === 'mixer';
    if (e.shiftKey) performRedo();
    else if (!(onMixer && window.mxUndoAuto && window.mxUndoAuto())) performUndo();
  }
  if ((e.ctrlKey||e.metaKey) && e.key.toLowerCase()==='y' && !inTextField) {
    e.preventDefault();
    performRedo();
  }
  const isArrow = e.key==='ArrowLeft' || e.key==='ArrowRight' || e.key==='ArrowUp' || e.key==='ArrowDown';
  if (isArrow && !inField && selectedNotes.size > 0) {
    e.preventDefault();
    const block = findBlock(activeBlockId);
    if (block) {
      if (!e.repeat) neArrowMoveSnapshot = { blockId: block.id, before: JSON.stringify(block.data), beforeLocked: snapshotLocked(block) };
      const g = readGrid();
      let changed;
      if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
        const step = g.snap * (e.shiftKey ? 4 : 1);
        changed = neMoveSelectedNotesTime(block, g, e.key==='ArrowLeft' ? -step : step);
      } else {
        changed = neMoveSelectedNotesArticulation(block, g, e.key==='ArrowUp' ? -1 : 1);
      }
      if (changed) { rebuildFullFromEditedBlocks(); renderNoteEditor(); }
    }
  }
});

let neArrowMoveSnapshot = null;
document.addEventListener('keyup', e => {
  const isArrow = e.key==='ArrowLeft' || e.key==='ArrowRight' || e.key==='ArrowUp' || e.key==='ArrowDown';
  if (!isArrow || !neArrowMoveSnapshot) return;
  const snap = neArrowMoveSnapshot;
  neArrowMoveSnapshot = null;
  const block = findBlock(snap.blockId);
  if (!block) return;
  const after = JSON.stringify(block.data);
  if (after === snap.before) return;
  const afterLocked = snapshotLocked(block);
  pushUndo('Move notes',
    () => { block.data = JSON.parse(snap.before); restoreLocked(block, snap.beforeLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); },
    () => { block.data = JSON.parse(after); restoreLocked(block, afterLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); }
  );
});

const ENERGY_KNOB_IDS = ['kickDensity','snareDensity','hihatDensity','rideDensity','tomsDensity','cymbalsDensity'];
const ENERGY_REGEN_BY_ID = { kickDensity: () => regenerateKickGroup({ suppressUndo: true }), snareDensity: () => regenerateSnareGroup({ suppressUndo: true }),
  hihatDensity: () => regenerateHihatGroup({ suppressUndo: true }), rideDensity: () => regenerateRideGroup({ suppressUndo: true }),
  tomsDensity: () => regenerateTomsGroup({ suppressUndo: true }), cymbalsDensity: () => regenerateCymbalsGroup({ suppressUndo: true }) };

let energyMacroGesture = null;

function beginEnergyMacroGesture() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  energyMacroGesture = {
    blockId: block.id,
    startVals: Object.fromEntries(ENERGY_KNOB_IDS.map(id => [id, +document.getElementById(id).value])),
    beforeData: JSON.stringify(block.data),
    beforeKnobs: JSON.stringify(block.knobs || {}),
  };
  suppressKnobRegen = true;
}

function applyEnergyMacro(draggedId, draggedVal) {
  if (!energyMacroGesture || energyMacroGesture.blockId !== activeBlockId) return;
  const startDragged = energyMacroGesture.startVals[draggedId];
  if (!(startDragged > 0)) return;
  const factor = draggedVal / startDragged;
  ENERGY_KNOB_IDS.forEach(id => {
    if (id === draggedId) return;
    const start = energyMacroGesture.startVals[id];
    if (!(start > 0)) return;
    const el = document.getElementById(id);
    const lo = parseFloat(el.min) || 0, hi = parseFloat(el.max) || 100;
    const next = Math.min(hi, Math.max(lo, Math.round(start * factor)));
    if (+el.value !== next) {
      el.value = next; el.dispatchEvent(new Event('input'));
      const lab = document.getElementById(id + 'Val'); if (lab) lab.textContent = next + '%';
    }
  });
}

async function endEnergyMacroGesture() {
  if (!energyMacroGesture) return;
  suppressKnobRegen = false;
  const g = energyMacroGesture; energyMacroGesture = null;
  const block = findBlock(g.blockId);
  if (!block) return;
  await Promise.all(ENERGY_KNOB_IDS
    .filter(id => +document.getElementById(id).value !== g.startVals[id])
    .map(id => ENERGY_REGEN_BY_ID[id]()));
  const afterData = JSON.stringify(block.data);
  const afterKnobs = JSON.stringify(block.knobs || {});
  if (afterData === g.beforeData && afterKnobs === g.beforeKnobs) return;
  const restore = (dataStr, knobsStr) => {
    block.data = JSON.parse(dataStr); block.knobs = JSON.parse(knobsStr); block.generated = false;
    rebuildFullFromEditedBlocks(); renderNoteEditor(); syncKnobsToBlock(block);
  };
  pushUndo('Energy Macro',
    () => restore(g.beforeData, g.beforeKnobs),
    () => restore(afterData, afterKnobs)
  );
}

function wireKnob(slider) {
  const ctrlBox = slider.closest('.ctrl'); if (!ctrlBox) return;
  if (ctrlBox.querySelector('.knob-visual')) return;
  const label = ctrlBox.querySelector('.ctrl-label'); const valSpan = label ? label.querySelector('.val') : null;
  if (valSpan) slider.after(valSpan);
  const visual = document.createElement('div'); visual.className='knob-visual';
  const marker = document.createElement('div'); marker.className='knob-marker';
  const dot = document.createElement('div'); dot.className='marker-dot'; marker.appendChild(dot);
  const tickStart = document.createElement('div'); tickStart.className='knob-tick start';
  const tickEnd = document.createElement('div'); tickEnd.className='knob-tick end';
  visual.appendChild(tickStart); visual.appendChild(tickEnd); visual.appendChild(marker);
  const editInput = document.createElement('input'); editInput.type='text'; editInput.className='knob-input'; editInput.value=slider.value;
  slider.parentNode.insertBefore(visual, slider); ctrlBox.appendChild(editInput);

  const updateRotation = () => { const min=+slider.min||0,max=+slider.max||100,val=+slider.value;
    const percent = Math.min(1,Math.max(0,(val-min)/(max-min)));
    marker.style.transform = `rotate(${(percent*270)-135}deg)`; editInput.value = slider.value; };
  updateRotation(); slider.addEventListener('input', updateRotation);

  let dragging=false,startY=0,startVal=0;
  const PPR=120;
  const onMove=(e)=>{ if(!dragging) return; e.preventDefault();
    const step=parseFloat(slider.step)||1, minAttr=parseFloat(slider.min)||0, maxAttr=parseFloat(slider.max)||100;
    const deltaY=startY-e.clientY; const range=maxAttr-minAttr;
    const rawVal=startVal+(deltaY/PPR)*range; const stepped=Math.round(rawVal/step)*step; const clamped=Math.min(maxAttr,Math.max(minAttr,stepped));
    if (+slider.value!==clamped){ slider.value=clamped; slider.dispatchEvent(new Event('input'));
      if (energyMacroGesture) applyEnergyMacro(slider.id, clamped); } };
  const endDrag=async(e)=>{ if(!dragging) return; dragging=false; ctrlBox.classList.remove('active'); document.body.classList.remove('knob-dragging');
    try{ visual.releasePointerCapture(e.pointerId); }catch(_){}
    await endEnergyMacroGesture();
    slider.dispatchEvent(new Event('change'));
    window.removeEventListener('pointermove',onMove); window.removeEventListener('pointerup',endDrag); window.removeEventListener('pointercancel',endDrag); };
  visual.addEventListener('pointerdown',(e)=>{ e.preventDefault(); if (slider.disabled) return;
    // Ctrl+Click → Reset to default value (audio standard convention).
    if ((e.ctrlKey || e.metaKey) && ctrlBox.dataset.def != null) { slider.value = ctrlBox.dataset.def; slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change')); return; }
    dragging=true; startY=e.clientY; startVal=+slider.value;
    ctrlBox.classList.add('active'); document.body.classList.add('knob-dragging'); try{ visual.setPointerCapture(e.pointerId); }catch(_){}
    if (e.shiftKey && ENERGY_KNOB_IDS.includes(slider.id)) beginEnergyMacroGesture();
    window.addEventListener('pointermove',onMove); window.addEventListener('pointerup',endDrag); window.addEventListener('pointercancel',endDrag); });
  let wheelChangeTimer = null;
  const onWheel=(e)=>{ if (slider.disabled) return; e.preventDefault(); e.stopPropagation(); const s=parseFloat(slider.step)||1, lo=parseFloat(slider.min)||0, hi=parseFloat(slider.max)||100;
    const dir=e.deltaY<0?1:-1; const nv=Math.min(hi,Math.max(lo,parseFloat(slider.value)+dir*s));
    if (+slider.value!==nv){ slider.value=nv; slider.dispatchEvent(new Event('input'));
      clearTimeout(wheelChangeTimer);
      wheelChangeTimer = setTimeout(() => slider.dispatchEvent(new Event('change')), 220);
    } };
  visual.addEventListener('wheel',onWheel,{passive:false}); ctrlBox.addEventListener('wheel',onWheel,{passive:false});
  if (valSpan) valSpan.addEventListener('dblclick',(e)=>{ e.stopPropagation(); valSpan.style.visibility='hidden'; editInput.style.display='block'; editInput.focus(); editInput.select(); });
  const saveAndClose=()=>{ const nv=parseFloat(editInput.value); const lo=parseFloat(slider.min)||0, hi=parseFloat(slider.max)||100;
    if (!isNaN(nv)){ slider.value=Math.min(hi,Math.max(lo,nv)); slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change')); }
    else editInput.value=slider.value;
    editInput.style.display='none'; if (valSpan) valSpan.style.visibility='visible'; updateRotation(); };
  editInput.addEventListener('keydown',(e)=>{ if(e.key==='Enter'){e.preventDefault();saveAndClose();} if(e.key==='Escape'){editInput.value=slider.value;saveAndClose();} });
  editInput.addEventListener('blur', saveAndClose);
}

function wireMiniSliderWheel(slider) {
  if (!slider || slider._miniWheelWired) return;
  slider._miniWheelWired = true;
  slider.addEventListener('wheel', (e) => {
    if (slider.disabled) return;
    e.preventDefault(); e.stopPropagation();
    const step = parseFloat(slider.step) || 1, lo = parseFloat(slider.min) || 0, hi = parseFloat(slider.max) || 100;
    const dir = e.deltaY < 0 ? 1 : -1;
    const nv = Math.min(hi, Math.max(lo, Math.round((parseFloat(slider.value) + dir*step) / step) * step));
    if (+slider.value !== nv) { slider.value = nv; slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change')); }
  }, { passive: false });
}

function initKnobs() {
  const sliders = document.querySelectorAll('input[type="range"]:not(#tlZoomSlider):not(.ne-mini-slider):not(.ne-vel-mini-slider)');
  sliders.forEach(wireKnob);
}

function updateNudgeDisplay() {
  const val = document.getElementById('nudgeVal');
  if (val) val.textContent = (neNudgeSteps > 0 ? '+' : '') + neNudgeSteps;
}
function wireNudgeButtons() {
  const back = document.getElementById('nudgeBackBtn');
  const fwd = document.getElementById('nudgeFwdBtn');
  if (!back || !fwd) return;
  const nudge = (dir) => {
    const block = findBlock(activeBlockId);
    if (!block) return;
    if (!selectedNotes.size) { const st = document.getElementById('statusText'); if (st) st.textContent = 'Select a note first to nudge it'; return; }
    const before = JSON.stringify(block.data);
    const beforeLocked = snapshotLocked(block);
    if (!neNudgeSelection(block, readGrid(), dir)) return;
    neNudgeSteps += dir;
    updateNudgeDisplay();
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    const after = JSON.stringify(block.data);
    const afterLocked = snapshotLocked(block);
    pushUndo(`Nudge ${dir > 0 ? '+1' : '-1'}`,
      () => { block.data = JSON.parse(before); restoreLocked(block, beforeLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); },
      () => { block.data = JSON.parse(after); restoreLocked(block, afterLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); }
    );
  };
  back.addEventListener('click', () => nudge(-1));
  fwd.addEventListener('click', () => nudge(1));
}

// =========================================
// INIT
// =========================================
(function labelizeSubdivisionKnobs() {
  [['kickSubdivision','kickSubdivisionVal'], ['snareSubdivision','snareSubdivisionVal'], ['hihatSubdivision','hihatSubdivisionVal'], ['rideSubdivision','rideSubdivisionVal'], ['tomsSubdivision','tomsSubdivisionVal'], ['cymbalsSubdivision','cymbalsSubdivisionVal']].forEach(([sliderId, valId]) => {
    const slider = document.getElementById(sliderId);
    const valSpan = document.getElementById(valId);
    if (!slider || !valSpan) return;
    const update = () => { valSpan.textContent = SUBDIVISION_KNOB_LABELS[+slider.value] || slider.value; };
    slider.addEventListener('input', update);
    update();
  });
})();

// =========================================
// CUSTOM TOOLTIPS
// =========================================
(function initTooltips() {
  const SHOW_DELAY_MS = 900;
  const OFFSET_X = 14;
  const OFFSET_Y = 18;
  const EDGE_PAD = 8;

  let el = null;
  let showTimer = null;
  let currentTarget = null;
  // The note editor's working area (grid, velocity lane, rulers) never shows tooltips: they cover what is being edited.
  const WORKSPACE_SELECTOR = '.ne-rows, .ne-vel-lane, .ne-ruler, .ne-loop-ruler';
  let buttonDown = false; // no tooltip while a mouse button is held (a drag in progress)

  function ensureEl() {
    if (!el) {
      el = document.createElement('div');
      el.className = 'dgg-tooltip';
      document.body.appendChild(el);
    }
    return el;
  }

  function position(x, y) {
    const tip = ensureEl();
    const vw = window.innerWidth, vh = window.innerHeight;
    let left = x + OFFSET_X;
    let top = y + OFFSET_Y;
    const rect = tip.getBoundingClientRect();
    let originX = 'left', originY = 'top';
    if (left + rect.width + EDGE_PAD > vw) { left = x - rect.width - OFFSET_X; originX = 'right'; }
    if (top + rect.height + EDGE_PAD > vh) { top = y - rect.height - OFFSET_Y; originY = 'bottom'; }
    left = Math.max(EDGE_PAD, left);
    top = Math.max(EDGE_PAD, top);
    tip.style.left = left + 'px';
    tip.style.top = top + 'px';
    tip.style.transformOrigin = originY + ' ' + originX;
  }

  function show(target, x, y) {
    const text = target.getAttribute('data-tooltip');
    if (!text) return;
    const tip = ensureEl();
    tip.textContent = text;
    tip.style.display = 'block';
    position(x, y);
  }

  function hide() {
    if (el) el.style.display = 'none';
    clearTimeout(showTimer);
    currentTarget = null;
  }

  document.addEventListener('mouseover', (e) => {
    if (!tooltipsEnabled() || buttonDown) return;
    const target = e.target.closest('[data-tooltip]');
    if (!target || target === currentTarget) return;
    if (target.closest(WORKSPACE_SELECTOR)) { hide(); return; }
    hide();
    currentTarget = target;
    const x = e.clientX, y = e.clientY;
    showTimer = setTimeout(() => {
      if (currentTarget === target && !buttonDown) show(target, x, y);
    }, SHOW_DELAY_MS);
  });

  document.addEventListener('mouseout', (e) => {
    const target = e.target.closest('[data-tooltip]');
    if (!target || target !== currentTarget) return;
    if (e.relatedTarget && target.contains(e.relatedTarget)) return;
    hide();
  });

  document.addEventListener('mousedown', hide, { capture: true });
  document.addEventListener('pointerdown', () => { buttonDown = true; hide(); }, { capture: true });
  const buttonUp = () => { buttonDown = false; };
  document.addEventListener('pointerup', buttonUp, { capture: true });
  document.addEventListener('pointercancel', buttonUp, { capture: true });
  window.addEventListener('blur', buttonUp);
  document.addEventListener('scroll', hide, { capture: true, passive: true });
})();

const TOOLTIPS_ENABLED_KEY = 'dggTooltipsEnabled';
const TOOLTIPS_TOGGLE_IDS = ['tooltipsToggle', 'tooltipsToggleSG', 'tooltipsToggleDE', 'tooltipsToggleMixer'];
const tooltipsToggles = TOOLTIPS_TOGGLE_IDS.map(id => document.getElementById(id)).filter(Boolean);
function tooltipsEnabled() { return dggGetItem(TOOLTIPS_ENABLED_KEY) !== '0'; }
function syncTooltipsToggles() {
  const enabled = tooltipsEnabled();
  tooltipsToggles.forEach(t => { t.checked = enabled; });
}
if (typeof dggOnReady === 'function') {
  dggOnReady(syncTooltipsToggles);
} else {
  syncTooltipsToggles();
}
tooltipsToggles.forEach(t => {
  t.addEventListener('change', (e) => {
    dggSetItem(TOOLTIPS_ENABLED_KEY, e.target.checked ? '1' : '0');
    syncTooltipsToggles();
  });
});

// Developer tools (right-click > Inspect). On by default while debugging (set back to off for a release): only an explicit
// "off" is remembered. The flag lives in plain localStorage, not in the per-instance dgg* storage, so it survives restarts of
// the host and applies to every instance.
const DEVTOOLS_ENABLED_KEY = 'dggDevToolsEnabled.v2';   // new key: the old one stored "0" on every start and would override the default
const DEVTOOLS_TOGGLE_IDS = ['devToolsToggle', 'devToolsToggleSG', 'devToolsToggleDE', 'devToolsToggleMixer'];
const devToolsToggles = DEVTOOLS_TOGGLE_IDS.map(id => document.getElementById(id)).filter(Boolean);
function devToolsEnabled() {
  try { return localStorage.getItem(DEVTOOLS_ENABLED_KEY) !== '0'; } catch (e) { return true; }
}
function applyDevTools(enabled) {
  try { localStorage.setItem(DEVTOOLS_ENABLED_KEY, enabled ? '1' : '0'); } catch (e) {}
  devToolsToggles.forEach(t => { t.checked = enabled; });
  hostBridge.send('setDevTools', { enabled });
}
devToolsToggles.forEach(t => {
  t.addEventListener('change', (e) => applyDevTools(e.target.checked));
});
applyDevTools(devToolsEnabled());

// Render cached samples (offline render / song export): on = cached samples like in the DAW, off = live physics synthesis (default).
// Same storage as the developer tools flag: plain localStorage, machine-wide; the host gets the value at every start.
const RENDER_CACHED_KEY = 'dggRenderCachedSamples.v2';
const renderCachedToggles = ['renderCachedToggle', 'renderCachedToggleSG', 'renderCachedToggleDE', 'renderCachedToggleMixer']
  .map(id => document.getElementById(id)).filter(Boolean);
function renderCachedEnabled() {
  try { return localStorage.getItem(RENDER_CACHED_KEY) === '1'; } catch (e) { return false; }
}
function applyRenderCached(enabled) {
  try { localStorage.setItem(RENDER_CACHED_KEY, enabled ? '1' : '0'); } catch (e) {}
  renderCachedToggles.forEach(t => { t.checked = enabled; });
  hostBridge.send('setRenderCachedSamples', { enabled });
}
renderCachedToggles.forEach(t => {
  t.addEventListener('change', (e) => applyRenderCached(e.target.checked));
});
applyRenderCached(renderCachedEnabled());

window.dggShowValueTip = function(text, x, y) {
  let tip = document.getElementById('dggValueTip');
  if (!tip) {
    tip = document.createElement('div');
    tip.id = 'dggValueTip';
    tip.className = 'dgg-tooltip dgg-value-tip';
    document.body.appendChild(tip);
  }
  tip.textContent = text;
  tip.style.display = 'block';
  tip.style.left = (x + 14) + 'px';
  tip.style.top = (y - 28) + 'px';
};
window.dggHideValueTip = function() {
  const tip = document.getElementById('dggValueTip');
  if (tip) tip.style.display = 'none';
};

function updatePaletteScrollbar() {
  const pal = document.getElementById('blockPalette');
  const track = document.getElementById('blockPaletteScrollTrack');
  const thumb = document.getElementById('blockPaletteScrollThumb');
  if (!pal || !track || !thumb) return;
  track.style.left = pal.offsetLeft + 'px';
  track.style.width = pal.offsetWidth + 'px';
  const overflow = pal.scrollWidth - pal.clientWidth;
  if (overflow <= 1) { track.classList.remove('visible'); return; }
  track.classList.add('visible');
  const trackWidth = track.clientWidth;
  const thumbWidth = Math.max(30, trackWidth * (pal.clientWidth / pal.scrollWidth));
  const thumbLeft = (trackWidth - thumbWidth) * (pal.scrollLeft / overflow);
  thumb.style.width = thumbWidth + 'px';
  thumb.style.left = thumbLeft + 'px';
}

(function initPaletteScroll() {
  const pal = document.getElementById('blockPalette');
  const track = document.getElementById('blockPaletteScrollTrack');
  const thumb = document.getElementById('blockPaletteScrollThumb');
  if (!pal || !track || !thumb) return;

  pal.addEventListener('wheel', (e) => {
    if (pal.scrollWidth <= pal.clientWidth) return;
    e.preventDefault();
    pal.scrollLeft += (e.deltaY !== 0 ? e.deltaY : e.deltaX);
  }, { passive: false });

  let pan = null;
  pal.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || e.target !== pal) return;
    pan = { startX: e.clientX, startScroll: pal.scrollLeft };
    pal.setPointerCapture(e.pointerId);
  });
  pal.addEventListener('pointermove', (e) => {
    if (!pan) return;
    pal.scrollLeft = pan.startScroll - (e.clientX - pan.startX);
  });
  const endPan = () => { pan = null; };
  pal.addEventListener('pointerup', endPan);
  pal.addEventListener('lostpointercapture', endPan);

  pal.addEventListener('scroll', updatePaletteScrollbar);
  window.addEventListener('resize', updatePaletteScrollbar);

  let thumbDrag = null;
  thumb.addEventListener('pointerdown', (e) => {
    e.stopPropagation();
    thumbDrag = { startX: e.clientX, startLeft: thumb.offsetLeft };
    thumb.classList.add('dragging');
    thumb.setPointerCapture(e.pointerId);
  });
  thumb.addEventListener('pointermove', (e) => {
    if (!thumbDrag) return;
    const trackWidth = track.clientWidth;
    const thumbWidth = thumb.offsetWidth;
    const overflow = pal.scrollWidth - pal.clientWidth;
    let newLeft = thumbDrag.startLeft + (e.clientX - thumbDrag.startX);
    newLeft = Math.max(0, Math.min(trackWidth - thumbWidth, newLeft));
    pal.scrollLeft = overflow * (newLeft / (trackWidth - thumbWidth));
  });
  const endThumbDrag = () => { thumbDrag = null; thumb.classList.remove('dragging'); };
  thumb.addEventListener('pointerup', endThumbDrag);
  thumb.addEventListener('lostpointercapture', endThumbDrag);

  track.addEventListener('pointerdown', (e) => {
    if (e.target !== track) return;
    const rect = track.getBoundingClientRect();
    const thumbWidth = thumb.offsetWidth;
    const trackWidth = track.clientWidth;
    const clickX = Math.max(0, Math.min(trackWidth - thumbWidth, e.clientX - rect.left - thumbWidth / 2));
    const overflow = pal.scrollWidth - pal.clientWidth;
    pal.scrollLeft = overflow * (clickX / (trackWidth - thumbWidth));
  });

  updatePaletteScrollbar();
})();
// One-off result messages in the status text (top right) fall back to the transport state after a few seconds.
// Ongoing states (baking, auto gain, exporting, saving/loading) are not in the list and stay until their own code changes them.
(function initStatusAutoReset() {
  const el = document.getElementById('statusText');
  if (!el) return;
  const TRANSIENT = /^(New project started|Project saved|Project loaded|MIDI Song exported|No blocks to export|Exported |Export complete|Export failed|Drag export failed)/;
  const RESET_MS = 4000;
  let timer = null;
  new MutationObserver(() => {
    clearTimeout(timer);
    const text = el.textContent;
    if (!TRANSIENT.test(text)) return;
    timer = setTimeout(() => {
      if (el.textContent === text) el.textContent = (typeof playing !== 'undefined' && playing) ? 'playing…' : 'ready';
    }, RESET_MS);
  }).observe(el, { childList: true, characterData: true, subtree: true });
})();

// Shortcut panel: the ? button next to Mute (also ? and F1) opens a floating list of the keys and gestures of the active tab.
// The lists come from howto-content.js (dggHotkeysHtml), the same source as the How To tab.
(function initHotkeysPanel() {
  const btn = document.getElementById('hotkeysBtn');
  if (!btn) return;
  let panel = null;
  function activeTab() { return document.querySelector('#mainTabBar .tab-btn.active')?.dataset.tab || 'noteEditor'; }
  function fill() {
    if (!panel) return;
    const tab = activeTab();
    const names = { noteEditor: 'Note Editor', timeline: 'Song Generator', mixer: 'Mixer', drumEngine: 'Drum Engine', howto: 'How To' };
    panel.querySelector('.hotkeys-panel-title').textContent = 'Shortcuts — ' + (names[tab] || '');
    panel.querySelector('.hotkeys-panel-body').innerHTML = dggHotkeysHtml(tab);
  }
  function place() {
    const r = btn.getBoundingClientRect();
    panel.style.top = (r.bottom + 6) + 'px';
    panel.style.right = Math.max(4, window.innerWidth - r.right) + 'px';
  }
  function close() { if (panel) { panel.remove(); panel = null; } btn.classList.remove('active'); }
  function open() {
    panel = document.createElement('div');
    panel.className = 'hotkeys-panel';
    panel.innerHTML = '<div class="hotkeys-panel-head"><span class="hotkeys-panel-title"></span><button class="hotkeys-panel-close" title="Close">×</button></div><div class="hotkeys-panel-body"></div>';
    panel.querySelector('.hotkeys-panel-close').addEventListener('click', close);
    document.body.appendChild(panel);
    fill(); place();
    btn.classList.add('active');
  }
  function toggle() { if (panel) close(); else open(); }
  btn.addEventListener('click', toggle);
  document.getElementById('mainTabBar')?.addEventListener('click', () => setTimeout(fill, 0));
  window.addEventListener('resize', () => { if (panel) place(); });
  document.addEventListener('keydown', (e) => {
    const t = e.target;
    const inField = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
    if (e.key === 'Escape' && panel) { close(); return; }
    if (inField || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key === 'F1' || e.key === '?') { e.preventDefault(); toggle(); }
  });
})();
