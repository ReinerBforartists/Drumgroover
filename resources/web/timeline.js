// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// RENDERING — Timeline (DAW-style: ruler + freeform draggable track)
// =========================================

// Fallback baseline when song is empty; otherwise superseded by fit-to-content.
const PX_PER_BAR = 40;
// Padding past song end to allow dropping next block. Not counted in fit baseline.
const TL_ENDLESS_EXTRA_BARS = 8;
const TL_ZOOM_MIN = 0.25, TL_ZOOM_MAX = 4; // Same range as Note Editor.
// One empty bar before bar 1 to give the playhead marker room (otherwise clipped).
const TL_LEAD_IN_BARS = 1;

// Height of the clip lane; the user resizes it with the handle under the timeline.
const TL_TRACK_H_MIN = 44, TL_TRACK_H_MAX = 400, TL_TRACK_H_DEFAULT = 80;
const TL_CLIP_NAME_H = 16;   // keep in sync with .tl-clip-name
const TL_CLIP_VPAD = 12;     // .tl-track top/bottom inset (4+4) + clip border (2+2)
let tlTrackH = TL_TRACK_H_DEFAULT;
try {
  const saved = parseInt(localStorage.getItem('dgg.tlTrackH.v2'), 10);
  if (saved >= TL_TRACK_H_MIN && saved <= TL_TRACK_H_MAX) tlTrackH = saved;
} catch (_) {}

let tlZoom = 1; // Multiplier on tlFitReferencePxPerBar.
// Base px/bar for zoom=1. Set only by explicit fit actions; content changes do not rescale.
let tlFitReferencePxPerBar = PX_PER_BAR;

// Shared time axis variables (in beats).
let tlPlayheadBeat = 0;
// Where the playhead is drawn: the start position (tlPlayheadBeat) while idle, the playback position while playing.
let tlShownBeat = 0;

function tlUpdatePlayheadEl() {
  const el = document.getElementById('tlPlayhead');
  if (!el) return;
  const pxPerBeat = tlCurrentPxPerBar / readGrid().numerator;
  el.style.left = (tlShownBeat * pxPerBeat + tlLeadInPx) + 'px';   // the height is the container's (CSS 100%)
}
let tlLoopStart = null, tlLoopEnd = null;

// Actual px/bar after last render; exposed for external sync.
let tlCurrentPxPerBar = PX_PER_BAR;
// Pixel offset for TL_LEAD_IN_BARS; set each render pass.
let tlLeadInPx = 0;

// tlSelectedIndices: set of selected slot indices (source of truth).
// selectedSegmentIdx is sole index if exactly one selected, else null.
// tlSelectAnchorIdx for shift-click range.
let tlSelectedIndices = new Set();
let tlSelectAnchorIdx = null;

// Replaces the whole selection and keeps selectedSegmentIdx in sync.
function tlSetSelection(indices) {
  tlSelectedIndices = new Set(indices);
  selectedSegmentIdx = (tlSelectedIndices.size === 1) ? [...tlSelectedIndices][0] : null;
}

// Ctrl+A: select all clips.
function tlSelectAll() {
  tlSetSelection(songEngine.songSequence.map((_, i) => i));
  tlSelectAnchorIdx = tlSelectedIndices.size ? Math.max(...tlSelectedIndices) : null;
  renderTimeline(); renderBlockTabs();
}

// Alt+A / Select None.
function tlSelectNone() {
  tlSetSelection([]);
  renderTimeline(); renderBlockTabs();
}

// Ctrl+I / Invert Selection.
function tlInvertSelection() {
  const inverted = songEngine.songSequence
    .map((_, i) => i)
    .filter(i => !tlSelectedIndices.has(i));
  tlSetSelection(inverted);
  renderTimeline(); renderBlockTabs();
}

