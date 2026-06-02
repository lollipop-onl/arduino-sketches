"use strict";

// ===== 共有状態 =====
// シリアル受信で更新し、描画ループ(rAF)が参照する。
const state = {
  x: 512,
  y: 512,
  sw: 1,        // 1=離す, 0=押下
  connected: false,
};
// 中央キャリブレーション offset(KY-023 は個体ごとに中央値がずれる)
const center = { x: 512, y: 512 };

// ===== DOM =====
const $ = (id) => document.getElementById(id);
const els = {
  unsupported: $("unsupported"),
  connect: $("connect"),
  disconnect: $("disconnect"),
  status: $("status"),
  vx: $("vx"), vy: $("vy"), vsw: $("vsw"),
  voff: $("voff"),
  led: $("led"),
  calibrate: $("calibrate"),
  log: $("log"),
  plot: $("plot"),
  stage: $("stage"),
};

// ===== Web Serial =====
let port = null;
let reader = null;
let keepReading = false;

function supported() {
  return "serial" in navigator;
}

async function connect() {
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
  } catch (e) {
    // ユーザがダイアログをキャンセルした等
    setStatus("接続キャンセル / 失敗: " + e.message, false);
    return;
  }
  state.connected = true;
  keepReading = true;
  els.connect.disabled = true;
  els.disconnect.disabled = false;
  setStatus("接続中", true);
  readLoop();
}

async function disconnect() {
  keepReading = false;
  try {
    if (reader) await reader.cancel();
  } catch (_) { /* noop */ }
}

// readLoop: TextDecoderStream で文字列化 → 行バッファリング → パース
async function readLoop() {
  const decoder = new TextDecoderStream();
  const readableClosed = port.readable.pipeTo(decoder.writable).catch(() => {});
  reader = decoder.readable.getReader();
  let buf = "";

  try {
    while (keepReading) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += value;
      let nl;
      // チャンク境界は行と無関係 → \n で分割し最後の不完全片を残す
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (line) handleLine(line);
      }
    }
  } catch (e) {
    setStatus("読み取りエラー: " + e.message, false);
  } finally {
    try { reader.releaseLock(); } catch (_) {}
    await readableClosed;
    try { await port.close(); } catch (_) {}
    cleanup();
  }
}

function cleanup() {
  state.connected = false;
  reader = null;
  port = null;
  els.connect.disabled = false;
  els.disconnect.disabled = true;
  setStatus("未接続", false);
}

// "x,y,sw" をパース。不正行はスキップしログに残す。
function handleLine(line) {
  const parts = line.split(",");
  if (parts.length !== 3) { pushLog("skip: " + line); return; }
  const x = Number(parts[0]);
  const y = Number(parts[1]);
  const sw = Number(parts[2]);
  if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(sw)) {
    pushLog("skip: " + line);
    return;
  }
  state.x = x;
  state.y = y;
  state.sw = sw;
  pushLog(line);
}

// ===== ログ(直近 N 行) =====
const LOG_MAX = 30;
const logLines = [];
function pushLog(line) {
  logLines.push(line);
  if (logLines.length > LOG_MAX) logLines.shift();
  els.log.textContent = logLines.join("\n");
  els.log.scrollTop = els.log.scrollHeight;
}

function setStatus(text, on) {
  els.status.textContent = text;
  els.status.classList.toggle("on", !!on);
}

// ===== ダッシュボード描画 =====
const plotCtx = els.plot.getContext("2d");
function drawPlot() {
  const w = els.plot.width, h = els.plot.height;
  plotCtx.clearRect(0, 0, w, h);
  // グリッド
  plotCtx.strokeStyle = "#222a35";
  plotCtx.lineWidth = 1;
  plotCtx.beginPath();
  plotCtx.moveTo(w / 2, 0); plotCtx.lineTo(w / 2, h);
  plotCtx.moveTo(0, h / 2); plotCtx.lineTo(w, h / 2);
  plotCtx.stroke();
  // 現在位置(0..1023 を canvas にマップ)
  const px = (state.x / 1023) * w;
  const py = (state.y / 1023) * h;
  plotCtx.fillStyle = state.sw === 0 ? "#45e07a" : "#4ea1ff";
  plotCtx.beginPath();
  plotCtx.arc(px, py, 8, 0, Math.PI * 2);
  plotCtx.fill();
}

function updateReadouts() {
  els.vx.textContent = state.x;
  els.vy.textContent = state.y;
  els.vsw.textContent = state.sw === 0 ? "押下" : "離す";
  els.led.classList.toggle("on", state.sw === 0);
}

// ===== ミニゲーム =====
const stageCtx = els.stage.getContext("2d");
const dot = { x: 240, y: 180, hue: 200 };
const DEAD = 40;     // 中央付近のデッドゾーン(生値)
const SPEED = 0.012; // 速度係数
let prevSw = 1;

function updateGame() {
  // 中央からの差分を速度に。デッドゾーン内は静止。
  let dx = state.x - center.x;
  let dy = state.y - center.y;
  if (Math.abs(dx) < DEAD) dx = 0;
  if (Math.abs(dy) < DEAD) dy = 0;
  dot.x += dx * SPEED;
  dot.y += dy * SPEED;
  // 画面端でクランプ
  const r = 12;
  dot.x = Math.max(r, Math.min(els.stage.width - r, dot.x));
  dot.y = Math.max(r, Math.min(els.stage.height - r, dot.y));
  // ボタン押下の立ち下がりで色変化
  if (state.sw === 0 && prevSw === 1) {
    dot.hue = (dot.hue + 60) % 360;
  }
  prevSw = state.sw;
}

function drawGame() {
  const w = els.stage.width, h = els.stage.height;
  stageCtx.clearRect(0, 0, w, h);
  stageCtx.fillStyle = `hsl(${dot.hue}, 80%, 60%)`;
  stageCtx.beginPath();
  stageCtx.arc(dot.x, dot.y, 12, 0, Math.PI * 2);
  stageCtx.fill();
}

// ===== メインループ =====
function loop() {
  drawPlot();
  updateReadouts();
  updateGame();
  drawGame();
  requestAnimationFrame(loop);
}

// ===== タブ切替 =====
document.querySelectorAll(".tab").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
    btn.classList.add("active");
    $(btn.dataset.tab).classList.add("active");
  });
});

// ===== キャリブレーション =====
els.calibrate.addEventListener("click", () => {
  center.x = state.x;
  center.y = state.y;
  els.voff.textContent = `${center.x}, ${center.y}`;
});

// ===== 初期化 =====
if (!supported()) {
  els.unsupported.classList.remove("hidden");
  els.connect.disabled = true;
}
els.connect.addEventListener("click", connect);
els.disconnect.addEventListener("click", disconnect);
requestAnimationFrame(loop);
