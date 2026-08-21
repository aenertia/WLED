// RLE roundtrip tests for rleEncode/rleDecode in common.js
// Run: npm test
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'fs';
import { execSync } from 'child_process';
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

const fnSrc = extractFn(src, 'rleEncode') + '\n' + extractFn(src, 'rleDecode')
  + '\n' + extractFn(src, 'rleTupleEncode') + '\n' + extractFn(src, 'rleTupleDecode')
  + '\n' + extractFn(src, 'rlePlanarEncode') + '\n' + extractFn(src, 'rlePlanarDecode');
const { rleEncode, rleDecode, rleTupleEncode, rleTupleDecode, rlePlanarEncode, rlePlanarDecode } = new Function(
  `${fnSrc}; return { rleEncode, rleDecode, rleTupleEncode, rleTupleDecode, rlePlanarEncode, rlePlanarDecode };`
)();

function roundtrip(data) {
  const input = new Uint8Array(data);
  const enc = rleEncode(input);
  const dec = rleDecode(enc);
  assert.deepStrictEqual(Array.from(dec), Array.from(input));
}

describe('byte-RLE', () => {
  it('empty', () => roundtrip([]));

  it('single byte', () => roundtrip([0xAB]));

  it('run of 3', () => roundtrip([0x42, 0x42, 0x42]));

  it('run of 128', () => roundtrip(new Array(128).fill(0xFF)));

  it('run of 129 (splits into two tokens)', () => roundtrip(new Array(129).fill(0x01)));

  it('alternating (worst case)', () => roundtrip([0xAA, 0xBB, 0xAA, 0xBB, 0xAA, 0xBB]));

  it('mixed runs and literals', () => roundtrip([0x01, 0x02, 0x03, 0x04, 0x04, 0x04, 0x04, 0x05, 0x06]));

  it('random roundtrip x100', () => {
    for (let i = 0; i < 100; i++) {
      const len = Math.floor(Math.random() * 256) + 1;
      const data = new Uint8Array(len);
      for (let j = 0; j < len; j++) data[j] = Math.floor(Math.random() * 256);
      const enc = rleEncode(data);
      const dec = rleDecode(enc);
      assert.deepStrictEqual(Array.from(dec), Array.from(data), `random[${i}] len=${len}`);
    }
  });

  it('full byte range', () => roundtrip(Array.from({length: 256}, (_, i) => i)));

  it('all zeros 512', () => roundtrip(new Array(512).fill(0)));

  it('RGBW pixels', () => {
    const rgbw = [];
    for (let i = 0; i < 50; i++) rgbw.push(255, 0, 0, 64);
    roundtrip(rgbw);
  });
});

function tupleRoundtrip(data, ch) {
  const input = new Uint8Array(data);
  const enc = rleTupleEncode(input, ch);
  const dec = rleTupleDecode(enc, ch);
  assert.deepStrictEqual(Array.from(dec), Array.from(input));
}

