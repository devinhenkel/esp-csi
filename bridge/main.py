"""
ESP-CSI Bridge Service
======================
HTTP ingest endpoints:  POST /ingest/rx   POST /ingest/tx
Calibration endpoints:  POST /calibrate/start   POST /calibrate/commit
WebSocket broadcast:    ws://<host>:<port>/ws

Run:
    python bridge/main.py
"""
from __future__ import annotations
import asyncio
import json
import logging
import time
from collections import deque
from typing import Optional

from aiohttp import web
import config
from inference import InferenceEngine

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
)
log = logging.getLogger("bridge")

# ----------------------------------------------------------------------- #
#  Shared state                                                             #
# ----------------------------------------------------------------------- #
ws_clients:       set[web.WebSocketResponse] = set()
tx_state:         dict                       = {}
rx_state:         dict                       = {}
event_log:        deque                      = deque(maxlen=200)
engine:           InferenceEngine            = InferenceEngine()
calibrating:      bool                       = False


# ----------------------------------------------------------------------- #
#  WebSocket broadcast helpers                                              #
# ----------------------------------------------------------------------- #
async def broadcast(msg_type: str, payload: dict):
    message = json.dumps({"type": msg_type, "payload": payload})
    dead = set()
    for ws in ws_clients:
        try:
            await ws.send_str(message)
        except Exception:
            dead.add(ws)
    ws_clients.difference_update(dead)


def log_event(level: str, message: str, data: Optional[dict] = None):
    entry = {
        "ts_ms": int(time.time() * 1000),
        "level": level,
        "message": message,
    }
    if data:
        entry["data"] = data
    event_log.append(entry)
    asyncio.get_event_loop().create_task(
        broadcast("system.alert", entry)
    )


# ----------------------------------------------------------------------- #
#  HTTP handlers                                                            #
# ----------------------------------------------------------------------- #
async def handle_ingest_rx(request: web.Request) -> web.Response:
    try:
        raw = await request.text()
        # receiver may send newline-terminated JSON lines
        for line in raw.strip().splitlines():
            if not line.strip():
                continue
            payload = json.loads(line)

            dev_id = payload.get("device_id", "rx-?")
            rx_state[dev_id] = {
                "last_seen_ms": int(time.time() * 1000),
                "rssi":         payload.get("rssi"),
                "health":       payload.get("health", {}),
            }

            # broadcast raw telemetry to dashboard
            asyncio.get_event_loop().create_task(
                broadcast("telemetry.rx", payload)
            )

            if calibrating:
                engine.add_calibration_sample(
                    dev_id, payload.get("features", {})
                )
            else:
                result = engine.ingest(payload)
                if result:
                    result["system"] = _system_status()
                    asyncio.get_event_loop().create_task(
                        broadcast("inference.state", result)
                    )

        return web.Response(text='{"ok":true}', content_type="application/json")
    except Exception as e:
        log.warning("ingest/rx error: %s", e)
        return web.Response(status=400, text=json.dumps({"ok": False, "error": str(e)}),
                            content_type="application/json")


async def handle_ingest_tx(request: web.Request) -> web.Response:
    try:
        raw = await request.text()
        for line in raw.strip().splitlines():
            if not line.strip():
                continue
            payload = json.loads(line)
            dev_id  = payload.get("device_id", "tx-?")
            tx_state[dev_id] = {
                "last_seen_ms": int(time.time() * 1000),
                **payload,
            }
            asyncio.get_event_loop().create_task(
                broadcast("telemetry.tx", payload)
            )
        return web.Response(text='{"ok":true}', content_type="application/json")
    except Exception as e:
        log.warning("ingest/tx error: %s", e)
        return web.Response(status=400, text=json.dumps({"ok": False, "error": str(e)}),
                            content_type="application/json")


async def handle_calibrate_start(request: web.Request) -> web.Response:
    global calibrating
    calibrating = True
    log.info("Calibration started")
    log_event("info", "Calibration started — collecting empty-room baseline")
    return web.Response(text='{"ok":true,"status":"calibrating"}',
                        content_type="application/json")


