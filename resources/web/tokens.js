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
