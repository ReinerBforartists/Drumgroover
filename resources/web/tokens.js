// Reads design tokens (CSS custom properties from style.css :root) for canvas / inline-style code.
// Values are cached; call DGTokens.refresh() after the theme changes.
const DGTokens = (() => {
  let cache = Object.create(null);
  function get(name) {
    if (!(name in cache)) cache[name] = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return cache[name];
  }
  // rgba() string from a #rrggbb token and an alpha 0..1
  function withAlpha(name, a) {
    const h = get(name).replace('#', '');
    const n = parseInt(h.length === 3 ? h.replace(/./g, '$&$&') : h, 16);
    return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
  }
  function refresh() { cache = Object.create(null); }
  return { get, withAlpha, refresh };
})();

// Instrument family colors. Hue / saturation per family and the lightness per use come from the --inst-* tokens,
// so every screen (note editor, mixer, keyboard, legend) derives its instrument color from the same values.
const DGInstrument = (() => {
  function key(family) { return family === 'hihat' ? 'hihat-closed' : family; }
  function hs(family) {
    let f = key(family);
    let h = parseFloat(DGTokens.get('--inst-' + f + '-h'));
    if (isNaN(h)) { f = 'none'; h = parseFloat(DGTokens.get('--inst-none-h')); }
    return { h, s: parseFloat(DGTokens.get('--inst-' + f + '-s')) };
  }
  function lightness(name) { return parseFloat(DGTokens.get('--inst-l-' + name)); }
  function css(family, l) { const c = hs(family); return `hsl(${c.h}, ${c.s}%, ${+l.toFixed(1)}%)`; }
  function hex(family, l, dh = 0) {
    const c = hs(family), s = c.s / 100, ll = l / 100, hue = (c.h + dh + 360) % 360;
    const a = s * Math.min(ll, 1 - ll);
    const f = n => { const k = (n + hue / 30) % 12; return ll - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
    return '#' + [f(0), f(8), f(4)].map(v => Math.round(v * 255).toString(16).padStart(2, '0')).join('');
  }
  // Note-editor track color: articulation `index` of `count` within its group steps through the light range
  // (--inst-l-note-*) and drifts in hue by up to ±--inst-note-hue-spread.
  function trackHex(family, index, count) {
    const lo = lightness('note-min'), hi = lightness('note-max');
    const t = count > 1 ? index / (count - 1) : 0.5;
    return hex(family, lo + (hi - lo) * t, parseFloat(DGTokens.get('--inst-note-hue-spread')) * (2 * t - 1));
  }
  return { hs, lightness, css, hex, trackHex };
})();