async def handle_calibrate_commit(request: web.Request) -> web.Response:
    global calibrating
    calibrating = False
    results = engine.commit_all_calibration()
    log.info("Calibration committed: %s", results)
    log_event("info", "Calibration committed", results)
    return web.Response(
        text=json.dumps({"ok": True, "results": results}),
        content_type="application/json",
    )


async def handle_status(request: web.Request) -> web.Response:
    return web.Response(
        text=json.dumps(_system_status()),
        content_type="application/json",
    )


async def handle_events(request: web.Request) -> web.Response:
    return web.Response(
        text=json.dumps(list(event_log)),
        content_type="application/json",
    )


def _system_status() -> dict:
    now_ms  = int(time.time() * 1000)
    stale   = config.STALE_TIMEOUT * 1000

    active_tx = sum(
        1 for v in tx_state.values()
        if now_ms - v.get("last_seen_ms", 0) < stale
    )
    active_rx = sum(
        1 for v in rx_state.values()
        if now_ms - v.get("last_seen_ms", 0) < stale
    )
    return {
        "active_tx":   active_tx,
        "active_rx":   active_rx,
        "stream_ok":   active_rx > 0,
        "calibrating": calibrating,
    }


# ----------------------------------------------------------------------- #
#  WebSocket handler                                                        #
# ----------------------------------------------------------------------- #
async def handle_ws(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=15)
    await ws.prepare(request)
    ws_clients.add(ws)
    log.info("WS client connected (%d total)", len(ws_clients))

    # send current system status immediately
    await ws.send_str(json.dumps({
        "type":    "system.alert",
        "payload": {"level": "info", "message": "Connected to bridge", **_system_status()},
    }))

    async for msg in ws:
        if msg.type == web.WSMsgType.TEXT:
            try:
                cmd = json.loads(msg.data)
                if cmd.get("type") == "calibrate.start":
                    await handle_calibrate_start(request)
                elif cmd.get("type") == "calibrate.commit":
                    await handle_calibrate_commit(request)
            except Exception:
                pass
        elif msg.type in (web.WSMsgType.ERROR, web.WSMsgType.CLOSE):
            break

    ws_clients.discard(ws)
    log.info("WS client disconnected (%d remaining)", len(ws_clients))
    return ws


# ----------------------------------------------------------------------- #
#  Stale-device watchdog                                                    #
# ----------------------------------------------------------------------- #
async def stale_watchdog():
    while True:
        await asyncio.sleep(config.STALE_TIMEOUT)
        now_ms  = int(time.time() * 1000)
        stale   = config.STALE_TIMEOUT * 1000
        for dev_id, info in list(rx_state.items()):
            if now_ms - info.get("last_seen_ms", 0) > stale:
                log_event("warn", f"Receiver {dev_id} is stale (no data > {config.STALE_TIMEOUT}s)")
        for dev_id, info in list(tx_state.items()):
            if now_ms - info.get("last_seen_ms", 0) > stale:
                log_event("warn", f"Transmitter {dev_id} is offline")


# ----------------------------------------------------------------------- #
#  App setup                                                                #
# ----------------------------------------------------------------------- #
def create_app() -> web.Application:
    app = web.Application()
    app.router.add_post("/ingest/rx",          handle_ingest_rx)
    app.router.add_post("/ingest/tx",          handle_ingest_tx)
    app.router.add_post("/calibrate/start",    handle_calibrate_start)
    app.router.add_post("/calibrate/commit",   handle_calibrate_commit)
    app.router.add_get("/status",              handle_status)
    app.router.add_get("/events",              handle_events)
    app.router.add_get("/ws",                  handle_ws)
    # Serve web dashboard
    app.router.add_static("/", path="../web", name="static", show_index=True)
    return app


async def main():
    app = create_app()
    asyncio.get_event_loop().create_task(stale_watchdog())
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, config.HOST, config.PORT)
    await site.start()
    log.info("Bridge listening on http://%s:%d", config.HOST, config.PORT)
    log.info("WebSocket endpoint:  ws://%s:%d/ws",  config.HOST, config.PORT)
    log.info("Dashboard:           http://%s:%d/",  config.HOST, config.PORT)
    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
