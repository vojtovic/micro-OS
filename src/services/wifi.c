#include "wifi.h"
#include "vconsole.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECT_TIMEOUT_MS  15000

static EventGroupHandle_t s_wifi_eg;
static esp_netif_t       *s_netif;
static wifi_state_t       s_state = WIFI_SVC_DISCONNECTED;
static char               s_ssid[33];
static int                s_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Reconnecting (%d/%d)...", s_retry_count, WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        } else {
            s_state = WIFI_SVC_ERROR;
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "Connection failed after %d retries", WIFI_MAX_RETRY);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_state = WIFI_SVC_CONNECTED;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_service_init(void)
{
    // Wi-Fi stack requires NVS for calibration and config storage.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_eg = xEventGroupCreate();
    if (!s_wifi_eg) return ESP_ERR_NO_MEM;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Wi-Fi service initialized (STA mode)");
    return ESP_OK;
}

esp_err_t wifi_connect(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password)
        strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);

    wifi_cfg.sta.threshold.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) return err;

    s_state = WIFI_SVC_CONNECTING;
    s_retry_count = 0;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);

    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_disconnect();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    s_state = WIFI_SVC_ERROR;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_disconnect(void)
{
    esp_err_t err = esp_wifi_disconnect();
    s_state = WIFI_SVC_DISCONNECTED;
    s_ssid[0] = '\0';
    return err;
}

wifi_state_t wifi_get_state(void)
{
    return s_state;
}

esp_err_t wifi_get_info(wifi_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));

    strncpy(info->ssid, s_ssid, sizeof(info->ssid) - 1);
    info->state = s_state;

    if (s_state == WIFI_SVC_CONNECTED) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
            snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip_info.ip));
        }

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            info->rssi = ap.rssi;
        }
    }
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_state == WIFI_SVC_CONNECTED;
}

static const hal_wifi_ops_t s_wifi_ops = {
    .connect      = wifi_connect,
    .disconnect   = wifi_disconnect,
    .get_state    = wifi_get_state,
    .get_info     = wifi_get_info,
    .is_connected = wifi_is_connected,
};

const hal_wifi_ops_t *wifi_get_ops(void)
{
    return &s_wifi_ops;
}

static const char *state_str(wifi_state_t st)
{
    switch (st) {
        case WIFI_SVC_DISCONNECTED: return "disconnected";
        case WIFI_SVC_CONNECTING:   return "connecting";
        case WIFI_SVC_CONNECTED:    return "connected";
        case WIFI_SVC_ERROR:        return "error";
        default:                    return "unknown";
    }
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: wifi <status|connect|disconnect|scan>\n");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        wifi_info_t info;
        wifi_get_info(&info);
        vconsole_printf("State: %s\n", state_str(info.state));
        if (info.ssid[0])
            vconsole_printf("SSID:  %s\n", info.ssid);
        if (info.ip[0])
            vconsole_printf("IP:    %s\n", info.ip);
        if (info.state == WIFI_SVC_CONNECTED)
            vconsole_printf("RSSI:  %d dBm\n", info.rssi);
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 3) {
            vconsole_printf("Usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        const char *pw = (argc >= 4) ? argv[3] : NULL;
        vconsole_printf("Connecting to '%s'...\n", argv[2]);
        esp_err_t err = wifi_connect(argv[2], pw);
        if (err == ESP_OK) {
            wifi_info_t info;
            wifi_get_info(&info);
            vconsole_printf("Connected! IP: %s\n", info.ip);
        } else {
            vconsole_printf("Connection failed\n");
        }
        return (err == ESP_OK) ? 0 : 1;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        wifi_disconnect();
        vconsole_printf("Disconnected\n");
        return 0;
    }

    if (strcmp(argv[1], "scan") == 0) {
        wifi_scan_config_t scan_cfg = { .show_hidden = true };
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            vconsole_printf("Scan failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count == 0) {
            vconsole_printf("No APs found\n");
            return 0;
        }

        if (ap_count > 20) ap_count = 20;
        wifi_ap_record_t *aps = heap_caps_malloc(ap_count * sizeof(wifi_ap_record_t),
                                                  MALLOC_CAP_SPIRAM);
        if (!aps) {
            vconsole_printf("Out of memory\n");
            return 1;
        }

        esp_wifi_scan_get_ap_records(&ap_count, aps);
        vconsole_printf("%-24s %4s %3s %s\n", "SSID", "RSSI", "CH", "AUTH");
        for (int i = 0; i < ap_count; i++) {
            const char *auth;
            switch (aps[i].authmode) {
                case WIFI_AUTH_OPEN:         auth = "open"; break;
                case WIFI_AUTH_WPA_PSK:      auth = "WPA";  break;
                case WIFI_AUTH_WPA2_PSK:     auth = "WPA2"; break;
                case WIFI_AUTH_WPA3_PSK:     auth = "WPA3"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA+"; break;
                default:                     auth = "other"; break;
            }
            vconsole_printf("%-24s %4d %3d %s\n",
                            (char *)aps[i].ssid, aps[i].rssi,
                            aps[i].primary, auth);
        }
        heap_caps_free(aps);
        vconsole_printf("(%d APs)\n", ap_count);
        return 0;
    }

    vconsole_printf("Unknown: wifi %s\n", argv[1]);
    return 1;
}

esp_err_t cmd_wifi_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wifi",
        .help = "Wi-Fi: status, connect <ssid> [pw], disconnect, scan",
        .func = cmd_wifi,
    };
    return esp_console_cmd_register(&cmd);
}