// Compute px/bar for fit-to-content: fit real songEndBar into visible width with trailing reserve.
// Used only by fitTimelineZoom().
const TL_FIT_TRAILING_BARS = 4;
const TL_FIT_TRAILING_MIN_PX = 60;
function tlComputeFitPxPerBar(songEndBar) {
  const availWidthBase = Math.max(50, tlMain.clientWidth);
  if (songEndBar <= 0) return PX_PER_BAR;
  const naivePxPerBar = availWidthBase / (songEndBar + TL_FIT_TRAILING_BARS);
  const trailingPx = Math.max(TL_FIT_TRAILING_MIN_PX, TL_FIT_TRAILING_BARS * naivePxPerBar);
  const availWidthReal = Math.max(50, availWidthBase - trailingPx);
  return availWidthReal / songEndBar;
}

// Remove one or more slots (by index) in a single undo. Descending order avoids index shifting.
// Leaves gaps; zoom unchanged.
function tlDeleteSlots(indices) {
  const idxArr = [...indices].sort((a, b) => b - a);
  if (!idxArr.length) return;
  const names = idxArr.map(i => {
    const s = songEngine.songSequence[i];
    const b = s ? findBlock(s.blockId) : null;
    return b ? blockDisplayName(b) : '?';
  }).reverse();
  const label = idxArr.length > 1 ? `Remove ${idxArr.length} clips` : `Remove clip (${names[0]})`;
  tlSetSelection([]);
  withSongSequenceUndo(label, () => {
    idxArr.forEach(i => songEngine.removeSlot(i));
  });
}

