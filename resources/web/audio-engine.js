// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
let audioCtx = null;
let playing = false, playToken = 0;
let mutedInstruments = new Set();
let soloedInstruments = new Set(); // non-empty = solo mode: only these play, mutedInstruments is ignored while any solo is active (standard DAW convention — unsoloing everything falls back to normal mute state)

// Single source of truth for the Note Editor's Mute/Solo: used by triggerInstrument (click previews), by playActiveBlock
// (the Note Editor's own playback) and for the row-label dimming in note-editor-render.js.
function isTrackAudible(trackId) {
  return soloedInstruments.size ? soloedInstruments.has(trackId) : !mutedInstruments.has(trackId);
}

// LAYER 1 — Blocks: named patterns. Continuous-time note lists, not step arrays.
// blocksList: [{id, name, generated, numerator, data: {trackId: [{beat, vel}]}}]
function getAudio(){ if(!audioCtx) audioCtx = new (window.AudioContext||window.webkitAudioContext)(); return audioCtx; }
// Fixed level of the placeholder sounds below (used only in a browser without the plugin host).
function masterGain(){ return 0.8; }

function playKick(ctx,t,vel){ const v=(vel/127)*masterGain(); const osc=ctx.createOscillator(),gain=ctx.createGain();
  osc.type='sine'; osc.frequency.setValueAtTime(150,t); osc.frequency.exponentialRampToValueAtTime(42,t+0.14);
  gain.gain.setValueAtTime(0.001,t); gain.gain.exponentialRampToValueAtTime(0.9*v,t+0.005); gain.gain.exponentialRampToValueAtTime(0.001,t+0.28);
  osc.connect(gain); gain.connect(ctx.destination); osc.start(t); osc.stop(t+0.3); }
function noiseBuffer(ctx,dur){ const n=Math.max(1,Math.floor(ctx.sampleRate*dur)); const buf=ctx.createBuffer(1,n,ctx.sampleRate); const d=buf.getChannelData(0);
  for(let i=0;i<n;i++) d[i]=Math.random()*2-1; return buf; }
function playSnare(ctx,t,vel){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.2);
  const bp=ctx.createBiquadFilter(); bp.type='bandpass'; bp.frequency.value=1800; bp.Q.value=0.8;
  const ng=ctx.createGain(); ng.gain.setValueAtTime(0.7*v,t); ng.gain.exponentialRampToValueAtTime(0.001,t+0.18);
  noise.connect(bp); bp.connect(ng); ng.connect(ctx.destination); noise.start(t); noise.stop(t+0.2);
  const osc=ctx.createOscillator(),og=ctx.createGain(); osc.type='triangle'; osc.frequency.value=190;
  og.gain.setValueAtTime(0.5*v,t); og.gain.exponentialRampToValueAtTime(0.001,t+0.1);
  osc.connect(og); og.connect(ctx.destination); osc.start(t); osc.stop(t+0.1); }
function playRim(ctx,t,vel){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.04);
  const bp=ctx.createBiquadFilter(); bp.type='bandpass'; bp.frequency.value=2600; bp.Q.value=3;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.5*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.04);
  noise.connect(bp); bp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+0.05); }
function playHat(ctx,t,vel,open){ const v=(vel/127)*masterGain(); const dur=open?0.35:0.06;
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,dur);
  const hp=ctx.createBiquadFilter(); hp.type='highpass'; hp.frequency.value=7000;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.35*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+dur);
  noise.connect(hp); hp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+dur+0.02); }
function playRide(ctx,t,vel){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.6);
  const hp=ctx.createBiquadFilter(); hp.type='highpass'; hp.frequency.value=5000;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.25*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.55);
  noise.connect(hp); hp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+0.6);
  const osc=ctx.createOscillator(),og=ctx.createGain(); osc.type='sine'; osc.frequency.value=800;
  og.gain.setValueAtTime(0.15*v,t); og.gain.exponentialRampToValueAtTime(0.001,t+0.4);
  osc.connect(og); og.connect(ctx.destination); osc.start(t); osc.stop(t+0.4); }
