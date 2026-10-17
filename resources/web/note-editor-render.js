// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// NOTE EDITOR — RENDERING
// Split out of note-editor.js. renderNoteEditor() is the top-level entry
// point/orchestrator (calls into note-editor-panels.js's panel builders and
// the row/ruler/velocity-lane builders below), composing everything into the
// live DOM. Everything else here is a pure "build me this piece" function —
// only renderNoteEditor() itself touches the live page directly.
// =========================================

// Where the playhead is drawn: the start position (nePlayheadBeat) while idle, the playback position while playing.
let neShownBeat = 0;

function neUpdatePlayheadEl() {
  const el = document.getElementById('nePlayhead');
  if (!el) return;
  const rowsCol = document.querySelector('#drumGrid .ne-rows');
  const scroll = rowsCol ? rowsCol.scrollLeft : 0;
  el.style.left = (NE_LEFT_PAD + neShownBeat * neCurrentPxPerBeat - scroll) + 'px';
}

// Vel/Dyn/Rnd nudge state, preserved across rebuilds to allow fine-tuning.
// Reset only on selection change. velNudgeBaseline captures velocities at
// selection time for stable reference.
let velNudgeSelKey = null;
let velNudgeBaseline = null;
let velNudgeVals = { vel: 0, dyn: 0, rnd: 0 };

// Velocity lane instrument filter: '' = no filter, 'g:<group>' = every articulation of a group, 't:<trackId>' = one track.
let velFilter = '';

function neVelFilterTrackIds() {
  if (!velFilter) return null;
  const ids = velFilter.startsWith('g:')
    ? trackList.filter(t => t.role !== 'custom' && t.group === velFilter.slice(2)).map(t => t.id)
    : trackList.filter(t => t.id === velFilter.slice(2)).map(t => t.id);
  if (!ids.length) { velFilter = ''; return null; }
  return ids;
}

// Custom menu instead of a native <select>: the native popup flashed at a wrong position before settling,
// and this one opens upward because the lane sits at the bottom of the window.
function neBuildVelFilterSelect() {
  const entries = [{ value: '', text: 'All instruments' }];
  const seenGroups = new Set();
  trackList.forEach(t => {
    if (t.role === 'custom') { entries.push({ value: 't:' + t.id, text: t.label }); return; }
    if (seenGroups.has(t.group)) return;
    seenGroups.add(t.group);
    entries.push({ value: 'g:' + t.group, text: GROUPS[t.group] || t.group });
  });
  if (!entries.some(e => e.value === velFilter)) velFilter = '';

  const wrap = document.createElement('div');
  wrap.className = 'ne-vel-filter-wrap';
  const btn = document.createElement('button');
  btn.type = 'button';
  btn.className = 'ne-vel-filter';
  btn.dataset.tooltip = 'Velocity filter\nShows only the velocities of one instrument. "All instruments" shows everything.';
  btn.textContent = entries.find(e => e.value === velFilter).text;
  const menu = document.createElement('div');
  menu.className = 'ne-vel-filter-menu';
  menu.style.display = 'none';
  entries.forEach(en => {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'ne-vel-filter-item' + (en.value === velFilter ? ' active' : '');
    item.textContent = en.text;
    item.addEventListener('click', (e) => {
      e.stopPropagation();
      velFilter = en.value; velNudgeSelKey = null;
      renderNoteEditor();
    });
    menu.appendChild(item);
  });
  const closeOnOutside = (e) => {
    if (wrap.contains(e.target)) return;
    menu.style.display = 'none';
    document.removeEventListener('pointerdown', closeOnOutside, true);
  };
  btn.addEventListener('click', (e) => {
    e.stopPropagation();
    if (menu.style.display !== 'none') { menu.style.display = 'none'; document.removeEventListener('pointerdown', closeOnOutside, true); return; }
    menu.style.display = 'block';
    const active = menu.querySelector('.active');
    if (active) menu.scrollTop = Math.max(0, active.offsetTop - menu.clientHeight / 2);
    document.addEventListener('pointerdown', closeOnOutside, true);
  });
  wrap.appendChild(btn); wrap.appendChild(menu);
  return wrap;
}

// Snapshot of block.data/lockedGroups before the current interaction's first
// change, captured lazily. Cleared on commit.
let velCommitBefore = null;

// Counts visible grid rows without building DOM, to determine if vertical
// scrollbar will be shown (affects horizontal fit width).
function neCountVisibleRows() {
  let lastGroup = null;
  let rows = 0;
  const seenTripletGroups = new Set();
  trackList.forEach((t, i) => {
    // Custom tracks are single-articulation groups of their own.
    const isCustom = t.role === 'custom';
    if ((t.group && t.group !== lastGroup) || isCustom) {
      rows++; // group header row
      const isOpen = isCustom ? expandedGroups.has('custom:' + t.id) : expandedGroups.has(t.group);
      const engineGroup = engineGroupKeyForTrack(t);
      if (isOpen && engineGroup && !seenTripletGroups.has(engineGroup)) {
        seenTripletGroups.add(engineGroup);
        rows++; // Triplet Zone row
      }
    }
    lastGroup = t.group || null;
    const open = isCustom ? expandedGroups.has('custom:' + t.id) : (t.group && expandedGroups.has(t.group));
    if (!open) return;
    rows++; // leaf row
    if (neIsLastOfGroup(i)) rows++; // Phrase row
  });
  return rows;
}

// True for the last articulation row of a group that has an engine group (and so a Phrase setting).
function neIsLastOfGroup(i) {
  const t = trackList[i], next = trackList[i + 1];
  if (!engineGroupKeyForTrack(t)) return false;
  return t.role === 'custom' || !next || next.role === 'custom' || next.group !== t.group;
}

