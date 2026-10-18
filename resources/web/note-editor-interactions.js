// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// NOTE EDITOR — POINTER INTERACTIONS
// Drag/select gestures on the grid: triplet-zone range drag, rectangle multi-select,
// note drag, velocity-lane drag. Mutates block data via neApplyNoteMoves etc.
// =========================================

function attachTripletRangeDrag(row, block, numerator, pxPerBeat, g, engineGroup, blockBeats) {
  const maxBeat = blockBeats ?? numerator;
  let drag = null;
  row.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    if (e.target !== row) return;
    e.stopPropagation();
    drag = { sx: e.clientX, moved: false };
  });
  row.addEventListener('pointermove', (e) => {
    if (!drag) return;
    e.stopPropagation();
    const dx = e.clientX - drag.sx;
    if (!drag.moved && Math.abs(dx) > 3) {
      drag.moved = true;
      row.setPointerCapture(e.pointerId);
      drag.rect = document.createElement('div');
      drag.rect.className = 'ne-triplet-range dragging';
      row.appendChild(drag.rect);
    }
    if (drag.rect) {
      const rect = row.getBoundingClientRect();
      const scale = currentUIScale();
      const x1 = (drag.sx - rect.left) / scale, x2 = (e.clientX - rect.left) / scale;
      drag.rect.style.left = Math.min(x1,x2)+'px';
      drag.rect.style.width = Math.abs(x2-x1)+'px';
    }
  });
  const finish = (e) => {
    if (!drag) return;
    e.stopPropagation();
    if (drag.moved) {
      suppressNextRowClick = true;
      const rect = row.getBoundingClientRect();
      const scale = currentUIScale();
      const x1 = (drag.sx - rect.left) / scale, x2 = (e.clientX - rect.left) / scale;
      const zoneSubdivision = isTripletSubdivision(g.subdivision)
        ? engineGroupStraightSubdivision(engineGroup)
        : engineGroupTripletSubdivision(engineGroup);
      const zoneSnap = 1/zoneSubdivision;
      let fromBeat = Math.min(maxBeat, Math.max(0, neQuantize((Math.min(x1,x2)-NE_LEFT_PAD)/pxPerBeat, zoneSnap)));
      let toBeat = Math.min(maxBeat, neQuantize((Math.max(x1,x2)-NE_LEFT_PAD)/pxPerBeat, zoneSnap));
      if (drag.rect) drag.rect.remove();
      if (toBeat - fromBeat >= Math.max(zoneSnap*0.5, 0.05)) {
        block.tripletLayers = block.tripletLayers || {};
        block.tripletLayers[engineGroup] = block.tripletLayers[engineGroup] || [];
        block.tripletLayers[engineGroup].push({ from: fromBeat, to: toBeat });
        if (engineGroup === 'kick') applyKickGroupToBlock(block, g);
        rebuildFullFromEditedBlocks();
      }
      renderNoteEditor();
    }
    drag = null;
  };
  row.addEventListener('pointerup', finish);
  row.addEventListener('pointercancel', finish);
}

function attachTripletRangeResize(handle, side, row, block, engineGroup, range, pxPerBeat, g, maxBeat) {
  let drag = null;
  handle.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    e.stopPropagation();
    handle.setPointerCapture(e.pointerId);
    handle.classList.add('resizing');
    drag = { startFrom: range.from, startTo: range.to };
  });
  handle.addEventListener('pointermove', (e) => {
    if (!drag) return;
    e.stopPropagation();
    const rect = row.getBoundingClientRect();
    const scale = currentUIScale();
    const x = (e.clientX - rect.left) / scale;
    const zoneSubdivision = isTripletSubdivision(g.subdivision)
      ? engineGroupStraightSubdivision(engineGroup)
      : engineGroupTripletSubdivision(engineGroup);
    const zoneSnap = 1/zoneSubdivision;
    const minGap = Math.max(zoneSnap, 0.05);
    let beat = neQuantize((x - NE_LEFT_PAD)/pxPerBeat, zoneSnap);
    const seg = handle.parentElement;
    if (side === 'left') {
      range.from = Math.max(0, Math.min(range.to - minGap, beat));
    } else {
      range.to = Math.min(maxBeat, Math.max(range.from + minGap, beat));
    }
    seg.style.left = (NE_LEFT_PAD + range.from*pxPerBeat)+'px';
    seg.style.width = ((range.to-range.from)*pxPerBeat)+'px';
  });
  const finish = (e) => {
    if (!drag) return;
    e.stopPropagation();
    handle.classList.remove('resizing');
    const changed = drag.startFrom !== range.from || drag.startTo !== range.to;
    drag = null;
    if (changed) {
      if (engineGroup === 'kick') applyKickGroupToBlock(block, g);
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
    }
  };
  handle.addEventListener('pointerup', finish);
  handle.addEventListener('pointercancel', finish);
}