function playRideBell(ctx,t,vel){ const v=(vel/127)*masterGain();
  const osc=ctx.createOscillator(),og=ctx.createGain(); osc.type='sine'; osc.frequency.value=1400;
  og.gain.setValueAtTime(0.3*v,t); og.gain.exponentialRampToValueAtTime(0.001,t+0.3);
  osc.connect(og); og.connect(ctx.destination); osc.start(t); osc.stop(t+0.3);
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.15);
  const hp=ctx.createBiquadFilter(); hp.type='highpass'; hp.frequency.value=6500;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.2*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.12);
  noise.connect(hp); hp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+0.15); }
function playHatPedal(ctx,t,vel){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.05);
  const bp=ctx.createBiquadFilter(); bp.type='bandpass'; bp.frequency.value=900; bp.Q.value=1.5;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.4*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.05);
  noise.connect(bp); bp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+0.06); }
function playTom(ctx,t,vel,baseFreq){ const v=(vel/127)*masterGain();
  const osc=ctx.createOscillator(),g=ctx.createGain(); osc.type='sine';
  osc.frequency.setValueAtTime(baseFreq,t); osc.frequency.exponentialRampToValueAtTime(baseFreq*0.6,t+0.18);
  g.gain.setValueAtTime(0.8*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.22);
  osc.connect(g); g.connect(ctx.destination); osc.start(t); osc.stop(t+0.24); }
function playCrash(ctx,t,vel,bright){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,1.4);
  const hp=ctx.createBiquadFilter(); hp.type='highpass'; hp.frequency.value=bright?5000:3800;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.35*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+1.3);
  noise.connect(hp); hp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+1.4); }
function playGeneric(ctx,t,vel){ const v=(vel/127)*masterGain();
  const noise=ctx.createBufferSource(); noise.buffer=noiseBuffer(ctx,0.08);
  const bp=ctx.createBiquadFilter(); bp.type='bandpass'; bp.frequency.value=1500; bp.Q.value=2;
  const g=ctx.createGain(); g.gain.setValueAtTime(0.5*v,t); g.gain.exponentialRampToValueAtTime(0.001,t+0.08);
  noise.connect(bp); bp.connect(g); g.connect(ctx.destination); noise.start(t); noise.stop(t+0.09); }

// Tells the host which track each custom instrument is assigned to (its id and GM note), so hits of that track play the engine
// (playback and preview). Sent when the assignment changes and before every pattern sync.
let customInstrumentSent = null;
function syncCustomInstruments() {
  if (typeof hostBridge === 'undefined' || !hostBridge.send) return;
  const assignments = [];
  for (const d of dggCustomInstruments()) {
    const t = trackList.find(x => x.customInstrument === d.id);
    if (t) assignments.push({ instrument: d.id, trackId: t.id, gm: t.gm });
  }
  const sig = JSON.stringify(assignments);
  if (sig === customInstrumentSent) return;
  customInstrumentSent = sig;
  hostBridge.send('setCustomInstruments', { assignments });
  if (typeof deRebuildCustomEngineCards === 'function') deRebuildCustomEngineCards();   // the cards know the note of their track
}

dggOnInstrumentTable(syncCustomInstruments);   // the instruments come with the table