// =========================================
// RENDERING — Timeline
// =========================================
function renderTimeline() {
  const savedScrollY = window.scrollY;
  const track = document.getElementById('tlTrack');
  const ruler = document.getElementById('tlRuler');
  track.innerHTML = ''; ruler.innerHTML = '';

  const g = readGrid();
  const numerator = g.numerator; // Shared time axis for the whole song.

  const songEndBar = songEngine.songSequence.length ? Math.max(...songEngine.songSequence.map(s => songEngine.slotEndBar(s))) : 0;
  const pxPerBar = tlFitReferencePxPerBar * tlZoom;
  tlCurrentPxPerBar = pxPerBar;
  const pxPerBeat = pxPerBar / numerator;
  tlLeadInPx = TL_LEAD_IN_BARS * pxPerBar;

  // Bars to fill viewport; independent of songEndBar to keep grid filled.
  const viewportBars = Math.max(0, Math.ceil((Math.max(50, tlMain.clientWidth) - tlLeadInPx) / pxPerBar));
  const totalBars = Math.max(songEndBar + TL_ENDLESS_EXTRA_BARS, viewportBars);
  const totalBeats = totalBars * numerator;
  const totalW = totalBeats * pxPerBeat + 20 + tlLeadInPx;
  track.style.width = totalW+'px';
  track.style.height = tlTrackH+'px';
  ruler.style.width = totalW+'px';
  attachTlResizeOnce();

  // Rows of the note preview: instruments used anywhere in the song, so every clip shares the same layout.
  const usedBlockIds = new Set(songEngine.songSequence.map(s => s.blockId));
  const previewTracks = trackList.filter(t => [...usedBlockIds].some(id => {
    const b = findBlock(id); return b && (b.data[t.id] || []).length;
  }));

  // Ruler — bold bars, sub-labels; auto-hiding based on density. Bars past song end dimmed.
  const showBeats = pxPerBeat >= 30;
  // Label stride: skip labels if too close.
  const MIN_BAR_LABEL_PX = 26;
  let barLabelStride = 1;
  while (barLabelStride * pxPerBar < MIN_BAR_LABEL_PX) barLabelStride *= 2;
  for (let gb = 0; gb < totalBeats; gb++) {
    const bar = Math.floor(gb / numerator);
    const bt = gb % numerator;
    if (bt > 0 && !showBeats) continue;
    if (bt === 0 && bar % barLabelStride !== 0) continue;
    const mark = document.createElement('div');
    mark.className = 'tl-ruler-mark' + (bt === 0 ? ' tl-ruler-bar' : '') + (bar >= songEndBar ? ' tl-ruler-endless' : '');
    mark.style.left = (gb*pxPerBeat + tlLeadInPx)+'px'; mark.style.width = pxPerBeat+'px';
    if (bt === 0) mark.textContent = String(bar+1);
    else mark.textContent = (bar+1) + '.' + (bt+1);
    ruler.appendChild(mark);
  }

  const beatAtRulerX = (clientX) => {
    const rect = ruler.getBoundingClientRect();
    const x = clientX - rect.left + ruler.scrollLeft - tlLeadInPx;
    return Math.max(0, Math.min(totalBeats, x / pxPerBeat));
  };
  // Scrubbing moves the playhead (the start position); the live playback position does not move it while it is dragged.
  let scrubbing = false;
  ruler.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    scrubbing = true;
    playheadScrubbing = performance.now();
    tlPlayheadBeat = beatAtRulerX(e.clientX);
    tlShownBeat = tlPlayheadBeat; tlUpdatePlayheadEl();
    ruler.setPointerCapture(e.pointerId);
  });
  ruler.addEventListener('pointermove', (e) => {
    if (!scrubbing) return;
    tlPlayheadBeat = beatAtRulerX(e.clientX);
    tlShownBeat = tlPlayheadBeat; tlUpdatePlayheadEl();
  });
  const finishScrub = () => {
    if (!scrubbing) return;
    scrubbing = false;
    playheadScrubbing = 0;
    requestPlaySeek('song', tlPlayheadBeat);   // during playback: continue from here, once, when the drag ends
    if (window.mxTimelineSetPlayhead) window.mxTimelineSetPlayhead(-1);   // the Mixer's copy of the timeline follows; no full redraw needed
  };
  ruler.addEventListener('pointerup', finishScrub);
  ruler.addEventListener('pointercancel', finishScrub);

  // Loop row — drag defines range; right-click clears it.
  const loopRuler = document.getElementById('tlLoopRuler');
  loopRuler.innerHTML = '';
  loopRuler.style.width = totalW+'px';
  loopRuler.dataset.tooltip = 'Loop Range\nDrag to set a loop range. Right-click to clear it.';
  const loopOverlay = document.createElement('div');
  loopOverlay.className = 'tl-loop-overlay';
  if (tlLoopStart != null) {
    loopOverlay.style.display = 'block';
    loopOverlay.style.left = (tlLoopStart*pxPerBeat + tlLeadInPx) + 'px';
    loopOverlay.style.width = ((tlLoopEnd-tlLoopStart)*pxPerBeat) + 'px';
  } else {
    loopOverlay.style.display = 'none';
  }
  ['start', 'end'].forEach(edge => {
    const h = document.createElement('div');
    h.className = 'loop-handle ' + edge; h.dataset.edge = edge;
    loopOverlay.appendChild(h);
  });
  loopRuler.appendChild(loopOverlay);
  const beatAtLoopRulerX = (clientX) => {
    const rect = loopRuler.getBoundingClientRect();
    const x = clientX - rect.left + loopRuler.scrollLeft - tlLeadInPx;
    return Math.max(0, Math.min(totalBeats, x / pxPerBeat));
  };
  let loopDrag = null;
  loopRuler.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    const handle = e.target.closest('.loop-handle');
    if (handle && tlLoopStart != null) {
      // Dragging an edge of the existing loop: only that edge moves (the loop stays at least one bar long).
      loopDrag = { edge: handle.dataset.edge, moved: true };
      loopRuler.setPointerCapture(e.pointerId);
      return;
    }
    loopDrag = { startBeat: beatAtLoopRulerX(e.clientX), moved: false };
    loopRuler.setPointerCapture(e.pointerId);
  });
  loopRuler.addEventListener('pointermove', (e) => {
    if (!loopDrag) return;
    const beat = beatAtLoopRulerX(e.clientX);
    if (loopDrag.edge) {
      const lo = loopDrag.edge === 'start' ? Math.min(beat, tlLoopEnd - numerator) : tlLoopStart;
      const hi = loopDrag.edge === 'end' ? Math.max(beat, tlLoopStart + numerator) : tlLoopEnd;
      loopDrag.lo = Math.max(0, lo); loopDrag.hi = hi;
      loopOverlay.style.left = (loopDrag.lo*pxPerBeat + tlLeadInPx) + 'px';
      loopOverlay.style.width = ((hi-loopDrag.lo)*pxPerBeat) + 'px';
      return;
    }
    if (Math.abs(beat - loopDrag.startBeat) * pxPerBeat > 4) loopDrag.moved = true;
    if (loopDrag.moved) {
      const lo = Math.min(loopDrag.startBeat, beat), hi = Math.max(loopDrag.startBeat, beat);
      loopOverlay.style.display = 'block';
      loopOverlay.style.left = (lo*pxPerBeat + tlLeadInPx) + 'px';
      loopOverlay.style.width = ((hi-lo)*pxPerBeat) + 'px';
    }
  });
  const finishLoopDrag = (e) => {
    if (!loopDrag) return;
    if (loopDrag.edge) {
      if (loopDrag.lo !== undefined) {
        tlLoopStart = loopDrag.lo; tlLoopEnd = loopDrag.hi;
        if (loopDrag.edge === 'start') tlPlayheadBeat = tlLoopStart;   // playback starts at the loop's start
      }
    } else if (loopDrag.moved) {
      const beat = beatAtLoopRulerX(e.clientX);
      const lo = Math.min(loopDrag.startBeat, beat), hi = Math.max(loopDrag.startBeat, beat);
      if (hi - lo >= numerator) { tlLoopStart = lo; tlLoopEnd = hi; tlPlayheadBeat = lo; } // At least one bar; snap playhead to loop start.
    }
    loopDrag = null;
    renderTimeline();
  };
  loopRuler.addEventListener('pointerup', finishLoopDrag);
  // Right-click clears the loop.
  loopRuler.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    tlLoopStart = null; tlLoopEnd = null;
    renderTimeline();
  });
  loopRuler.addEventListener('pointercancel', () => { loopDrag = null; });

  // Grid cells — one per bar. Bars past song end dimmed.
  for (let bar = 0; bar < totalBars; bar++) {
    const cell = document.createElement('div');
    cell.className = 'tl-grid-cell' + (bar >= songEndBar ? ' tl-grid-endless' : '');
    cell.style.left = (bar*numerator*pxPerBeat + tlLeadInPx)+'px';
    cell.style.width = (numerator*pxPerBeat)+'px';
    track.appendChild(cell);
  }

  // Render clips — width/position based on real bar span. Gaps are transparent.
  songEngine.songSequence.forEach((slot, idx) => {
    const block = findBlock(slot.blockId);
    const spanBars = songEngine.slotSpanBars(slot);
    const clip = document.createElement('div');
    clip.className = 'tl-clip' + (tlSelectedIndices.has(idx) ? ' selected' : '');
    clip.style.left = (slot.startBar*numerator*pxPerBeat + tlLeadInPx)+'px';
    clip.style.width = Math.max(4, spanBars*numerator*pxPerBeat - 2)+'px';
    clip.style.background = block ? blockColor(slot.blockId) : 'transparent';
    clip.dataset.idx = idx;

    const nameStrip = document.createElement('div');
    nameStrip.className = 'tl-clip-name';
    nameStrip.textContent = block ? blockDisplayName(block) : '?';
    clip.appendChild(nameStrip);
    const body = document.createElement('div');
    body.className = 'tl-clip-body';
    clip.appendChild(body);
    if (block) body.appendChild(buildClipNotePreview(block, slot, previewTracks, spanBars * pxPerBar, tlTrackH - TL_CLIP_VPAD - TL_CLIP_NAME_H, pxPerBar));

    // Right-click: if multi-selected, delete all; else delete this one.
    clip.addEventListener('contextmenu', (e) => {
      e.preventDefault(); e.stopPropagation();
      if (suppressNextContextMenu) { suppressNextContextMenu = false; return; }
      const targets = (tlSelectedIndices.has(idx) && tlSelectedIndices.size > 1) ? tlSelectedIndices : [idx];
      tlDeleteSlots(targets);
    });

    // Double-click to open in Note Editor.
    clip.addEventListener('dblclick', () => {
      if (block) {
        activeBlockId = block.id;
        tlSetSelection([idx]);
        tlSelectAnchorIdx = idx;
        syncKnobsToBlock(block);
        renderBlockTabs(); renderNoteEditor(); renderTimeline();
      }
    });

    // Click selection: plain selects/deselects; Shift extends range; Ctrl/Cmd toggles.
    clip.addEventListener('click', (e) => {
      if (e.shiftKey && tlSelectAnchorIdx != null) {
        const lo = Math.min(tlSelectAnchorIdx, idx), hi = Math.max(tlSelectAnchorIdx, idx);
        const range = [];
        for (let i = lo; i <= hi; i++) range.push(i);
        tlSetSelection(range);
      } else if (e.ctrlKey || e.metaKey) {
        const next = new Set(tlSelectedIndices);
        next.has(idx) ? next.delete(idx) : next.add(idx);
        tlSetSelection(next);
        tlSelectAnchorIdx = idx;
      } else {
        const isLoneSelection = tlSelectedIndices.size === 1 && tlSelectedIndices.has(idx);
        tlSetSelection(isLoneSelection ? [] : [idx]);
        tlSelectAnchorIdx = idx;
      }
      renderTimeline(); renderBlockTabs();
    });

    attachClipDrag(clip, idx, pxPerBar, numerator);
    track.appendChild(clip);
  });

  // One playhead element over ruler, loop row and clips; it lives in the scrolling container, so it scrolls with the content.
  const tlMainEl = document.getElementById('tlMain');
  if (!document.getElementById('tlPlayhead')) {
    const ph = document.createElement('div');
    ph.id = 'tlPlayhead'; ph.className = 'dg-playhead';
    tlMainEl.appendChild(ph);
  }
  if (!(playing && playbackMode === 'song')) tlShownBeat = tlPlayheadBeat;
  tlUpdatePlayheadEl();

  // Lasso rectangle overlay, recreated each render; its pointer listeners are attached once to track.
  const lassoRect = document.createElement('div');
  lassoRect.id = 'tlLassoRect'; lassoRect.className = 'tl-lasso-rect';
  track.appendChild(lassoRect);
  attachLassoSelectOnce();
  attachPaintDeleteOnce();

  if (window.renderMixerTimeline) window.renderMixerTimeline();
  requestAnimationFrame(() => window.scrollTo(0, savedScrollY));
}

