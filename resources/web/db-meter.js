// =========================================
// DB METER — reusable peak meter: absolute-dB color zones, adjustable
// falloff, peak-hold.
// =========================================
// Color zones are anchored to absolute dB positions on the meter's fixed
// scale (DB_METER_MIN..DB_METER_MAX) via a full-height gradient revealed
// through clip-path, so yellow appears only once the signal reaches
// DB_METER_YELLOW_AT and red at/above DB_METER_RED_AT — the same scale
// regardless of how tall the current fill is.
//
// The bar has instant attack and a fixed falloff (release) rate; the peak
// marker holds the loudest level for a user-adjustable time in seconds
// (see dbMeterPeakHoldSec below), then falls back like the bar.
//
// Usage: createDbMeter(containerEl, opts) — sizing comes from the
// container's own CSS; this only fills it. Returns
// { setDb(db), reset(), destroy() }.

// dB range the meter's scale covers. DB_METER_MAX is intentionally > 0 —
// headroom above digital zero so a hot signal visibly runs into the red
// zone instead of pinning flat at the top.
const DB_METER_MIN = -60;
const DB_METER_MAX = 18;

// Zone boundaries: green below -6 dBFS, yellow -6..<0 dBFS, red >= 0 dBFS.
const DB_METER_YELLOW_AT = -6;
const DB_METER_RED_AT = 0;

// Scale tick labels. The full set adds -18 dBFS (a common digital-audio
// reference point) to the compact set. Pass `ticks:'full'` to get it.
const DB_METER_TICKS_COMPACT = [-24, -12, -6, -3, 0];
const DB_METER_TICKS_FULL = [-24, -18, -12, -6, -3, 0];
// Matches the mixer's master scale (+18 .. -60): the same tick set the mixer
// draws next to its master fader (see SCALE_TICKS in mixer-tab.js).
const DB_METER_TICKS_MASTER = [-60, -48, -36, -24, -12, 0, 6, 12, 18];

// Well below DB_METER_MIN — used as the "no signal yet" resting value so
// a freshly created meter (or one fed a non-finite dB, e.g. -Infinity)
// reads as fully empty rather than clamping to some arbitrary in-range
// value.
const DB_METER_FLOOR = -60;

function dbMeterPercent(db) {
  const clamped = Math.max(DB_METER_MIN, Math.min(DB_METER_MAX, db));
  return ((clamped - DB_METER_MIN) / (DB_METER_MAX - DB_METER_MIN)) * 100;
}

// Same color-zone gradient for both layers (dim "track" background, bright
// "fill" overlay) — computed from the constants above rather than
// hardcoded in CSS, so it can't drift out of sync with the zone
// boundaries.
function _dbMeterGradientCss(bright) {
  const green  = bright ? '#4ade80' : '#1f3d2a';
  const yellow = bright ? '#facc15' : '#4a3f14';
  const red    = bright ? '#ef4444' : '#4a1f1f';
  const y = dbMeterPercent(DB_METER_YELLOW_AT);
  const r = dbMeterPercent(DB_METER_RED_AT);
  return `linear-gradient(to top, ${green} 0%, ${green} ${y}%, ${yellow} ${y}%, ${yellow} ${r}%, ${red} ${r}%, ${red} 100%)`;
}

// The single solid color for whichever zone `db` falls in — used by the
// peak marker. Same three colors/thresholds as _dbMeterGradientCss's
// bright layer, just picking one instead of blending.
function _dbMeterZoneColor(db) {
  if (db >= DB_METER_RED_AT) return '#ef4444';
  if (db >= DB_METER_YELLOW_AT) return '#facc15';
  return '#4ade80';
}

// Shared updater for the optional numeric peak readout element (mixer strips,
// drum-engine side meter): shows the held peak in its zone color, hides it
// once the peak decays back to the floor (channel not carrying audio).
function _dbMeterUpdateReadout(readoutEl, peakDb) {
  if (!readoutEl) return;
  if (peakDb > DB_METER_MIN) {
    readoutEl.style.display = 'block';
    readoutEl.textContent = (peakDb >= 0 ? '+' : '') + peakDb.toFixed(1);
    readoutEl.style.color = _dbMeterZoneColor(peakDb);
  } else {
    readoutEl.style.display = 'none';
  }
}