// Ctrl+drag = paint/erase along snap points in this row (right-drag erase is attachRightDragErase).
function attachRowPaintDrag(row, block, t, ctx, refs) {
  const { g, blockBeats, totalBeats, pxPerBeat, breakLocked, grooveBeats } = ctx;
  const { rowsCol } = refs;
  let paint = null;

  const beatAtClientX = (clientX) => {
    const rect = rowsCol.getBoundingClientRect();
    const x = (clientX - rect.left) / currentUIScale() + rowsCol.scrollLeft;
    const raw = Math.max(0, Math.min(totalBeats - 0.01, (x - NE_LEFT_PAD) / pxPerBeat));
    return neQuantizeForTrack(raw, g, block, t.id);
  };

  const makeTempDia = (beat) => {
    const dia = document.createElement('div');
    dia.className = 'ne-note ne-note-painting';
    dia.style.left = (NE_LEFT_PAD + beat * pxPerBeat) + 'px';
    dia.style.top = (NE_ROW_H / 2) + 'px';
    dia.style.background = fadedColor(t.color, 100 / 127);
    row.appendChild(dia);
    return dia;
  };
  const findLiveDia = (beat) => Array.from(row.querySelectorAll('.ne-note'))
    .find(d => d.dataset.selKey === (t.id + '|' + beat));

  const paintAt = (beat) => {
    const key = beat.toFixed(4);
    if (paint.touched.has(key)) return;
    if (beat >= blockBeats) return;
    if (breakLocked && beat < grooveBeats) return;
    paint.touched.add(key);
    const arr = block.data[t.id];
    const collide = neFindCollision(arr, beat, -1);
    if (paint.mode === 'add') {
      if (collide >= 0) return;
      const newNote = { beat, vel: 100 };
      arr.push(newNote);
      paint.entries.push({ type: 'add', note: newNote });
      makeTempDia(beat);
    } else {
      if (collide < 0) return;
      const noteRef = arr[collide];
      arr.splice(collide, 1);
      paint.entries.push({ type: 'delete', note: noteRef });
      const dia = findLiveDia(noteRef.beat);
      if (dia) dia.remove();
    }
  };

  row.addEventListener('pointerdown', (e) => {
    if (e.button === 0) {
      if (!e.ctrlKey) return;
      if (e.target !== row) return;
      e.stopPropagation();
      paint = { sx: e.clientX, sy: e.clientY, moved: false, mode: null, touched: new Set(), entries: [] };
    }
  });

  row.addEventListener('pointermove', (e) => {
    if (!paint) return;
    e.stopPropagation();
    if (!paint.moved) {
      const dx = e.clientX - paint.sx, dy = e.clientY - paint.sy;
      if (Math.abs(dx) < 4 && Math.abs(dy) < 4) return;
      paint.moved = true;
      suppressNextRowClick = true;
      row.setPointerCapture(e.pointerId);
      selectedNotes.clear();
      if (paint.mode === null) paint.mode = neFindCollision(block.data[t.id], beatAtClientX(paint.sx), -1) >= 0 ? 'delete' : 'add';
      paint.lastX = paint.sx;
    }
    const scale = currentUIScale();
    const step = Math.max(2, (pxPerBeat / 6) * scale);
    const dir = e.clientX >= paint.lastX ? 1 : -1;
    for (let x = paint.lastX; dir > 0 ? x <= e.clientX : x >= e.clientX; x += dir * step) {
      paintAt(beatAtClientX(x));
    }
    paintAt(beatAtClientX(e.clientX));
    paint.lastX = e.clientX;
  });

  const finish = (e) => {
    if (!paint) return;
    const p = paint; paint = null;
    if (!p.moved) return;
    e.stopPropagation();
    if (p.button === 2) suppressNextContextMenu = true;
    if (!p.entries.length) return;
    const beforeLocked = snapshotLocked(block);
    markTrackManuallyEdited(block, t.id, p.entries.map(en => en.note));
    const afterLocked = snapshotLocked(block);
    block.data[t.id].sort((a, b) => a.beat - b.beat);
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    const verb = p.mode === 'add' ? 'Paint' : 'Erase';
    const label = p.entries.length > 1 ? `${verb} ${p.entries.length} notes` : `${verb} note`;
    const entries = p.entries;
    const applyEntries = (adding) => {
      const arr = block.data[t.id];
      entries.forEach(en => {
        const wantPresent = (en.type === 'add') === adding;
        const i = arr.indexOf(en.note);
        if (wantPresent && i < 0) arr.push(en.note);
        else if (!wantPresent && i >= 0) arr.splice(i, 1);
      });
      arr.sort((a, b) => a.beat - b.beat);
      restoreLocked(block, adding ? afterLocked : beforeLocked);
      block.generated = false;
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
    };
    pushUndo(label, () => applyEntries(false), () => applyEntries(true));
  };
  row.addEventListener('pointerup', finish);
  row.addEventListener('pointercancel', finish);
}