// Note overview of one clip (like FL Studio's playlist): one row per used instrument, one tick per note,
// the block repeated for every loop. Opacity follows velocity.
function buildClipNotePreview(block, slot, rowTracks, clipW, h, pxPerBar) {
  const canvas = document.createElement('canvas');
  canvas.className = 'tl-clip-notes';
  const w = Math.max(1, Math.round(clipW - 2));
  h = Math.max(1, Math.round(h));
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  const backW = Math.min(16384, Math.round(w * dpr));
  canvas.width = backW; canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx || !rowTracks.length) return canvas;
  ctx.scale(backW / w, dpr);

  const rowH = h / rowTracks.length;
  const noteH = Math.max(2, Math.min(6, rowH - 1));
  const pxPerBeat = pxPerBar / block.numerator;
  const noteW = Math.max(2, pxPerBeat * 0.12);
  const blockBeats = (block.length || 1) * block.numerator;
  const loops = slot.loopCount || 1;
  rowTracks.forEach((t, row) => {
    const y = row * rowH + (rowH - noteH) / 2;
    (block.data[t.id] || []).forEach(note => {
      ctx.fillStyle = `rgba(15,23,42,${(0.35 + 0.6 * Math.min(1, (note.vel || 100) / 127)).toFixed(2)})`;
      for (let k = 0; k < loops; k++) ctx.fillRect((note.beat + k * blockBeats) * pxPerBeat, y, noteW, noteH);
    });
  });
  return canvas;
}

