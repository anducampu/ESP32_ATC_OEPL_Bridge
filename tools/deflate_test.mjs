// Web-port twin of deflate_test.cpp: node tools/deflate_test.mjs < in > out (same bytes expected).
// Needs the web page's oepl.js: OEPL_JS=/path/to/oepl.js, default ../../oepl_web/oepl.js.
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { readFileSync } from 'node:fs';
const url = process.env.OEPL_JS ? pathToFileURL(resolve(process.env.OEPL_JS)).href
  : new URL('../../oepl_web/oepl.js', import.meta.url).href;
const { zlibCompress } = await import(url);
process.stdout.write(zlibCompress(new Uint8Array(readFileSync(0))));
