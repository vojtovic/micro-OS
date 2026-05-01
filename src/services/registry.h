#ifndef REGISTRY_H
#define REGISTRY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SERVICE_MAX       16
#define SERVICE_NAME_LEN  32

typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_RUNNING,
    SERVICE_ERROR,
} service_state_t;

typedef struct {
    char     name[SERVICE_NAME_LEN];
    uint16_t version;
    service_state_t state;
    void    *vtable;
    void    *ctx;
} service_entry_t;

esp_err_t registry_init(void);
esp_err_t registry_add(const char *name, uint16_t version, void *vtable, void *ctx);
esp_err_t registry_set_state(const char *name, service_state_t state);
service_entry_t *registry_find(const char *name);
int registry_count(void);
const service_entry_t *registry_get(int index);

esp_err_t cmd_service_register(void);

#endif