// Resize handle under the timeline: drag to change the clip lane height (kept across sessions).
let tlResizeAttached = false;
function attachTlResizeOnce() {
  if (tlResizeAttached) return;
  const handle = document.getElementById('tlResizeHandle');
  if (!handle) return;
  tlResizeAttached = true;
  let drag = null, raf = 0;
  handle.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    drag = { startY: e.clientY, startH: tlTrackH };
    handle.setPointerCapture(e.pointerId);
    e.preventDefault();
  });
  handle.addEventListener('pointermove', (e) => {
    if (!drag) return;
    tlTrackH = Math.max(TL_TRACK_H_MIN, Math.min(TL_TRACK_H_MAX, Math.round(drag.startH + (e.clientY - drag.startY) / currentUIScale())));
    if (!raf) raf = requestAnimationFrame(() => { raf = 0; renderTimeline(); });
  });
  const end = () => {
    if (!drag) return;
    drag = null;
    try { localStorage.setItem('dgg.tlTrackH.v2', String(tlTrackH)); } catch (_) {}
  };
  handle.addEventListener('pointerup', end);
  handle.addEventListener('pointercancel', end);
  handle.addEventListener('dblclick', () => {
    tlTrackH = TL_TRACK_H_DEFAULT;
    try { localStorage.setItem('dgg.tlTrackH.v2', String(tlTrackH)); } catch (_) {}
    renderTimeline();
  });
}