function triggerInstrument(ctx, track, t, vel) {
  if (!isTrackAudible(track.id)) return;

  // Embedded: play through the selected sound engine (Drum Engine tab
  // dropdown) via 'midiNoteOn' — the same path as its test keyboard —
  // instead of the local WebAudio placeholder synths below. Velocity is
  // included: HandleMidiNoteOn defaults it to 127 otherwise. No
  // 'midiNoteOff' is sent: native voices are self-contained one-shots.
  // This path is for live one-shot triggers (test keyboard / pad clicks)
  // only; pattern playback goes through playPatternNative below.
  if (typeof hostBridge !== 'undefined' && hostBridge.isEmbedded) {
    const note = track.customInstrument ? track.gm : dggGmNoteOfTrackId(track.id);
    if (note != null) {
      hostBridge.send('midiNoteOn', { note, velocity: Math.max(1, Math.min(127, Math.round(vel))) });
      return;
    }
    // Unmapped id — fall through to the local synth as a safety net.
  }

  switch(track.role) {
    case 'kick': playKick(ctx,t,vel); break;
    case 'snare': playSnare(ctx,t,vel); break;
    case 'rim': playRim(ctx,t,vel); break;
    case 'chh': playHat(ctx,t,vel,false); break;
    case 'ohh': playHat(ctx,t,vel,true); break;
    case 'hhPedal': playHatPedal(ctx,t,vel); break;
    case 'ride': playRide(ctx,t,vel); break;
    case 'rideBell': playRideBell(ctx,t,vel); break;
    case 'tomLow': playTom(ctx,t,vel,110); break;
    case 'tomMid': playTom(ctx,t,vel,150); break;
    case 'tomHi': playTom(ctx,t,vel,210); break;
    case 'crash1': playCrash(ctx,t,vel,false); break;
    case 'crash2': playCrash(ctx,t,vel,true); break;
    default: playGeneric(ctx,t,vel);
  }
}
// Note Editor preview — a single one-shot trigger for clicking an
// articulation or instrument name, or placing/dragging a note in the grid
// (see note-editor-render.js / note-editor-interactions.js). Reuses
// triggerInstrument, so preview and playback share the same native-engine /
// mute-solo path.
function nePreviewTrack(trackId, vel) {
  const track = trackList.find(t => t.id === trackId);
  if (!track) return;
  const ctx = getAudio();
  if (ctx.state === 'suspended') ctx.resume().catch(() => {});
  triggerInstrument(ctx, track, ctx.currentTime, vel != null ? vel : 100);
}

function sleep(ms){ return new Promise(r=>setTimeout(r,ms)); }

// Shared playback entry point — both modes (block preview, full song) funnel
// through this; only the note source and the cursor callback differ.
// Embedded: playback runs natively (C++ PreviewScheduler, sample-accurate,
// at the host's live tempo — see dsp/PreviewScheduler.h). Standalone in a
// browser: WebAudio-lookahead JS scheduler (no host tempo to sync to).
function playPattern(getEvents, totalBeats, cursorFn, onStop, opts) {
  opts = opts || {};
  playing = true; playToken++; const token = playToken;
  document.getElementById('statusArea').classList.add('playing');
  document.getElementById('statusText').textContent = 'playing…';

  const loopStart = Math.max(0, Math.min(totalBeats, opts.loopStart ?? 0));
  const loopEnd = Math.max(loopStart, Math.min(totalBeats, opts.loopEnd ?? totalBeats));
  const startBeat = Math.max(loopStart, Math.min(loopEnd, opts.startBeat ?? loopStart));

  const getBounds = opts.getBounds || null;
  if (typeof hostBridge !== 'undefined' && hostBridge.isEmbedded) {
    return playPatternNative(getEvents, totalBeats, cursorFn, onStop, token, loopStart, loopEnd, startBeat, getBounds);
  }
  return playPatternLocal(getEvents, totalBeats, cursorFn, onStop, token, loopStart, loopEnd, startBeat, getBounds);
}

// Optional live bounds source of a running pattern: { key, totalBeats, loopStart, loopEnd }.
// A changed key (e.g. another block opened mid-playback) restarts the loop on the new bounds.
function readBounds(getBounds) {
  if (!getBounds) return null;
  const b = getBounds();
  if (!b) return null;
  const total = Math.max(0, b.totalBeats);
  const ls = Math.max(0, Math.min(total, b.loopStart ?? 0));
  const le = Math.max(ls, Math.min(total, b.loopEnd ?? total));
  return { key: b.key, totalBeats: total, loopStart: ls, loopEnd: le };
}

// Build the fullPattern JSON that HandleSyncPlaybackPattern /
// HandlePreviewPlay parse via FullPatternFromJson (DrumGroover.cpp). One
// shared builder so block-preview and full-song patterns can't diverge in
// shape.
function buildFullPatternPayload(getEvents, totalBeats) {
  const events = getEvents();
  return {
    totalBeats,
    events: events.map(e => ({ trackId: e.trackId, beatAbs: e.beatAbs, vel: e.vel }))
  };
}

