#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include <stddef.h>

typedef struct {
    char   *body;
    size_t  body_len;
    int     status_code;
} http_response_t;

esp_err_t http_get(const char *url, http_response_t *response);
esp_err_t http_download_file(const char *url, const char *dest_path);
void      http_response_free(http_response_t *response);

// Module-safe convenience (no http_response_t needed): GET a URL and return the
// NUL-terminated body (caller frees with http_free), setting *out_len. HTTPS
// works via the CA bundle. Returns NULL on failure.
char *http_fetch(const char *url, int *out_len);
void  http_free(char *body);

#endif
