// =========================================
// HOST BRIDGE — iPlug2 WebView <-> C++ message layer
// =========================================
// Protocol (IPlugWebViewEditorDelegate.h):
//
//   C++ -> JS: EvaluateJavaScript calls one of five GLOBAL JS FUNCTIONS BY
//   NAME — SCVFD(ctrlTag, normalizedValue), SCMFD(ctrlTag, msgTag, dataSize,
//   base64Data), SPVFD(paramIdx, normalizedValue), SAMFD(msgTag, dataSize,
//   base64Data), SMMFD(statusByte, data1, data2).
//
//   JS -> C++: OnMessageFromWebView(jsonStr) switches on a "msg" field:
//   "SPVFUI" {paramIdx,value}, "BPCFUI"/"EPCFUI" {paramIdx}, "SAMFUI"
//   {msgTag,ctrlTag,data:base64}, "SMMFUI" {statusByte,dataByte1,dataByte2},
//   "SKPFUI" {keyCode,utf8,S,C,A,isUp}. Custom messages (exportSong,
//   saveProject, ...) ride the "SAMFUI" channel.
//
//   msgTag == -1 means the base64 data decodes to a UTF8 JSON string
//   (SendJSONFromDelegate's convention). hostBridge reuses this for its own
//   send()/on() traffic in both directions, so the C++ side only has to
//   special-case msgTag == -1 once.
// =========================================

const hostBridge = (() => {
  const embedded = !!(window.chrome && window.chrome.webview)
    || !!(window.webkit && window.webkit.messageHandlers);

  const JSON_ENVELOPE_MSGTAG = -1; // per SendJSONFromDelegate's convention: msgTag -1 = UTF8 JSON payload
  const K_NO_TAG = -1; // assumed value of IPlug's kNoTag sentinel

  const listeners = new Map(); // type -> Set<fn>

  // --- UTF8-safe base64 helpers: plain btoa/atob mangle non-Latin1
  // characters, which would break our UTF8 JSON payloads. ---
  function utf8ToBase64(str) {
    const bytes = new TextEncoder().encode(str);
    let binary = '';
    for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
    return btoa(binary);
  }
  function base64ToUtf8(b64) {
    const binary = atob(b64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return new TextDecoder().decode(bytes);
  }

  // JS -> C++, via the SAMFUI channel + the JSON-envelope convention.
  //
  // Post the envelope OBJECT directly, never a string: the C++ side reads it
  // with get_WebMessageAsJson(), which re-encodes a posted string as a JSON
  // string literal ("{\"msg\":...}", quoted+escaped) and fails to parse it
  // as an object.
  function send(type, payload) {
    if (!embedded) return false;
    const envelope = { msg: 'SAMFUI', msgTag: JSON_ENVELOPE_MSGTAG, ctrlTag: K_NO_TAG, data: utf8ToBase64(JSON.stringify({ type, payload })) };
    if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(envelope);
    } else if (window.webkit && window.webkit.messageHandlers) {
      const handler = window.webkit.messageHandlers.iplug || Object.values(window.webkit.messageHandlers)[0];
      if (handler) handler.postMessage(envelope);
      else return false;
    } else {
      return false;
    }
    return true;
  }

  function on(type, fn) {
    if (!listeners.has(type)) listeners.set(type, new Set());
    listeners.get(type).add(fn);
    return () => listeners.get(type).delete(fn);
  }

  function dispatch(type, payload) {
    const set = listeners.get(type);
    if (set) set.forEach(fn => { try { fn(payload); } catch (err) { console.error('hostBridge listener for', type, 'threw:', err); } });
  }

  // --- The five global entry points iPlug2's EvaluateJavaScript calls by
  // name (signatures in the file header). Only SAMFD carries our own bridge
  // traffic; the rest are stubs so a C++ call doesn't fail with
  // "not a function". ---

  window.SAMFD = function (msgTag, dataSize, base64Data) {
    if (msgTag !== JSON_ENVELOPE_MSGTAG) return; // not our envelope convention — ignore (plugin-specific opaque message)
    let msg;
    try { msg = JSON.parse(base64ToUtf8(base64Data)); } catch (err) { console.error('hostBridge: malformed SAMFD JSON payload', err); return; }
    if (!msg || typeof msg.type !== 'string') return;
    dispatch(msg.type, msg.payload);
  };

  window.SPVFD = function (/* paramIdx, normalizedValue */) {
    // Intentionally empty: host parameter automation is not implemented
    // (no IParam behind most knobs).
  };
  window.SCVFD = function (/* ctrlTag, normalizedValue */) {};
  window.SCMFD = function (/* ctrlTag, msgTag, dataSize, base64Data */) {};

  // C++ delivers physical MIDI (e.g. Nektar keyboard) via
  // EvaluateJavaScript("SMMFD(status, data1, data2)") — a direct call like
  // the other four entry points above.
  //
  // statusByte: high nibble = message type (0x90 Note On, 0x80 Note Off),
  // low nibble = MIDI channel. Note On with velocity 0 is the MIDI
  // convention for Note Off (running-status optimization of many
  // keyboards/hosts) — same check as DrumGroover.cpp's ProcessMidiMsg().
  //
  // Dispatches 'nativeNoteOn'/'nativeNoteOff', which drum-engine-tab.js
  // listens for via hostBridge.on(...).
  window.SMMFD = function (statusByte, data1, data2) {
    const type = statusByte & 0xF0;
    if (type === 0x90 && data2 > 0) {
      dispatch('nativeNoteOn', { note: data1 });
    } else if (type === 0x80 || (type === 0x90 && data2 === 0)) {
      dispatch('nativeNoteOff', { note: data1 });
    }
  };

  return { isEmbedded: embedded, send, on };
})();