describe('tuple-RLE', () => {
  it('empty input', () => tupleRoundtrip([], 3));

  it('single pixel (3 bytes)', () => tupleRoundtrip([255, 0, 0], 3));

  it('run of 3 identical pixels encodes as RUN (4 bytes)', () => {
    const px = [10, 20, 30];
    const input = new Uint8Array([...px, ...px, ...px]);
    const enc = rleTupleEncode(input, 3);
    assert.strictEqual(enc.length, 4); // 1 ctrl + 3 data
    assert.strictEqual(enc[0], 2); // run-1 = 2, bit7=0
    assert.deepStrictEqual(Array.from(enc.subarray(1)), px);
    const dec = rleTupleDecode(enc, 3);
    assert.deepStrictEqual(Array.from(dec), Array.from(input));
  });

  it('run of 128 identical pixels is single RUN token', () => {
    const px = [0xAA, 0xBB, 0xCC];
    const input = new Uint8Array(128 * 3);
    for (let i = 0; i < 128; i++) { input[i*3]=px[0]; input[i*3+1]=px[1]; input[i*3+2]=px[2]; }
    const enc = rleTupleEncode(input, 3);
    assert.strictEqual(enc.length, 4); // 1 ctrl + 3 data
    assert.strictEqual(enc[0], 127); // 128-1
    const dec = rleTupleDecode(enc, 3);
    assert.deepStrictEqual(Array.from(dec), Array.from(input));
  });

  it('alternating pixels are literals, expansion < 1.5%', () => {
    const a = [255, 0, 0], b = [0, 255, 0];
    const input = new Uint8Array(200 * 3);
    for (let i = 0; i < 200; i++) {
      const p = i % 2 === 0 ? a : b;
      input[i*3]=p[0]; input[i*3+1]=p[1]; input[i*3+2]=p[2];
    }
    const enc = rleTupleEncode(input, 3);
    assert.ok(enc.length < input.length * 1.015, `expansion ${enc.length}/${input.length}`);
    const dec = rleTupleDecode(enc, 3);
    assert.deepStrictEqual(Array.from(dec), Array.from(input));
  });

  it('random roundtrip x20 (3-byte aligned)', () => {
    for (let t = 0; t < 20; t++) {
      const nPx = Math.floor(Math.random() * 100) + 1;
      const data = new Uint8Array(nPx * 3);
      for (let j = 0; j < data.length; j++) data[j] = Math.floor(Math.random() * 256);
      tupleRoundtrip(data, 3);
    }
  });

  it('RGBW (channels=4) roundtrip', () => {
    const buf = [];
    for (let i = 0; i < 40; i++) buf.push(255, 0, 0, 128);
    for (let i = 0; i < 10; i++) buf.push(0, 255, 0, 64);
    tupleRoundtrip(buf, 4);
  });

  it('cross-impl: solid red 800px JS encode matches Python', () => {
    const input = new Uint8Array(800 * 3);
    for (let i = 0; i < 800; i++) { input[i*3]=255; input[i*3+1]=0; input[i*3+2]=0; }
    const jsEnc = rleTupleEncode(input, 3);
    const pyScript = [
      'import sys; sys.path.insert(0,"tools")',
      'from ddp_codec import rle_tuple_encode',
      'buf = bytes([255,0,0]*800)',
      'enc = rle_tuple_encode(buf, 3)',
      'sys.stdout.buffer.write(enc)'
    ].join('; ');
    const pyEnc = execSync(`python3 -c '${pyScript}'`, { cwd: join(__dir, '../..') });
    assert.deepStrictEqual(Array.from(jsEnc), Array.from(pyEnc));
  });
});

function planarRoundtrip(data, ch) {
  const input = new Uint8Array(data);
  const enc = rlePlanarEncode(input, ch);
  const dec = rlePlanarDecode(enc, ch);
  assert.deepStrictEqual(Array.from(dec), Array.from(input));
}

describe('planar-RLE', () => {
  it('empty input roundtrip', () => {
    const enc = rlePlanarEncode(new Uint8Array(0), 3);
    assert.strictEqual(enc.length, 6); // 3 channels * 2-byte zero length
    for (let i = 0; i < 6; i++) assert.strictEqual(enc[i], 0);
    const dec = rlePlanarDecode(enc, 3);
    assert.strictEqual(dec.length, 0);
  });

  it('single pixel (3 bytes) roundtrip', () => planarRoundtrip([255, 128, 0], 3));

  it('solid red 800px roundtrip', () => {
    const buf = new Uint8Array(800 * 3);
    for (let i = 0; i < 800; i++) { buf[i*3]=255; buf[i*3+1]=0; buf[i*3+2]=0; }
    planarRoundtrip(buf, 3);
  });

  it('rainbow gradient: planar < byte-level RLE', () => {
    // Red-to-blue sweep: R ramps down, G=0 (constant -- compresses to one run),
    // B ramps up. Interleaved bytes are all different so byte-RLE can't compress,
    // but per-channel planes have long runs (G) and smooth ramps (R, B).
    const buf = new Uint8Array(800 * 3);
    for (let i = 0; i < 800; i++) {
      buf[i*3]   = 255 - ((i * 255 / 799) | 0);
      buf[i*3+1] = 0;
      buf[i*3+2] = (i * 255 / 799) | 0;
    }
    const planarEnc = rlePlanarEncode(buf, 3);
    const byteEnc = rleEncode(buf);
    assert.ok(planarEnc.length < byteEnc.length,
      `planar ${planarEnc.length} should be < byte-RLE ${byteEnc.length}`);
    planarRoundtrip(buf, 3);
  });

  it('random roundtrip x20 (3-byte aligned)', () => {
    for (let t = 0; t < 20; t++) {
      const nPx = Math.floor(Math.random() * 100) + 1;
      const data = new Uint8Array(nPx * 3);
      for (let j = 0; j < data.length; j++) data[j] = Math.floor(Math.random() * 256);
      planarRoundtrip(data, 3);
    }
  });

  it('RGBW (channels=4) roundtrip', () => {
    const buf = [];
    for (let i = 0; i < 40; i++) buf.push(255, 0, 0, 128);
    for (let i = 0; i < 10; i++) buf.push(0, 255, 0, 64);
    planarRoundtrip(buf, 4);
  });

  it('cross-impl: solid red 800px JS encode matches Python', () => {
    const input = new Uint8Array(800 * 3);
    for (let i = 0; i < 800; i++) { input[i*3]=255; input[i*3+1]=0; input[i*3+2]=0; }
    const jsEnc = rlePlanarEncode(input, 3);
    const pyScript = [
      'import sys; sys.path.insert(0,"tools")',
      'from ddp_codec import rle_planar_encode',
      'buf = bytes([255,0,0]*800)',
      'enc = rle_planar_encode(buf)',
      'sys.stdout.buffer.write(enc)'
    ].join('; ');
    const pyEnc = execSync(`python3 -c '${pyScript}'`, { cwd: join(__dir, '../..') });
    assert.deepStrictEqual(Array.from(jsEnc), Array.from(pyEnc));
  });
});