// Right-drag erases every note the pointer sweeps over, in whatever rows it crosses (one undo step).
function attachRightDragErase(rowsCol, block) {
  let drag = null;

  rowsCol.addEventListener('pointerdown', (e) => {
    if (e.button !== 2) return;
    if (!e.target.closest('.ne-row')) return;
    // Notes as they are drawn right now (windowed, locked ones excluded): the sweep hits what is on screen.
    const targets = [];
    rowsCol.querySelectorAll('.ne-note[data-sel-key]').forEach(el => {
      if (el.classList.contains('ne-note-locked') || el.classList.contains('ne-note-painting')) return;
      const sep = el.dataset.selKey.lastIndexOf('|');
      const r = el.getBoundingClientRect();
      targets.push({ el, trackId: el.dataset.selKey.slice(0, sep), beat: parseFloat(el.dataset.selKey.slice(sep + 1)), r, gone: false });
    });
    drag = { sx: e.clientX, sy: e.clientY, lx: e.clientX, ly: e.clientY, moved: false, targets, erased: [] };
  });

  rowsCol.addEventListener('pointermove', (e) => {
    if (!drag) return;
    if (!drag.moved) {
      if (Math.abs(e.clientX - drag.sx) < 4 && Math.abs(e.clientY - drag.sy) < 4) return;
      drag.moved = true;
      rowsCol.setPointerCapture(e.pointerId);
    }
    const dx = e.clientX - drag.lx, dy = e.clientY - drag.ly;
    const steps = Math.max(1, Math.ceil(Math.hypot(dx, dy) / 3));
    for (let i = 1; i <= steps; i++) {
      const px = drag.lx + dx * i / steps, py = drag.ly + dy * i / steps;
      drag.targets.forEach(t => {
        if (t.gone) return;
        if (px < t.r.left - 2 || px > t.r.right + 2 || py < t.r.top - 2 || py > t.r.bottom + 2) return;
        const arr = block.data[t.trackId];
        const idx = arr ? arr.findIndex(n => Math.abs(n.beat - t.beat) < 1e-6) : -1;
        if (idx < 0) return;
        drag.erased.push({ trackId: t.trackId, note: arr.splice(idx, 1)[0] });
        t.gone = true;
        t.el.remove();
      });
    }
    drag.lx = e.clientX; drag.ly = e.clientY;
  });

  const finish = (e) => {
    if (!drag) return;
    const d = drag; drag = null;
    if (!d.moved) return;
    suppressNextContextMenu = true;
    suppressNextRowClick = true;
    setTimeout(() => { suppressNextRowClick = false; }, 0);
    if (!d.erased.length) return;
    const beforeLocked = snapshotLocked(block);
    const byTrack = new Map();
    d.erased.forEach(en => (byTrack.get(en.trackId) || byTrack.set(en.trackId, []).get(en.trackId)).push(en.note));
    byTrack.forEach((notes, tid) => markTrackManuallyEdited(block, tid, notes));
    const afterLocked = snapshotLocked(block);
    d.erased.forEach(en => selectedNotes.delete(en.trackId + '|' + en.note.beat));
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    const entries = d.erased;
    const apply = (adding) => {
      entries.forEach(en => {
        const arr = block.data[en.trackId] || (block.data[en.trackId] = []);
        const i = arr.indexOf(en.note);
        if (adding && i < 0) arr.push(en.note);
        else if (!adding && i >= 0) arr.splice(i, 1);
      });
      Object.keys(block.data).forEach(tid => block.data[tid].sort((a, b) => a.beat - b.beat));
      restoreLocked(block, adding ? beforeLocked : afterLocked);
      block.generated = false;
      rebuildFullFromEditedBlocks();
      renderNoteEditor();
    };
    pushUndo(entries.length > 1 ? 'Erase ' + entries.length + ' notes' : 'Erase note', () => apply(true), () => apply(false));
  };
  rowsCol.addEventListener('pointerup', finish);
  rowsCol.addEventListener('pointercancel', finish);
}

