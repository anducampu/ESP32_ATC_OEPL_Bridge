// Web-port twin of render_test.cpp: node tools/render_test.mjs stats 250 128 [name mv board screen updated]
// Needs the web page's render.js (and fonts.js next to it): RENDER_JS=/path/to/render.js, default
// ../../oepl_web/render.js (the layout of the project this sketch comes from).
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const renderUrl = process.env.RENDER_JS ? pathToFileURL(resolve(process.env.RENDER_JS)).href
  : new URL('../../oepl_web/render.js', import.meta.url).href;
const { Plane, renderStats, renderSpec } = await import(renderUrl);
const args = process.argv.slice(2);
let hid;
if (args.length > 3 && args.at(-1).startsWith('hid=')) {
  const [t, r, b, l] = args.pop().slice(4).split(',').map(Number);
  hid = { t, r, b, l };
}
const [mode, W, H, ...a] = args;
const blk = new Plane(+W, +H), red = new Plane(+W, +H);
if (mode === 'stats') {
  renderStats({ name: a[0] ?? 'ATC_123456', mv: +(a[1] ?? 3030), boardC: +(a[2] ?? 14), screenC: +(a[3] ?? 18),
    updated: a[4] ?? '14:37' }, blk, red, true, hid);
} else renderSpec(a[0], blk, red, true, hid);
let s = '';
for (let y = 0; y < +H; y++) {
  for (let x = 0; x < +W; x++) s += red.d[y * W + x] ? 'r' : blk.d[y * W + x] ? '#' : '.';
  s += '\n';
}
process.stdout.write(s);