// Embedded path — see dsp/PreviewScheduler.h. C++ owns timing and
// note-triggering (ProcessBlock, sample-accurate, at GetTempo()); this only
// sends the pattern once, drives the cursor from 'previewPosition' pushes,
// and re-sends the pattern from the current position when a live edit
// changes it mid-playback.
async function playPatternNative(getEvents, totalBeats, cursorFn, onStop, token, loopStart, loopEnd, startBeat, getBounds) {
  let lastSeenGeneration = dataGeneration;
  let lastKnownBeat = startBeat;
  let boundsKey = readBounds(getBounds)?.key;

  hostBridge.send('previewPlay', {
    fullPattern: buildFullPatternPayload(getEvents, totalBeats),
    startBeat, loopStart, loopEnd
  });

  const offPosition = hostBridge.on('previewPosition', (msg) => {
    if (!playing || token !== playToken) return;
    if (msg.playing) { lastKnownBeat = msg.beat; cursorFn(msg.beat); }
  });

  // UI-thread watcher — no sample accuracy needed, so a plain interval
  // suffices. 120ms matches the knob-drag debounce used elsewhere.
  await new Promise(resolve => {
    const watcher = setInterval(() => {
      if (!playing || token !== playToken) { clearInterval(watcher); resolve(); return; }
      const nb = readBounds(getBounds);
      if (nb && nb.key !== boundsKey) {
        boundsKey = nb.key;
        totalBeats = nb.totalBeats; loopStart = nb.loopStart; loopEnd = nb.loopEnd;
        lastKnownBeat = loopStart;
        lastSeenGeneration = dataGeneration;
        hostBridge.send('previewPlay', {
          fullPattern: buildFullPatternPayload(getEvents, totalBeats),
          startBeat: loopStart, loopStart, loopEnd
        });
        return;
      }
      if (dataGeneration !== lastSeenGeneration) {
        lastSeenGeneration = dataGeneration;
        hostBridge.send('previewPlay', {
          fullPattern: buildFullPatternPayload(getEvents, totalBeats),
          startBeat: lastKnownBeat, loopStart, loopEnd
        });
      }
    }, 120);
  });

  offPosition();
  // Only the most recent play session (highest token) cleans up the shared
  // transport UI; a newer playPattern() call already owns it.
  if (token === playToken) {
    cursorFn(-1);
    if (onStop) onStop();
    document.getElementById('statusArea').classList.remove('playing');
    document.getElementById('statusText').textContent = 'ready';
  }
}

// Standalone-browser fallback — WebAudio-lookahead scheduler, no host tempo
// to sync to. The `bpm` param falls back to 120 (no live UI control in this
// mode).
async function playPatternLocal(getEvents, totalBeats, cursorFn, onStop, token, loopStart, loopEnd, startBeat, getBounds) {
  const ctx = getAudio();
  let boundsKey = readBounds(getBounds)?.key;
  if (ctx.state==='suspended') await ctx.resume().catch(()=>{});

  const QUANTUM = 1/8; // scheduling resolution in beats
  let beatPos = startBeat;
  let evIdx = 0;
  let events = getEvents();
  let lastSeenGeneration = dataGeneration;
  let nextTime = ctx.currentTime + 0.05;
  const seekToIdx = (pos) => { const idx = events.findIndex(ev => ev.beatAbs >= pos); return idx < 0 ? events.length : idx; };
  evIdx = seekToIdx(beatPos);

  while (playing && token===playToken && loopEnd > loopStart) {
    const bpm = getParamNum('bpm', 120);
    // BPM is quarter-note-referenced (SD3/DAW convention); the denominator
    // only changes how long one PULSE (one "beat" unit here) lasts:
    // secPerBeat = (4/denominator) * (60/bpm).
    const denominator = readGrid().denominator || 4;
    const secPerBeat = (4/denominator) * (60/bpm);
    const waitMs = Math.max(0, (nextTime - ctx.currentTime)*1000);
    await sleep(waitMs);
    if (!playing || token!==playToken) break;

    const nb = readBounds(getBounds);
    if (nb && nb.key !== boundsKey) {
      boundsKey = nb.key;
      loopStart = nb.loopStart; loopEnd = nb.loopEnd;
      beatPos = loopStart; events = getEvents(); lastSeenGeneration = dataGeneration;
      evIdx = seekToIdx(beatPos);
      if (loopEnd <= loopStart) break;
    }
    if (dataGeneration !== lastSeenGeneration) {
      // Live edit landed mid-playback (reroll, knob, ...) — re-fetch now
      // and resume evIdx from the current playhead, not from the start.
      lastSeenGeneration = dataGeneration;
      events = getEvents();
      evIdx = seekToIdx(beatPos);
    }

    cursorFn(beatPos);
    while (evIdx < events.length && events[evIdx].beatAbs < beatPos + QUANTUM) {
      const ev = events[evIdx];
      const track = trackList.find(t => t.id === ev.trackId);
      if (track) triggerInstrument(ctx, track, nextTime, ev.vel);
      evIdx++;
    }

    nextTime += QUANTUM*secPerBeat;
    beatPos += QUANTUM;
    // Loop wrap re-fetches as a fallback and wraps to loopStart — a defined
    // loop range repeats itself, not the whole block/song.
    if (beatPos >= loopEnd) { beatPos = loopStart; evIdx = seekToIdx(beatPos); events = getEvents(); lastSeenGeneration = dataGeneration; }
  }
  if (token === playToken) {
    cursorFn(-1);
    if (onStop) onStop();
    document.getElementById('statusArea').classList.remove('playing');
    document.getElementById('statusText').textContent = 'ready';
  }
}