function renderNoteEditor() {
  neSyncCustomCluster();       // Custom cluster visibility/title/knob values follow the focused custom track
  updateGroupLockUI();
  neRefreshAddTrackButton();   // one track per custom instrument — enable/disable "+ Add Track" accordingly

  // Capture scroll position before rebuild, restore after.
  const oldRowsCol = document.querySelector('#drumGrid .ne-rows');
  let savedScrollLeft = oldRowsCol ? oldRowsCol.scrollLeft : 0;
  const oldRowsWrap = document.querySelector('#drumGrid .ne-rows-wrap');
  const savedScrollTop = oldRowsWrap ? oldRowsWrap.scrollTop : 0; // vertical position of the row list — must survive the rebuild
  const prevRowsWrapH = (oldRowsWrap && oldRowsWrap.clientHeight > 0) ? oldRowsWrap.clientHeight : 0; // rows area is a variable height now — measure pre-rebuild for the scrollbar-width reservation
  const savedScrollY = window.scrollY;
  const container = document.getElementById('drumGrid');
  const sidebar = document.getElementById('neSidebar');
  const fillHost = document.getElementById('neFillClusters');
  const hoveredDialRow = sidebar ? neHoveredSidebarRowIndex(sidebar) : -1;
  const hoveredFillCluster = fillHost ? [...fillHost.children].findIndex(c => c.matches(':hover')) : -1;
  container.innerHTML = '';
  if (sidebar) sidebar.innerHTML = '';
  if (fillHost) fillHost.innerHTML = '';
  neSyncZoomHeader();
  const block = findBlock(activeBlockId);

  if (neStatOverrideBlockId !== null && neStatOverrideBlockId !== activeBlockId) neClearStatOverride();
  if (!block) { requestAnimationFrame(() => window.scrollTo(0, savedScrollY)); return; }

  // Reset nudge counter if selection changed (by resolved note objects, not keys).
  {
    const resolvedNow = new Set(neResolveSelection(block).map(it => it.note));
    let sameSelection = resolvedNow.size === neNudgeSelectedRefs.size && resolvedNow.size > 0;
    if (sameSelection) for (const n of resolvedNow) { if (!neNudgeSelectedRefs.has(n)) { sameSelection = false; break; } }
    if (!sameSelection) {
      neNudgeSteps = 0;
      const rSlider = document.getElementById('randomizeAmt');
      if (rSlider && +rSlider.value !== 0 && !randomizeGestureItems) {
        suppressKnobRegen = true;
        rSlider.value = 0;
        rSlider.dispatchEvent(new Event('input'));
        suppressKnobRegen = false;
      }
    }
    neNudgeSelectedRefs = resolvedNow;
    updateNudgeDisplay();
  }
  const g = readGrid();
  const numerator = block.numerator;
  const blockLen = Math.max(1, block.length || 1);
  const blockBeats = blockLen * numerator;

  // Handle block switch: reset playhead/loop, with special case for locked Break.
  const isBlockSwitch = block.id !== neLastPlayheadBlockId;
  let neAutoFitBreak = false;
  if (isBlockSwitch) {
    if (neAutoFitZoomBlockId != null && neAutoFitZoomBlockId !== block.id) {
      neZoom = neZoomBeforeAutoFit;
      neAutoFitZoomBlockId = null;
      neZoomBeforeAutoFit = null;
    }
    neLastPlayheadBlockId = block.id;
    if (isBreakBlock(block) && !block.breakUnlocked) {
      const gb = Math.max(0, blockBeats - (block.breakBeats ?? BREAK_DEFAULT_BEATS));
      neLoopStart = gb;
      neLoopEnd = blockBeats;
      nePlayheadBeat = neLoopStart;
      neAutoFitBreak = true;
      neLoopFollowsBreak = true;
    } else {
      nePlayheadBeat = 0; neLoopStart = null; neLoopEnd = null;
      neLoopFollowsBreak = false;
    }
  }

  const totalBeats = blockBeats + NE_ENDLESS_EXTRA_BARS * numerator;

  // Calculate available width and fit-to-width baseline.
  const gridSection = container.closest('.grid-section');
  const gridContentWidth = (gridSection ? gridSection.clientWidth : container.clientWidth + 232) - 24;
  const stackedLayout = window.innerWidth <= 900;
  const drumGridWidth = stackedLayout ? gridContentWidth : gridContentWidth - (neSidebarCollapsed ? 0 : 12 + 220);
  const NE_RIGHT_PAD = 28;
  const rowsWrapH = prevRowsWrapH || NE_ROWS_WRAP_H;
  const rowsScrollbarVisible = (neCountVisibleRows() * NE_ROW_H) > rowsWrapH;
  const scrollbarPx = rowsScrollbarVisible ? 10 : 0;
  const availWidth = Math.max(50, drumGridWidth - 170 - NE_LEFT_PAD - NE_RIGHT_PAD - scrollbarPx);
  const fitPxPerBeat = availWidth / blockBeats;

  // Auto-fit for locked Break block to show the break-tail.
  if (neAutoFitBreak) {
    neZoomBeforeAutoFit = neZoom;
    const visibleBeats = Math.max(0.5, blockBeats - neLoopStart);
    neZoom = Math.max(NE_ZOOM_MIN, Math.min(NE_ZOOM_MAX, blockBeats / visibleBeats));
    neAutoFitZoomBlockId = block.id;
  }
  const pxPerBeat = fitPxPerBeat * neZoom;
  neCurrentPxPerBeat = pxPerBeat;
  const totalW = totalBeats * pxPerBeat + NE_LEFT_PAD + NE_RIGHT_PAD;

  // Set initial scroll for auto-fit break, else reset on block switch.
  if (neAutoFitBreak) {
    savedScrollLeft = Math.max(0, NE_LEFT_PAD + neLoopStart * pxPerBeat - 40);
  } else if (isBlockSwitch) {
    savedScrollLeft = 0;
  }

  const isBreak = isBreakBlock(block);
  const breakLocked = isBreak && !block.breakUnlocked;
  const grooveBeats = isBreak ? Math.max(0, blockBeats - (block.breakBeats ?? BREAK_DEFAULT_BEATS)) : 0;

  const ctx = { block, g, numerator, blockLen, blockBeats, totalBeats, pxPerBeat, totalW, isBreak, breakLocked, grooveBeats };

  if (breakLocked) neShowBreakLockPill(ctx);

  const controls = neBuildControlsPanel(ctx);
  (sidebar || container).appendChild(controls);
  if (hoveredDialRow >= 0) neRestoreSidebarRowZoom(hoveredDialRow);
  if (hoveredFillCluster >= 0 && fillHost.children[hoveredFillCluster]) neHoldZoom(fillHost.children[hoveredFillCluster], null);

  const ruler = neBuildRuler(ctx);
  container.appendChild(ruler);

  const loopRuler = neBuildLoopRuler(ctx);
  container.appendChild(loopRuler);

  const { rowsWrap, rowsCol } = neBuildRows(ctx);
  container.appendChild(rowsWrap);
  attachRectSelect(rowsCol);
  neAttachGridZoomAndPan(rowsCol);

  const { velWrap, velLane } = neBuildVelocityLane(ctx);
  container.appendChild(velWrap);

  // Sync horizontal scroll across rulers, rows, and velocity lane.
  // The playhead is one layer over the whole grid (ruler, rows and velocity lane), positioned from the shared scroll offset.
  const phLayer = document.createElement('div');
  phLayer.className = 'ne-ph-layer';
  const ph = document.createElement('div');
  ph.id = 'nePlayhead'; ph.className = 'dg-playhead';
  phLayer.appendChild(ph);
  container.appendChild(phLayer);
  if (!(playing && playbackMode === 'block')) neShownBeat = nePlayheadBeat;

  const syncScroll = (src) => { ruler.scrollLeft = src.scrollLeft; loopRuler.scrollLeft = src.scrollLeft; rowsCol.scrollLeft = src.scrollLeft; velLane.scrollLeft = src.scrollLeft; neUpdatePlayheadEl(); };
  rowsCol.addEventListener('scroll', () => syncScroll(rowsCol));
  velLane.addEventListener('scroll', () => syncScroll(velLane));
  if (savedScrollLeft > 0) syncScroll({ scrollLeft: savedScrollLeft });
  neUpdatePlayheadEl();
  const freshRowsWrap = document.querySelector('#drumGrid .ne-rows-wrap');
  if (freshRowsWrap && savedScrollTop > 0) freshRowsWrap.scrollTop = savedScrollTop;
  requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
  neResyncA11yHover();
}

// ── Ruler / rows / velocity lane builders ──

// Beat/bar ruler above the rows.
function neBuildRuler(ctx) {
  const { numerator, totalBeats, blockBeats, pxPerBeat, totalW } = ctx;
  const ruler = document.createElement('div');
  ruler.className = 'ne-ruler'; ruler.style.width = 'calc(100% - 170px)';
  ruler.style.overflowX = 'hidden';
  const rulerInner = document.createElement('div');
  rulerInner.style.cssText = `position:relative; width:${totalW}px; height:100%;`;

  const showBeats = pxPerBeat >= 30;
  for (let gb = 0; gb < totalBeats; gb++) {
    const bar = Math.floor(gb / numerator);
    const bt = gb % numerator;
    if (bt > 0 && !showBeats) continue;
    const mark = document.createElement('div');
    mark.className = 'ne-ruler-mark' + (bt === 0 ? ' ne-ruler-bar' : '') + (gb >= blockBeats ? ' ne-ruler-endless' : '');
    mark.style.left = (NE_LEFT_PAD + gb*pxPerBeat)+'px'; mark.style.width = pxPerBeat+'px';
    mark.textContent = bt === 0 ? String(bar+1) : ((bar+1) + '.' + (bt+1));
    rulerInner.appendChild(mark);
  }

  const endLine = document.createElement('div');
  endLine.className = 'ne-block-end-line';
  endLine.style.left = (NE_LEFT_PAD + blockBeats*pxPerBeat) + 'px';
  rulerInner.appendChild(endLine);

  const beatAtClientX = (clientX) => {
    const rect = ruler.getBoundingClientRect();
    const x = (clientX - rect.left) / currentUIScale() + ruler.scrollLeft;
    return Math.max(0, Math.min(blockBeats, (x - NE_LEFT_PAD) / pxPerBeat));
  };
  let scrubbing = false;
  ruler.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    scrubbing = true;
    nePlayheadBeat = beatAtClientX(e.clientX);
    neShownBeat = nePlayheadBeat; neUpdatePlayheadEl();
    requestPlaySeek('block', nePlayheadBeat);   // during playback: continue from here, and this is the new start position
    ruler.setPointerCapture(e.pointerId);
  });
  ruler.addEventListener('pointermove', (e) => {
    if (!scrubbing) return;
    nePlayheadBeat = beatAtClientX(e.clientX);
    neShownBeat = nePlayheadBeat; neUpdatePlayheadEl();
    requestPlaySeek('block', nePlayheadBeat);
  });
  const finishScrub = () => {
    if (!scrubbing) return;
    scrubbing = false;
    renderNoteEditor();
  };
  ruler.addEventListener('pointerup', finishScrub);
  ruler.addEventListener('pointercancel', finishScrub);

  ruler.appendChild(rulerInner);
  return ruler;
}

