/**
 * ESP-CSI Receiver — PlatformIO version
 * Run from firmware/receiver/arduino/:
 *   pio run -e esp32s3 -t upload
 *   pio device monitor
 *
 * To override SSID/password without editing this file, set build_flags in
 * platformio.ini:
 *   build_flags = -DWIFI_SSID=\"MyNet\" -DWIFI_PASSWORD=\"secret\"
 *
 * CSI capture uses esp_wifi.h (available in the Arduino ESP32 framework).
 * Must run on ESP32-S3 or ESP32-C6 — standard ESP32 has limited CSI support.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_wifi.h"
#include <math.h>

// ── User config ───────────────────────────────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID      "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD  "your_wifi_password"
#endif

#define DEVICE_ID      "rx-01"
#define TX_DEVICE_ID   "tx-01"
#define BRIDGE_HOST    "192.168.1.100"
#define BRIDGE_PORT    8765
#define BRIDGE_PATH    "/ingest/rx"
#define FEATURE_WINDOW 20
#define TX_MAC_FILTER  ""   // "AA:BB:CC:DD:EE:FF" or "" for all

// ── Ring buffer ───────────────────────────────────────────────────────────
#define RING_SIZE    64
#define MAX_CSI_LEN  384

struct CsiFrame {
  int64_t  tsMs;
  int8_t   rssi;
  uint16_t len;
  int8_t   buf[MAX_CSI_LEN];
};

struct Features {
  float ampMean;
  float ampStd;
  float ampP2p;
  float phaseVar;
  float energyDelta;
};

static CsiFrame       ring[RING_SIZE];
static volatile int   ringHead = 0;
static volatile int   ringTail = 0;
static volatile uint32_t rxTotal = 0;
static volatile uint32_t rxLost  = 0;
static portMUX_TYPE   ringMux = portMUX_INITIALIZER_UNLOCKED;

static bool    macFilterActive = false;
static uint8_t filterMac[6]   = {0};
static char    bridgeUrl[128];
static uint32_t outSeq        = 0;
static float   baselineEnergy = -1.0f;
static CsiFrame window[FEATURE_WINDOW];
static int     wIdx           = 0;
static uint32_t lastWifiMs    = 0;

// ── Prototypes ────────────────────────────────────────────────────────────
static bool parseMac(const char *str, uint8_t *mac);
static void IRAM_ATTR wifiCsiCb(void *ctx, wifi_csi_info_t *csi);
static void csiInit(void);
static void extractFeatures(CsiFrame *frames, int n, Features *out);
static void postFeatures(const Features &feat, int8_t rssi, int64_t tsMs);
static void processRingBuffer(void);

// ─────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("{\"msg\":\"ESP-CSI Receiver booting\"}");

  snprintf(bridgeUrl, sizeof(bridgeUrl),
           "http://%s:%d%s", BRIDGE_HOST, BRIDGE_PORT, BRIDGE_PATH);

  if (parseMac(TX_MAC_FILTER, filterMac)) {
    macFilterActive = true;
    Serial.printf("{\"msg\":\"TX MAC filter active\",\"mac\":\"%s\"}\n",
                  TX_MAC_FILTER);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("{\"msg\":\"Connecting\"}");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n{\"msg\":\"WiFi connected\",\"ip\":\"%s\"}\n",
                WiFi.localIP().toString().c_str());
  csiInit();
}

void loop() {
  processRingBuffer();

  uint32_t now = millis();
  if (now - lastWifiMs > 5000) {
    lastWifiMs = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("{\"warn\":\"WiFi lost, reconnecting\"}");
      WiFi.reconnect();
      delay(500);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
static void IRAM_ATTR wifiCsiCb(void *ctx, wifi_csi_info_t *csi) {
  if (!csi || csi->len == 0) return;
  if (macFilterActive && memcmp(csi->mac, filterMac, 6) != 0) return;

  portENTER_CRITICAL(&ringMux);
  int next = (ringHead + 1) % RING_SIZE;
  if (next == ringTail) {
    rxLost++;
    ringTail = (ringTail + 1) % RING_SIZE;
  }
  CsiFrame &f = ring[ringHead];
  f.tsMs = (int64_t)millis();
  f.rssi = csi->rx_ctrl.rssi;
  f.len  = (csi->len > MAX_CSI_LEN) ? MAX_CSI_LEN : (uint16_t)csi->len;
  memcpy(f.buf, csi->buf, f.len);
  ringHead = next;
  rxTotal++;
  portEXIT_CRITICAL(&ringMux);
}

static void csiInit(void) {
  wifi_csi_config_t cfg = {};
  cfg.lltf_en            = true;
  cfg.htltf_en           = true;
  cfg.stbc_htltf2_en     = true;
  cfg.ltf_merge_en       = true;
  cfg.channel_filter_en  = true;
  cfg.manu_scale         = false;
  cfg.shift              = false;
  esp_wifi_set_csi_config(&cfg);
  esp_wifi_set_csi_rx_cb(wifiCsiCb, nullptr);
  esp_wifi_set_csi(true);
  Serial.println("{\"msg\":\"CSI capture enabled\"}");
}

static void extractFeatures(CsiFrame *frames, int n, Features *out) {
  double sumAmp = 0, sumAmp2 = 0;
  float  ampMin = 1e9f, ampMax = 0.0f;
  double sumPh  = 0,    sumPh2 = 0;
  int    total  = 0;

  for (int fi = 0; fi < n; fi++) {
    int nSub = frames[fi].len / 2;
    for (int i = 0; i < nSub; i++) {
      float re  = (float)frames[fi].buf[2 * i];
      float im  = (float)frames[fi].buf[2 * i + 1];
      float amp = sqrtf(re * re + im * im);
      float ph  = atan2f(im, re);
      sumAmp  += amp;
      sumAmp2 += amp * amp;
      if (amp < ampMin) ampMin = amp;
      if (amp > ampMax) ampMax = amp;
      sumPh   += ph;
      sumPh2  += ph * ph;
      total++;
    }
  }
  if (total == 0) { memset(out, 0, sizeof(*out)); return; }

  float mean  = (float)(sumAmp  / total);
  float var   = (float)(sumAmp2 / total) - mean * mean;
  out->ampMean = mean;
  out->ampStd  = sqrtf(var < 0 ? 0 : var);
  out->ampP2p  = ampMax - ampMin;

  float phMean  = (float)(sumPh  / total);
  float phVar   = (float)(sumPh2 / total) - phMean * phMean;
  out->phaseVar = phVar < 0 ? 0 : phVar;

  float energy = (float)(sumAmp2 / total);
  if (baselineEnergy < 0) baselineEnergy = energy;
  out->energyDelta = energy - baselineEnergy;
}

static void postFeatures(const Features &feat, int8_t rssi, int64_t tsMs) {
  char json[512];

  portENTER_CRITICAL(&ringMux);
  int fill = ((ringHead - ringTail + RING_SIZE) % RING_SIZE) * 100 / RING_SIZE;
  portEXIT_CRITICAL(&ringMux);

  float loss = (rxTotal > 0) ? (100.0f * rxLost / rxTotal) : 0.0f;

  int n = snprintf(json, sizeof(json),
    "{\"device_id\":\"%s\",\"tx_id\":\"%s\",\"ts_ms\":%lld,"
    "\"seq\":%lu,\"rssi\":%d,"
    "\"features\":{"
      "\"amp_mean\":%.4f,\"amp_std\":%.4f,\"amp_p2p\":%.4f,"
      "\"phase_var\":%.4f,\"energy_delta\":%.4f"
    "},"
    "\"health\":{"
      "\"buffer_fill_pct\":%d,\"packet_loss_pct\":%.2f,"
      "\"uptime_s\":%lu"
    "}}\n",
    DEVICE_ID, TX_DEVICE_ID, (long long)tsMs,
    (unsigned long)outSeq++, (int)rssi,
    feat.ampMean, feat.ampStd, feat.ampP2p,
    feat.phaseVar, feat.energyDelta,
    fill, loss, (unsigned long)(millis() / 1000));

  Serial.write((const uint8_t *)json, n);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(bridgeUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(2000);
    http.POST(json);
    http.end();
  }
}

static void processRingBuffer(void) {
  while (true) {
    portENTER_CRITICAL(&ringMux);
    bool empty = (ringTail == ringHead);
    CsiFrame f;
    if (!empty) {
      f = ring[ringTail];
      ringTail = (ringTail + 1) % RING_SIZE;
    }
    portEXIT_CRITICAL(&ringMux);
    if (empty) break;

    window[wIdx % FEATURE_WINDOW] = f;
    wIdx++;

    if (wIdx >= FEATURE_WINDOW) {
      Features feat;
      extractFeatures(window, FEATURE_WINDOW, &feat);
      postFeatures(feat, f.rssi, f.tsMs);

      int half = FEATURE_WINDOW / 2;
      memmove(window, window + half, sizeof(CsiFrame) * half);
      wIdx = half;
    }
  }
}

static bool parseMac(const char *str, uint8_t *mac) {
  if (!str || strlen(str) < 17) return false;
  return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}
