// RLE roundtrip tests for rleEncode/rleDecode in common.js
// Run: node tools/tests/test_js_rle.mjs
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __dir = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(__dir, '../../wled00/data/common.js'), 'utf8');

// Extract just the rleEncode and rleDecode function bodies
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

const fnSrc = extractFn(src, 'rleEncode') + '\n' + extractFn(src, 'rleDecode');
const { rleEncode, rleDecode } = new Function(`${fnSrc}; return { rleEncode, rleDecode };`)();

let passed = 0, failed = 0;

function assert(name, got, expected) {
  const ok = got.length === expected.length && got.every((v, i) => v === expected[i]);
  if (ok) { passed++; }
  else { failed++; console.error(`FAIL: ${name}\n  got:      [${Array.from(got)}]\n  expected: [${Array.from(expected)}]`); }
}

function roundtrip(name, data) {
  const src = new Uint8Array(data);
  const enc = rleEncode(src);
  const dec = rleDecode(enc);
  assert(name, dec, src);
}

// 1. Empty input
roundtrip('empty', []);

// 2. Single byte
roundtrip('single byte', [0xAB]);

// 3. Run of 3
roundtrip('run of 3', [0x42, 0x42, 0x42]);

// 4. Run of 128
roundtrip('run of 128', new Array(128).fill(0xFF));

// 5. Run of 129 (splits into two tokens)
roundtrip('run of 129', new Array(129).fill(0x01));

// 6. Alternating bytes (worst case for RLE)
roundtrip('alternating', [0xAA, 0xBB, 0xAA, 0xBB, 0xAA, 0xBB]);

// 7. Mixed runs and literals
roundtrip('mixed', [0x01, 0x02, 0x03, 0x04, 0x04, 0x04, 0x04, 0x05, 0x06]);

// 8. Random data (100 iterations)
let allRandOk = true;
for (let i = 0; i < 100; i++) {
  const len = Math.floor(Math.random() * 256) + 1;
  const data = new Uint8Array(len);
  for (let j = 0; j < len; j++) data[j] = Math.floor(Math.random() * 256);
  const enc = rleEncode(data);
  const dec = rleDecode(enc);
  if (dec.length !== data.length || !dec.every((v, k) => v === data[k])) {
    allRandOk = false;
    console.error(`FAIL: random[${i}] len=${len}`);
    failed++;
    break;
  }
}
if (allRandOk) { passed++; }

// 9. Worst case: 256 distinct bytes
roundtrip('full byte range', Array.from({length: 256}, (_, i) => i));

// 10. All zeros (best case compression)
roundtrip('all zeros 512', new Array(512).fill(0));

// 11. RGBW pixel pattern (4-byte pixels, temporal coherence)
const rgbw = [];
for (let i = 0; i < 50; i++) rgbw.push(255, 0, 0, 64);
roundtrip('RGBW pixels', rgbw);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
