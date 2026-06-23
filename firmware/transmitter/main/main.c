#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/uart.h"

#define TAG "TX"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define UART_NUM        UART_NUM_0
#define UART_BUF_SIZE   256
#define CMD_BUF_SIZE    64

#ifndef CONFIG_ESP_TX_DEVICE_ID
#define CONFIG_ESP_TX_DEVICE_ID "tx-01"
#endif
#ifndef CONFIG_ESP_WIFI_SSID
#define CONFIG_ESP_WIFI_SSID "your_wifi_ssid"
#endif
#ifndef CONFIG_ESP_WIFI_PASSWORD
#define CONFIG_ESP_WIFI_PASSWORD "your_wifi_password"
#endif
#ifndef CONFIG_ESP_TX_CHANNEL
#define CONFIG_ESP_TX_CHANNEL 6
#endif
#ifndef CONFIG_ESP_TX_RATE_HZ
#define CONFIG_ESP_TX_RATE_HZ 100
#endif
#ifndef CONFIG_ESP_TX_UDP_PORT
#define CONFIG_ESP_TX_UDP_PORT 5555
#endif

static EventGroupHandle_t s_wifi_event_group;
static volatile int  s_tx_rate_hz   = CONFIG_ESP_TX_RATE_HZ;
static volatile int  s_tx_channel   = CONFIG_ESP_TX_CHANNEL;
static volatile bool s_tx_running   = true;
static volatile uint32_t s_seq      = 0;

static int64_t get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t s_boot_time_ms = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting…");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

    ESP_LOGI(TAG, "Waiting for WiFi…");
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

static void tx_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        vTaskDelete(NULL);
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in dest = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_ESP_TX_UDP_PORT),
        .sin_addr.s_addr = inet_addr("255.255.255.255"),
    };

    char buf[256];
    while (1) {
        if (s_tx_running) {
            int64_t ts = s_boot_time_ms + get_time_ms();
            int len = snprintf(buf, sizeof(buf),
                "{\"device_id\":\"%s\",\"seq\":%lu,\"ts_ms\":%lld,"
                "\"channel\":%d,\"tx_rate_hz\":%d}\n",
                CONFIG_ESP_TX_DEVICE_ID, (unsigned long)s_seq++,
                ts, s_tx_channel, s_tx_rate_hz);
            sendto(sock, buf, len, 0, (struct sockaddr *)&dest, sizeof(dest));
        }
        vTaskDelay(pdMS_TO_TICKS(1000 / s_tx_rate_hz));
    }
}

static void heartbeat_cb(void *arg)
{
    int64_t ts      = s_boot_time_ms + get_time_ms();
    int64_t uptime  = get_time_ms() / 1000;
    printf("{\"device_id\":\"%s\",\"ts_ms\":%lld,\"channel\":%d,"
           "\"tx_rate_hz\":%d,\"uptime_s\":%lld,\"status\":\"%s\"}\n",
           CONFIG_ESP_TX_DEVICE_ID, ts, s_tx_channel, s_tx_rate_hz,
           uptime, s_tx_running ? "running" : "stopped");
}

static void serial_cmd_task(void *arg)
{
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_cfg);

    char line[CMD_BUF_SIZE];
    int  pos = 0;

    while (1) {
        uint8_t c;
        int n = uart_read_bytes(UART_NUM, &c, 1, pdMS_TO_TICKS(20));
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            if (pos == 0) continue;
            line[pos] = '\0';
            pos = 0;

            if (strncmp(line, "set_rate ", 9) == 0) {
                int hz = atoi(line + 9);
                if (hz > 0 && hz <= 500) {
                    s_tx_rate_hz = hz;
                    printf("{\"ok\":true,\"tx_rate_hz\":%d}\n", hz);
                } else {
                    printf("{\"ok\":false,\"error\":\"rate out of range\"}\n");
                }
            } else if (strncmp(line, "set_channel ", 12) == 0) {
                int ch = atoi(line + 12);
                if (ch >= 1 && ch <= 13) {
                    s_tx_channel = ch;
                    printf("{\"ok\":true,\"channel\":%d}\n", ch);
                } else {
                    printf("{\"ok\":false,\"error\":\"channel out of range\"}\n");
                }
            } else if (strcmp(line, "start_tx") == 0) {
                s_tx_running = true;
                printf("{\"ok\":true,\"status\":\"running\"}\n");
            } else if (strcmp(line, "stop_tx") == 0) {
                s_tx_running = false;
                printf("{\"ok\":true,\"status\":\"stopped\"}\n");
            } else if (strcmp(line, "status") == 0) {
                heartbeat_cb(NULL);
            } else {
                printf("{\"ok\":false,\"error\":\"unknown command: %s\"}\n", line);
            }
        } else if (pos < CMD_BUF_SIZE - 1) {
            line[pos++] = (char)c;
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    s_boot_time_ms = 0; /* adjust if RTC/SNTP available */

    wifi_init();

    /* Heartbeat timer every 5 seconds */
    esp_timer_handle_t hb_timer;
    esp_timer_create_args_t hb_args = {
        .callback = heartbeat_cb,
        .name     = "heartbeat",
    };
    ESP_ERROR_CHECK(esp_timer_create(&hb_args, &hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(hb_timer, 5000000)); /* 5 s */

    xTaskCreate(tx_task,          "tx",     4096, NULL, 5, NULL);
    xTaskCreate(serial_cmd_task,  "serial", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Transmitter started — device_id=%s rate=%dHz channel=%d",
             CONFIG_ESP_TX_DEVICE_ID, s_tx_rate_hz, s_tx_channel);
}