// What the running playback is: 'block' (Note Editor loop) or 'song' (Song Generator / Mixer). The two are separate modes; entering a tab
// of the other mode stops the running one (see switchToTab in ui-controller.js).
let playbackMode = null;
function stopPlayback(){ playing=false; playToken++; playbackMode = null;
  if (typeof hostBridge !== 'undefined' && hostBridge.isEmbedded) hostBridge.send('previewStop', {});
  document.getElementById('playBtn').disabled=false; document.getElementById('stopBtn').disabled=true;
  document.getElementById('statusArea').classList.remove('playing'); document.getElementById('statusText').textContent='ready';
  highlightBeat(-1); highlightTimelineBeat(-1);
}

// Top Play button — loops just the currently open block (editor preview).
// Events are re-fetched live (see playPattern), so edits made while looping
// (e.g. a group's density slider) apply on the next pass.
function playActiveBlock() {
  const block = findBlock(activeBlockId);
  if (!block) return;
  const getEvents = () => {
    const b = findBlock(activeBlockId) || block;
    const evts = [];
    trackList.forEach(t => {
      if (!isTrackAudible(t.id)) return;   // the Note Editor's Mute/Solo
      (b.data[t.id]||[]).forEach(note => evts.push({ trackId:t.id, beatAbs:note.beat, vel:note.vel }));
    });
    evts.sort((a,b2)=>a.beatAbs-b2.beatAbs);
    return evts;
  };
  playbackMode = 'block';
  document.getElementById('playBtn').disabled = true;
  document.getElementById('stopBtn').disabled = false;
  const totalBeats = (block.length || 1) * block.numerator;
  const getBounds = () => {
    const b = findBlock(activeBlockId);
    if (!b) return null;
    const total = (b.length || 1) * b.numerator;
    return { key: b.id, totalBeats: total, loopStart: neLoopStart ?? 0, loopEnd: neLoopEnd ?? total };
  };
  playPattern(getEvents, totalBeats, highlightBeat, () => {
    document.getElementById('playBtn').disabled = false;
    document.getElementById('stopBtn').disabled = true;
  }, { startBeat: nePlayheadBeat, loopStart: neLoopStart ?? 0, loopEnd: neLoopEnd ?? totalBeats, getBounds }).catch(console.error);
}

// Song Generator playback — plays the whole assembled song; the cursor
// sweeps the song timeline instead of the note editor. Live re-fetch on
// every loop pass, same as playActiveBlock(). The shared playBtn/stopBtn
// dispatch is in ui-controller.js.
function playFullSong() {
  if (!fullPattern) generateSequence();
  playbackMode = 'song';
  document.getElementById('playBtn').disabled = true;
  document.getElementById('stopBtn').disabled = false;
  playPattern(() => fullPattern.events, fullPattern.totalBeats, highlightTimelineBeat, () => {
    document.getElementById('playBtn').disabled = false;
    document.getElementById('stopBtn').disabled = true;
  }, { startBeat: tlPlayheadBeat, loopStart: tlLoopStart ?? 0, loopEnd: tlLoopEnd ?? fullPattern.totalBeats }).catch(console.error);
}