// Dedicated loop-range row.
function neBuildLoopRuler(ctx) {
  const { blockBeats, pxPerBeat, totalW } = ctx;
  const loopRuler = document.createElement('div');
  loopRuler.className = 'ne-loop-ruler'; loopRuler.style.width = 'calc(100% - 170px)';
  loopRuler.dataset.tooltip = 'Loop Range\nDrag to set a loop range. Right-click to clear it.';
  loopRuler.style.overflowX = 'hidden';
  const inner = document.createElement('div');
  inner.style.cssText = `position:relative; width:${totalW}px; height:100%;`;

  const loopOverlay = document.createElement('div');
  loopOverlay.className = 'ne-loop-overlay';
  if (neLoopStart != null) {
    loopOverlay.style.display = 'block';
    loopOverlay.style.left = (NE_LEFT_PAD + neLoopStart*pxPerBeat) + 'px';
    loopOverlay.style.width = ((neLoopEnd-neLoopStart)*pxPerBeat) + 'px';
  } else {
    loopOverlay.style.display = 'none';
  }
  ['start', 'end'].forEach(edge => {
    const h = document.createElement('div');
    h.className = 'loop-handle ' + edge; h.dataset.edge = edge;
    loopOverlay.appendChild(h);
  });
  inner.appendChild(loopOverlay);

  let dragState = null;
  const beatAtClientX = (clientX) => {
    const rect = loopRuler.getBoundingClientRect();
    const x = (clientX - rect.left) / currentUIScale() + loopRuler.scrollLeft;
    return Math.max(0, Math.min(blockBeats, (x - NE_LEFT_PAD) / pxPerBeat));
  };
  loopRuler.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    const handle = e.target.closest('.loop-handle');
    if (handle && neLoopStart != null) {
      // Dragging an edge of the existing loop: only that edge moves.
      dragState = { edge: handle.dataset.edge, moved: true };
      loopRuler.setPointerCapture(e.pointerId);
      return;
    }
    dragState = { startBeat: beatAtClientX(e.clientX), moved: false };
    loopRuler.setPointerCapture(e.pointerId);
  });
  loopRuler.addEventListener('pointermove', (e) => {
    if (!dragState) return;
    const beat = beatAtClientX(e.clientX);
    if (dragState.edge) {
      const lo = dragState.edge === 'start' ? Math.min(beat, neLoopEnd - 0.01) : neLoopStart;
      const hi = dragState.edge === 'end' ? Math.max(beat, neLoopStart + 0.01) : neLoopEnd;
      dragState.lo = lo; dragState.hi = hi;
      loopOverlay.style.left = (NE_LEFT_PAD + lo*pxPerBeat) + 'px';
      loopOverlay.style.width = ((hi-lo)*pxPerBeat) + 'px';
      return;
    }
    if (Math.abs(beat - dragState.startBeat) * pxPerBeat > 4) dragState.moved = true;
    if (dragState.moved) {
      const lo = Math.min(dragState.startBeat, beat), hi = Math.max(dragState.startBeat, beat);
      loopOverlay.style.display = 'block';
      loopOverlay.style.left = (NE_LEFT_PAD + lo*pxPerBeat) + 'px';
      loopOverlay.style.width = ((hi-lo)*pxPerBeat) + 'px';
    }
  });
  const finishDrag = (e) => {
    if (!dragState) return;
    if (dragState.edge) {
      if (dragState.lo !== undefined) {
        neLoopStart = dragState.lo; neLoopEnd = dragState.hi; neLoopFollowsBreak = false;
        if (dragState.edge === 'start') nePlayheadBeat = neLoopStart;   // playback starts at the loop's start
      }
    } else if (dragState.moved) {
      const beat = beatAtClientX(e.clientX);
      const lo = Math.min(dragState.startBeat, beat), hi = Math.max(dragState.startBeat, beat);
      neLoopStart = lo; neLoopEnd = hi; nePlayheadBeat = lo; neLoopFollowsBreak = false;
    }
    dragState = null;
    renderNoteEditor();
  };
  loopRuler.addEventListener('pointerup', finishDrag);
  // Right-click clears the loop, like right-click deletes in the grid.
  loopRuler.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    neLoopStart = null; neLoopEnd = null; neLoopFollowsBreak = false;
    renderNoteEditor();
  });
  loopRuler.addEventListener('pointercancel', () => { dragState = null; });

  loopRuler.appendChild(inner);
  return loopRuler;
}

// Track rows: group headers + leaf rows.
function neBuildRows(ctx) {
  const { totalW, breakLocked, grooveBeats, pxPerBeat, blockBeats, totalBeats, block, numerator, blockLen } = ctx;

  const rowsWrap = document.createElement('div');
  rowsWrap.className = 'ne-rows-wrap';
  const labelsCol = document.createElement('div');
  labelsCol.className = 'ne-row-labels';
  const rowsCol = document.createElement('div');
  rowsCol.className = 'ne-rows';
  const rowsInner = document.createElement('div');
  rowsInner.style.cssText = `position:relative; width:${totalW}px;`;

  let lastGroup = null;
  let renderedRows = 0;
  const renderedTripletGroups = new Set();
  const refs = { labelsCol, rowsInner, rowsCol, renderedTripletGroups };
  trackList.forEach((t, i) => {
    // Custom tracks are single-articulation groups of their own (the instrument is its only articulation).
    const isCustom = t.role === 'custom';
    if ((t.group && t.group !== lastGroup) || isCustom) {
      renderedRows += neRenderGroupHeader(t, ctx, refs);
    }
    lastGroup = t.group || null;
    const open = isCustom ? expandedGroups.has('custom:' + t.id) : (t.group && expandedGroups.has(t.group));
    if (!open) return;
    renderedRows += neRenderTrackLeafRow(t, ctx, refs);
    if (neIsLastOfGroup(i)) renderedRows += neRenderPhraseRow(t, ctx, refs);
  });
  rowsInner.style.height = (renderedRows*NE_ROW_H)+'px';

  // Locked groove-copy zone overlay.
  if (breakLocked && grooveBeats > 0) {
    const lockOverlay = document.createElement('div');
    lockOverlay.className = 'ne-break-lock-overlay';
    lockOverlay.style.left = NE_LEFT_PAD + 'px';
    lockOverlay.style.width = (grooveBeats * pxPerBeat) + 'px';
    lockOverlay.style.height = (renderedRows*NE_ROW_H) + 'px';
    rowsInner.appendChild(lockOverlay);
  }

  // Turnaround-accent bars overlay.
  const turnaroundInterval = block && block.turnaroundInterval || 0;
  if (turnaroundInterval > 0) {
    const len = Math.max(1, blockLen || 1);
    const lengthBeats = resolveTurnaroundLengthBeats(block, numerator);
    for (let barIdx = 0; barIdx < len; barIdx++) {
      if ((barIdx + 1) % turnaroundInterval !== 0) continue;
      const zone = document.createElement('div');
      zone.className = 'ne-turnaround-zone';
      zone.style.left = (NE_LEFT_PAD + (barIdx * numerator + (numerator - lengthBeats)) * pxPerBeat) + 'px';
      zone.style.width = (lengthBeats * pxPerBeat) + 'px';
      zone.style.height = (renderedRows*NE_ROW_H) + 'px';
      rowsInner.appendChild(zone);
    }
  }

  // Endless padding overlay.
  if (totalBeats > blockBeats) {
    const endlessOverlay = document.createElement('div');
    endlessOverlay.className = 'ne-endless-overlay';
    endlessOverlay.style.left = (NE_LEFT_PAD + blockBeats*pxPerBeat) + 'px';
    endlessOverlay.style.width = ((totalBeats - blockBeats) * pxPerBeat) + 'px';
    endlessOverlay.style.height = (renderedRows*NE_ROW_H) + 'px';
    rowsInner.appendChild(endlessOverlay);
  }
  rowsCol.appendChild(rowsInner);
  rowsWrap.appendChild(labelsCol); rowsWrap.appendChild(rowsCol);
  return { rowsWrap, rowsCol };
}

// State for checkbox paint gesture.
let articulationCbPaint = null;

function attachArticulationCheckboxPaintDrag(cb, block, trackId) {
  const applyTo = (el) => {
    if (!articulationCbPaint || !el || !el.classList || !el.classList.contains('ne-articulation-enable-cb') || el.disabled) return;
    const id = el.dataset.trackId;
    if (!id || articulationCbPaint.touched.has(id)) return;
    articulationCbPaint.touched.add(id);
    if (setArticulationEnabled(articulationCbPaint.block, id, articulationCbPaint.target)) el.checked = articulationCbPaint.target;
  };
  cb.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    const target = !cb.checked;
    articulationCbPaint = { block, target, touched: new Set(), before: JSON.stringify({ enabledArticulations: block.enabledArticulations || {}, data: block.data }) };
    applyTo(cb);
    cb.setPointerCapture(e.pointerId);
  });
  cb.addEventListener('pointermove', (e) => {
    if (!articulationCbPaint) return;
    applyTo(document.elementFromPoint(e.clientX, e.clientY));
  });
  const finish = () => {
    if (!articulationCbPaint) return;
    const { block: b, before, touched } = articulationCbPaint;
    articulationCbPaint = null;
    if (!touched.size) return;
    const after = JSON.stringify({ enabledArticulations: b.enabledArticulations || {}, data: b.data });
    renderNoteEditor();
    if (after === before) return;
    pushUndo(touched.size > 1 ? `Set ${touched.size} articulations` : 'Toggle articulation',
      () => { const s = JSON.parse(before); b.enabledArticulations = s.enabledArticulations; b.data = s.data; renderNoteEditor(); },
      () => { const s = JSON.parse(after); b.enabledArticulations = s.enabledArticulations; b.data = s.data; renderNoteEditor(); }
    );
  };
  cb.addEventListener('pointerup', finish);
  cb.addEventListener('pointercancel', finish);
  cb.addEventListener('change', () => {
    if (articulationCbPaint) return;
    const wanted = cb.checked;
    if (!toggleArticulationEnabled(block, trackId)) cb.checked = !wanted;
  });
}

