// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
class ArticulationAndCCMapper {
  constructor() {
    // Articulation velocity bands tailored for SD3 & Battery 4 sample triggering
    this.articulationProfiles = {
      kick:   { soft: [1, 40], normal: [41, 80], hard: [81, 110], accent: [111, 127] },
      snare:  { soft: [1, 45], normal: [46, 85], hard: [86, 115], accent: [116, 127] },
      rim:    { soft: [1, 40], normal: [41, 75], hard: [76, 105], accent: [106, 127] },
      chh:    { soft: [1, 50], normal: [51, 90], hard: [91, 115], accent: [116, 127] },
      ohh:    { soft: [1, 50], normal: [51, 90], hard: [91, 115], accent: [116, 127] },
      hhPedal:{ soft: [1, 45], normal: [46, 80], hard: [81, 105], accent: [106, 127] },
      ride:   { soft: [1, 45], normal: [46, 85], hard: [86, 110], accent: [111, 127] },
      rideBell:{ soft: [1, 50], normal: [51, 90], hard: [91, 115], accent: [116, 127] },
      tomLow: { soft: [1, 45], normal: [46, 80], hard: [81, 105], accent: [106, 127] },
      tomMid: { soft: [1, 45], normal: [46, 80], hard: [81, 105], accent: [106, 127] },
      tomHi:  { soft: [1, 45], normal: [46, 80], hard: [81, 105], accent: [106, 127] },
      crash1: { soft: [1, 50], normal: [51, 90], hard: [91, 115], accent: [116, 127] },
      crash2: { soft: [1, 50], normal: [51, 90], hard: [91, 115], accent: [116, 127] },
      custom: { soft: [1, 45], normal: [46, 80], hard: [81, 105], accent: [106, 127] }
    };

    // Default CC values per role optimized for SD3 / Battery 4 dynamics & panning
    this.ccDefaults = {
      kick:   { cc1: 80, cc10: 64, cc11: 100 },
      snare:  { cc1: 80, cc10: 64, cc11: 95 },
      rim:    { cc1: 75, cc10: 60, cc11: 90 },
      chh:    { cc1: 85, cc10: 40, cc11: 100 },
      ohh:    { cc1: 85, cc10: 80, cc11: 100 },
      hhPedal:{ cc1: 70, cc10: 64, cc11: 85 },
      ride:   { cc1: 80, cc10: 70, cc11: 95 },
      rideBell:{ cc1: 75, cc10: 65, cc11: 90 },
      tomLow: { cc1: 80, cc10: 30, cc11: 95 },
      tomMid: { cc1: 80, cc10: 50, cc11: 95 },
      tomHi:  { cc1: 80, cc10: 70, cc11: 95 },
      crash1: { cc1: 85, cc10: 75, cc11: 100 },
      crash2: { cc1: 85, cc10: 80, cc11: 100 },
      custom: { cc1: 80, cc10: 64, cc11: 90 }
    };
  }

  mapArticulation(role, velocity) {
    const profile = this.articulationProfiles[role] || this.articulationProfiles.custom;
    if (velocity <= profile.soft[1]) return { type: 'soft', index: 0 };
    if (velocity <= profile.normal[1]) return { type: 'normal', index: 1 };
    if (velocity <= profile.hard[1]) return { type: 'hard', index: 2 };
    return { type: 'accent', index: 3 };
  }

  // Generate CC automation events for a role over totalTicks. Step resolution
  // is fixed to one quarter-note (tpq) regardless of denominator — only
  // totalTicks is denominator-aware, via the caller's ticksPerPulse math.
  generateCCAutomation(role, totalTicks, tpq) {
    const cc = this.ccDefaults[role] || this.ccDefaults.custom;
    const events = [];
    const step = tpq;
    for (let tick = 0; tick <= totalTicks; tick += step) {
      events.push({ tick, data: [0xb9, 1, cc.cc1] });
      events.push({ tick, data: [0xb9, 10, cc.cc10] });
      events.push({ tick, data: [0xb9, 11, cc.cc11] });
    }
    return events;
  }

