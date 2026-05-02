#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_SVC_DISCONNECTED = 0,
    WIFI_SVC_CONNECTING,
    WIFI_SVC_CONNECTED,
    WIFI_SVC_ERROR,
} wifi_state_t;

typedef struct {
    char ssid[33];
    char ip[16];
    int8_t rssi;
    wifi_state_t state;
} wifi_info_t;

esp_err_t wifi_service_init(void);
esp_err_t wifi_connect(const char *ssid, const char *password);
esp_err_t wifi_disconnect(void);
wifi_state_t wifi_get_state(void);
esp_err_t wifi_get_info(wifi_info_t *info);
bool wifi_is_connected(void);

typedef struct {
    esp_err_t    (*connect)(const char *ssid, const char *password);
    esp_err_t    (*disconnect)(void);
    wifi_state_t (*get_state)(void);
    esp_err_t    (*get_info)(wifi_info_t *info);
    bool         (*is_connected)(void);
} hal_wifi_ops_t;

const hal_wifi_ops_t *wifi_get_ops(void);

esp_err_t cmd_wifi_register(void);

#endif
