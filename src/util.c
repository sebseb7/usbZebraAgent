#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void util_load_config(app_t *app) {
    const char *url = getenv("AGENT_SERVER_URL");
    if (!url || !*url) {
        url = "http://127.0.0.1:3847";
    }
    snprintf(app->server_url, sizeof(app->server_url), "%s", url);

    /* strip a single trailing slash so we can append "/agent/<id>" cleanly */
    size_t n = strlen(app->server_url);
    if (n > 0 && app->server_url[n - 1] == '/') {
        app->server_url[n - 1] = '\0';
    }
}

static void vlog(FILE *out, const char *fmt, va_list ap) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(out, "[%s.%03ldZ] ", stamp, ts.tv_nsec / 1000000);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    fflush(out);
}

void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, fmt, ap);
    va_end(ap);
}

void log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, fmt, ap);
    va_end(ap);
}

char *json_escape(const unsigned char *data, size_t len) {
    /* worst case every byte becomes \uXXXX (6 chars) */
    char *out = malloc(len * 6 + 1);
    if (!out) {
        return NULL;
    }
    static const char hex[] = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"';  break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\b': out[j++] = '\\'; out[j++] = 'b';  break;
            case '\f': out[j++] = '\\'; out[j++] = 'f';  break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) {
                    out[j++] = '\\';
                    out[j++] = 'u';
                    out[j++] = '0';
                    out[j++] = '0';
                    out[j++] = hex[(c >> 4) & 0xf];
                    out[j++] = hex[c & 0xf];
                } else {
                    out[j++] = (char)c;
                }
        }
    }
    out[j] = '\0';
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *json_get_string(const char *json, const char *key, size_t *out_len) {
    size_t keylen = strlen(key);
    const char *p = json;

    /* find "<key>" used as an object key (followed by optional ws then ':') */
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, keylen) == 0 && p[1 + keylen] == '"') {
            const char *q = p + 2 + keylen;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                p = q + 1;
                break;
            }
        }
        p++;
    }
    if (!p) {
        return NULL;
    }

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') {
        return NULL; /* not a string value */
    }
    p++;

    char *out = malloc(strlen(p) + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  out[j++] = '"';  break;
                case '\\': out[j++] = '\\'; break;
                case '/':  out[j++] = '/';  break;
                case 'b':  out[j++] = '\b'; break;
                case 'f':  out[j++] = '\f'; break;
                case 'n':  out[j++] = '\n'; break;
                case 'r':  out[j++] = '\r'; break;
                case 't':  out[j++] = '\t'; break;
                case 'u': {
                    int h1 = hex_val(p[1]), h2 = hex_val(p[2]);
                    int h3 = hex_val(p[3]), h4 = hex_val(p[4]);
                    if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) {
                        free(out);
                        return NULL;
                    }
                    unsigned cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                    /* encode as UTF-8 (BMP only; sufficient for ZPL/status) */
                    if (cp < 0x80) {
                        out[j++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[j++] = (char)(0xC0 | (cp >> 6));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[j++] = (char)(0xE0 | (cp >> 12));
                        out[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[j++] = (char)(0x80 | (cp & 0x3F));
                    }
                    p += 4;
                    break;
                }
                default:
                    out[j++] = *p;
            }
            if (*p) p++;
        } else {
            out[j++] = *p++;
        }
    }
    out[j] = '\0';
    if (out_len) {
        *out_len = j;
    }
    return out;
}