  // Enrich fullPattern with articulation metadata and CC tracks for export
  enrichExportPayload(fullPattern, grid, params) {
    const { events, totalBeats } = fullPattern;
    const tpq = 480;
    // beatAbs is a PULSE index (numerator per bar), not a quarter note:
    // ticksPerPulse = tpq * (4 / denominator).
    const denominator = (grid && grid.denominator) || 4;
    const ticksPerPulse = tpq * (4 / denominator);
    const totalTicks = totalBeats * ticksPerPulse;
    const enriched = {
      meta: [],
      tracks: {},
      cc: {}
    };

    trackList.forEach(t => { enriched.tracks[t.id] = []; enriched.cc[t.id] = []; });

    events.forEach(ev => {
      const track = trackList.find(t => t.id === ev.trackId);
      if (!track) return;
      const articulation = this.mapArticulation(track.role, ev.vel);
      enriched.tracks[track.id].push({
        tick: Math.max(0, Math.round(ev.beatAbs * ticksPerPulse)),
        noteOn: [0x99, track.gm, ev.vel],
        noteOff: [0x89, track.gm, 0],
        articulation,
        velocity: ev.vel
      });
    });

    trackList.forEach(t => {
      enriched.cc[t.id] = this.generateCCAutomation(t.role, totalTicks, tpq);
    });

    return enriched;
  }
}

// =========================================
// MIDI FILE EXPORTER (Type 1, Multi-Track, SD3/B4 Compatible + Markers)
// =========================================
// Embedded in the plugin, byte generation is done by the C++ port
// (dsp/MidiFileExporter.cpp, dsp/ArticulationAndCCMapper.cpp): exportSong /
// exportBlock / exportAllBlocks ship the pattern data to C++ over hostBridge,
// and C++ encodes the file and shows the native Save dialog. Standalone in a
// browser, this class does the byte generation itself.
class MidiFileExporter {
  constructor() {
    this.tpq = 480;
    this.mapper = new ArticulationAndCCMapper();
  }

  vlq(n) {
    n = Math.max(0, n | 0);
    const buf = [n & 0x7f];
    n >>>= 7;
    while (n > 0) {
      buf.unshift((n & 0x7f) | 0x80);
      n >>>= 7;
    }
    return buf;
  }

  writeMetaEvent(absoluteTick, type, data) {
    const bytes = [];
    bytes.push(0xff, type);
    if (Array.isArray(data)) {
      bytes.push(data.length);
      bytes.push(...data);
    } else {
      bytes.push(data);
    }
    return bytes;
  }

  writeTempoEvent(absoluteTick, bpm) {
    const us = Math.round(60000000 / bpm);
    return this.writeMetaEvent(absoluteTick, 0x51, [
      (us >> 16) & 0xff,
      (us >> 8) & 0xff,
      us & 0xff
    ]);
  }

  writeTimeSigEvent(absoluteTick, numerator, denominator, clocksPerClick = 24, notation = 4) {
    const denLog2 = Math.log2(denominator);
    return this.writeMetaEvent(absoluteTick, 0x58, [
      numerator,
      denLog2,
      clocksPerClick,
      notation
    ]);
  }

  writeKitMeta(absoluteTick, kitName, genre) {
    const nameBytes = kitName.split('').map(c => c.charCodeAt(0));
    const genreBytes = genre.split('').map(c => c.charCodeAt(0));
    return [
      { tick: absoluteTick, data: this.writeMetaEvent(absoluteTick, 0x03, nameBytes) },
      { tick: absoluteTick, data: this.writeMetaEvent(absoluteTick, 0x01, genreBytes) }
    ];
  }

  // Write Section Marker (0x05)
  writeSectionMarker(absoluteTick, sectionName) {
    const bytes = sectionName.split('').map(c => c.charCodeAt(0));
    return this.writeMetaEvent(absoluteTick, 0x05, bytes);
  }

  writeTrackChunk(events) {
    events.sort((a, b) => a.tick - b.tick);
    const trackData = [];
    let lastTick = 0;
    for (const ev of events) {
      const delta = ev.tick - lastTick;
      if (delta < 0) continue;
      trackData.push(...this.vlq(delta));
      trackData.push(...ev.data);
      lastTick = ev.tick;
    }
    trackData.push(...this.vlq(0), 0xff, 0x2f, 0x00);
    const header = [0x4d, 0x54, 0x72, 0x6b, (trackData.length >> 24) & 0xff, (trackData.length >> 16) & 0xff, (trackData.length >> 8) & 0xff, trackData.length & 0xff];
    return new Uint8Array([...header, ...trackData]);
  }

