#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

#define TAG "RX"

/* ---------- Kconfig defaults ------------------------------------------- */
#ifndef CONFIG_ESP_RX_DEVICE_ID
#define CONFIG_ESP_RX_DEVICE_ID "rx-01"
#endif
#ifndef CONFIG_ESP_TX_DEVICE_ID
#define CONFIG_ESP_TX_DEVICE_ID "tx-01"
#endif
#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "your_wifi_ssid"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD "your_wifi_password"
#endif
#ifndef CONFIG_ESP_BRIDGE_HOST
#define CONFIG_ESP_BRIDGE_HOST "192.168.1.100"
#endif
#ifndef CONFIG_ESP_BRIDGE_PORT
#define CONFIG_ESP_BRIDGE_PORT 8765
#endif
#ifndef CONFIG_ESP_BRIDGE_PATH
#define CONFIG_ESP_BRIDGE_PATH "/ingest/rx"
#endif
#ifndef CONFIG_ESP_FEATURE_WINDOW
#define CONFIG_ESP_FEATURE_WINDOW 20
#endif
#ifndef CONFIG_ESP_TX_MAC_FILTER
#define CONFIG_ESP_TX_MAC_FILTER ""
#endif
#ifndef CONFIG_ESP_OUTPUT_MODE
#define CONFIG_ESP_OUTPUT_MODE "feature"
#endif

/* ---------- Ring buffer ------------------------------------------------- */
#define RING_SIZE       128
#define MAX_CSI_LEN     384   /* bytes — covers LLTF+HT-LTF for 40 MHz */

typedef struct {
    int64_t  ts_ms;
    int8_t   rssi;
    int8_t   noise_floor;
    uint32_t seq;            /* extracted from UDP payload if matched */
    uint8_t  mac[6];
    uint16_t len;
    int8_t   buf[MAX_CSI_LEN];
} csi_frame_t;

static csi_frame_t  s_ring[RING_SIZE];
static volatile int s_ring_head   = 0;
static volatile int s_ring_tail   = 0;
static volatile uint32_t s_rx_total  = 0;
static volatile uint32_t s_rx_lost   = 0;

static bool ring_push(const csi_frame_t *f)
{
    int next = (s_ring_head + 1) % RING_SIZE;
    if (next == s_ring_tail) {
        /* overwrite oldest */
        s_rx_lost++;
        s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
    }
    s_ring[s_ring_head] = *f;
    s_ring_head = next;
    s_rx_total++;
    return true;
}

static bool ring_pop(csi_frame_t *f)
{
    if (s_ring_tail == s_ring_head) return false;
    *f = s_ring[s_ring_tail];
    s_ring_tail = (s_ring_tail + 1) % RING_SIZE;
    return true;
}

static int ring_fill_pct(void)
{
    int used = (s_ring_head - s_ring_tail + RING_SIZE) % RING_SIZE;
    return (used * 100) / RING_SIZE;
}

/* ---------- TX MAC filter ----------------------------------------------- */
static bool  s_mac_filter_active = false;
static uint8_t s_tx_mac[6] = {0};