// Playhead sweeps the currently-open block's full length (all bars are
// visible, so this is an absolute position, no bar-clipping). Auto-scrolls
// the rows so the sweep stays visible on long blocks.
function highlightBeat(beatPos) {
  const el = document.getElementById('nePlayhead');
  const staticEl = document.querySelector('.ne-ruler-playhead');
  if (beatPos < 0) {
    if (el) el.style.display = 'none';
    if (staticEl) { staticEl.style.display = 'block'; staticEl.style.left = (NE_LEFT_PAD + nePlayheadBeat*neCurrentPxPerBeat) + 'px'; }
    return;
  }
  const block = findBlock(activeBlockId);
  if (!block) { if (el) el.style.display = 'none'; return; }
  if (staticEl) staticEl.style.display = 'none'; // the live cursor below takes over while actually playing
  const x = NE_LEFT_PAD + beatPos * neCurrentPxPerBeat;
  el.style.display = 'block';
  el.style.left = x + 'px';
  const rowsCol = document.querySelector('.ne-rows');
  if (rowsCol) {
    const margin = 60;
    if (x < rowsCol.scrollLeft + margin || x > rowsCol.scrollLeft + rowsCol.clientWidth - margin) {
      rowsCol.scrollLeft = Math.max(0, x - rowsCol.clientWidth/2);
    }
  }
}

// Playhead sweeps across the song timeline's clips as the full song plays.
// beatPos is an absolute beat position from the top of the song (see assembleFullPattern)
// — converts directly to pixels using the same formula as renderTimeline.
function highlightTimelineBeat(beatPos) {
  if (window.mxTimelineSetPlayhead) window.mxTimelineSetPlayhead(beatPos);
  const el = document.getElementById('tlPlayhead');
  const staticEl = document.querySelector('.tl-ruler-playhead');
  const g = readGrid();
  const pxPerBeat = tlCurrentPxPerBar / g.numerator;
  if (beatPos < 0) {
    // Idle — the line stays visible, parked at tlPlayheadBeat (where
    // playback will next start) instead of disappearing.
    if (el) el.style.left = (tlPlayheadBeat * pxPerBeat + tlLeadInPx) + 'px';
    if (staticEl) {
      staticEl.style.display = 'block'; staticEl.style.left = (tlPlayheadBeat*pxPerBeat + tlLeadInPx) + 'px';
    }
    return;
  }
  if (!el) return;
  if (staticEl) staticEl.style.display = 'none'; // the live cursor below takes over while actually playing
  el.style.left = (beatPos * pxPerBeat + tlLeadInPx) + 'px';
}

// Timeline cursor for HOST-driven full-song playback — separate from the
// Play button's per-play-session 'previewPosition' listener above.
// Stateless: DrumGroover.cpp's OnIdle derives the position from the host's
// live PPQ every tick (see dsp/Scheduler.h), so this just draws what it's
// sent.
//
// Registered on 'load' (not at top-level) because bridge.js may not have
// run yet when this script is parsed — by 'load', all script tags have.
window.addEventListener('load', () => {
  if (typeof hostBridge === 'undefined' || !hostBridge.isEmbedded) return;
  hostBridge.on('songPosition', (msg) => {
    // Host transport wins: starting it while a Play-button preview runs
    // would layer two patterns on each other — auto-stop the preview.
    // stopPlayback() is idempotent, so re-checking every tick is harmless.
    if (msg.playing && playing) stopPlayback();

    // Play button stays disabled while the host is playing OR a preview
    // session is running.
    document.getElementById('playBtn').disabled = msg.playing || playing;

    // Yield to an active preview session so the two cursor sources don't
    // fight over the same #tlPlayhead element.
    if (playing) return;
    highlightTimelineBeat(msg.playing ? msg.beat : -1);
  });
});