  // Builds the raw .mid bytes for one pattern (song or single block).
  // Shared by exportSong/exportBlock/exportAllBlocks; sectionMarkerLabel is
  // the only content difference between whole-song and single-block exports.
  buildMidiBytes(fullPattern, params, grid, sectionMarkerLabel) {
    const enriched = this.mapper.enrichExportPayload(fullPattern, grid, params);
    const tpq = this.tpq;
    const denominator = (grid && grid.denominator) || 4;
    const ticksPerPulse = tpq * (4 / denominator);
    const trackChunks = [];

    // 1. Meta Track
    const metaEvents = [
      { tick: 0, data: this.writeTempoEvent(0, params.bpm) },
      { tick: 0, data: this.writeTimeSigEvent(0, grid.numerator, denominator) },
      ...this.writeKitMeta(0, params.kitName || 'DGG-Generated', params.genre || 'Custom'),
      { tick: 0, data: this.writeSectionMarker(0, sectionMarkerLabel) }
    ];
    trackChunks.push(this.writeTrackChunk(metaEvents));

    // 2. Instrument Tracks
    for (const track of trackList) {
      const evts = enriched.tracks[track.id] || [];
      if (evts.length === 0) continue;

      const nameStr = dggTrackNameOfRole(track.role) || track.name || 'Custom';
      const nameEvent = [{ tick: 0, data: this.writeMetaEvent(0, 0x03, nameStr.split('').map(c => c.charCodeAt(0))) }];

      const noteOns = evts.map(e => ({ tick: e.tick, data: e.noteOn }));
      const noteOffs = evts.map(e => ({ tick: e.tick + Math.max(10, Math.round(grid.snap * ticksPerPulse * 0.5)), data: e.noteOff }));

      const trackData = [...nameEvent, ...noteOns, ...noteOffs];
      trackChunks.push(this.writeTrackChunk(trackData));
    }

    // 3. CC Tracks
    for (const track of trackList) {
      const ccEvts = enriched.cc[track.id] || [];
      if (ccEvts.length === 0) continue;
      const trackData = ccEvts;
      trackChunks.push(this.writeTrackChunk(trackData));
    }

    // 4. Header
    const header = new Uint8Array([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, trackChunks.length, (tpq >> 8) & 0xff, tpq & 0xff]);
    const totalSize = header.length + trackChunks.reduce((n, c) => n + c.length, 0);
    const out = new Uint8Array(totalSize);
    let off = 0;
    out.set(header, off); off += header.length;
    for (const chunk of trackChunks) { out.set(chunk, off); off += chunk.length; }
    return out;
  }

  downloadBytes(bytes, filename) {
    const url = URL.createObjectURL(new Blob([bytes], { type: 'audio/midi' }));
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    a.click(); URL.revokeObjectURL(url);
  }

  slugify(str) {
    return String(str || 'block').trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '') || 'block';
  }

  // Whole-song export. Embedded: ships {fullPattern, params, grid} to C++,
  // shaped to match the dgg::FullPattern / dgg::ExportParams / dgg::Grid types.
  exportSong(fullPattern, params, grid) {
    if (hostBridge.isEmbedded) {
      hostBridge.send('exportSong', { fullPattern, params, grid });
      return;
    }
    const bytes = this.buildMidiBytes(fullPattern, params, grid, 'Full Pattern');
    this.downloadBytes(bytes, `dgg-song-${timestampSlug()}.mid`);
    document.getElementById('statusText').textContent = 'MIDI Song exported (with Markers)';
  }

  // Single-block export. Embedded: ships the Block to C++ — id/name/length/
  // numerator/data match dgg::Block field-for-field.
  exportBlock(block, params, grid, stamp) {
    if (hostBridge.isEmbedded) {
      hostBridge.send('exportBlock', { block, params, grid, stamp: stamp || timestampSlug() });
      return `dgg-block-${this.slugify(block.name)}-${stamp || timestampSlug()}.mid`;
    }
    const events = [];
    trackList.forEach(t => {
      (block.data[t.id] || []).forEach(note => events.push({ trackId: t.id, beatAbs: note.beat, vel: note.vel }));
    });
    events.sort((a, b) => a.beatAbs - b.beatAbs);
    const totalBeats = (block.length || 1) * block.numerator;
    const blockPattern = { events, totalBeats };
    const blockGrid = { ...grid, numerator: block.numerator };

    const bytes = this.buildMidiBytes(blockPattern, params, blockGrid, block.name || 'Block');
    const filename = `dgg-block-${this.slugify(block.name)}-${stamp || timestampSlug()}.mid`;
    this.downloadBytes(bytes, filename);
    return filename;
  }

  // Export every block as its own .mid file, sharing one timestamp slug so
  // filenames stay consistent across the batch. The standalone setTimeout
  // staggering exists only because browsers prompt for multiple downloads.
  exportAllBlocks(blocks, params, grid) {
    if (!blocks || !blocks.length) {
      document.getElementById('statusText').textContent = 'No blocks to export';
      return;
    }
    const stamp = timestampSlug();
    if (hostBridge.isEmbedded) {
      hostBridge.send('exportBlocks', { blocks, params, grid });   // one save dialog for the whole batch
      return;
    }
    blocks.forEach((block, i) => {
      setTimeout(() => this.exportBlock(block, params, grid, stamp), i * 200);
    });
    document.getElementById('statusText').textContent = `Exporting ${blocks.length} block${blocks.length === 1 ? '' : 's'}…`;
  }
}