let tlLassoAttached = false;
// Lasso select: attached once to track (survives renders), updates geometry directly during drag, commits on pointerup.
function attachLassoSelectOnce() {
  if (tlLassoAttached) return;
  tlLassoAttached = true;
  const track = document.getElementById('tlTrack');
  let lasso = null; // { startBar, mode, baseSelection, moved, rectEl }

  const barAtTrackX = (clientX) => {
    const rect = track.getBoundingClientRect();
    return (clientX - rect.left - tlLeadInPx) / tlCurrentPxPerBar;
  };

  track.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return; // Left-drag only.
    if (e.target.closest('.tl-clip')) return; // Clips handle their own interactions.
    lasso = {
      startBar: barAtTrackX(e.clientX),
      mode: (e.ctrlKey || e.metaKey) ? 'toggle' : e.shiftKey ? 'add' : 'replace',
      baseSelection: new Set(tlSelectedIndices),
      moved: false,
      rectEl: document.getElementById('tlLassoRect'),
    };
    track.setPointerCapture(e.pointerId);
  });

  track.addEventListener('pointermove', (e) => {
    if (!lasso) return;
    const curBar = barAtTrackX(e.clientX);
    if (!lasso.moved && Math.abs(curBar - lasso.startBar) * tlCurrentPxPerBar > 3) lasso.moved = true;
    if (!lasso.moved) return;
    const lo = Math.min(lasso.startBar, curBar), hi = Math.max(lasso.startBar, curBar);
    if (lasso.rectEl) {
      lasso.rectEl.style.display = 'block';
      lasso.rectEl.style.left = (lo * tlCurrentPxPerBar + tlLeadInPx) + 'px';
      lasso.rectEl.style.width = Math.max(1, (hi - lo) * tlCurrentPxPerBar) + 'px';
    }
    const hitIdx = new Set(
      songEngine.songSequence
        .map((slot, idx) => ({ idx, start: slot.startBar, end: songEngine.slotEndBar(slot) }))
        .filter(s => s.start < hi && s.end > lo)
        .map(s => s.idx)
    );
    let preview;
    if (lasso.mode === 'toggle') {
      preview = new Set(lasso.baseSelection);
      hitIdx.forEach(i => preview.has(i) ? preview.delete(i) : preview.add(i));
    } else if (lasso.mode === 'add') {
      preview = new Set([...lasso.baseSelection, ...hitIdx]);
    } else {
      preview = hitIdx;
    }
    lasso.previewSelection = preview;
    track.querySelectorAll('.tl-clip').forEach(el => {
      el.classList.toggle('selected', preview.has(+el.dataset.idx));
    });
  });

  const finishLasso = () => {
    if (!lasso) return;
    if (lasso.moved) {
      tlSetSelection(lasso.previewSelection || []);
      renderTimeline(); renderBlockTabs();
    } else if (tlSelectedIndices.size) {
      // Plain click on empty canvas clears selection.
      tlSetSelection([]);
      renderTimeline(); renderBlockTabs();
    } else if (lasso.rectEl) {
      lasso.rectEl.style.display = 'none';
    }
    lasso = null;
  };
  track.addEventListener('pointerup', finishLasso);
  track.addEventListener('pointercancel', finishLasso);
}