function attachRectSelect(rowsCol) {
  let drag = null;
  rowsCol.addEventListener('contextmenu', e => e.preventDefault(), true);
  rowsCol.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    if (!e.target.classList.contains('ne-row')) return;
    drag = { sx:e.clientX, sy:e.clientY, moved:false, rect:null, toggle:e.shiftKey };
  });
  rowsCol.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const dx = e.clientX-drag.sx, dy = e.clientY-drag.sy;
    if (!drag.moved && (Math.abs(dx)>4 || Math.abs(dy)>4)) {
      drag.moved = true;
      rowsCol.setPointerCapture(e.pointerId);
      drag.rect = document.createElement('div');
      drag.rect.className = 'ne-select-rect';
      document.body.appendChild(drag.rect);
    }
    if (drag.rect) {
      const left = Math.min(drag.sx, e.clientX), top = Math.min(drag.sy, e.clientY);
      drag.rect.style.left = left+'px'; drag.rect.style.top = top+'px';
      drag.rect.style.width = Math.abs(dx)+'px'; drag.rect.style.height = Math.abs(dy)+'px';
    }
  });
  const finish = (e) => {
    if (!drag) return;
    if (drag.moved) {
      const rr = drag.rect.getBoundingClientRect();
      const touched = [];
      document.querySelectorAll('.ne-rows .ne-row .ne-note').forEach(dia => {
        if (dia.classList.contains('ne-note-locked')) return;
        const nr = dia.getBoundingClientRect();
        const overlaps = nr.left < rr.right && nr.right > rr.left && nr.top < rr.bottom && nr.bottom > rr.top;
        if (overlaps) touched.push(dia.dataset.selKey);
      });
      suppressNextRowClick = true;
      setTimeout(() => { suppressNextRowClick = false; }, 0);
      drag.rect.remove();
      // Shift toggles what the rectangle catches; without Shift the rectangle replaces the selection.
      if (drag.toggle) touched.forEach(key => { if (selectedNotes.has(key)) selectedNotes.delete(key); else selectedNotes.add(key); });
      else { selectedNotes.clear(); touched.forEach(key => selectedNotes.add(key)); }
      renderNoteEditor();
    }
    drag = null;
  };
  rowsCol.addEventListener('pointerup', finish);
  rowsCol.addEventListener('pointercancel', finish);
}

