#ifndef USBPRINTAGENT_UTIL_H
#define USBPRINTAGENT_UTIL_H

#include <stddef.h>

#include "app.h"

/* Load config from environment into app (server_url). */
void util_load_config(app_t *app);

/* Timestamped logging to stdout / stderr. */
void log_msg(const char *fmt, ...);
void log_err(const char *fmt, ...);

/* JSON-escape `len` raw bytes into a freshly malloc'd NUL-terminated string.
 * Returns NULL on allocation failure. Caller frees. */
char *json_escape(const unsigned char *data, size_t len);

/* Extract the value of string field `key` from a flat JSON object.
 * On success returns a malloc'd, unescaped, NUL-terminated buffer and sets
 * *out_len to its byte length (NUL not counted). Returns NULL if not found. */
char *json_get_string(const char *json, const char *key, size_t *out_len);

#endif /* USBPRINTAGENT_UTIL_H */