// Renders one instrument group header row.
function neRenderGroupHeader(t, ctx, refs) {
  const { block, g, numerator, totalBeats, blockBeats, pxPerBeat, totalW, breakLocked, grooveBeats } = ctx;
  const { labelsCol, rowsInner, renderedTripletGroups, rowsCol } = refs;
  let rows = 0;

  // Custom tracks render as single-articulation groups: the instrument itself is the only articulation.
  const isCustom = t.role === 'custom';
  const groupKey = isCustom ? 'custom:' + t.id : t.group;
  const isOpen = expandedGroups.has(groupKey);
  const groupChildIds = isCustom ? [t.id] : trackList.filter(gt => gt.group === t.group).map(gt => gt.id);
  const enabledIds = isCustom ? [t.id] : getEnabledArticulations(block, t.group);
  const defaultTrack = trackList.find(gt => groupChildIds.includes(gt.id) && enabledIds.includes(gt.id)) || t;
  const isGroupMuted = groupChildIds.every(id => mutedInstruments.has(id));
  const isGroupSoloed = groupChildIds.every(id => soloedInstruments.has(id));
  const isGroupSilencedBySolo = soloedInstruments.size > 0 && !isGroupSoloed;

  const header = document.createElement('div');
  header.className = 'ne-row-label ne-group-header' + (isGroupMuted ? ' muted' : '') + (isGroupSoloed ? ' soloed' : '') + (!isGroupMuted && isGroupSilencedBySolo ? ' solo-silenced' : '');
  const displayName = isCustom ? t.label : (GROUPS[t.group]||t.group);
  header.innerHTML = `<span class="ne-group-arrow" style="transform:rotate(${isOpen?90:0}deg);"></span><span>${displayName}</span>`;
  header.dataset.tooltip = 'Group\nClick to expand or collapse its articulations.';
  header.addEventListener('click', () => {
    isOpen ? expandedGroups.delete(groupKey) : expandedGroups.add(groupKey);
    if (isCustom) neSetActiveCustomTrack(t.id);   // selecting a custom group also steers the Custom cluster
    renderNoteEditor();
  });

  const groupPreviewBtn = document.createElement('span');
  groupPreviewBtn.className = 'ne-preview-btn'; groupPreviewBtn.textContent = '▶';
  groupPreviewBtn.dataset.tooltip = 'Preview\nPlays the default articulation of this group.';
  groupPreviewBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    nePreviewTrack(defaultTrack.id, 100);
  });
  header.appendChild(groupPreviewBtn);

  const groupMuteBtn = document.createElement('span');
  groupMuteBtn.className = 'ne-mute-btn'; groupMuteBtn.textContent = 'M';
  groupMuteBtn.dataset.tooltip = "Mute\nToggles mute for every articulation in this group. Affects the Note Editor's own playback and the sounds you click; not the Song Generator, the DAW or exports.";
  groupMuteBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    groupChildIds.forEach(id => {
      if (isGroupMuted) mutedInstruments.delete(id);
      else { mutedInstruments.add(id); soloedInstruments.delete(id); }
    });
    dataGeneration++;   // a running Note Editor playback re-reads its events
    renderNoteEditor();
  });
  header.appendChild(groupMuteBtn);

  const groupSoloBtn = document.createElement('span');
  groupSoloBtn.className = 'ne-solo-btn'; groupSoloBtn.textContent = 'S';
  groupSoloBtn.dataset.tooltip = "Solo\nToggles solo for every articulation in this group. Only one group can be soloed at a time. Affects the Note Editor's own playback and the sounds you click; not the Song Generator, the DAW or exports.";
  groupSoloBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    soloedInstruments.clear();
    if (!isGroupSoloed) groupChildIds.forEach(id => { soloedInstruments.add(id); mutedInstruments.delete(id); });
    dataGeneration++;   // a running Note Editor playback re-reads its events
    renderNoteEditor();
  });
  header.appendChild(groupSoloBtn);
  labelsCol.appendChild(header);

  const headerRow = document.createElement('div');
  headerRow.className = 'ne-row ne-group-header-row';
  headerRow.dataset.trackId = defaultTrack.id;
  headerRow.style.width = totalW+'px';
  const engineGroup = engineGroupKeyForTrack(t);
  neDrawRowGrid(headerRow, totalBeats, pxPerBeat, g.snap, numerator, block.tripletLayers && engineGroup && block.tripletLayers[engineGroup], engineGroup, g);

  if (!isOpen) {
    // Collapsed: show merged notes from all articulations.
    groupChildIds.forEach(id => {
      const gt = trackList.find(x => x.id === id);
      (block.data[gt.id] || []).forEach((note, ni) => {
        const dot = document.createElement('div');
        const selKey = gt.id+'|'+note.beat;
        const noteLocked = breakLocked && note.beat < grooveBeats;
        dot.className = 'ne-note ne-note-summary' + (selectedNotes.has(selKey) ? ' selected' : '') + (noteLocked ? ' ne-note-locked' : '') + (note.turnaround ? ' ne-note-turnaround' : '');
        dot.dataset.selKey = selKey;
        dot.dataset.tooltip = '';
        dot.style.left = (NE_LEFT_PAD + note.beat*pxPerBeat)+'px';
        dot.style.top = (NE_ROW_H/2)+'px';
        dot.style.background = noteLocked ? 'var(--muted)' : fadedColor(gt.color, note.vel/127);
        if (!noteLocked) {
          dot.addEventListener('contextmenu', (e) => {
            e.preventDefault(); e.stopPropagation();
            if (suppressNextContextMenu) { suppressNextContextMenu = false; return; }
            const idx = block.data[gt.id].findIndex(n => n.beat === note.beat);
            if (idx < 0) return;
            const beforeLocked = snapshotLocked(block);
            const removed = block.data[gt.id].splice(idx, 1)[0];
            markTrackManuallyEdited(block, gt.id);
            const afterLocked = snapshotLocked(block);
            block.generated = false;
            rebuildFullFromEditedBlocks();
            renderNoteEditor();
            pushUndo('Delete note',
              () => {
                block.data[gt.id].push(removed);
                block.data[gt.id].sort((a,b) => a.beat-b.beat);
                restoreLocked(block, beforeLocked);
                block.generated = false;
                rebuildFullFromEditedBlocks();
                renderNoteEditor();
              },
              () => {
                const i = block.data[gt.id].indexOf(removed);
                if (i >= 0) block.data[gt.id].splice(i, 1);
                restoreLocked(block, afterLocked);
                block.generated = false;
                rebuildFullFromEditedBlocks();
                renderNoteEditor();
              }
            );
          });
          attachNoteDrag(dot, block, gt.id, ni, numerator, g, pxPerBeat);
        }
        headerRow.appendChild(dot);
      });
    });
    headerRow.addEventListener('click', (e) => {
      if (suppressNextRowClick) { suppressNextRowClick = false; return; }
      selectedNotes.clear();
      const rect = rowsCol.getBoundingClientRect();
      const x = (e.clientX - rect.left) / currentUIScale() + rowsCol.scrollLeft;
      let beat = Math.max(0, Math.min(totalBeats-0.01, (x-NE_LEFT_PAD)/pxPerBeat));
      beat = neQuantizeForTrack(beat, g, block, defaultTrack.id);
      if (beat >= blockBeats) { neMsg('Beyond this block\'s length — grow Length (Bars) to use that space.'); return; }
      if (breakLocked && beat < grooveBeats) { neMsg('Copied bars are locked — see the Break tab.'); return; }
      const arr = block.data[defaultTrack.id];
      const collide = neFindCollision(arr, beat, -1);
      const beforeLocked = snapshotLocked(block);
      let undoFn, redoFn, previewVel;
      if (collide >= 0) {
        const noteRef = arr[collide], oldBeat = noteRef.beat;
        if (noteRef.turnaround) delete noteRef.turnaround;
        noteRef.beat = beat;
        previewVel = noteRef.vel;
        neMsg('Replaced existing note');
        undoFn = () => { noteRef.beat = oldBeat; arr.sort((a,b)=>a.beat-b.beat); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
        redoFn = () => { noteRef.beat = beat; arr.sort((a,b)=>a.beat-b.beat); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
      } else {
        const newNote = { beat, vel:100 };
        arr.push(newNote);
        previewVel = newNote.vel;
        undoFn = () => { const i = arr.indexOf(newNote); if (i>=0) arr.splice(i,1); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
        redoFn = () => { if (arr.indexOf(newNote)<0) { arr.push(newNote); arr.sort((a,b)=>a.beat-b.beat); } block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
      }
      nePreviewTrack(defaultTrack.id, previewVel);
      markTrackManuallyEdited(block, defaultTrack.id);
      const afterLocked = snapshotLocked(block);
      const plainUndoFn = undoFn, plainRedoFn = redoFn;
      undoFn = () => { plainUndoFn(); restoreLocked(block, beforeLocked); };
      redoFn = () => { plainRedoFn(); restoreLocked(block, afterLocked); };
      block.generated = false;
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
      pushUndo(collide >= 0 ? 'Move note' : 'Add note', undoFn, redoFn);
    });
    headerRow.addEventListener('contextmenu', (e) => {
      if (e.target !== headerRow) return;
      e.preventDefault();
      if (selectedNotes.size > 0) { selectedNotes.clear(); renderNoteEditor(); }
    });
    attachRowPaintDrag(headerRow, block, defaultTrack, ctx, refs);
  }
  rowsInner.appendChild(headerRow);
  rows++;

  // Triplet Zone row.
  if (isOpen && engineGroup && !renderedTripletGroups.has(engineGroup)) {
    renderedTripletGroups.add(engineGroup);
    const zoneKind = isTripletSubdivision(g.subdivision) ? 'Straight' : 'Triplet';
    const ctrlLabel = document.createElement('div');
    ctrlLabel.className = 'ne-row-label ne-control-row-label';
    ctrlLabel.innerHTML = `<span>${zoneKind} Zone</span>`;
    ctrlLabel.dataset.tooltip = `${zoneKind} Zone\nDrag to mark a ${zoneKind}-only range for this group — everywhere else plays the opposite (${zoneKind === 'Triplet' ? 'Straight' : 'Triplet'}). Right-click a marked range to remove it · drag its edges to resize.`;
    labelsCol.appendChild(ctrlLabel);

    const ctrlRow = document.createElement('div');
    ctrlRow.className = 'ne-row ne-control-row';
    ctrlRow.style.width = totalW+'px';
    ctrlRow.dataset.tooltip = ctrlLabel.dataset.tooltip;
    neDrawRowGrid(ctrlRow, totalBeats, pxPerBeat, g.snap, numerator);

    block.tripletLayers = block.tripletLayers || {};
    block.tripletLayers[engineGroup] = block.tripletLayers[engineGroup] || [];
    block.tripletLayers[engineGroup].forEach((range, ri2) => {
      const seg = document.createElement('div');
      seg.className = 'ne-triplet-range';
      seg.style.left = (NE_LEFT_PAD + range.from*pxPerBeat)+'px';
      seg.style.width = ((range.to-range.from)*pxPerBeat)+'px';
      seg.dataset.tooltip = `${zoneKind} Range\nBeats ${range.from.toFixed(2)}–${range.to.toFixed(2)}. Right-click to remove · drag either edge to resize.`;
      seg.addEventListener('contextmenu', (e) => {
        e.preventDefault(); e.stopPropagation();
        if (suppressNextContextMenu) { suppressNextContextMenu = false; return; }
        block.tripletLayers[engineGroup].splice(ri2, 1);
        if (engineGroup === 'kick') applyKickGroupToBlock(block, g);
        rebuildFullFromEditedBlocks();
        renderNoteEditor();
      });
      const leftHandle = document.createElement('div');
      leftHandle.className = 'ne-triplet-handle ne-triplet-handle-left';
      leftHandle.dataset.tooltip = `Start\nDrag to resize the start of this range.`;
      seg.appendChild(leftHandle);
      const rightHandle = document.createElement('div');
      rightHandle.className = 'ne-triplet-handle ne-triplet-handle-right';
      rightHandle.dataset.tooltip = `End\nDrag to resize the end of this range.`;
      seg.appendChild(rightHandle);
      attachTripletRangeResize(leftHandle, 'left', ctrlRow, block, engineGroup, range, pxPerBeat, g, blockBeats);
      attachTripletRangeResize(rightHandle, 'right', ctrlRow, block, engineGroup, range, pxPerBeat, g, blockBeats);
      ctrlRow.appendChild(seg);
    });

    attachTripletRangeDrag(ctrlRow, block, numerator, pxPerBeat, g, engineGroup, blockBeats);
    rowsInner.appendChild(ctrlRow);
    rows++;
  }

  return rows;
}

// Read-only "Phrase" row under an expanded group: the first phrase (bars generated on their own)
// as a solid segment, the bars that merely repeat it as dimmer segments.
function neRenderPhraseRow(t, ctx, refs) {
  const { block, g, numerator, totalBeats, blockBeats, pxPerBeat, totalW } = ctx;
  const { labelsCol, rowsInner } = refs;
  const engineGroup = engineGroupKeyForTrack(t);
  const phraseKey = t.role === 'custom' ? customGroupId(t.id) : t.group;
  const phraseBars = getPhraseLength(block, phraseKey);
  const phraseBeats = phraseBars * numerator;
  const blockBars = Math.max(1, block.length || 1);
  const handEdited = !!(block.lockedGroups && block.lockedGroups[engineGroup]);
  const tip = `Phrase Length\n${phraseBars} bar${phraseBars === 1 ? '' : 's'} are played before this instrument's pattern repeats. Drag the right edge of the first segment to change it.` +
    (handEdited ? ' This group was edited by hand: discard the edits (lock icon) before changing the phrase.' : '');

  const label = document.createElement('div');
  label.className = 'ne-row-label ne-control-row-label';
  label.innerHTML = '<span>Phrase Length</span>';
  label.dataset.tooltip = tip;
  labelsCol.appendChild(label);

  const row = document.createElement('div');
  row.className = 'ne-row ne-control-row ne-phrase-row' + (handEdited ? ' ne-phrase-edited' : '');
  row.style.width = totalW + 'px';
  row.dataset.tooltip = tip;
  neDrawRowGrid(row, totalBeats, pxPerBeat, g.snap, numerator);
  for (let start = 0, k = 0; start < blockBeats; start += phraseBeats, k++) {
    const seg = document.createElement('div');
    seg.className = 'ne-phrase-seg' + (k === 0 ? ' ne-phrase-first' : '');
    seg.style.left = (NE_LEFT_PAD + start * pxPerBeat) + 'px';
    seg.style.width = (Math.min(phraseBeats, blockBeats - start) * pxPerBeat - 2) + 'px';
    seg.textContent = k === 0 ? `${phraseBars}` : '→';
    if (k === 0) {
      const handle = document.createElement('div');
      handle.className = 'ne-phrase-handle';
      handle.dataset.tooltip = tip;
      attachPhraseHandleDrag(handle, seg, block, t, { numerator, pxPerBeat, blockBars, phraseBars });
      seg.appendChild(handle);
    }
    row.appendChild(seg);
  }
  rowsInner.appendChild(row);
  return 1;
}

// Dragging the first segment's right edge sets the instrument's phrase in whole bars; the group is regenerated on release.
function attachPhraseHandleDrag(handle, seg, block, track, m) {
  const pxPerBar = m.pxPerBeat * m.numerator;
  let drag = null;
  handle.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault(); e.stopPropagation();
    drag = { startX: e.clientX, bars: m.phraseBars };
    handle.setPointerCapture(e.pointerId);
  });
  handle.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const dx = (e.clientX - drag.startX) / currentUIScale();
    drag.bars = Math.max(1, Math.min(m.blockBars, Math.round(m.phraseBars + dx / pxPerBar)));
    seg.style.width = (drag.bars * pxPerBar - 2) + 'px';
    seg.firstChild.textContent = String(drag.bars);
  });
  const finish = (e) => {
    if (!drag) return;
    const bars = drag.bars;
    drag = null;
    try { handle.releasePointerCapture(e.pointerId); } catch (_) {}
    neCommitInstrumentPhrase(block, track, bars);
  };
  handle.addEventListener('pointerup', finish);
  handle.addEventListener('pointercancel', () => { drag = null; renderNoteEditor(); });
  handle.addEventListener('click', (e) => e.stopPropagation());
}