static bool parse_mac(const char *str, uint8_t *mac)
{
    if (!str || strlen(str) < 17) return false;
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

static bool mac_matches(const uint8_t *m)
{
    if (!s_mac_filter_active) return true;
    return memcmp(m, s_tx_mac, 6) == 0;
}

/* ---------- CSI callback ------------------------------------------------ */
static void wifi_csi_cb(void *ctx, wifi_csi_info_t *csi)
{
    if (!csi || csi->len == 0) return;
    if (!mac_matches(csi->mac)) return;

    csi_frame_t f = {
        .ts_ms       = esp_timer_get_time() / 1000,
        .rssi        = csi->rx_ctrl.rssi,
        .noise_floor = csi->rx_ctrl.noise_floor,
        .seq         = 0,
        .len         = (csi->len > MAX_CSI_LEN) ? MAX_CSI_LEN : csi->len,
    };
    memcpy(f.mac, csi->mac, 6);
    memcpy(f.buf, csi->buf, f.len);
    ring_push(&f);
}

/* ---------- Feature extraction ------------------------------------------ */
typedef struct {
    float amp_mean;
    float amp_std;
    float amp_p2p;
    float phase_var;
    float energy_delta;
} features_t;

static float s_baseline_energy = -1.0f;   /* set during calibration */

static void extract_features(const csi_frame_t *frames, int n, features_t *out)
{
    /* accumulate across all frames in the window */
    double sum_amp = 0, sum_amp2 = 0;
    float  amp_min = 1e9f, amp_max = 0.0f;
    double sum_phase = 0, sum_phase2 = 0;
    int    total_sub = 0;

    for (int fi = 0; fi < n; fi++) {
        const csi_frame_t *f  = &frames[fi];
        int n_sub = f->len / 2;
        for (int i = 0; i < n_sub; i++) {
            float re  = (float)f->buf[2 * i];
            float im  = (float)f->buf[2 * i + 1];
            float amp = sqrtf(re * re + im * im);
            float ph  = atan2f(im, re);

            sum_amp   += amp;
            sum_amp2  += amp * amp;
            if (amp < amp_min) amp_min = amp;
            if (amp > amp_max) amp_max = amp;

            sum_phase  += ph;
            sum_phase2 += ph * ph;
            total_sub++;
        }
    }

    if (total_sub == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    float mean = (float)(sum_amp / total_sub);
    float var  = (float)(sum_amp2 / total_sub) - mean * mean;

    out->amp_mean  = mean;
    out->amp_std   = sqrtf(var < 0 ? 0 : var);
    out->amp_p2p   = amp_max - amp_min;

    float ph_mean = (float)(sum_phase  / total_sub);
    float ph_var  = (float)(sum_phase2 / total_sub) - ph_mean * ph_mean;
    out->phase_var = ph_var < 0 ? 0 : ph_var;

    float energy = (float)(sum_amp2 / total_sub);
    if (s_baseline_energy < 0) {
        s_baseline_energy = energy;   /* auto-seed on first window */
    }
    out->energy_delta = energy - s_baseline_energy;
}

/* ---------- HTTP POST --------------------------------------------------- */
static char s_bridge_url[128];

static void http_post_json(const char *json, size_t len)
{
    esp_http_client_config_t cfg = {
        .url            = s_bridge_url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = 2000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, (int)len);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ---------- Feature output task ----------------------------------------- */
static uint32_t s_out_seq = 0;

static void feature_task(void *arg)
{
    csi_frame_t  window[CONFIG_ESP_FEATURE_WINDOW];
    int          w_idx = 0;
    char         json[512];

    while (1) {
        csi_frame_t f;
        if (!ring_pop(&f)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        window[w_idx % CONFIG_ESP_FEATURE_WINDOW] = f;
        w_idx++;

        if (w_idx < CONFIG_ESP_FEATURE_WINDOW) continue;

        features_t feat;
        extract_features(window, CONFIG_ESP_FEATURE_WINDOW, &feat);

        int64_t  ts      = f.ts_ms;
        int32_t  uptime  = (int32_t)(esp_timer_get_time() / 1000000);
        float    loss_pct = (s_rx_total > 0)
                              ? (100.0f * s_rx_lost / s_rx_total) : 0.0f;

        int n = snprintf(json, sizeof(json),
            "{\"device_id\":\"%s\",\"tx_id\":\"%s\",\"ts_ms\":%lld,"
            "\"seq\":%lu,\"rssi\":%d,"
            "\"features\":{"
              "\"amp_mean\":%.4f,\"amp_std\":%.4f,\"amp_p2p\":%.4f,"
              "\"phase_var\":%.4f,\"energy_delta\":%.4f"
            "},"
            "\"health\":{"
              "\"buffer_fill_pct\":%d,\"packet_loss_pct\":%.2f,"
              "\"uptime_s\":%d"
            "}}\n",
            CONFIG_ESP_RX_DEVICE_ID, CONFIG_ESP_TX_DEVICE_ID, ts,
            (unsigned long)s_out_seq++, f.rssi,
            feat.amp_mean, feat.amp_std, feat.amp_p2p,
            feat.phase_var, feat.energy_delta,
            ring_fill_pct(), loss_pct, uptime);

        /* always print to UART */
        fputs(json, stdout);

        /* post to bridge if connected */
        http_post_json(json, n);

        /* slide window by half */
        w_idx = CONFIG_ESP_FEATURE_WINDOW / 2;
        memmove(window, window + CONFIG_ESP_FEATURE_WINDOW / 2,
                sizeof(csi_frame_t) * w_idx);
    }
}

/* ---------- WiFi -------------------------------------------------------- */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi lost, reconnecting…");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

static void csi_init(void)
{
    wifi_csi_config_t csi_cfg = {
        .lltf_en          = true,
        .htltf_en         = true,
        .stbc_htltf2_en   = true,
        .ltf_merge_en     = true,
        .channel_filter_en = true,
        .manu_scale       = false,
        .shift            = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_LOGI(TAG, "CSI capture enabled");
}

/* ---------- app_main ---------------------------------------------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    snprintf(s_bridge_url, sizeof(s_bridge_url),
             "http://%s:%d%s",
             CONFIG_ESP_BRIDGE_HOST, CONFIG_ESP_BRIDGE_PORT,
             CONFIG_ESP_BRIDGE_PATH);
    ESP_LOGI(TAG, "Bridge URL: %s", s_bridge_url);

    if (parse_mac(CONFIG_ESP_TX_MAC_FILTER, s_tx_mac)) {
        s_mac_filter_active = true;
        ESP_LOGI(TAG, "Filtering CSI by TX MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 s_tx_mac[0], s_tx_mac[1], s_tx_mac[2],
                 s_tx_mac[3], s_tx_mac[4], s_tx_mac[5]);
    }

    wifi_init();
    csi_init();

    xTaskCreate(feature_task, "features", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Receiver started — device_id=%s mode=%s window=%d",
             CONFIG_ESP_RX_DEVICE_ID, CONFIG_ESP_OUTPUT_MODE,
             CONFIG_ESP_FEATURE_WINDOW);
}
