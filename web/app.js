'use strict';

// ── Config ───────────────────────────────────────────────────────────────
const BRIDGE_WS_URL   = `ws://${location.hostname}:8765/ws`;
const MAX_POINTS      = 300;
const STALE_MS        = 5000;
const RECONNECT_BASE  = 1000;
const RECONNECT_MAX   = 30000;

// ── Toggle mock mode via the button or by setting MOCK_MODE=true here ───
let MOCK_MODE = false;

// ── Data buffers ─────────────────────────────────────────────────────────
const series = {
  ampStd:      [],   // {ts, v}
  energyDelta: [],
  phaseVar:    [],
};

let lastRxMs        = 0;
let lastTxMs        = 0;
let lastInferenceMs = 0;
let txDevices       = {};   // device_id → payload
let rxDevices       = {};   // device_id → payload
let currentOccupied = false;
let currentProb     = 0;
let currentZone     = null;
let currentConf     = 0;
let zones           = ['zone_1', 'zone_2', 'zone_3'];

// ── DOM refs ─────────────────────────────────────────────────────────────
const $connStatus   = document.getElementById('conn-status');
const $txStatus     = document.getElementById('tx-status');
const $txRate       = document.getElementById('tx-rate');
const $rxCount      = document.getElementById('rx-count');
const $rxHealth     = document.getElementById('rx-health');
const $pktLoss      = document.getElementById('pkt-loss');
const $pktRssi      = document.getElementById('pkt-rssi');
const $latency      = document.getElementById('latency');
const $occState     = document.getElementById('occ-state');
const $occProb      = document.getElementById('occ-prob');
const $zoneLabel    = document.getElementById('zone-label');
const $zoneConf     = document.getElementById('zone-conf');
const $eventLog     = document.getElementById('event-log');
const $btnMock      = document.getElementById('btn-mock');
const $btnCalStart  = document.getElementById('btn-calibrate-start');
const $btnCalCommit = document.getElementById('btn-calibrate-commit');
const $zoneNames    = document.getElementById('zone-names');
const gaugeCanvas   = document.getElementById('gauge');
const zoneCanvas    = document.getElementById('zone-map');
const tsCanvas      = document.getElementById('timeseries');

// ── WebSocket ─────────────────────────────────────────────────────────────
let ws = null;
let reconnectDelay = RECONNECT_BASE;
let reconnectTimer = null;

function connect() {
  if (MOCK_MODE) return;
  ws = new WebSocket(BRIDGE_WS_URL);

  ws.onopen = () => {
    setConnStatus('online', 'Connected');
    reconnectDelay = RECONNECT_BASE;
    logEvent('info', 'Connected to bridge');
  };

  ws.onmessage = (evt) => {
    try {
      const msg = JSON.parse(evt.data);
      handleMessage(msg);
    } catch (e) { /* ignore malformed */ }
  };

  ws.onclose = () => {
    setConnStatus('offline', 'Disconnected');
    scheduleReconnect();
  };

  ws.onerror = () => ws.close();
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, reconnectDelay);
  reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX);
  logEvent('warn', `Reconnecting in ${reconnectDelay / 1000}s…`);
}