let tlPaintDeleteAttached = false;
// Right-drag delete: attached once to track, deletes clips as cursor passes over them in one undo step.
function attachPaintDeleteOnce() {
  if (tlPaintDeleteAttached) return;
  tlPaintDeleteAttached = true;
  const track = document.getElementById('tlTrack');
  let painting = null; // { touchedIdx, moved }

  // Use elementFromPoint because pointer capture makes e.target always track.
  const clipAtPoint = (clientX, clientY) => {
    const el = document.elementFromPoint(clientX, clientY);
    return el && el.closest('.tl-clip');
  };

  track.addEventListener('pointerdown', (e) => {
    if (e.button !== 2) return; // Right button only.
    e.preventDefault();
    painting = { touchedIdx: new Set(), moved: false };
    const clip = e.target.closest('.tl-clip');
    if (clip) painting.touchedIdx.add(+clip.dataset.idx);
    track.setPointerCapture(e.pointerId);
  });

  track.addEventListener('pointermove', (e) => {
    if (!painting) return;
    const clip = clipAtPoint(e.clientX, e.clientY);
    if (!clip) return;
    const idx = +clip.dataset.idx;
    if (!painting.touchedIdx.has(idx)) {
      painting.moved = true;
      painting.touchedIdx.add(idx);
      clip.classList.add('paint-marked');
    }
  });

  const finishPaint = () => {
    if (!painting) return;
    const { touchedIdx, moved } = painting;
    painting = null;
    if (moved && touchedIdx.size) {
      armSuppressNextContextMenu();
      tlDeleteSlots(touchedIdx);
    }
    // No real drag: let native contextmenu handle normal right-click.
  };
  track.addEventListener('pointerup', finishPaint);
  track.addEventListener('pointercancel', () => { painting = null; });
}

// Build a merged pattern from selected slots, re-based to start at beat 0. Used for drag-out export.
function buildSelectionFullPattern(indices, g) {
  const slots = Array.from(indices)
    .map(i => songEngine.songSequence[i])
    .filter(Boolean)
    .sort((a, b) => a.startBar - b.startBar);
  if (!slots.length) return null;

  const minStartBar = slots[0].startBar;
  const events = [];
  let maxEndBeat = 0;
  slots.forEach(slot => {
    const block = findBlock(slot.blockId);
    if (!block) return;
    const startBeat = (slot.startBar - minStartBar) * g.numerator;
    trackList.forEach(t => {
      (block.data[t.id] || []).forEach(note => {
        events.push({ trackId: t.id, beatAbs: startBeat + note.beat, vel: note.vel });
      });
    });
    maxEndBeat = Math.max(maxEndBeat, startBeat + (block.length || 1) * g.numerator);
  });
  events.sort((a, b) => a.beatAbs - b.beatAbs);
  return { events, totalBeats: maxEndBeat };
}

