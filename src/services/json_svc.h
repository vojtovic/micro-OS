#ifndef JSON_SVC_H
#define JSON_SVC_H

// Module-safe JSON access — a thin wrapper over cJSON kept kernel-side (modules
// can't include cJSON.h). Nodes are opaque void* handles. Parse a document, walk
// it by key/index, then read string/number values. Free the ROOT once when done.

void *json_parse(const char *text);          // parse -> root node, or NULL
void  json_free (void *root);                // free a parsed document (root only)

void *json_get(void *node, const char *key); // object member, or NULL
void *json_at (void *node, int index);       // array element, or NULL
int   json_len(void *node);                  // array length (0 if not an array)

int   json_str(void *node, char *out, int len);  // string value → out; 1 if ok
int   json_num(void *node, double *out);         // number value → out; 1 if ok

// Member value in one call (node + key).
int   json_get_str(void *node, const char *key, char *out, int len);
int   json_get_num(void *node, const char *key, double *out);

// Get a number as a rounded integer scaled by `scale` (e.g. scale=10 → tenths).
// The float math stays kernel-side so modules avoid the libgcc soft-float
// helpers (__adddf3/__muldf3/...) that aren't in the symbol table. 1 if found.
int   json_get_int(void *node, const char *key, int scale, int *out);

#endif // JSON_SVC_H