function wsSend(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

// ── Message routing ───────────────────────────────────────────────────────
function handleMessage(msg) {
  const { type, payload } = msg;
  if (!type || !payload) return;

  if (type === 'telemetry.tx') onTxTelemetry(payload);
  else if (type === 'telemetry.rx') onRxTelemetry(payload);
  else if (type === 'inference.state') onInferenceState(payload);
  else if (type === 'system.alert') onSystemAlert(payload);
}

function onTxTelemetry(p) {
  lastTxMs = Date.now();
  const id = p.device_id || 'tx';
  txDevices[id] = p;
  $txStatus.textContent = p.status || 'running';
  $txRate.textContent   = `${p.tx_rate_hz || '?'} Hz  ch ${p.channel || '?'}`;
  document.getElementById('card-tx').className =
    'card ' + (p.status === 'running' ? 'ok' : 'warn');
}

function onRxTelemetry(p) {
  lastRxMs = Date.now();
  const id  = p.device_id || 'rx';
  rxDevices[id] = p;
  const feat = p.features || {};
  const health = p.health || {};

  pushSeries(series.ampStd,      p.ts_ms, feat.amp_std      ?? 0);
  pushSeries(series.energyDelta, p.ts_ms, feat.energy_delta ?? 0);
  pushSeries(series.phaseVar,    p.ts_ms, feat.phase_var    ?? 0);

  // latency: ts_ms in payload vs now
  if (p.ts_ms) {
    const lag = Date.now() - p.ts_ms;
    $latency.textContent = lag > 0 ? `${lag} ms` : '—';
  }

  updateRxCards(health);
  drawTimeSeries();
}

function onInferenceState(p) {
  lastInferenceMs = Date.now();
  const occ  = p.occupancy || {};
  const pos  = p.position  || {};
  const sys  = p.system    || {};

  currentOccupied = occ.occupied || false;
  currentProb     = occ.probability || 0;
  currentZone     = pos.zone || null;
  currentConf     = pos.confidence || 0;

  $occState.textContent = currentOccupied ? 'OCCUPIED' : 'EMPTY';
  $occState.className   = 'occ-state ' + (currentOccupied ? 'occupied' : 'empty');
  $occProb.textContent  = `${Math.round(currentProb * 100)}%`;

  $zoneLabel.textContent = currentZone || '—';
  $zoneConf.textContent  = `Confidence ${Math.round(currentConf * 100)}%`;

  // update system status cards
  if (sys.active_rx !== undefined) {
    $rxCount.textContent = `${sys.active_rx} active`;
  }

  drawGauge(currentProb);
  drawZoneMap();
}

function onSystemAlert(p) {
  logEvent(p.level || 'info', p.message || JSON.stringify(p));
  if (p.stream_ok === false) {
    setConnStatus('stale', 'Stream degraded');
  }
}

// ── UI helpers ────────────────────────────────────────────────────────────
function setConnStatus(cls, text) {
  $connStatus.className   = `pill ${cls}`;
  $connStatus.textContent = text;
}

function updateRxCards(health) {
  const n    = Object.keys(rxDevices).length;
  $rxCount.textContent  = `${n} device${n !== 1 ? 's' : ''}`;

  const fill  = health.buffer_fill_pct ?? 0;
  const loss  = health.packet_loss_pct ?? 0;
  const rssi  = rxDevices[Object.keys(rxDevices)[0]]?.rssi ?? null;

  $rxHealth.textContent = `buf ${fill}%`;
  $pktLoss.textContent  = `${loss.toFixed(1)}%`;
  $pktRssi.textContent  = rssi !== null ? `RSSI ${rssi} dBm` : 'RSSI —';

  document.getElementById('card-rx').className   = 'card ' + (n > 0 ? 'ok' : 'error');
  document.getElementById('card-loss').className = 'card ' + (loss < 5 ? 'ok' : loss < 15 ? 'warn' : 'error');
}

function pushSeries(arr, ts, v) {
  arr.push({ ts, v });
  if (arr.length > MAX_POINTS) arr.shift();
}

function logEvent(level, message) {
  const ts  = new Date().toLocaleTimeString();
  const el  = document.createElement('div');
  el.className = `log-${level}`;
  el.textContent = `[${ts}] ${message}`;
  $eventLog.appendChild(el);
  while ($eventLog.children.length > 100) $eventLog.removeChild($eventLog.firstChild);
  $eventLog.scrollTop = $eventLog.scrollHeight;
}

// ── Stale data watchdog ───────────────────────────────────────────────────
setInterval(() => {
  if (MOCK_MODE) return;
  if (lastRxMs && Date.now() - lastRxMs > STALE_MS) {
    setConnStatus('stale', 'Data stale');
    logEvent('warn', 'No receiver data for >5s');
    lastRxMs = 0; // suppress repeat
  }
}, 1000);

// ── Canvas: gauge ─────────────────────────────────────────────────────────
function drawGauge(prob) {
  const ctx = gaugeCanvas.getContext('2d');
  const W = gaugeCanvas.width, H = gaugeCanvas.height;
  ctx.clearRect(0, 0, W, H);

  const cx = W / 2, cy = H - 12, r = H - 22;
  const startA = Math.PI, endA = 2 * Math.PI;

  // background arc
  ctx.beginPath();
  ctx.arc(cx, cy, r, startA, endA);
  ctx.strokeStyle = '#2a2d3a';
  ctx.lineWidth = 12;
  ctx.stroke();

  // value arc
  const angle = startA + prob * Math.PI;
  const color = prob < 0.4 ? '#4ade80' : prob < 0.7 ? '#fbbf24' : '#f87171';
  ctx.beginPath();
  ctx.arc(cx, cy, r, startA, angle);
  ctx.strokeStyle = color;
  ctx.lineWidth = 12;
  ctx.lineCap = 'round';
  ctx.stroke();
}

// ── Canvas: zone map ──────────────────────────────────────────────────────
function drawZoneMap() {
  const ctx = zoneCanvas.getContext('2d');
  const W = zoneCanvas.width, H = zoneCanvas.height;
  ctx.clearRect(0, 0, W, H);

  const cols  = Math.min(zones.length, 3);
  const rows  = Math.ceil(zones.length / cols);
  const pad   = 10;
  const cellW = (W - pad * (cols + 1)) / cols;
  const cellH = (H - pad * (rows + 1)) / rows;

  zones.forEach((z, i) => {
    const col = i % cols;
    const row = Math.floor(i / cols);
    const x   = pad + col * (cellW + pad);
    const y   = pad + row * (cellH + pad);
    const active = z === currentZone;

    ctx.fillStyle   = active ? 'rgba(56,189,248,.25)' : 'rgba(26,29,39,.9)';
    ctx.strokeStyle = active ? '#38bdf8' : '#2a2d3a';
    ctx.lineWidth   = active ? 2 : 1;
    roundRect(ctx, x, y, cellW, cellH, 6);
    ctx.fill();
    ctx.stroke();

    ctx.fillStyle  = active ? '#e2e8f0' : '#64748b';
    ctx.font       = `${active ? 'bold ' : ''}12px Inter, system-ui, sans-serif`;
    ctx.textAlign  = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(z, x + cellW / 2, y + cellH / 2);

    if (active && currentConf) {
      ctx.fillStyle  = '#38bdf8';
      ctx.font       = '10px Inter, system-ui, sans-serif';
      ctx.fillText(`${Math.round(currentConf * 100)}%`, x + cellW / 2, y + cellH / 2 + 16);
    }
  });
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + r);
  ctx.lineTo(x + w, y + h - r);
  ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
  ctx.lineTo(x + r, y + h);
  ctx.quadraticCurveTo(x, y + h, x, y + h - r);
  ctx.lineTo(x, y + r);
  ctx.quadraticCurveTo(x, y, x + r, y);
  ctx.closePath();
}

