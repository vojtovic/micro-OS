#include "registry.h"
#include "vconsole.h"
#include "esp_console.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "registry";

static service_entry_t s_services[SERVICE_MAX];
static int s_count = 0;

esp_err_t registry_init(void)
{
    s_count = 0;
    memset(s_services, 0, sizeof(s_services));
    ESP_LOGI(TAG, "Service registry initialized (max %d)", SERVICE_MAX);
    return ESP_OK;
}

esp_err_t registry_add(const char *name, uint16_t version, void *vtable, void *ctx)
{
    if (s_count >= SERVICE_MAX) {
        ESP_LOGE(TAG, "Registry full, cannot add '%s'", name);
        return ESP_ERR_NO_MEM;
    }

    if (registry_find(name)) {
        ESP_LOGE(TAG, "Service '%s' already registered", name);
        return ESP_ERR_INVALID_STATE;
    }

    service_entry_t *e = &s_services[s_count++];
    // Module .rodata pointers are data-bus addressable — the historic
    // 0x42xxxxxx LoadStoreError was a loader mapping bug, fixed by
    // PATCH-001 in components/elf_loader (see PATCHES.md).
    strncpy(e->name, name, SERVICE_NAME_LEN - 1);
    e->name[SERVICE_NAME_LEN - 1] = '\0';
    e->version = version;
    e->state = SERVICE_RUNNING;
    e->vtable = vtable;
    e->ctx = ctx;

    ESP_LOGI(TAG, "Registered: %s v%d", e->name, version);
    return ESP_OK;
}

esp_err_t registry_set_state(const char *name, service_state_t state)
{
    service_entry_t *e = registry_find(name);
    if (!e) return ESP_ERR_NOT_FOUND;
    e->state = state;
    return ESP_OK;
}

service_entry_t *registry_find(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_services[i].name, name) == 0)
            return &s_services[i];
    }
    return NULL;
}

int registry_count(void)
{
    return s_count;
}

const service_entry_t *registry_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_services[index];
}

static const char *state_str(service_state_t s)
{
    switch (s) {
        case SERVICE_STOPPED: return "stopped";
        case SERVICE_RUNNING: return "running";
        case SERVICE_ERROR:   return "error";
        default:              return "unknown";
    }
}

static int cmd_service(int argc, char **argv)
{
    if (argc < 2) {
        vconsole_printf("Usage: service list\n");
        vconsole_printf("       service info <name>\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        int n = registry_count();
        vconsole_printf("SERVICE          VER  STATE\n");
        for (int i = 0; i < n; i++) {
            const service_entry_t *e = registry_get(i);
            vconsole_printf("%-16s %3d  %s\n", e->name, e->version, state_str(e->state));
        }
        if (n == 0) vconsole_printf("(no services registered)\n");

    } else if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            vconsole_printf("Usage: service info <name>\n");
            return 1;
        }
        service_entry_t *e = registry_find(argv[2]);
        if (!e) {
            vconsole_printf("Service not found: %s\n", argv[2]);
            return 1;
        }
        vconsole_printf("Name:    %s\n", e->name);
        vconsole_printf("Version: %d\n", e->version);
        vconsole_printf("State:   %s\n", state_str(e->state));
        vconsole_printf("VTable:  %p\n", e->vtable);

    } else {
        vconsole_printf("Unknown: service %s\n", argv[1]);
        return 1;
    }
    return 0;
}

esp_err_t cmd_service_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "service",
        .help = "Service registry: service <list|info> [name]",
        .func = cmd_service,
    };
    return esp_console_cmd_register(&cmd);
}
