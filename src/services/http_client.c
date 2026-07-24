#include "http_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>

static const char *TAG = "http_client";

#define HTTP_MAX_RESPONSE  (256 * 1024)
#define HTTP_CHUNK_SIZE    4096

esp_err_t http_get(const char *url, http_response_t *response)
{
    if (!url || !response) return ESP_ERR_INVALID_ARG;
    memset(response, 0, sizeof(*response));

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,   // trust the ESP-IDF CA bundle (HTTPS)
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    response->status_code = esp_http_client_get_status_code(client);

    size_t alloc_size = (content_len > 0) ? (size_t)content_len + 1 : HTTP_CHUNK_SIZE;
    if (alloc_size > HTTP_MAX_RESPONSE) {
        ESP_LOGE(TAG, "Response too large: %zu", alloc_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    response->body = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!response->body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (true) {
        if (total + HTTP_CHUNK_SIZE > HTTP_MAX_RESPONSE) break;

        if (total + HTTP_CHUNK_SIZE > alloc_size) {
            size_t new_size = alloc_size * 2;
            if (new_size > HTTP_MAX_RESPONSE) new_size = HTTP_MAX_RESPONSE;
            char *tmp = heap_caps_malloc(new_size, MALLOC_CAP_SPIRAM);
            if (!tmp) break;
            memcpy(tmp, response->body, total);
            heap_caps_free(response->body);
            response->body = tmp;
            alloc_size = new_size;
        }

        int read = esp_http_client_read(client, response->body + total,
                                        HTTP_CHUNK_SIZE);
        if (read <= 0) break;
        total += read;
    }

    response->body[total] = '\0';
    response->body_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "GET %s — %d (%zu bytes)", url, response->status_code, total);
    return ESP_OK;
}

esp_err_t http_download_file(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,   // HTTPS
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        // Try to create the parent directory once and retry, in case
        // /sdcard/tmp got removed or the umask hid it.
        int saved_errno = errno;
        char parent[256];
        strncpy(parent, dest_path, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            mkdir(parent, 0755);
        }
        f = fopen(dest_path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot create %s: errno=%d (%s)",
                     dest_path, saved_errno, strerror(saved_errno));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    char *buf = heap_caps_malloc(HTTP_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    esp_task_wdt_add(NULL);
    size_t total = 0;
    while (true) {
        int read = esp_http_client_read(client, buf, HTTP_CHUNK_SIZE);
        if (read <= 0) break;
        size_t written = fwrite(buf, 1, read, f);
        if (written != (size_t)read) {
            ESP_LOGE(TAG, "Write error at %zu bytes", total);
            err = ESP_FAIL;
            break;
        }
        total += read;
        esp_task_wdt_reset();
    }
    esp_task_wdt_delete(NULL);

    heap_caps_free(buf);
    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "Downloaded %s → %s (%zu bytes)", url, dest_path, total);
    return err;
}

void http_response_free(http_response_t *response)
{
    if (!response) return;
    if (response->body) {
        heap_caps_free(response->body);
        response->body = NULL;
    }
    response->body_len = 0;
}

char *http_fetch(const char *url, int *out_len)
{
    http_response_t r;
    if (http_get(url, &r) != ESP_OK) return NULL;
    if (out_len) *out_len = (int)r.body_len;
    return r.body;                                  // caller frees via http_free
}

void http_free(char *body)
{
    if (body) heap_caps_free(body);
}