function attachNoteDrag(dia, block, trackId, idx, numerator, g, pxPerBeat) {
  let dragging=false, startX=0, startY=0, origBeat=0, moved=false, dragSnapshot=null;
  let dragItems=null;
  let followEls=null;
  let targetTrackId=trackId;
  let rowRects=null;
  let originRowLeft=0;
  let lastPreviewSig=null;
  dia.addEventListener('click', (e) => e.stopPropagation());
  dia.addEventListener('pointerdown', (e) => {
    if (e.button===2) return;
    e.stopPropagation();
    dragging=true; moved=false; startX=e.clientX; startY=e.clientY; origBeat=block.data[trackId][idx].beat;
    targetTrackId = trackId;
    lastPreviewSig = null;
    const scale = currentUIScale();
    const appRect = document.querySelector('.app').getBoundingClientRect();
    rowRects = Array.from(document.querySelectorAll('.ne-row[data-track-id]')).map(el => {
      const r = el.getBoundingClientRect();
      return {
        el, trackId: el.dataset.trackId,
        top: r.top, bottom: r.bottom,
        localTop: (r.top - appRect.top) / scale, localBottom: (r.bottom - appRect.top) / scale,
      };
    });
    const originRect = dia.closest('.ne-row').getBoundingClientRect();
    originRowLeft = (originRect.left - appRect.left) / scale;
    const myKey = trackId+'|'+origBeat;
    dragItems = (selectedNotes.size > 1 && selectedNotes.has(myKey)) ? neResolveSelection(block) : null;
    dragSnapshot = JSON.stringify(block.data);
    if (dragItems) {
      const selectedEls = Array.from(document.querySelectorAll('.ne-note.selected'));
      followEls = dragItems.map(it => {
        const key = it.trackId+'|'+it.note.beat;
        const el = selectedEls.find(e => e.dataset.selKey === key);
        return el ? { el, trackId: it.trackId, origBeat: it.note.beat } : null;
      }).filter(Boolean);
    } else {
      followEls = [{ el: dia, trackId, origBeat }];
    }
    followEls.forEach(f => {
      // Switching to fixed re-anchors the leftover inline left/top (row-relative
      // from render) against the viewport, which would jump the note to the
      // window's top-left. Pin it at its current position first, in the same
      // .app-relative coordinates pointermove writes.
      const rowInfo = rowRects.find(r => r.trackId === f.trackId);
      if (!rowInfo) return;
      f.el.style.position = 'fixed';
      f.el.style.left = (originRowLeft + NE_LEFT_PAD + f.origBeat*pxPerBeat) + 'px';
      f.el.style.top = ((rowInfo.localTop+rowInfo.localBottom)/2) + 'px';
      f.el.style.zIndex = 20;
      f.el.style.pointerEvents = 'none';
    });
    dia.style.cursor='grabbing'; dia.style.pointerEvents='auto';
    dia.setPointerCapture(e.pointerId);
  });
  const totalBeats = (block.length || 1) * numerator;
  const rowAtY = (y) => rowRects && rowRects.find(r => y >= r.top && y < r.bottom);
  const rowFor = (tid) => rowRects && rowRects.find(r => r.trackId === tid);
  dia.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const scale = currentUIScale();
    const dx = (e.clientX-startX) / scale, dy = (e.clientY-startY) / scale;
    if (Math.abs(dx)>2 || Math.abs(dy)>2) moved=true;
    const hit = rowAtY(e.clientY);
    targetTrackId = hit ? hit.trackId : trackId;
    let newBeat = origBeat + dx/pxPerBeat;
    newBeat = Math.max(0, Math.min(totalBeats-0.01, neQuantizeForTrack(newBeat, g, block, targetTrackId)));
    block.data[trackId][idx].beat = newBeat;
    const sig = targetTrackId + '|' + newBeat;
    if (sig !== lastPreviewSig) {
      lastPreviewSig = sig;
      nePreviewTrack(targetTrackId, block.data[trackId][idx].vel);
    }
    const deltaBeat = newBeat - origBeat;
    followEls.forEach(f => {
      const beat = Math.max(0, Math.min(totalBeats-0.01, f.origBeat + deltaBeat));
      const rowInfo = hit || rowFor(f.trackId);
      if (!rowInfo) return;
      f.el.style.left = (originRowLeft + NE_LEFT_PAD + beat*pxPerBeat) + 'px';
      f.el.style.top = ((rowInfo.localTop+rowInfo.localBottom)/2) + 'px';
    });
  });
  const finish = (e) => {
    if (!dragging) return;
    dragging=false; dia.style.cursor='grab';
    if (!moved) {
      followEls.forEach(f => { f.el.style.position=''; f.el.style.left=''; f.el.style.top=''; f.el.style.zIndex=''; f.el.style.pointerEvents=''; });
      const key = trackId+'|'+block.data[trackId][idx].beat;
      if (e && e.shiftKey) {   // Shift+click toggles the note within the selection
        if (selectedNotes.has(key)) selectedNotes.delete(key); else selectedNotes.add(key);
      } else {
        selectedNotes.clear();
        selectedNotes.add(key);
      }
      if (selectedNotes.has(key)) nePreviewTrack(trackId, block.data[trackId][idx].vel);
      renderNoteEditor();
      return;
    }
    const finalBeat = block.data[trackId][idx].beat;
    const deltaBeat = finalBeat - origBeat;
    let proposals;
    if (dragItems) {
      proposals = dragItems.map(it => {
        const isAnchor = it.trackId === trackId && it.note === block.data[trackId][idx];
        const targetBeat = isAnchor
          ? finalBeat
          : Math.max(0, Math.min(totalBeats-0.01, it.note.beat + deltaBeat));
        return { trackId: it.trackId, note: it.note, targetTrackId, targetBeat };
      });
    } else {
      const targetBeat = Math.max(0, Math.min(totalBeats-0.01, neQuantizeForTrack(finalBeat, g, block, targetTrackId)));
      proposals = [{ trackId, note: block.data[trackId][idx], targetTrackId, targetBeat }];
    }
    const beforeLocked = snapshotLocked(block);
    neApplyNoteMoves(block, proposals);
    const afterLocked = snapshotLocked(block);
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    const before = dragSnapshot;
    const after = JSON.stringify(block.data);
    pushUndo(dragItems ? `Move ${dragItems.length} notes` : 'Move note',
      () => { block.data = JSON.parse(before); restoreLocked(block, beforeLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); },
      () => { block.data = JSON.parse(after); restoreLocked(block, afterLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); }
    );
  };
  dia.addEventListener('pointerup', finish);
  dia.addEventListener('pointercancel', finish);
}

