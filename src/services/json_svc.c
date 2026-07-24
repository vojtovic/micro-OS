#include "json_svc.h"
#include "cJSON.h"
#include <string.h>

void *json_parse(const char *text) { return text ? cJSON_Parse(text) : NULL; }
void  json_free (void *root)        { if (root) cJSON_Delete((cJSON *)root); }

void *json_get(void *node, const char *key)
{
    return (node && key) ? cJSON_GetObjectItemCaseSensitive((cJSON *)node, key) : NULL;
}

void *json_at(void *node, int index)
{
    return node ? cJSON_GetArrayItem((cJSON *)node, index) : NULL;
}

int json_len(void *node)
{
    return (node && cJSON_IsArray((cJSON *)node)) ? cJSON_GetArraySize((cJSON *)node) : 0;
}

int json_str(void *node, char *out, int len)
{
    cJSON *n = (cJSON *)node;
    if (n && cJSON_IsString(n) && n->valuestring && out && len > 0) {
        strncpy(out, n->valuestring, len - 1);
        out[len - 1] = '\0';
        return 1;
    }
    if (out && len > 0) out[0] = '\0';
    return 0;
}

int json_num(void *node, double *out)
{
    cJSON *n = (cJSON *)node;
    if (n && cJSON_IsNumber(n)) { if (out) *out = n->valuedouble; return 1; }
    return 0;
}

int json_get_str(void *node, const char *key, char *out, int len)
{
    return json_str(json_get(node, key), out, len);
}

int json_get_num(void *node, const char *key, double *out)
{
    return json_num(json_get(node, key), out);
}

int json_get_int(void *node, const char *key, int scale, int *out)
{
    double v;
    if (!json_num(json_get(node, key), &v)) return 0;
    if (out) *out = (int)(v * scale + (v < 0 ? -0.5 : 0.5));
    return 1;
}
