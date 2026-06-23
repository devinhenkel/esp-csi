# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

## Project Overview

ESP-CSI Environmental Presence and Positioning System — a Wi-Fi CSI pipeline for indoor occupancy detection and zone-level positioning. The system has four components:

1. **`firmware/transmitter/`** — ESP32 firmware that emits a stable Wi-Fi packet stream on a fixed channel
2. **`firmware/receiver/`** — ESP32-S3/C6 firmware that captures CSI frames and computes features
3. **`bridge/`** — Backend service (Python or Node.js) that ingests device data, runs inference, and broadcasts over WebSocket
4. **`web/`** — Plain HTML/CSS/JS single-page dashboard (`index.html`, `styles.css`, `app.js`)

A shared JSON schema defines the wire format between all four components (see Sections 5.2.1, 5.3.2, 6.3, and 7.5 in the PRD below).

---

## Development Commands

### Firmware (ESP-IDF)

Both firmware projects use ESP-IDF. Set up the toolchain once:

```bash
. $IDF_PATH/export.sh          # activate ESP-IDF environment
```

Build and flash (run from `firmware/transmitter/` or `firmware/receiver/`):

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash
idf.py -p /dev/ttyUSB0 monitor   # serial console / log output
idf.py -p /dev/ttyUSB0 flash monitor  # flash then open monitor
```

Configure target chip (must match hardware):

```bash
idf.py set-target esp32      # transmitter
idf.py set-target esp32s3    # receiver (preferred)
idf.py menuconfig            # adjust sdkconfig options
```

Run ESP-IDF component unit tests (when tests are added under `test/`):

```bash
idf.py -C test build flash monitor
```

### Backend Bridge

Commands will depend on chosen language. Likely:

```bash
# Python
pip install -r requirements.txt
python bridge/main.py

