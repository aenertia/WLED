// Cross-implementation compression tests: JS encode vs Python encode
// 3 RLE variants x 2 patterns at 800px, plus JS encode timing
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'fs';
import { execSync } from 'child_process';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { performance } from 'perf_hooks';

const __dir = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(__dir, '../..');
const src = readFileSync(join(repoRoot, 'wled00/data/common.js'), 'utf8');

// Extract rleEncode from common.js (only byte-level RLE lives there)
function extractFn(source, name) {
  const start = source.indexOf(`function ${name}(`);
  if (start < 0) throw new Error(`${name} not found`);
  let depth = 0, i = start;
  while (i < source.length) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}') { depth--; if (depth === 0) return source.slice(start, i + 1); }
    i++;
  }
  throw new Error(`${name}: unmatched braces`);
}

const { rleEncode } = new Function(
  `${extractFn(src, 'rleEncode')}; return { rleEncode };`
)();

// Tuple-RLE: same PackBits control byte but operates on ch-byte tuples.
// Ported from ddp_codec.py rle_tuple_encode.
function rleTupleEncode(src, ch) {
  const n = src.length;
  if (n === 0) return new Uint8Array(0);
  const nt = (n / ch) | 0;
  const out = [];
  let i = 0;

  function eq(aOff, bOff) {
    for (let k = 0; k < ch; k++) if (src[aOff + k] !== src[bOff + k]) return false;
    return true;
  }

  while (i < nt) {
    const off = i * ch;
    let run = 1;
    while (i + run < nt && eq(off, (i + run) * ch) && run < 128) run++;

    if (run >= 3) {
      out.push(run - 1);
      for (let k = 0; k < ch; k++) out.push(src[off + k]);
      i += run;
    } else {
      const litStart = i;
      let litLen = 0;
      while (i < nt && litLen < 128) {
        const toff = i * ch;
        let ahead = 1;
        while (i + ahead < nt && eq(toff, (i + ahead) * ch) && ahead < 3) ahead++;
        if (ahead >= 3) break;
        i++;
        litLen++;
      }
      if (litLen) {
        out.push(0x80 | (litLen - 1));
        for (let k = litStart * ch; k < (litStart + litLen) * ch; k++) out.push(src[k]);
      }
    }
  }
  return new Uint8Array(out);
}

// Planar-RLE: split interleaved pixels into per-channel planes,
// byte-RLE each plane, prefix with 2-byte LE length.
// Ported from ddp_codec.py rle_planar_encode.
function rlePlanarEncode(src, ch) {
  const n = src.length;
  if (n === 0) {
    return new Uint8Array(ch * 2); // ch x 2-byte zero length
  }
  const pixelCount = (n / ch) | 0;
  const out = [];
  for (let c = 0; c < ch; c++) {
    const plane = new Uint8Array(pixelCount);
    for (let p = 0; p < pixelCount; p++) plane[p] = src[p * ch + c];
    const enc = rleEncode(plane);
    out.push(enc.length & 0xFF, (enc.length >> 8) & 0xFF);
    for (let k = 0; k < enc.length; k++) out.push(enc[k]);
  }
  return new Uint8Array(out);
}

// --- pattern generators ---

function hsvToRgb(h, s, v) {
  h = ((h % 360) + 360) % 360;
  const c = v * s;
  const x = c * (1 - Math.abs((h / 60) % 2 - 1));
  const m = v - c;
  let r, g, b;
  if (h < 60)       { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else              { r = c; g = 0; b = x; }
  return [(r + m) * 255 | 0, (g + m) * 255 | 0, (b + m) * 255 | 0];
}

function rainbow(n) {
  const buf = new Uint8Array(n * 3);
  for (let i = 0; i < n; i++) {
    const [r, g, b] = hsvToRgb(i * 360 / n, 1, 1);
    buf[i * 3] = r; buf[i * 3 + 1] = g; buf[i * 3 + 2] = b;
  }
  return buf;
}

// 2% random pixels lit on black, deterministic LCG seed
function sparseTwinkle(n) {
  const buf = new Uint8Array(n * 3);
  let seed = 12345;
  function lcg() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; }
  for (let i = 0; i < n; i++) {
    if (lcg() % 100 < 2) {
      buf[i * 3]     = lcg() & 0xff;
      buf[i * 3 + 1] = lcg() & 0xff;
      buf[i * 3 + 2] = lcg() & 0xff;
    }
  }
  return buf;
}

// --- Python encode via child_process ---

function pythonEncode(variant, data) {
  const hex = Buffer.from(data).toString('hex');
  let pyFn;
  if (variant === 'byte') pyFn = 'rle_encode';
  else if (variant === 'tuple') pyFn = 'rle_tuple_encode';
  else if (variant === 'planar') pyFn = 'rle_planar_encode';
  else throw new Error(`unknown variant: ${variant}`);

  const tupleArg = variant === 'byte' ? '' : ', 3';
  const script = [
    'import sys; sys.path.insert(0,"tools")',
    `from ddp_codec import ${pyFn}`,
    `data = bytes.fromhex("${hex}")`,
    `enc = ${pyFn}(data${tupleArg})`,
    'sys.stdout.buffer.write(enc)'
  ].join('; ');
  return new Uint8Array(execSync(`python3 -c '${script}'`, { cwd: repoRoot }));
}

// --- JS encode dispatch ---

function jsEncode(variant, data) {
  if (variant === 'byte') return rleEncode(data);
  if (variant === 'tuple') return rleTupleEncode(data, 3);
  if (variant === 'planar') return rlePlanarEncode(data, 3);
  throw new Error(`unknown variant: ${variant}`);
}

// --- tests ---

const N = 800;
const patterns = {
  rainbow: rainbow(N),
  sparse_twinkle: sparseTwinkle(N),
};

describe('cross-impl compression (800px)', () => {
  for (const variant of ['byte', 'tuple', 'planar']) {
    for (const [name, data] of Object.entries(patterns)) {
      it(`${variant}-RLE ${name} JS matches Python`, () => {
        const js = jsEncode(variant, data);
        const py = pythonEncode(variant, data);
        assert.deepStrictEqual(
          Array.from(js), Array.from(py),
          `${variant}-RLE ${name}: JS len=${js.length} vs Python len=${py.length}`
        );
      });
    }
  }
});

describe('JS encode timing (800px)', () => {
  for (const variant of ['byte', 'tuple', 'planar']) {
    it(`${variant}-RLE encode < 5ms`, () => {
      const data = patterns.rainbow;
      // warmup
      jsEncode(variant, data);
      // timed run (median of 11)
      const times = [];
      for (let i = 0; i < 11; i++) {
        const t0 = performance.now();
        jsEncode(variant, data);
        times.push(performance.now() - t0);
      }
      times.sort((a, b) => a - b);
      const median = times[5];
      console.log(`    ${variant}-RLE 800px encode: ${median.toFixed(3)}ms (median of 11)`);
      assert.ok(median < 5, `${variant}-RLE encode took ${median.toFixed(3)}ms, expected < 5ms`);
    });
  }
});
