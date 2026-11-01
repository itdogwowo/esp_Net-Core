#!/usr/bin/env node
// host.mjs — 效果模組 ABI 的 host 端參考實作（Mac 上跑，不需要板子）
//
// 它模擬 esp_Net-Core 的 compute 這一格：
//   ① 依 manifest 算佈局（desc / frame / scratch）
//   ② 把 descriptor 寫進 guest 線性記憶體
//   ③ 呼叫 fx_init(desc)，失敗就拒絕載入
//   ④ 每幀呼叫 fx_render(frame_id, time_us, span)，讀回幀緩衝
//
// 也可以模擬「分離式固件」的熱替換：
//   --swap <module.wasm>   在第 N 幀換成另一個模組（同一塊記憶體、同一個 descriptor）
//   --bad  <module.wasm>   試著載入壞模組，驗證「被拒絕、舊的繼續跑」
//
// 用法：
//   node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm [--frames 240] [--ppm out.ppm]
//   node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --swap tools/poc/fx_poc/fx2.wasm --at 120
//   node tools/poc/fx_poc/host.mjs tools/poc/fx_poc/fx.wasm --bad  tools/poc/fx_poc/fxbad.wasm --at 120
//
// 這個檔案刻意零依賴（只用 node 內建的 WebAssembly）。

import fs from 'node:fs';
import path from 'node:path';

// ── manifest：載入前就知道要多少記憶體（對應 config.json / .fxe 的 header）──
const MANIFEST = {
  name: 'poc',
  abi: 0,
  num_pixels: 676,     // P4 port 的 config.json
  matrix: [26, 26],    // [w, h]；條燈填 [n, 1]
  fps: 30,
};

const args = process.argv.slice(2);
const wasmPath = args[0];
if (!wasmPath) {
  console.error('usage: node host.mjs <module.wasm> [--frames N] [--swap m.wasm] [--bad m.wasm] [--at N] [--ppm out.ppm]');
  process.exit(2);
}
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : def;
};
const frames = Number(opt('--frames', 240));
const ppmPath = opt('--ppm', null);
const swapPath = opt('--swap', null);
const badPath = opt('--bad', null);
const swapAt = (swapPath || badPath) ? Number(opt('--at', Math.floor(frames / 2))) : -1;

// ── 佈局：desc 32 B，frame / scratch 各自 32 B 對齊（DMA / cache line）──
const ALIGN = 32;
const align = (n) => (n + ALIGN - 1) & ~(ALIGN - 1);

const px = MANIFEST.num_pixels;
const [w, h] = MANIFEST.matrix;
const fbBytes = px * 4;
const scBytes = px * 4;

let cursor = 64;                       // 0..63 留給 desc
const fbOff = align(cursor); cursor = fbOff + fbBytes;
const scOff = align(cursor); cursor = scOff + scBytes;
const totalBytes = cursor;

// ── 載入 + 接手（②③）：descriptor 只算一次，之後所有模組共用 ──
async function load(p) {
  const bytes = fs.readFileSync(p);
  const { instance } = await WebAssembly.instantiate(bytes, {
    // v0 故意不提供任何 import：模組只能碰自己的線性記憶體。
    // 想要 host 能力（log / rand / 時間）才逐步加，加了就是一次 ABI 版本。
    env: {},
  });
  return { name: path.basename(p), bytes, ex: instance.exports, mem: null };
}

function attach(mod) {
  const abi = mod.ex.fx_abi_version ? mod.ex.fx_abi_version() : -1;
  if (abi !== MANIFEST.abi) return `ABI mismatch: module=${abi} host=${MANIFEST.abi}`;

  mod.mem = new Uint8Array(mod.ex.memory.buffer);
  if (mod.mem.length < totalBytes) {
    return `memory ${mod.mem.length} B < layout ${totalBytes} B`;
  }

  const dv = new DataView(mod.ex.memory.buffer);
  dv.setUint32(0, px, true);
  dv.setUint32(4, w, true);
  dv.setUint32(8, h, true);
  dv.setUint32(12, fbOff, true);
  dv.setUint32(16, fbBytes, true);
  dv.setUint32(20, scOff, true);
  dv.setUint32(24, scBytes, true);

  const rc = mod.ex.fx_init(0);
  return rc === 0 ? null : `fx_init 回 ${rc}`;
}