// Click on a note's velocity bar or head: it becomes the selection; with Shift it is toggled within the selection instead.
function neClickSelectNote(key, additive) {
  if (additive) { if (selectedNotes.has(key)) selectedNotes.delete(key); else selectedNotes.add(key); }
  else { selectedNotes.clear(); selectedNotes.add(key); }
  velNudgeSelKey = null;
  renderNoteEditor();
}

function attachVelDrag(myRef, allRefs, block) {
  let dragging=false, startY=0, origVels=[];
  myRef.handle.addEventListener('pointerdown', (e) => {
    dragging=true; startY=e.clientY;
    origVels = allRefs.map(r => r.note.vel);
    myRef.handle.style.cursor='grabbing';
    myRef.handle.setPointerCapture(e.pointerId);
    neVelPreviewStart(myRef.trackId, myRef.note);   // the grabbed note is the one that plays
  });
  myRef.handle.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const dy = (startY - e.clientY) / currentUIScale();
    const delta = dy*1.4;
    let lastV = null;
    allRefs.forEach((r, i) => {
      const v = Math.round(Math.max(1, Math.min(127, origVels[i] + delta)));
      r.note.vel = v;
      lastV = v;
      const h = (v/127)*(NE_VEL_H-14);
      r.line.style.height = h+'px';
      r.handle.style.bottom = (h-5.5)+'px';
      neVelocityLive(r.trackId, r.note);
    });
    window.dggShowValueTip && window.dggShowValueTip('Vel ' + lastV, e.clientX, e.clientY);
  });
  const finish = (e) => {
    if (!dragging) return;
    dragging=false; myRef.handle.style.cursor='ns-resize';
    neVelPreviewStop();
    window.dggHideValueTip && window.dggHideValueTip();
    neFlushVelocities();   // the host holds the last values of the gesture even when nothing is applied below
    const actuallyChanged = allRefs.some((r, i) => r.note.vel !== origVels[i]);
    if (!actuallyChanged) {   // a click on the head: select it (Shift toggles), nothing was edited so nothing locks
      neClickSelectNote(myRef.trackId + '|' + myRef.note.beat, !!(e && e.shiftKey));
      return;
    }
    const beforeLocked = snapshotLocked(block);
    const touched = new Set(allRefs.map(r => r.trackId));
    touched.forEach(tid => markTrackManuallyEdited(block, tid, allRefs.filter(r => r.trackId === tid).map(r => r.note)));
    const afterLocked = snapshotLocked(block);
    block.generated = false;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    if (actuallyChanged) {
      const notes = allRefs.map(r => r.note);
      const before = origVels.slice();
      const after = notes.map(n => n.vel);
      const label = allRefs.length > 1 ? `Change velocity (${allRefs.length} notes)` : 'Change velocity';
      pushUndo(label,
        () => { notes.forEach((n, i) => { n.vel = before[i]; }); restoreLocked(block, beforeLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); },
        () => { notes.forEach((n, i) => { n.vel = after[i]; }); restoreLocked(block, afterLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); }
      );
    }
  };
  myRef.handle.addEventListener('pointerup', finish);
  myRef.handle.addEventListener('pointercancel', finish);
}