describe('cross-impl roundtrip', () => {
    function randomPixels(n) {
        const buf = new Uint8Array(n * 3);
        for (let i = 0; i < buf.length; i++) buf[i] = Math.floor(Math.random() * 256);
        return buf;
    }

    function pyCodec(variant, direction, data) {
        const funcs = {
            byte:   { encode: 'rle_encode',        decode: 'rle_decode' },
            tuple:  { encode: 'rle_tuple_encode',   decode: 'rle_tuple_decode' },
            planar: { encode: 'rle_planar_encode',  decode: 'rle_planar_decode' },
        };
        const fn = funcs[variant][direction];
        const call = variant === 'byte' ? `${fn}(d)` : `${fn}(d,3)`;
        const script = `import sys;sys.path.insert(0,"tools");from ddp_codec import ${fn};d=sys.stdin.buffer.read();sys.stdout.buffer.write(${call})`;
        const result = execSync(`python3 -c '${script}'`, {
            input: Buffer.from(data),
            cwd: join(__dir, '../..'),
        });
        return new Uint8Array(result);
    }

    it('byte-RLE: JS encode -> Python decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = pyCodec('byte', 'decode', rleEncode(original));
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });

    it('byte-RLE: Python encode -> JS decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = rleDecode(pyCodec('byte', 'encode', original));
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });

    it('tuple-RLE: JS encode -> Python decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = pyCodec('tuple', 'decode', rleTupleEncode(original, 3));
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });

    it('tuple-RLE: Python encode -> JS decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = rleTupleDecode(pyCodec('tuple', 'encode', original), 3);
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });

    it('planar-RLE: JS encode -> Python decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = pyCodec('planar', 'decode', rlePlanarEncode(original, 3));
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });

    it('planar-RLE: Python encode -> JS decode (10 random)', () => {
        for (let i = 0; i < 10; i++) {
            const original = randomPixels(800);
            const decoded = rlePlanarDecode(pyCodec('planar', 'encode', original), 3);
            assert.deepStrictEqual(decoded, original, `round ${i}`);
        }
    });
});

describe('delta-only', () => {
    it('XOR roundtrip', () => {
        const a = new Uint8Array([255, 0, 0, 0, 255, 0, 0, 0, 255]);
        const b = new Uint8Array([128, 64, 32, 16, 8, 4, 2, 1, 0]);
        const delta = new Uint8Array(a.length);
        for (let i = 0; i < a.length; i++) delta[i] = a[i] ^ b[i];
        const recovered = new Uint8Array(delta.length);
        for (let i = 0; i < delta.length; i++) recovered[i] = delta[i] ^ b[i];
        assert.deepStrictEqual(recovered, a);
    });
});