# Node.js
npm install
node bridge/index.js
```

### Web Dashboard

No build step — open `web/index.html` directly in a browser. Enable mock-data mode in `app.js` to develop without hardware:

```js
const MOCK_MODE = true;   // toggle at top of app.js
```

---

## Architecture Notes

### Data Flow

```
Tx firmware  →  RF  →  Rx firmware  →  UART/WebSocket  →  Bridge  →  WebSocket  →  Web dashboard
```

- Tx emits 100 packets/sec on a fixed channel (default: ch 6, 20 MHz).
- Rx filters by Tx MAC, maintains a ring buffer, and outputs JSON lines in either `raw` or `feature` mode.
- Bridge normalizes timestamps, runs the inference window (0.5–2.0 s), and fans out `inference.state` / `telemetry.*` / `system.alert` WebSocket messages.
- Dashboard keeps rolling 300-point buffers, reconnects with exponential backoff, and marks status degraded after 5 s of silence.

### Key Contracts

- **Tx telemetry** — `{"device_id", "ts_ms", "channel", "tx_rate_hz", "uptime_s", "status"}`
- **Rx feature payload** — `{"device_id", "tx_id", "ts_ms", "seq", "rssi", "features": {amp_mean, amp_std, amp_p2p, phase_var, energy_delta}, "health": {buffer_fill_pct, packet_loss_pct, uptime_s}}`
- **Inference output** — `{"ts_ms", "occupancy": {occupied, probability}, "position": {zone, confidence}, "system": {active_tx, active_rx, stream_ok}}`
- **WebSocket message envelope** — `{"type": "<telemetry.tx|telemetry.rx|inference.state|system.alert>", "payload": {...}}`

### Firmware Module Boundaries

- Tx: packet generator task, serial command parser (`set_rate`, `set_channel`, `start_tx`, `stop_tx`), heartbeat timer.
- Rx: CSI callback → ring buffer → feature extractor → transport (UART and/or MQTT/WebSocket client), watchdog + auto-reconnect.

### Inference (Bridge)

- Rule-based threshold model for MVP; pluggable to logistic regression / random forest.
- Calibration saves an empty-room baseline; inference normalizes live features against it before classification.
- Emit `P(occupied)` and zone label with confidence for every inference window.

---

# Product Requirements Document (PRD)

## Product Name
ESP-CSI Environmental Presence and Positioning System

## Version
v1.0

## Document Purpose
Define requirements and implementation instructions for an ESP-CSI system that includes:
1. Transmitter firmware
2. Receiver firmware
3. A browser-based monitoring application (HTML/CSS/JS)

The system goal is to monitor indoor environmental dynamics and estimate where people are located within the monitored space.

---

## 1. Problem Statement
Traditional camera-based occupancy systems raise privacy concerns and often require high bandwidth and controlled lighting. Wi-Fi Channel State Information (CSI) can detect human movement and location changes with low cost hardware and improved privacy characteristics.

This project must deliver an end-to-end ESP-based CSI pipeline:
1. A stable Wi-Fi signal source (transmitter)
2. One or more CSI collection nodes (receivers)
3. A backend-capable monitoring UI to visualize live signal quality, occupancy probability, and estimated person position in-zone

---

## 2. Goals and Non-Goals

## 2.1 Goals
1. Collect CSI data streams in real time from ESP devices.
2. Detect environmental disturbances caused by people.
3. Estimate person location by mapping signal changes to defined zones.
4. Provide a live web dashboard to monitor:
	- Device health
	- CSI feature trends
	- Occupancy status
	- Zone-level person placement
5. Support future expansion to multi-person classification.

## 2.2 Non-Goals (v1)
1. Identity recognition of specific people.
2. Millimeter-level positioning.
3. Outdoor deployment.
4. Native mobile apps.

---

## 3. Users and Use Cases

## 3.1 Primary Users
1. Embedded developer implementing ESP firmware.
2. ML/signal engineer building feature extraction and inference.
3. Operator monitoring occupancy via browser dashboard.

## 3.2 Core Use Cases
1. Start system and verify transmitter + receiver health.
2. View live CSI and derived features.
3. Calibrate baseline (empty room reference).
4. Detect when a person enters/leaves monitored area.
5. Show person placement in predefined zones (for example, Zone A/B/C).

---

## 4. High-Level System Architecture

1. ESP Transmitter continuously emits Wi-Fi traffic with stable cadence.
2. ESP Receiver(s) capture CSI frames and compute first-pass features.
3. Receiver sends packetized data to local edge service or directly to web backend via WebSocket/MQTT bridge.
4. Inference module classifies occupancy and zone.
5. Web app displays live telemetry and historical trends.

### 4.1 Reference Data Flow
1. Tx beacon/frame -> RF channel
2. Rx CSI callback -> raw CSI + metadata
3. Feature extraction -> amplitude stats, phase stability, variance, Doppler-like proxy
4. Inference -> occupancy probability + zone label
5. Publish -> JSON stream to dashboard

---

## 5. Hardware and Firmware Requirements

## 5.1 Recommended Hardware
1. ESP32-S3 or ESP32-C6 (receiver preferred with supported CSI APIs).
2. ESP32 (transmitter) configured as AP/station traffic source.
3. Stable 5V power supplies.
4. Optional: multiple receivers for triangulation improvements.

## 5.2 Transmitter Firmware Requirements
1. Configure fixed channel (for example, channel 6).
2. Set fixed bandwidth (20 MHz initially).
3. Emit predictable packet stream at configurable rate (for example, 100 packets/sec).
4. Include sequence number and timestamp in payload.
5. Expose runtime configuration over serial command interface:
	- `set_rate <hz>`
	- `set_channel <n>`
	- `start_tx`
	- `stop_tx`
6. Provide heartbeat status every 5 seconds.

### 5.2.1 Transmitter Telemetry Schema
```json
{
  "device_id": "tx-01",
  "ts_ms": 1710000000000,
  "channel": 6,
  "tx_rate_hz": 100,
  "uptime_s": 452,
  "status": "running"
}
```

## 5.3 Receiver Firmware Requirements
1. Enable CSI capture mode using supported ESP-IDF APIs.
2. Filter packets to target transmitter MAC/BSSID.
3. Capture for each frame:
	- Timestamp
	- RSSI
	- Noise floor (if available)
	- CSI complex values/subcarriers
	- Tx sequence number
4. Implement ring buffer to avoid data loss under burst loads.
5. Support two modes:
	- `raw`: emit full CSI
	- `feature`: emit downsampled derived features
6. Provide transport output via UART and/or Wi-Fi (WebSocket or MQTT client).
7. Include watchdog and automatic recovery for Wi-Fi disconnects.

### 5.3.1 Receiver Feature Set (v1)
1. Mean amplitude per frame/window.
2. Standard deviation per window.
3. Peak-to-peak amplitude.
4. Phase variance proxy (if stable enough on chosen chipset).
5. Short-time energy delta against baseline.

### 5.3.2 Receiver Output Schema (Feature Mode)
```json
{
  "device_id": "rx-01",
  "tx_id": "tx-01",
  "ts_ms": 1710000000123,
  "seq": 32144,
  "rssi": -51,
  "features": {
	 "amp_mean": 0.61,
	 "amp_std": 0.14,
	 "amp_p2p": 0.49,
	 "phase_var": 0.08,
	 "energy_delta": 0.21
  },
  "health": {
	 "buffer_fill_pct": 37,
	 "packet_loss_pct": 0.8,
	 "uptime_s": 901
  }
}
```

---

## 6. Positioning and Inference Requirements

## 6.1 Environment Model
1. The room is divided into named zones (for example, `zone_1`, `zone_2`, `zone_3`).
2. Zone map dimensions are configurable in meters.
3. At least one calibration run is required for:
	- Empty room baseline
	- Single-person samples per zone

## 6.2 Inference Pipeline (v1)
1. Window incoming features over 0.5-2.0 seconds.
2. Normalize by baseline statistics.
3. Run lightweight classifier:
	- Rule-based threshold model for MVP, or
	- Small ML model (logistic regression / random forest)
4. Emit:
	- Occupancy probability: $P(occupied)$
	- Most likely zone label
	- Confidence score [0,1]

## 6.3 Output Contract for Dashboard
```json
{
  "ts_ms": 1710000000456,
  "occupancy": {
	 "occupied": true,
	 "probability": 0.91
  },
  "position": {
	 "zone": "zone_2",
	 "confidence": 0.78
  },
  "system": {
	 "active_tx": 1,
	 "active_rx": 2,
	 "stream_ok": true
  }
}
```

---

## 7. Web Application Requirements (HTML/CSS/JS)

## 7.1 Technical Constraints
1. Plain HTML, CSS, and JavaScript (no framework required for v1).
2. Real-time updates via WebSocket.
3. Responsive layout for desktop and tablet.
4. Works in modern Chromium and Firefox browsers.

## 7.2 Required Pages/Views
1. Single-page dashboard is acceptable for v1.
2. Sections required:
	- System status bar
	- Live CSI feature charts
	- Occupancy indicator
	- Zone map panel
	- Event log panel

## 7.3 UI Components
1. Status cards:
	- Transmitter online/offline
	- Receiver count and health
	- Packet loss and latency
2. Occupancy widget:
	- Large binary state (Occupied/Empty)
	- Probability gauge
3. Position map:
	- 2D room grid
	- Highlighted active zone
	- Confidence label
4. Time-series chart:
	- `amp_std`
	- `energy_delta`
	- optional `phase_var`
5. Controls:
	- Start/Stop monitoring
	- Baseline recalibration
	- Zone naming/config panel

## 7.4 Front-End Data Handling
1. WebSocket client reconnect with exponential backoff.
2. Keep rolling buffers (for example, last 300 points).
3. Handle stale data (>5 sec) by marking status degraded.
4. Client-side validation for missing fields.

## 7.5 WebSocket Message Types
1. `telemetry.tx`
2. `telemetry.rx`
3. `inference.state`
4. `system.alert`

Example:
```json
{
  "type": "inference.state",
  "payload": {
	 "ts_ms": 1710000000456,
	 "occupancy": { "occupied": true, "probability": 0.91 },
	 "position": { "zone": "zone_2", "confidence": 0.78 }
  }
}
```

---

## 8. Backend Bridge Requirements (Minimum)
Although the UI is HTML/CSS/JS, a lightweight bridge service is required to aggregate device streams.

1. Accept receiver/transmitter data over MQTT or HTTP ingest.
2. Perform timestamp normalization.
3. Run or call inference function.
4. Broadcast unified events to web clients over WebSocket.
5. Persist optional rolling history (for example, last 24h).

---

## 9. Functional Requirements

1. System shall ingest CSI-derived data at >= 50 feature frames/sec per receiver.
2. System shall detect occupancy transitions within <= 2 seconds.
3. System shall place a single person into the correct zone with target >= 80% accuracy after calibration.
4. Dashboard shall update visible status within <= 500 ms of backend message reception.
5. System shall expose health alerts for device offline, packet loss, and stale inference.

---

## 10. Non-Functional Requirements

1. Reliability: recover from Wi-Fi interruptions without manual reboot.
2. Security:
	- Local network only for v1, with optional auth token on ingestion.
	- No camera/audio collection.
3. Performance:
	- End-to-end latency target <= 1.5 seconds for occupancy state.
4. Maintainability:
	- Clear module boundaries: firmware, bridge, inference, frontend.
5. Observability:
	- Structured logs and basic metrics (message rate, loss, model confidence).

---

## 11. Calibration and Testing Plan

## 11.1 Calibration Steps
1. Place devices in fixed final positions.
2. Capture 2-5 minutes empty-room baseline.
3. Capture labeled movement samples in each zone.
4. Fit/update threshold or ML parameters.
5. Save calibration profile with timestamp and room ID.

## 11.2 Test Cases
1. Device connectivity test:
	- Transmitter online heartbeat visible
	- Receiver stream active
2. Occupancy test:
	- Empty room stays below occupancy threshold
	- Entry event raises occupancy probability above threshold
3. Zone placement test:
	- Single person standing in each zone yields expected label
4. Resilience test:
	- Receiver reboot auto-rejoins stream
	- Web dashboard reconnects after network interruption

---

## 12. Milestones

1. Milestone A: Firmware MVP
	- Tx frame generator stable
	- Rx CSI capture and feature output
2. Milestone B: Inference MVP
	- Baseline normalization
	- Occupancy + zone classifier
3. Milestone C: Web Dashboard MVP
	- Live telemetry, occupancy, and zone map
4. Milestone D: Field Validation
	- Accuracy and latency benchmarking in target environment

---

## 13. Risks and Mitigations

1. CSI instability across chipsets
	- Mitigation: pin supported hardware and firmware versions.
2. RF interference in crowded Wi-Fi environments
	- Mitigation: channel scanning and configurable channel selection.
3. False positives from non-human motion (fans, doors)
	- Mitigation: calibration with nuisance events and temporal smoothing.
4. Zone ambiguity with single receiver
	- Mitigation: support multiple receivers and sensor fusion in next iteration.

---

## 14. Definition of Done

1. Transmitter and receiver firmware compile and run on target ESP hardware.
2. Receiver emits valid feature payloads continuously for >= 30 minutes.
3. Inference outputs occupancy and zone messages in real time.
4. Web app displays all required panels and handles reconnects.
5. End-to-end demo shows ability to monitor room and place a person in the correct zone with agreed baseline accuracy.

---

## 15. Implementation Notes for Coding Agent

1. Create transmitter and receiver as separate firmware projects/folders.
2. Define shared schema file for telemetry and inference payloads.
3. Keep serial logs machine-parseable (JSON lines where possible).
4. For frontend, create at minimum:
	- `web/index.html`
	- `web/styles.css`
	- `web/app.js`
5. Use charting library only if needed; otherwise implement canvas/SVG charts directly.
6. Include a mock-data mode in `app.js` so UI can be developed before hardware is online.
7. Document all runtime config variables in project README.

This PRD is the authoritative implementation guide for v1.