// Velocity lane background: drag = rectangle select (the notes whose velocity head the rectangle touches, shown in colour or gray),
// right-drag = rectangle deselect, right-click = clear the selection, Shift+drag = toggle what the rectangle catches, Ctrl+drag = freehand paint,
// Alt+drag = straight-line ramp.
function attachVelLanePaint(velInner, velRefs, block) {
  let gesture = null;

  const velAtClientY = (clientY) => {
    const rect = velInner.getBoundingClientRect();
    const h = Math.max(0, Math.min(NE_VEL_H - 14, (rect.bottom - clientY) / currentUIScale()));
    return Math.max(1, Math.min(127, Math.round((h / (NE_VEL_H - 14)) * 127)));
  };
  const applyVel = (ref, v) => {
    v = Math.max(1, Math.min(127, Math.round(v)));
    ref.note.vel = v;
    const h = (v / 127) * (NE_VEL_H - 14);
    ref.line.style.height = h + 'px';
    ref.handle.style.bottom = (h - 5.5) + 'px';
    neVelocityLive(ref.trackId, ref.note);
    neVelPreviewStart(ref.trackId, ref.note);   // the note the stroke has just reached plays
  };

  // Rectangle select: picks the notes whose velocity head (handle) the rectangle touches, so a height range selects by loudness.
  let rectDrag = null;
  const finishRect = (e) => {
    if (!rectDrag) return;
    const d = rectDrag; rectDrag = null;
    if (!d.moved) {
      // Right-click without dragging clears the whole selection.
      if (d.deselect) { if (selectedNotes.size) { selectedNotes.clear(); velNudgeSelKey = null; renderNoteEditor(); } return; }
      // Left click on a bar: select that note (Shift toggles it within the selection).
      const gray = velInner.querySelector('.ne-vel-gray-canvas');
      const hit = gray && gray.itemAtClient && e ? gray.itemAtClient(e) : null;
      if (hit) {
        const track = block.data[hit.trackId], note = track && track.find(n => n.beat === hit.beat);
        if (note) neVelPreviewOnce(hit.trackId, note);
        neClickSelectNote(hit.trackId + '|' + hit.beat, !!d.toggle);
      }
      return;
    }
    const rr = d.el.getBoundingClientRect();
    d.el.remove();
    const keys = [];
    velRefs.forEach(r => {
      const hr = r.handle.getBoundingClientRect();
      if (hr.left < rr.right && hr.right > rr.left && hr.top < rr.bottom && hr.bottom > rr.top) keys.push(r.trackId + '|' + r.note.beat);
    });
    const grayCanvas = velInner.querySelector('.ne-vel-gray-canvas');
    if (grayCanvas && grayCanvas.keysInRect) grayCanvas.keysInRect(rr).forEach(k => keys.push(k));
    if (d.deselect) keys.forEach(k => selectedNotes.delete(k));
    else if (d.toggle) keys.forEach(k => { if (selectedNotes.has(k)) selectedNotes.delete(k); else selectedNotes.add(k); });
    else { selectedNotes.clear(); keys.forEach(k => selectedNotes.add(k)); }
    velNudgeSelKey = null;
    renderNoteEditor();
  };
  velInner.addEventListener('pointerup', finishRect);
  velInner.addEventListener('pointercancel', finishRect);

  velInner.addEventListener('contextmenu', (e) => e.preventDefault());
  velInner.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 && e.button !== 2) return;
    if (e.target.closest('.ne-vel-handle')) return;
    if (e.button === 2) {
      rectDrag = { sx: e.clientX, sy: e.clientY, moved: false, el: null, deselect: true };
      velInner.setPointerCapture(e.pointerId);
      return;
    }
    if (!e.ctrlKey && !e.metaKey && !e.altKey) {
      rectDrag = { sx: e.clientX, sy: e.clientY, moved: false, el: null, deselect: false, toggle: e.shiftKey };
      velInner.setPointerCapture(e.pointerId);
      return;
    }
    if (!velRefs.length) return;
    const rect = velInner.getBoundingClientRect();
    const x = (e.clientX - rect.left) / currentUIScale();
    const y = velAtClientY(e.clientY);
    gesture = {
      mode: e.altKey ? 'ramp' : 'paint', // only reached with Ctrl or Alt held
      startX: x, startY: y,
      lastX: x, lastY: y,
      origVels: velRefs.map(r => r.note.vel),
      touched: new Set(),
    };
    velInner.setPointerCapture(e.pointerId);
    if (gesture.mode === 'paint') {
      let nearest = null;
      velRefs.forEach(r => { if (!nearest || Math.abs(r.cx - x) < Math.abs(nearest.cx - x)) nearest = r; });
      if (nearest && Math.abs(nearest.cx - x) < 14) { applyVel(nearest, y); gesture.touched.add(nearest); }
    }
  });

  velInner.addEventListener('pointermove', (e) => {
    if (rectDrag) {
      const dx = e.clientX - rectDrag.sx, dy = e.clientY - rectDrag.sy;
      if (!rectDrag.moved && (Math.abs(dx) > 4 || Math.abs(dy) > 4)) {
        rectDrag.moved = true;
        rectDrag.el = document.createElement('div');
        rectDrag.el.className = 'ne-select-rect' + (rectDrag.deselect ? ' delete-mode' : '');
        document.body.appendChild(rectDrag.el);
      }
      if (rectDrag.el) {
        rectDrag.el.style.left = Math.min(rectDrag.sx, e.clientX) + 'px';
        rectDrag.el.style.top = Math.min(rectDrag.sy, e.clientY) + 'px';
        rectDrag.el.style.width = Math.abs(dx) + 'px';
        rectDrag.el.style.height = Math.abs(dy) + 'px';
      }
      return;
    }
    if (!gesture) return;
    const rect = velInner.getBoundingClientRect();
    const x = (e.clientX - rect.left) / currentUIScale();
    const y = velAtClientY(e.clientY);
    if (gesture.mode === 'paint') {
      const lo = Math.min(gesture.lastX, x), hi = Math.max(gesture.lastX, x);
      velRefs.forEach(r => {
        if (r.cx < lo - 0.01 || r.cx > hi + 0.01) return;
        const t = (hi === lo) ? 0 : (r.cx - gesture.lastX) / (x - gesture.lastX);
        applyVel(r, gesture.lastY + t * (y - gesture.lastY));
        gesture.touched.add(r);
      });
      gesture.lastX = x; gesture.lastY = y;
    } else {
      const lo = Math.min(gesture.startX, x), hi = Math.max(gesture.startX, x);
      velRefs.forEach((r, i) => {
        if (r.cx < lo - 0.01 || r.cx > hi + 0.01) { applyVel(r, gesture.origVels[i]); return; }
        const t = (hi === lo) ? 0 : (r.cx - gesture.startX) / (x - gesture.startX);
        applyVel(r, gesture.startY + t * (y - gesture.startY));
        gesture.touched.add(r);
      });
    }
    window.dggShowValueTip && window.dggShowValueTip('Vel ' + y, e.clientX, e.clientY);
  });

  const finish = () => {
    if (!gesture) return;
    window.dggHideValueTip && window.dggHideValueTip();
    const g = gesture; gesture = null;
    neVelPreviewStop();
    neFlushVelocities();
    const changed = velRefs.some((r, i) => r.note.vel !== g.origVels[i]);
    if (!changed) return;
    const beforeLocked = snapshotLocked(block);
    const touchedTrackIds = new Set([...g.touched].map(r => r.trackId));
    touchedTrackIds.forEach(tid => markTrackManuallyEdited(block, tid, [...g.touched].filter(r => r.trackId === tid).map(r => r.note)));
    const afterLocked = snapshotLocked(block);
    block.generated = false;
    velNudgeSelKey = null;
    rebuildFullFromEditedBlocks();
    renderNoteEditor();
    const notes = velRefs.map(r => r.note);
    const before = g.origVels.slice();
    const after = notes.map(n => n.vel);
    const n = before.filter((v, i) => v !== after[i]).length;
    const label = g.mode === 'ramp' ? `Velocity ramp (${n} note${n === 1 ? '' : 's'})` : `Paint velocity (${n} note${n === 1 ? '' : 's'})`;
    pushUndo(label,
      () => { notes.forEach((nt, i) => { nt.vel = before[i]; }); restoreLocked(block, beforeLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); },
      () => { notes.forEach((nt, i) => { nt.vel = after[i]; }); restoreLocked(block, afterLocked); block.generated = false; rebuildFullFromEditedBlocks(); renderNoteEditor(); }
    );
  };
  velInner.addEventListener('pointerup', finish);
  velInner.addEventListener('pointercancel', finish);
}