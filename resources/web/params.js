// =========================================
// PARAM ACCESS LAYER
// =========================================
// Centralized read/write for global plugin-level controls (BPM, time signature,
// subdivision, master volume, group knobs, randomize amount, etc.), backed by DOM elements.

function getParamEl(id) {
  return document.getElementById(id);
}

// Numeric read. Returns fallback if element missing or value not finite.
function getParamNum(id, fallback = 0) {
  const el = getParamEl(id);
  if (!el) return fallback;
  const n = +el.value;
  return Number.isFinite(n) ? n : fallback;
}

// Write value to control. Does not dispatch 'input'/'change' events;
// callers that need listeners to react must trigger them explicitly.
// Silently no-ops if the element does not exist.
function setParamValue(id, value) {
  const el = getParamEl(id);
  if (el) el.value = value;
}