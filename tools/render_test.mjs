// Web-port twin of render_test.cpp: node tools/render_test.mjs stats 250 128 [name mv board screen updated]
// Needs the web page's render.js: RENDER_JS=/path/to/render.js (default: ../../oepl_web/render.js).
import { pathToFileURL } from 'node:url';
const { Plane, renderStats, renderSpec } = await import(
  pathToFileURL(process.env.RENDER_JS || new URL('../../oepl_web/render.js', import.meta.url).pathname).href);
const [mode, W, H, ...a] = process.argv.slice(2);
const blk = new Plane(+W, +H), red = new Plane(+W, +H);
if (mode === 'stats') {
  renderStats({ name: a[0] ?? 'ATC_9B0442', mv: +(a[1] ?? 3030), boardC: +(a[2] ?? 14), screenC: +(a[3] ?? 18),
    updated: a[4] ?? '14:37' }, blk, red, true);
} else renderSpec(a[0], blk, red, true);
let s = '';
for (let y = 0; y < +H; y++) {
  for (let x = 0; x < +W; x++) s += red.d[y * W + x] ? 'r' : blk.d[y * W + x] ? '#' : '.';
  s += '\n';
}
process.stdout.write(s);
