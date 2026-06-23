/**
 * ESP-CSI Transmitter — PlatformIO version
 * Run from firmware/transmitter/arduino/:
 *   pio run -e esp32 -t upload
 *   pio device monitor
 *
 * To override SSID/password without editing this file, set build_flags in
 * platformio.ini:
 *   build_flags = -DWIFI_SSID=\"MyNet\" -DWIFI_PASSWORD=\"secret\"
 *
 * Serial commands (115200 baud):
 *   set_rate <hz>     packet rate (1–500)
 *   set_channel <n>   logical channel (1–13)
 *   start_tx / stop_tx
 *   status
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ── User config ───────────────────────────────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID      "your_wifi_ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD  "your_wifi_password"
#endif

#define DEVICE_ID       "tx-01"
#define UDP_BROADCAST   "255.255.255.255"
#define UDP_PORT        5555
#define DEFAULT_RATE_HZ 100
#define DEFAULT_CHANNEL   6

// ── State ─────────────────────────────────────────────────────────────────
static WiFiUDP  udp;
static int      txRateHz  = DEFAULT_RATE_HZ;
static int      txChannel = DEFAULT_CHANNEL;
static bool     txRunning = true;
static uint32_t seq       = 0;
static uint32_t lastTxMs  = 0;
static uint32_t lastHbMs  = 0;
static char     cmdBuf[64];
static int      cmdPos    = 0;

// ── Prototypes ────────────────────────────────────────────────────────────
static void printHeartbeat(uint32_t now);
static void handleCommand(const char *line);

// ─────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("{\"msg\":\"ESP-CSI Transmitter booting\"}");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n{\"msg\":\"WiFi connected\",\"ip\":\"%s\"}\n",
                WiFi.localIP().toString().c_str());
  udp.begin(UDP_PORT);
  Serial.printf("{\"device_id\":\"%s\",\"msg\":\"ready\","
                "\"rate_hz\":%d,\"channel\":%d}\n",
                DEVICE_ID, txRateHz, txChannel);
}

void loop() {
  uint32_t now = millis();

  if (txRunning && (now - lastTxMs) >= (uint32_t)(1000 / txRateHz)) {
    lastTxMs = now;
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
      "{\"device_id\":\"%s\",\"seq\":%lu,\"ts_ms\":%lu,"
      "\"channel\":%d,\"tx_rate_hz\":%d}\n",
      DEVICE_ID, (unsigned long)seq++, (unsigned long)now,
      txChannel, txRateHz);
    udp.beginPacket(UDP_BROADCAST, UDP_PORT);
    udp.write((const uint8_t *)buf, n);
    udp.endPacket();
  }

  if (now - lastHbMs >= 5000) {
    lastHbMs = now;
    printHeartbeat(now);
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      cmdBuf[cmdPos] = '\0';
      handleCommand(cmdBuf);
      cmdPos = 0;
    } else if (cmdPos < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdPos++] = c;
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("{\"warn\":\"WiFi lost, reconnecting\"}");
    WiFi.reconnect();
    delay(1000);
  }
}

// ─────────────────────────────────────────────────────────────────────────
static void printHeartbeat(uint32_t now) {
  Serial.printf(
    "{\"device_id\":\"%s\",\"ts_ms\":%lu,\"channel\":%d,"
    "\"tx_rate_hz\":%d,\"uptime_s\":%lu,\"status\":\"%s\"}\n",
    DEVICE_ID, (unsigned long)now, txChannel, txRateHz,
    (unsigned long)(now / 1000),
    txRunning ? "running" : "stopped");
}

static void handleCommand(const char *line) {
  if (strlen(line) == 0) return;

  if (strncmp(line, "set_rate ", 9) == 0) {
    int hz = atoi(line + 9);
    if (hz > 0 && hz <= 500) {
      txRateHz = hz;
      Serial.printf("{\"ok\":true,\"tx_rate_hz\":%d}\n", hz);
    } else {
      Serial.println("{\"ok\":false,\"error\":\"rate out of range 1-500\"}");
    }
  } else if (strncmp(line, "set_channel ", 12) == 0) {
    int ch = atoi(line + 12);
    if (ch >= 1 && ch <= 13) {
      txChannel = ch;
      Serial.printf("{\"ok\":true,\"channel\":%d}\n", ch);
    } else {
      Serial.println("{\"ok\":false,\"error\":\"channel out of range 1-13\"}");
    }
  } else if (strcmp(line, "start_tx") == 0) {
    txRunning = true;
    Serial.println("{\"ok\":true,\"status\":\"running\"}");
  } else if (strcmp(line, "stop_tx") == 0) {
    txRunning = false;
    Serial.println("{\"ok\":true,\"status\":\"stopped\"}");
  } else if (strcmp(line, "status") == 0) {
    lastHbMs = 0;
  } else {
    Serial.printf("{\"ok\":false,\"error\":\"unknown command: %s\"}\n", line);
  }
}