const NE_REGENERATE_BY_GROUP = {
  kick: () => regenerateKickGroup({ suppressUndo: true }),
  snare: () => regenerateSnareGroup({ suppressUndo: true }),
  hihat: () => regenerateHihatGroup({ suppressUndo: true }),
  ride: () => regenerateRideGroup({ suppressUndo: true }),
  toms: () => regenerateTomsGroup({ suppressUndo: true }),
  cymbals: () => regenerateCymbalsGroup({ suppressUndo: true }),
};

async function neCommitInstrumentPhrase(block, track, bars) {
  const isCustom = track.role === 'custom';
  const key = isCustom ? customGroupId(track.id) : track.group;
  const engineGroup = engineGroupKeyForTrack(track);
  if (getPhraseLength(block, key) === bars) { renderNoteEditor(); return; }
  if (block.lockedGroups && block.lockedGroups[engineGroup]) {
    neMsg('This group was edited by hand — discard the edits (lock icon) before changing the phrase.');
    renderNoteEditor();
    return;
  }
  const snap = () => JSON.stringify({ data: block.data, locked: block.lockedGroups || {}, knobs: block.knobs || {}, phrase: block.phraseLength || {} });
  const before = snap();
  block.phraseLength = block.phraseLength || {};
  block.phraseLength[key] = bars;
  if (isCustom) { neSetActiveCustomTrack(track.id); await regenerateCustomGroup({ suppressUndo: true }); }
  else if (NE_REGENERATE_BY_GROUP[engineGroup]) await NE_REGENERATE_BY_GROUP[engineGroup]();
  const after = snap();
  const restore = (str) => {
    const s = JSON.parse(str);
    block.data = s.data; block.lockedGroups = s.locked; block.knobs = s.knobs; block.phraseLength = s.phrase;
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    updateGroupLockUI();
  };
  pushUndo(`Phrase Length (${isCustom ? track.label : (GROUPS[track.group] || track.group)})`, () => restore(before), () => restore(after));
}