// ── Canvas: time-series ───────────────────────────────────────────────────
function drawTimeSeries() {
  const ctx = tsCanvas.getContext('2d');
  const W = tsCanvas.width, H = tsCanvas.height;
  ctx.clearRect(0, 0, W, H);

  if (series.ampStd.length < 2) return;

  const allVals = [
    ...series.ampStd.map(p => p.v),
    ...series.energyDelta.map(p => p.v),
    ...series.phaseVar.map(p => p.v),
  ];
  const vMin = Math.min(0, ...allVals);
  const vMax = Math.max(1, ...allVals);
  const pad  = { t: 10, b: 20, l: 40, r: 10 };
  const plotW = W - pad.l - pad.r;
  const plotH = H - pad.t - pad.b;

  const toX = (i, len) => pad.l + (i / (len - 1)) * plotW;
  const toY = (v)       => pad.t + plotH - ((v - vMin) / (vMax - vMin || 1)) * plotH;

  // grid line at y=0
  const y0 = toY(0);
  ctx.strokeStyle = '#2a2d3a';
  ctx.lineWidth   = 1;
  ctx.setLineDash([4, 4]);
  ctx.beginPath(); ctx.moveTo(pad.l, y0); ctx.lineTo(W - pad.r, y0); ctx.stroke();
  ctx.setLineDash([]);

  // y-axis labels
  ctx.fillStyle = '#64748b';
  ctx.font      = '9px monospace';
  ctx.textAlign = 'right';
  [vMin, 0, vMax].forEach(v => {
    ctx.fillText(v.toFixed(1), pad.l - 4, toY(v) + 3);
  });

  function drawLine(data, color) {
    if (data.length < 2) return;
    ctx.strokeStyle = color;
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    data.forEach((pt, i) => {
      const x = toX(i, data.length);
      const y = toY(pt.v);
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  drawLine(series.ampStd,      getComputedStyle(document.documentElement).getPropertyValue('--c-std').trim()    || '#38bdf8');
  drawLine(series.energyDelta, getComputedStyle(document.documentElement).getPropertyValue('--c-energy').trim() || '#4ade80');
  drawLine(series.phaseVar,    getComputedStyle(document.documentElement).getPropertyValue('--c-phase').trim()  || '#a78bfa');
}

// ── Zone config ───────────────────────────────────────────────────────────
$zoneNames.addEventListener('change', () => {
  zones = $zoneNames.value.split(',').map(z => z.trim()).filter(Boolean);
  drawZoneMap();
});

// ── Calibration controls ──────────────────────────────────────────────────
$btnCalStart.addEventListener('click', () => {
  if (!MOCK_MODE) {
    fetch('/calibrate/start', { method: 'POST' })
      .then(() => {
        $btnCalStart.classList.add('active');
        $btnCalCommit.disabled = false;
        logEvent('info', 'Calibration started — keep room empty');
      })
      .catch(() => logEvent('error', 'Failed to start calibration'));
  } else {
    $btnCalStart.classList.add('active');
    $btnCalCommit.disabled = false;
    logEvent('info', '[mock] Calibration started');
  }
});

$btnCalCommit.addEventListener('click', () => {
  if (!MOCK_MODE) {
    fetch('/calibrate/commit', { method: 'POST' })
      .then(r => r.json())
      .then(d => {
        $btnCalStart.classList.remove('active');
        $btnCalCommit.disabled = true;
        logEvent('info', 'Baseline committed: ' + JSON.stringify(d));
      })
      .catch(() => logEvent('error', 'Failed to commit calibration'));
  } else {
    $btnCalStart.classList.remove('active');
    $btnCalCommit.disabled = true;
    logEvent('info', '[mock] Baseline committed');
  }
});

// ── Mock data mode ────────────────────────────────────────────────────────
let mockTimer = null;
let mockSeq   = 0;

function startMock() {
  MOCK_MODE = true;
  $btnMock.textContent = 'Mock Data: ON';
  $btnMock.classList.add('active');
  setConnStatus('online', 'Mock Mode');
  logEvent('info', 'Mock data mode enabled');

  mockTimer = setInterval(() => {
    const t   = Date.now();
    const osc = Math.sin(t / 3000);

    const feat = {
      amp_mean:     8 + 4 * Math.abs(osc),
      amp_std:      1.5 + 2.5 * Math.abs(osc) + Math.random() * 0.3,
      amp_p2p:      3 + 3 * Math.abs(osc),
      phase_var:    0.05 + 0.1 * Math.abs(osc),
      energy_delta: 3 * osc + Math.random() * 0.5,
    };

    handleMessage({ type: 'telemetry.rx', payload: {
      device_id: 'rx-01', tx_id: 'tx-01', ts_ms: t, seq: mockSeq++,
      rssi: -55 + Math.round(5 * osc),
      features: feat,
      health: { buffer_fill_pct: 30, packet_loss_pct: 0.5, uptime_s: Math.round(t / 1000) },
    }});

    const occupied    = Math.abs(osc) > 0.5;
    const zoneIdx     = Math.floor(Math.abs(osc) * zones.length) % zones.length;
    handleMessage({ type: 'inference.state', payload: {
      ts_ms: t,
      occupancy: { occupied, probability: Math.abs(osc) },
      position:  { zone: zones[zoneIdx], confidence: 0.5 + 0.4 * Math.abs(osc) },
      system:    { active_tx: 1, active_rx: 1, stream_ok: true },
    }});

    handleMessage({ type: 'telemetry.tx', payload: {
      device_id: 'tx-01', ts_ms: t, channel: 6, tx_rate_hz: 100,
      uptime_s: Math.round(t / 1000), status: 'running',
    }});
  }, 200);
}

function stopMock() {
  MOCK_MODE = false;
  $btnMock.textContent = 'Mock Data: OFF';
  $btnMock.classList.remove('active');
  if (mockTimer) { clearInterval(mockTimer); mockTimer = null; }
  setConnStatus('offline', 'Disconnected');
  connect();
}

$btnMock.addEventListener('click', () => {
  if (MOCK_MODE) stopMock(); else startMock();
});

// ── Init ──────────────────────────────────────────────────────────────────
drawGauge(0);
drawZoneMap();
connect();