// ── 跑幀，模擬 frame clock ──
function render(mod, f) {
  const tUs = Math.round((f * 1e6) / MANIFEST.fps);
  mod.ex.fx_render(f, BigInt(tUs), 0, px);   // span 全幀；v0 約定
}

function checksum(mod) {
  const fb = mod.mem.subarray(fbOff, fbOff + fbBytes);
  let c = 0;
  for (let i = 0; i < fbBytes; i++) c = (c + fb[i] * (i + 1)) >>> 0;
  return c;
}

// ── 主流程 ──
const A = await load(wasmPath);
let err = attach(A);
if (err) {
  console.error(`拒絕載入 ${A.name}：${err}`);
  process.exit(1);
}
console.log(`layout      px=${px} ${w}x${h}  fb=${fbOff}+${fbBytes}  scratch=${scOff}+${scBytes}  ` +
            `total=${totalBytes} B (memory ${A.mem.length} B)`);
console.log(`module A    ${A.name} (${A.bytes.length} B wasm, abi ${MANIFEST.abi})  fx_init ok`);

let cur = A;
let swapped = false;
let checksumA = 0;
let nA = 0, nB = 0;
let usA = 0, usB = 0;

for (let f = 0; f < frames; f++) {
  if (!swapped && f === swapAt) {
    swapped = true;
    const target = badPath ?? swapPath;
    const t0 = performance.now();
    const M = await load(target);
    const e = attach(M);
    const dtUs = (performance.now() - t0) * 1000;
    if (e) {
      console.log(`swap @${f}   拒絕載入 ${M.name}（${e}）→ 繼續用 ${cur.name}，畫面不中斷`);
      checksumA = checksum(cur);
    } else {
      checksumA = checksum(cur);
      console.log(`swap @${f}   ${M.name} → fx_init ok（${dtUs.toFixed(0)} us）；` +
                  `同一個 descriptor、同一組 offset，host 沒有重算佈局`);
      cur = M;
    }
  }
  const t0 = performance.now();
  render(cur, f);
  const us = (performance.now() - t0) * 1000;
  if (cur === A) { nA++; usA += us; } else { nB++; usB += us; }
}

if (checksumA === 0) checksumA = checksum(A);
console.log(`phase A     ${nA} 幀  avg ${(usA / Math.max(nA, 1)).toFixed(1)} us  checksum 0x${checksumA.toString(16)}`);
if (nB > 0) {
  console.log(`phase B     ${nB} 幀  avg ${(usB / Math.max(nB, 1)).toFixed(1)} us  ` +
              `checksum 0x${checksum(cur).toString(16)}  (${cur.name})`);
} else if (swapped) {
  console.log(`phase B     —（換檔被拒絕，全程都是 ${A.name}，沒有中斷）`);
}
const nCur = cur === A ? nA : nB;
console.log(`scratch[0]  ${new DataView(cur.ex.memory.buffer).getUint32(scOff, true)} 次  ` +
            `(${cur.name} 被呼叫 ${nCur} 次)`);
console.log('');
console.log('note: 這裡的 us 是 x86/arm64 主機數字，只證明邊界開銷可忽略（每次呼叫 ~ns 級）；');
console.log('      板上要的是 P4 400 MHz 的 cycles/frame，那是 P0 spike 的事。');

if (ppmPath) {
  // 轉成 PPM（P6），方便直接轉 PNG 看畫面
  const fb = cur.mem.subarray(fbOff, fbOff + fbBytes);
  const header = Buffer.from(`P6\n${w} ${h}\n255\n`);
  const out = Buffer.alloc(header.length + px * 3);
  header.copy(out, 0);
  for (let i = 0; i < px; i++) {
    out[header.length + i * 3 + 0] = fb[i * 4 + 0];
    out[header.length + i * 3 + 1] = fb[i * 4 + 1];
    out[header.length + i * 3 + 2] = fb[i * 4 + 2];
  }
  fs.writeFileSync(ppmPath, out);
  console.log(`wrote ${ppmPath}`);
}