// Falloff (release) rate, dB/sec — how fast the bar (and, after its hold time,
// the peak marker) drops back down once the signal gets quieter. 150 dB/sec = 0.44 s for the full 78 dB scale:
// the bar tracks the signal almost live, with just enough smoothing to avoid single-frame flicker.
const DB_METER_FALLOFF_DB_PER_SEC = 150;

// Peak hold time, seconds — how long the peak marker and the peak value stay
// at the loudest level before they fall back. User-adjustable (Options menu >
// Meter Peak Hold, available in both Drum Engine and Mixer) and shared by every
// meter instance. A louder reading replaces the peak and restarts the hold.
const DB_METER_PEAK_HOLD_KEY = 'dggMeterPeakHoldSec';
let dbMeterPeakHoldSec = parseFloat(dggGetItem(DB_METER_PEAK_HOLD_KEY));
if (!(dbMeterPeakHoldSec > 0)) dbMeterPeakHoldSec = 2;

function setDbMeterPeakHold(sec) {
  dbMeterPeakHoldSec = sec;
  dggSetItem(DB_METER_PEAK_HOLD_KEY, String(sec));
}

// One shared requestAnimationFrame loop drives every live meter instance
// (instead of one rAF callback per meter) — cheap even with many meters
// open at once.
const _dbMeterInstances = new Set();
let _dbMeterRafRunning = false;

function _dbMeterTick(nowMs) {
  for (const m of _dbMeterInstances) m._tick(nowMs);
  if (_dbMeterInstances.size) requestAnimationFrame(_dbMeterTick);
  else _dbMeterRafRunning = false;
}
function _dbMeterEnsureRaf() {
  if (!_dbMeterRafRunning) { _dbMeterRafRunning = true; requestAnimationFrame(_dbMeterTick); }
}

// Builds one meter's DOM inside `container` (replaces its contents) and
// returns a controller.
//   opts.ticks: 'compact' (default) | 'full' | 'none'
function createDbMeter(container, opts) {
  opts = opts || {};
  const ticks = opts.ticks === 'none' ? []
    : (opts.ticks === 'full'   ? DB_METER_TICKS_FULL
    : (opts.ticks === 'master' ? DB_METER_TICKS_MASTER
    :                            DB_METER_TICKS_COMPACT));
  // Optional per-tick callback — lets a wrapper (e.g. the stereo meter's
  // numeric peak readout) piggyback on the shared rAF loop instead of
  // running its own.
  const onTick = typeof opts.onTick === 'function' ? opts.onTick : null;
  // Optional read-only numeric peak value element (e.g. above the meter);
  // driven from this meter's own tick — see _dbMeterUpdateReadout.
  const readoutEl = opts.readoutEl || null;

  container.classList.add('db-meter');
  container.innerHTML = `
    <div class="db-meter-bar">
      <div class="db-meter-track"></div>
      <div class="db-meter-fill"></div>
      <div class="db-meter-peak"></div>
    </div>
    <div class="db-meter-ticks">${ticks.map(db => `<div class="db-meter-tick${db >= 0 ? ' db-meter-tick-nosign' : ''}" style="bottom:${dbMeterPercent(db)}%"><span>${db}</span></div>`).join('')}</div>
  `;
  const trackEl = container.querySelector('.db-meter-track');
  const fillEl  = container.querySelector('.db-meter-fill');
  const peakEl  = container.querySelector('.db-meter-peak');
  trackEl.style.background = _dbMeterGradientCss(false);
  fillEl.style.background  = _dbMeterGradientCss(true);

  const state = {
    displayedDb: DB_METER_FLOOR,
    targetDb: DB_METER_FLOOR,
    peakDb: DB_METER_FLOOR,
    peakSetMs: null,
    lastTickMs: null,
    _tick(nowMs) {
      if (this.lastTickMs == null) this.lastTickMs = nowMs;
      // Clamped so a stalled/backgrounded tab resuming later can't produce
      // one giant catch-up jump in the falloff math.
      const dtSec = Math.min(0.25, (nowMs - this.lastTickMs) / 1000);
      this.lastTickMs = nowMs;

      if (this.targetDb >= this.displayedDb) this.displayedDb = this.targetDb; // attack: instant
      else this.displayedDb = Math.max(this.targetDb, this.displayedDb - DB_METER_FALLOFF_DB_PER_SEC * dtSec); // release: fixed rate

      // Peak: loudest raw reading (targetDb, not the falloff-smoothed
      // displayedDb — a real transient counts even if the bar hasn't
      // caught up yet). It stays for dbMeterPeakHoldSec after it was set (a
      // louder reading replaces it and restarts the hold), then falls at the
      // bar's rate, down to the current bar level.
      if (this.targetDb >= this.peakDb) {
        this.peakDb = this.targetDb;
        this.peakSetMs = nowMs;
      } else if (this.peakSetMs == null || nowMs - this.peakSetMs >= dbMeterPeakHoldSec * 1000) {
        this.peakDb = Math.max(this.displayedDb, this.peakDb - DB_METER_FALLOFF_DB_PER_SEC * dtSec);
      }

      const fillPct = dbMeterPercent(this.displayedDb);
      fillEl.style.clipPath = `inset(${100 - fillPct}% 0 0 0)`;
      const showPeak = this.peakDb > DB_METER_MIN;
      peakEl.style.display = showPeak ? '' : 'none';
      if (showPeak) {
        peakEl.style.bottom = dbMeterPercent(this.peakDb) + '%';
        peakEl.style.background = _dbMeterZoneColor(this.peakDb);
      }
      _dbMeterUpdateReadout(readoutEl, this.peakDb);
      if (onTick) onTick();
    },
    // db may be null/-Infinity/NaN (e.g. silence) — all treated as "floor".
    setDb(db) {
      this.targetDb = (db == null || !isFinite(db)) ? DB_METER_FLOOR : db;
      _dbMeterInstances.add(this);
      _dbMeterEnsureRaf();
    },
    reset() {
      this.displayedDb = this.targetDb = this.peakDb = DB_METER_FLOOR;
      this.lastTickMs = this.peakSetMs = null;
      _dbMeterUpdateReadout(readoutEl, DB_METER_FLOOR);
    },
    // Removes the meter from the shared rAF loop (no leak if a channel's
    // meter is torn down).
    destroy() { _dbMeterInstances.delete(this); },
  };

  _dbMeterInstances.add(state);
  _dbMeterEnsureRaf();
  return state;
}