// True once a captured drag has left the plugin window (the OS keeps delivering
// pointer moves while a button is held, with coordinates outside the viewport).
function pointerLeftPluginWindow(e) {
  return e.clientX <= 0 || e.clientY <= 0 || e.clientX >= window.innerWidth - 1 || e.clientY >= window.innerHeight - 1;
}

// A timeline drag that leaves the plugin window hands over to the OS drag (DoDragDrop) with the
// WHOLE song as MIDI, so a DAW target can take it. Single blocks are dragged out of the block palette.
function startSongDragOut() {
  if (!hostBridge.isEmbedded) return;
  const g = readGrid();
  const all = songEngine.songSequence.map((_, i) => i);
  const fullPattern = buildSelectionFullPattern(all, g);
  if (!fullPattern) return;
  hostBridge.send('startNativeDragExportSelection', { fullPattern, params: readParams(), grid: g });
}

// Clip drag: reorder or duplicate with Shift. Snaps to free bar on release.
// Leaving the plugin window turns it into a MIDI drag-out to the host.
function attachClipDrag(clip, startIdx, pxPerBar, numerator) {
  let dragging = false, startX = 0, origLeftPx = 0, moved = false;
  const slot = songEngine.songSequence[startIdx];

  clip.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return; // Right-click reserved for delete.
    dragging = true; moved = false;
    startX = e.clientX;
    origLeftPx = slot.startBar * pxPerBar + tlLeadInPx;
    clip.classList.add('dragging');
    clip.setPointerCapture(e.pointerId);
  });

  clip.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const dx = e.clientX - startX;
    if (Math.abs(dx) > 3) moved = true;
    if (moved && pointerLeftPluginWindow(e)) {
      dragging = false;
      try { clip.releasePointerCapture(e.pointerId); } catch(_) {}
      hideDropMarker();
      renderTimeline();   // snaps the clip back; nothing moved inside the plugin
      startSongDragOut();
      return;
    }
    if (moved) {
      const leftPx = origLeftPx + dx / currentUIScale();
      clip.style.left = leftPx + 'px';
      clip.classList.toggle('duplicating', e.shiftKey);
      const desiredBar = computeDropBarFromLocalX(leftPx, pxPerBar);
      // Shift+drag duplicates; plain move excludes current spot.
      const snappedBar = songEngine.nearestBoundary(Math.max(0, desiredBar), e.shiftKey ? null : slot);
      showDropMarker(snappedBar, pxPerBar);
    }
  });

  const finishDrag = (e) => {
    if (!dragging) return;
    dragging = false;
    clip.classList.remove('dragging', 'duplicating');
    hideDropMarker();
    if (!moved) return;
    const leftPx = origLeftPx + (e.clientX - startX) / currentUIScale();
    const desiredBar = computeDropBarFromLocalX(leftPx, pxPerBar);
    const clipBlock = findBlock(slot.blockId);
    const clipName = clipBlock ? blockDisplayName(clipBlock) : '?';
    tlSetSelection([]);
    if (e.shiftKey) {
      // Duplicate: original stays, copy inserted at drop point, pushing others.
      withSongSequenceUndo(`Duplicate clip (${clipName})`, () => {
        songEngine.duplicateSlotAtRipple(startIdx, desiredBar);
      });
    } else {
      // Move: relocates to drop point, pushing others; old spot becomes a gap.
      withSongSequenceUndo(`Move clip (${clipName})`, () => {
        songEngine.moveSlotRipple(startIdx, desiredBar);
      });
    }
  };
  clip.addEventListener('pointerup', finishDrag);
  clip.addEventListener('pointercancel', finishDrag);
}