// Per-instance localStorage namespacing
// Instance ID is passed via URL query parameter ?instanceId=... so it is
// available synchronously on page load, before any localStorage access.
// The C++ side also sends an 'instanceId' message as a fallback.
window.DGG_INSTANCE_ID = null;
window.DGG_INSTANCE_READY = false;
window._dggInstanceReadyCbs = [];
function _dggNotifyInstanceReady() {
  if (window.DGG_INSTANCE_READY) return;
  window.DGG_INSTANCE_READY = true;
  const cbs = window._dggInstanceReadyCbs.slice();
  window._dggInstanceReadyCbs.length = 0;
  cbs.forEach(cb => { try { cb(); } catch(e) { console.error('dgg instance ready cb error', e); } });
}
function dggOnReady(cb) {
  if (window.DGG_INSTANCE_READY) { cb(); } else { window._dggInstanceReadyCbs.push(cb); }
}
(function() {
  try {
    const params = new URLSearchParams(window.location.search);
    const idFromUrl = params.get('instanceId');
    if (idFromUrl) window.DGG_INSTANCE_ID = idFromUrl;
  } catch(e) {}
  if (window.DGG_INSTANCE_ID) _dggNotifyInstanceReady();
})();
hostBridge.on('instanceId', (payload) => {
  if (payload && payload.id) {
    if (!window.DGG_INSTANCE_ID) {
      window.DGG_INSTANCE_ID = payload.id;
      _dggNotifyInstanceReady();
    }
  }
});

// Instrument table (host: dsp/808InstrumentTable.h): identity, GM note, engine, knob schema and defaults of every instrument
// instance. Sent once per editor load, before the instance id; dggOnInstrumentTable(cb) runs cb with it as soon as it is here.
let INSTRUMENT_TABLE = null;
const dggInstrumentTableWaiters = [];
function dggOnInstrumentTable(cb) {
  if (INSTRUMENT_TABLE) cb(INSTRUMENT_TABLE); else dggInstrumentTableWaiters.push(cb);
}
function dggReceiveInstrumentTable(payload) {
  if (INSTRUMENT_TABLE || !payload || !Array.isArray(payload.instruments)) return;
  INSTRUMENT_TABLE = payload;
  dggInstrumentTableWaiters.splice(0).forEach(cb => { try { cb(INSTRUMENT_TABLE); } catch(e) { console.error('dgg instrument table cb error', e); } });
}
hostBridge.on('instrumentTable', dggReceiveInstrumentTable);
// The custom instruments (rows of family 'custom' of the table), in the order of their paramIndex: [{ id, name, index }]. A custom
// instrument is a circuit of the 808 engine that a track can be assigned to (Add Track dialog); empty until the table has arrived.
// The kit track of an id or role: its GM note and its name in the track list / MIDI export ('custom' is the generic name of a custom track).
function dggGmNoteOfTrackId(id) {
  const r = INSTRUMENT_TABLE && INSTRUMENT_TABLE.instruments.find(x => x.id === id && x.gm !== null);
  return r ? r.gm : undefined;
}
function dggTrackNameOfRole(role) {
  if (role === 'custom') return 'Custom';
  const r = INSTRUMENT_TABLE && INSTRUMENT_TABLE.instruments.find(x => x.id === role && x.trackName !== null);
  return r ? r.trackName : undefined;
}
function dggCustomInstruments() {
  if (!INSTRUMENT_TABLE) return [];
  return INSTRUMENT_TABLE.instruments.filter(r => r.family === 'custom').sort((a, b) => a.paramIndex - b.paramIndex)
    .map(r => ({ id: r.id, name: r.name, index: r.paramIndex }));
}
function dggStorageKey(key) {
  return window.DGG_INSTANCE_ID ? `dgg:${window.DGG_INSTANCE_ID}:${key}` : null;
}
function dggGetItem(key) {
  const k = dggStorageKey(key);
  return k ? localStorage.getItem(k) : null;
}
function dggSetItem(key, value) {
  const k = dggStorageKey(key);
  if (k) localStorage.setItem(k, value);
}

// Suppress WebView2/Chromium page zoom (Ctrl+wheel, pinch): the app has its
// own dedicated zoom control where it's wanted (Timeline), and there's no
// scrollbar fallback if the whole page zooms instead. preventDefault() on
// the ctrlKey 'wheel' event stops the browser zoom before it applies.
window.addEventListener('wheel', (e) => {
  if (e.ctrlKey) e.preventDefault();
}, { passive: false });