// Stereo meter: a thin L/R wrapper around createDbMeter() — two mono
// meters side by side packaged as one controller.
//
// Usage: createStereoDbMeter(containerEl, opts) — same opts as
// createDbMeter (ticks are drawn only on the right/R bar). Sizing: give
// the container a width covering BOTH bars plus the 2px gap between them
// (e.g. 16px total for two 7px bars).
function createStereoDbMeter(container, opts) {
  opts = opts || {};
  container.classList.add('db-meter-stereo');
  container.innerHTML = `
    <div class="db-meter-stereo-bar" data-chan="l"></div>
    <div class="db-meter-stereo-bar" data-chan="r"></div>
  `;
  // Optional numeric peak readout element (e.g. the mixer's value sitting
  // directly above the meter). Driven from the shared rAF tick: shows the
  // louder of the two bars' held peaks in its zone color, and hides itself
  // as soon as the peak decays back to the floor — i.e. only while the
  // channel actually carries audio. Read-only; it never drives the meter.
  const readoutEl = opts.readoutEl || null;
  let l = null, r = null;
  // Louder of the two bars decides value + color (same rule as the marker).
  const updateReadout = () => { if (l && r) _dbMeterUpdateReadout(readoutEl, Math.max(l.peakDb, r.peakDb)); };
  l = createDbMeter(container.querySelector('[data-chan="l"]'), { ...opts, ticks: 'none', onTick: updateReadout });
  r = createDbMeter(container.querySelector('[data-chan="r"]'), { ...opts, onTick: updateReadout });
  if (readoutEl) readoutEl.style.display = 'none'; // hidden until the first peak
  return {
    // dbR defaults to dbL, so a mono value drives both bars.
    setDb(dbL, dbR) { l.setDb(dbL); r.setDb(dbR == null ? dbL : dbR); },
    reset() { l.reset(); r.reset(); updateReadout(); },
    destroy() { l.destroy(); r.destroy(); },
  };
}