// Renders one leaf articulation row.
function neRenderTrackLeafRow(t, ctx, refs) {
  const { block, g, numerator, totalBeats, blockBeats, pxPerBeat, totalW, breakLocked, grooveBeats } = ctx;
  const { labelsCol, rowsInner, rowsCol } = refs;

  // Fill/Break blocks (and any regeneration that replaces block.data) can omit an
  // articulation track's key entirely — guarantee it exists so rendering and editing work.
  if (!block.data[t.id]) block.data[t.id] = [];

  const isFocused = t.id === focusedTrackId;
  const isMuted = mutedInstruments.has(t.id);
  const isSoloed = soloedInstruments.has(t.id);
  const isSilencedBySolo = soloedInstruments.size > 0 && !isSoloed;
  const displayLabel = t.group ? t.groupLabel : t.label;

  const label = document.createElement('div');
  label.className = 'ne-row-label' + (isFocused?' focused':'') + (isMuted?' muted':'') + (isSoloed?' soloed':'') + (!isMuted && isSilencedBySolo?' solo-silenced':'') + (t.role==='custom'?' custom':'') + (t.group || t.role==='custom' ? ' ne-sub-label':'');
  const nameSpan = document.createElement('span'); nameSpan.textContent = displayLabel;
  label.appendChild(nameSpan);

  // Custom tracks: delete lives in the subtab, behind the name — expanding the group is already
  // a deliberate step, so no button is needed on the header row.
  if (t.role === 'custom') {
    const delBtn = document.createElement('span'); delBtn.className='ne-delete-btn'; delBtn.textContent='✕';
    delBtn.dataset.tooltip = 'Delete\nRemoves this track and its notes from all blocks.';
    delBtn.addEventListener('click', async (e) => { e.stopPropagation(); await neDeleteCustomTrack(t); });
    label.appendChild(delBtn);
  }

  // Articulation checkbox.
  const groupSiblings = t.group ? trackList.filter(gt => gt.group === t.group) : [];
  if (groupSiblings.length > 1) {
    const engineGroup = UI_GROUP_TO_ENGINE_GROUP[t.group];
    const handEdited = !!(engineGroup && block.lockedGroups && block.lockedGroups[engineGroup]);
    const enabled = isArticulationEnabled(block, t.id);
    const wrap = document.createElement('span');
    wrap.className = 'ne-articulation-enable-wrap';
    wrap.style.cursor = handEdited ? 'not-allowed' : 'pointer';
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.className = 'ne-articulation-enable-cb';
    cb.checked = enabled;
    cb.disabled = handEdited;
    cb.dataset.trackId = t.id;
    cb.dataset.tooltip = handEdited
      ? 'Enabled\nLocked because this group was hand-edited. Regenerate it via the lock icon on its knobs to change which articulations are enabled.'
      : 'Enabled\nToggles this articulation for Up/Down cycling, the default click/paint target, and variation/turnaround fills. At least one per group must stay enabled — existing notes remain editable either way. Drag across several checkboxes to set them all at once.';
    wrap.dataset.tooltip = cb.dataset.tooltip;
    wrap.appendChild(cb);
    label.insertBefore(wrap, nameSpan);
    attachArticulationCheckboxPaintDrag(cb, block, t.id);
  }
  if (showGmNotes) {
    const gmInput = document.createElement('input');
    gmInput.type = 'number';
    gmInput.className = 'ne-gm-input';
    gmInput.value = t.gm;
    gmInput.dataset.tooltip = 'GM Note\nSets the MIDI/GM note number.';
    gmInput.addEventListener('click', e => e.stopPropagation());
    gmInput.addEventListener('pointerdown', e => e.stopPropagation());
    gmInput.addEventListener('change', () => {
      const v = parseInt(gmInput.value);
      if (!isNaN(v)) { t.gm = v; rebuildFullFromEditedBlocks(); }
    });
    label.appendChild(gmInput);
  }
  label.dataset.tooltip = 'Articulation\nClick to preview and focus this row in the velocity lane.';
  label.addEventListener('click', () => { nePreviewTrack(t.id, 100); focusedTrackId = t.id; if (t.role === 'custom') neSetActiveCustomTrack(t.id); renderNoteEditor(); });
  labelsCol.appendChild(label);

  const row = document.createElement('div');
  row.className = 'ne-row' + (isFocused?' focused':'');
  row.dataset.trackId = t.id;
  row.style.width = totalW+'px';
  const engineGroup = engineGroupKeyForTrack(t);
  neDrawRowGrid(row, totalBeats, pxPerBeat, g.snap, numerator, block.tripletLayers && engineGroup && block.tripletLayers[engineGroup], engineGroup, g);

  row.addEventListener('click', (e) => {
    if (suppressNextRowClick) { suppressNextRowClick = false; return; }
    if (e.target !== row) return;
    focusedTrackId = t.id;
    selectedNotes.clear();
    const rect = rowsCol.getBoundingClientRect();
    const x = (e.clientX - rect.left) / currentUIScale() + rowsCol.scrollLeft;
    let beat = Math.max(0, Math.min(totalBeats-0.01, (x-NE_LEFT_PAD)/pxPerBeat));
    beat = neQuantizeForTrack(beat, g, block, t.id);
    if (beat >= blockBeats) { neMsg('Beyond this block\'s length — grow Length (Bars) to use that space.'); return; }
    if (breakLocked && beat < grooveBeats) { neMsg('Copied bars are locked — see the Break tab.'); return; }
    const arr = block.data[t.id];
    const collide = neFindCollision(arr, beat, -1);
    const beforeLocked = snapshotLocked(block);
    let undoFn, redoFn, previewVel;
    if (collide >= 0) {
      const noteRef = arr[collide], oldBeat = noteRef.beat;
      if (noteRef.turnaround) delete noteRef.turnaround;
      noteRef.beat = beat;
      previewVel = noteRef.vel;
      neMsg('Replaced existing note');
      undoFn = () => { noteRef.beat = oldBeat; arr.sort((a,b)=>a.beat-b.beat); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
      redoFn = () => { noteRef.beat = beat; arr.sort((a,b)=>a.beat-b.beat); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
    } else {
      const newNote = { beat, vel:100 };
      arr.push(newNote);
      previewVel = newNote.vel;
      undoFn = () => { const i = arr.indexOf(newNote); if (i>=0) arr.splice(i,1); block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
      redoFn = () => { if (arr.indexOf(newNote)<0) { arr.push(newNote); arr.sort((a,b)=>a.beat-b.beat); } block.generated=false; rebuildFullFromEditedBlocks(); renderNoteEditor(); };
    }
    nePreviewTrack(t.id, previewVel);
    markTrackManuallyEdited(block, t.id);
    const afterLocked = snapshotLocked(block);
    const plainUndoFn = undoFn, plainRedoFn = redoFn;
    undoFn = () => { plainUndoFn(); restoreLocked(block, beforeLocked); };
    redoFn = () => { plainRedoFn(); restoreLocked(block, afterLocked); };
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    pushUndo(collide >= 0 ? 'Move note' : 'Add note', undoFn, redoFn);
  });

  row.addEventListener('contextmenu', (e) => {
    if (e.target !== row) return;
    e.preventDefault();
    if (selectedNotes.size > 0) { selectedNotes.clear(); renderNoteEditor(); }
  });

  attachRowPaintDrag(row, block, t, ctx, refs);

  block.data[t.id].forEach((note, ni) => {
    const dia = document.createElement('div');
    const selKey = t.id+'|'+note.beat;
    const noteLocked = breakLocked && note.beat < grooveBeats;
    dia.className = 'ne-note' + (selectedNotes.has(selKey) ? ' selected' : '') + (noteLocked ? ' ne-note-locked' : '') + (note.turnaround ? ' ne-note-turnaround' : '');
    dia.dataset.selKey = selKey;
    dia.dataset.tooltip = '';
    dia.style.left = (NE_LEFT_PAD + note.beat*pxPerBeat)+'px';
    dia.style.top = (NE_ROW_H/2)+'px';
    dia.style.background = noteLocked ? 'var(--muted)' : fadedColor(t.color, note.vel/127);
    if (!noteLocked) {
      dia.addEventListener('contextmenu', (e) => {
        e.preventDefault(); e.stopPropagation();
        if (suppressNextContextMenu) { suppressNextContextMenu = false; return; }
        const beforeLocked = snapshotLocked(block);
        const removed = block.data[t.id].splice(ni,1)[0];
        markTrackManuallyEdited(block, t.id);
        const afterLocked = snapshotLocked(block);
        block.generated = false;
        rebuildFullFromEditedBlocks();
        renderNoteEditor();
        pushUndo('Delete note',
          () => {
            block.data[t.id].push(removed);
            block.data[t.id].sort((a,b) => a.beat-b.beat);
            restoreLocked(block, beforeLocked);
            block.generated = false;
            rebuildFullFromEditedBlocks();
            renderNoteEditor();
          },
          () => {
            const i = block.data[t.id].indexOf(removed);
            if (i >= 0) block.data[t.id].splice(i, 1);
            restoreLocked(block, afterLocked);
            block.generated = false;
            rebuildFullFromEditedBlocks();
            renderNoteEditor();
          }
        );
      });
      attachNoteDrag(dia, block, t.id, ni, numerator, g, pxPerBeat);
    }
    row.appendChild(dia);
  });

  rowsInner.appendChild(row);
  return 1;
}

// Sync zoom header with neZoom.
function neSyncZoomHeader() {
  const slider = document.getElementById('neZoomSlider');
  const val = document.getElementById('neZoomVal');
  if (slider) slider.value = String(neZoom);
  if (val) val.textContent = Math.round(neZoom * 100) + '%';
}

// Attach zoom (Ctrl+scroll) and pan (middle-drag) to grid.
function neAttachGridZoomAndPan(rowsCol) {
  rowsCol.addEventListener('wheel', (e) => {
    if (!e.ctrlKey && !e.metaKey) return;
    e.preventDefault();
    const rect = rowsCol.getBoundingClientRect();
    const cursorOffsetInViewport = (e.clientX - rect.left) / currentUIScale();
    const beatAtCursor = (cursorOffsetInViewport + rowsCol.scrollLeft - NE_LEFT_PAD) / neCurrentPxPerBeat;
    const dir = e.deltaY < 0 ? 1 : -1;
    const newZoom = Math.max(NE_ZOOM_MIN, Math.min(NE_ZOOM_MAX, neZoom * (1 + dir * 0.1)));
    if (newZoom === neZoom) return;
    neZoom = newZoom;
    neAutoFitZoomBlockId = null;
    renderNoteEditor();
    const freshRowsCol = document.querySelector('.ne-rows');
    if (freshRowsCol) freshRowsCol.scrollLeft = Math.max(0, beatAtCursor * neCurrentPxPerBeat + NE_LEFT_PAD - cursorOffsetInViewport);
  }, { passive: false });

  let panDrag = null;
  rowsCol.addEventListener('pointerdown', (e) => {
    if (e.button !== 1) return;
    e.preventDefault();
    panDrag = { startX: e.clientX, startScroll: rowsCol.scrollLeft };
    rowsCol.setPointerCapture(e.pointerId);
    rowsCol.style.cursor = 'grabbing';
  });
  rowsCol.addEventListener('pointermove', (e) => {
    if (!panDrag) return;
    rowsCol.scrollLeft = panDrag.startScroll - (e.clientX - panDrag.startX);
  });
  const endPan = (e) => { if (!panDrag) return; panDrag = null; rowsCol.style.cursor = ''; try { rowsCol.releasePointerCapture(e.pointerId); } catch(_){} };
  rowsCol.addEventListener('pointerup', endPan);
  rowsCol.addEventListener('pointercancel', endPan);
  rowsCol.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
}

// Velocity lane.
function neBuildVelocityLane(ctx) {
  const { block, g, totalBeats, pxPerBeat, totalW, isBreak } = ctx;

  let wheelCommitTimer = null;
  function debounceWheelCommit(commitFn) {
    clearTimeout(wheelCommitTimer);
    wheelCommitTimer = setTimeout(commitFn, 220);
  }

  const velWrap = document.createElement('div');
  velWrap.className = 'ne-vel-wrap';
  const velLabels = document.createElement('div');
  velLabels.className = 'ne-vel-labels';

  // Determine scope: filter > selection > focused track > all tracks.
  const filterTrackIds = neVelFilterTrackIds();
  const scopeTrackIds = filterTrackIds
    ? filterTrackIds
    : selectedNotes.size > 0
      ? [...new Set([...selectedNotes].map(k => k.split('|')[0]))]
      : (focusedTrackId ? [focusedTrackId] : trackList.map(t => t.id));

  let activeItems = [];
  if (selectedNotes.size > 0) {
    const selTrackIds = new Set([...selectedNotes].map(k => k.split('|')[0]));
    selTrackIds.forEach(tid => {
      if (!scopeTrackIds.includes(tid)) return;
      const tr = trackList.find(t => t.id === tid);
      (block.data[tid]||[]).forEach((note, ni) => {
        if (selectedNotes.has(tid+'|'+note.beat)) activeItems.push({ trackId: tid, track: tr, note, ni });
      });
    });
  }
  const activeKeys = new Set(activeItems.map(it => it.trackId+'|'+it.note.beat));
  const grayItems = [];
  scopeTrackIds.forEach(tid => {
    const tr = trackList.find(t => t.id === tid);
    (block.data[tid]||[]).forEach((note, ni) => {
      if (activeKeys.has(tid+'|'+note.beat)) return;
      grayItems.push({ trackId: tid, track: tr, note, ni });
    });
  });

  // Reset nudge state if selection changed.
  const velSelKey = block.id + '::' + [...activeKeys].sort().join(',');
  if (velSelKey !== velNudgeSelKey) {
    velNudgeSelKey = velSelKey;
    velNudgeVals = { vel: 0, dyn: 0, rnd: 0 };
    velNudgeBaseline = activeItems.map(it => it.note.vel);
    velCommitBefore = null;
  }

  velLabels.innerHTML = `
    <div class="ne-vel-mini-ctrl ne-vel-nudge-ctrl">
      <span class="ne-vel-mini-label" data-tooltip="Velocity&#10;Shifts the selected note(s)&rsquo; velocity by a fixed amount, up or down. Requires a selection.">Vel</span>
      <input type="range" class="ne-vel-mini-slider" id="veAdjVelSlider" data-tooltip="Velocity&#10;Shifts the selected note(s)&rsquo; velocity by a fixed amount, up or down. Requires a selection." min="-63" max="63" value="${velNudgeVals.vel}" step="1">
      <input type="number" class="ne-vel-mini-val" id="veAdjVelNum" data-tooltip="Velocity&#10;Shifts the selected note(s)&rsquo; velocity by a fixed amount, up or down. Requires a selection." min="-127" max="127" value="${velNudgeVals.vel}">
    </div>
    <div class="ne-vel-mini-ctrl ne-vel-nudge-ctrl">
      <span class="ne-vel-mini-label" data-tooltip="Dynamics&#10;Spreads or compresses the selected note(s)&rsquo; velocities around their average — positive adds contrast, negative flattens. Requires a selection.">Dyn</span>
      <input type="range" class="ne-vel-mini-slider" id="veAdjDynSlider" data-tooltip="Dynamics&#10;Spreads or compresses the selected note(s)&rsquo; velocities around their average — positive adds contrast, negative flattens. Requires a selection." min="-100" max="100" value="${velNudgeVals.dyn}" step="1">
      <input type="number" class="ne-vel-mini-val" id="veAdjDynNum" data-tooltip="Dynamics&#10;Spreads or compresses the selected note(s)&rsquo; velocities around their average — positive adds contrast, negative flattens. Requires a selection." min="-100" max="100" value="${velNudgeVals.dyn}">
    </div>
    <div class="ne-vel-mini-ctrl ne-vel-nudge-ctrl">
      <span class="ne-vel-mini-label" data-tooltip="Randomize&#10;Randomizes the selected note(s)&rsquo; velocity by up to ± this amount, per note. Requires a selection.">Rnd</span>
      <input type="range" class="ne-vel-mini-slider" id="veAdjRndSlider" data-tooltip="Randomize&#10;Randomizes the selected note(s)&rsquo; velocity by up to ± this amount, per note. Requires a selection." min="0" max="100" value="${velNudgeVals.rnd}" step="1">
      <input type="number" class="ne-vel-mini-val" id="veAdjRndNum" data-tooltip="Randomize&#10;Randomizes the selected note(s)&rsquo; velocity by up to ± this amount, per note. Requires a selection." min="0" max="100" value="${velNudgeVals.rnd}">
    </div>
  `;
  velLabels.insertBefore(neBuildVelFilterSelect(), velLabels.firstChild);

  // Build control (for regular blocks only).
  const showBuild = !isBreak && !block.isFillBlock;
  const buildCtrl = document.createElement('div');
  buildCtrl.className = 'ne-vel-mini-ctrl';
  buildCtrl.style.cssText = 'border-top:1px solid var(--border); margin-top:6px; padding-top:6px;';
  buildCtrl.innerHTML = `
    <span class="ne-vel-mini-label" data-tooltip="Build&#10;Ramps velocity toward the end of this block. 0 = straight through; raise it for a build-up.">Build</span>
    <input type="range" class="ne-vel-mini-slider" id="veBuildSlider" data-tooltip="Build&#10;Ramps velocity toward the end of this block. 0 = straight through; raise it for a build-up." min="0" max="100" value="${Math.round((block.velocityBuild || 0) * 100)}" step="5">
    <input type="number" class="ne-vel-mini-val" id="veBuildNum" data-tooltip="Build&#10;Ramps velocity toward the end of this block. 0 = straight through; raise it for a build-up." min="0" max="100" value="${Math.round((block.velocityBuild || 0) * 100)}">
  `;
  velLabels.appendChild(buildCtrl);
  if (showBuild) {
    const buildSlider = buildCtrl.querySelector('#veBuildSlider');
    const buildNum = buildCtrl.querySelector('#veBuildNum');
    const commitBuild = (val) => neUndoable('Velocity build', () => {
      block.velocityBuild = val / 100;
      block.generated = false;
      applyAllUnlockedGroups(block, g);
    });
    buildSlider.addEventListener('input', () => { buildNum.value = buildSlider.value; });
    // Ctrl+Click → Reset to 0 (straight through).
    buildSlider.addEventListener('pointerdown', (e) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      if (buildSlider.disabled) return;
      buildSlider.value = '0'; buildNum.value = '0'; commitBuild(0);
    });
    buildSlider.addEventListener('change', () => commitBuild(+buildSlider.value));
    buildSlider.addEventListener('wheel', (e) => {
      if (buildSlider.disabled) return;
      e.preventDefault(); e.stopPropagation();
      const step = parseFloat(buildSlider.step) || 1, lo = parseFloat(buildSlider.min) || 0, hi = parseFloat(buildSlider.max) || 100;
      const dir = e.deltaY < 0 ? 1 : -1;
      const nv = Math.min(hi, Math.max(lo, Math.round((parseFloat(buildSlider.value) + dir*step) / step) * step));
      if (+buildSlider.value === nv) return;
      buildSlider.value = nv; buildNum.value = nv;
      debounceWheelCommit(() => commitBuild(+buildSlider.value));
    }, { passive: false });
    buildNum.addEventListener('change', () => {
      const val = Math.max(0, Math.min(100, +buildNum.value || 0));
      buildSlider.value = String(val);
      commitBuild(val);
    });
    const groupHasNotes = (gr) => tracksInEngineGroup(gr).some(tid => (block.data[tid] || []).length > 0);
    const fullyLocked = ALL_ENGINE_GROUPS.length > 0 && ALL_ENGINE_GROUPS.every(gr => (block.lockedGroups && block.lockedGroups[gr]) || !groupHasNotes(gr));
    buildSlider.disabled = fullyLocked;
    buildNum.disabled = fullyLocked;
  }

  // Measure lane height.
  velLabels.style.visibility = 'hidden';
  document.body.appendChild(velLabels);
  NE_VEL_H = Math.max(80, velLabels.offsetHeight);
  document.body.removeChild(velLabels);
  velLabels.style.visibility = '';
  if (!showBuild) buildCtrl.remove();

  const velLane = document.createElement('div');
  velLane.className = 'ne-vel-lane';
  const velInner = document.createElement('div');
  velInner.style.cssText = `position:relative; width:${totalW}px; height:${NE_VEL_H}px;`;
  velInner.dataset.tooltip = 'Velocity\nDrag to paint velocities freehand · Shift+drag for a straight line between two points.';
  for (let x = 0; x <= totalBeats; x += g.snap) {
    const sl = document.createElement('div');
    sl.className = 'ne-snap-line'; sl.style.left = (NE_LEFT_PAD + x*pxPerBeat)+'px';
    velInner.appendChild(sl);
  }
  const baseline = document.createElement('div');
  baseline.style.cssText = 'position:absolute;bottom:0;left:0;right:0;height:1px;background:var(--border);';
  velInner.appendChild(baseline);

  // Gray context bars.
  grayItems.forEach(({trackId, note}) => {
    const cx = NE_LEFT_PAD + note.beat*pxPerBeat;
    const h = (note.vel/127)*(NE_VEL_H-14);
    const line = document.createElement('div');
    line.className = 'ne-vel-line ne-vel-line-gray';
    line.style.cssText += `left:${cx}px; height:${h}px;`;
    line.dataset.tooltip = 'Velocity\nClick to select · then drag to change it.';
    velInner.appendChild(line);
    const handle = document.createElement('div');
    handle.className = 'ne-vel-handle ne-vel-handle-gray';
    handle.style.left = (cx-5.5)+'px'; handle.style.bottom = (h-5.5)+'px';
    handle.dataset.tooltip = 'Velocity\nClick to select · then drag to change it.';
    const selectThis = (e) => { e.stopPropagation(); selectedNotes.add(trackId+'|'+note.beat); renderNoteEditor(); };
    handle.addEventListener('click', selectThis);
    line.addEventListener('click', selectThis);
    velInner.appendChild(handle);
  });

  const velRefs = [];
  activeItems.forEach(({trackId, track, note, ni}) => {
    const cx = NE_LEFT_PAD + note.beat*pxPerBeat;
    const h = (note.vel/127)*(NE_VEL_H-14);
    const line = document.createElement('div');
    line.className = 'ne-vel-line';
    line.style.cssText += `left:${cx}px; height:${h}px; background:${track.color};`;
    line.dataset.tooltip = activeItems.length > 1 ? 'Velocity\nDrag vertically to change the velocity of the whole selection.' : 'Velocity\nDrag vertically to change its velocity.';
    velInner.appendChild(line);
    const handle = document.createElement('div');
    handle.className = 'ne-vel-handle';
    handle.style.left = (cx-5.5)+'px'; handle.style.bottom = (h-5.5)+'px';
    handle.style.background = track.color;
    handle.dataset.tooltip = activeItems.length > 1 ? 'Velocity\nDrag vertically to change the velocity of the whole selection.' : 'Velocity\nDrag vertically to change its velocity.';
    velInner.appendChild(handle);
    velRefs.push({ trackId, note, ni, line, handle, cx });
  });
  velRefs.forEach(ref => attachVelDrag(ref, velRefs, block));
  attachVelLanePaint(velInner, velRefs, block);
  velLane.appendChild(velInner);
  velWrap.appendChild(velLabels); velWrap.appendChild(velLane);

  // Nudge controls.
  function applyVelocityShift(baseline, delta) {
    activeItems.forEach((it, i) => { it.note.vel = Math.round(Math.max(1, Math.min(127, baseline[i] + delta))); });
  }
  function applyDynamics(baseline, factorPct) {
    const mean = baseline.reduce((a,b)=>a+b, 0) / baseline.length;
    const factor = 1 + (factorPct/100);
    activeItems.forEach((it, i) => { it.note.vel = Math.round(Math.max(1, Math.min(127, mean + (baseline[i]-mean)*factor))); });
  }
  function applyRandomize(baseline, amount) {
    const rng = new RNG(Date.now() ^ Math.floor(Math.random()*1e9));
    activeItems.forEach((it, i) => { it.note.vel = Math.round(Math.max(1, Math.min(127, baseline[i] + rng.int(-amount, amount)))); });
  }
  function refreshVelVisuals() {
    velRefs.forEach(r => {
      const h = (r.note.vel/127)*(NE_VEL_H-14);
      r.line.style.height = h+'px';
      r.handle.style.bottom = (h-5.5)+'px';
    });
  }
  function commitVelChange() {
    if (!velCommitBefore) return;
    const touched = new Set(activeItems.map(it => it.trackId));
    touched.forEach(tid => markTrackManuallyEdited(block, tid));
    block.generated = false;
    const beforeData = velCommitBefore.data, beforeLocked = velCommitBefore.locked;
    const afterData = JSON.stringify(block.data), afterLocked = JSON.stringify(block.lockedGroups || {});
    velCommitBefore = null;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    updateGroupLockUI();
    const restore = (dataStr, lockedStr) => {
      block.data = JSON.parse(dataStr);
      block.lockedGroups = JSON.parse(lockedStr);
      block.generated = false;
      velNudgeSelKey = null;
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
      updateGroupLockUI();
    };
    pushUndo('Nudge velocity', () => restore(beforeData, beforeLocked), () => restore(afterData, afterLocked));
  }
  function wireNudgeControl(sliderId, numId, applyFn, valKey) {
    const slider = velLabels.querySelector('#'+sliderId);
    const num = velLabels.querySelector('#'+numId);
    const preview = (val) => {
      if (!velCommitBefore) velCommitBefore = { data: JSON.stringify(block.data), locked: JSON.stringify(block.lockedGroups || {}) };
      applyFn(velNudgeBaseline, val); refreshVelVisuals(); num.value = val; slider.value = val; velNudgeVals[valKey] = val;
    };
    slider.addEventListener('input', () => preview(+slider.value));
    // Ctrl+Click → Reset to neutral (0 = no adjustment).
    slider.addEventListener('pointerdown', (e) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      if (slider.disabled) return;
      preview(0); commitVelChange();
    });
    slider.addEventListener('pointerup', () => commitVelChange());
    num.addEventListener('change', () => {
      const val = Math.max(+num.min, Math.min(+num.max, +num.value || 0));
      preview(val); commitVelChange();
    });
    slider.addEventListener('wheel', (e) => {
      if (slider.disabled || !activeItems.length) return;
      e.preventDefault(); e.stopPropagation();
      const step = parseFloat(slider.step) || 1, lo = parseFloat(slider.min) || 0, hi = parseFloat(slider.max) || 100;
      const dir = e.deltaY < 0 ? 1 : -1;
      const val = Math.max(lo, Math.min(hi, velNudgeVals[valKey] + dir*step));
      preview(val);
      debounceWheelCommit(commitVelChange);
    }, { passive: false });
  }
  if (activeItems.length > 0) {
    wireNudgeControl('veAdjVelSlider', 'veAdjVelNum', applyVelocityShift, 'vel');
    wireNudgeControl('veAdjDynSlider', 'veAdjDynNum', applyDynamics, 'dyn');
    wireNudgeControl('veAdjRndSlider', 'veAdjRndNum', applyRandomize, 'rnd');
  } else {
    velLabels.querySelectorAll('.ne-vel-nudge-ctrl input').forEach(i => i.disabled = true);
  }

  return { velWrap, velLane };
}