const midiExporter = new MidiFileExporter();

// Embedded-only replies from C++ confirming an export finished or failed.
hostBridge.on('exportComplete', (payload) => {
  const status = document.getElementById('statusText');
  if (status) status.textContent = payload && payload.filename ? `Exported ${payload.filename}` : 'Export complete';
});
hostBridge.on('exportFailed', (payload) => {
  const status = document.getElementById('statusText');
  if (status) status.textContent = payload && payload.error ? `Export failed: ${payload.error}` : 'Export failed';
});

// Drag-out-of-plugin export (native OLE drag, started by C++): dragExportReady / dragExportFailed are status-bar feedback only.
// Both replies arrive only after the OS drag has ended, so neither may leave
// text behind: a finished or cancelled drag restores the normal status, a real
// export error is shown briefly.
function resetDragStatus() {
  const status = document.getElementById('statusText');
  if (status) status.textContent = (typeof playing !== 'undefined' && playing) ? 'playing…' : 'ready';
}
hostBridge.on('dragExportReady', () => resetDragStatus());
hostBridge.on('dragExportFailed', (payload) => {
  const status = document.getElementById('statusText');
  if (payload && payload.error && payload.error !== 'drag cancelled or rejected') {
    if (status) status.textContent = `Drag export failed: ${payload.error}`;
    setTimeout(resetDragStatus, 4000);
  } else {
    resetDragStatus();
  }
});

// A file dropped onto the plugin window (e.g. our own drag-out coming back in) would
// otherwise be treated as a navigation and downloaded by WebView2. Refuse file drops.
const hasFiles = (e) => e.dataTransfer && Array.from(e.dataTransfer.types || []).includes('Files');
window.addEventListener('dragover', (e) => { if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = 'none'; } });
window.addEventListener('drop', (e) => { if (hasFiles(e)) e.preventDefault(); });

// =========================================
// STARTUP
// =========================================

// generateSequence is async (it awaits hostBridge round trips to generate the
// default blocks), so the knob sync below must wait for it — otherwise it runs
// against a still-empty activeBlockId. Wrapped in an async IIFE because this
// is a classic <script> (no top-level await).
(async () => {
  try {
    await new Promise(resolve => dggOnInstrumentTable(resolve));   // the track list is built from the instrument table
    await generateSequence(true);
    syncKnobsToBlock(findBlock(activeBlockId));
    // Dirty baseline: set once the async default blocks exist.
    markProjectClean();
    initKnobs();
    wireNudgeButtons();
    if (typeof dggOnReady === 'function') {
      dggOnReady(initTabs);
    } else {
      initTabs();
    }

    // Fit the timeline to the default song so "100%" shows the whole song
    // (same "100% = fits" convention as the note editor). Deferred to the next
    // frame for an accurate clientWidth; tlFittedOnce provides the lazy fallback
    // when this tab was hidden at load.
    if (document.querySelector('.tab-panel[data-tab-panel="timeline"]')?.classList.contains('active')) {
      tlFittedOnce = true;
      requestAnimationFrame(() => fitTimelineZoom());
    }
  } finally {
    // The UI is fully built now; only then may the host start baking the factory-default cache sets, so on first start the
    // dialog and its progress bar are visible from the beginning (deSendFactoryDefaults in drum-engine-tab.js).
    if (typeof deSendFactoryDefaults === 'function') deSendFactoryDefaults();
  }
})();
