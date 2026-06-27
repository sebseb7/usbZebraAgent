#include "app.h"

#include <stdlib.h>
#include <string.h>

void sse_parser_init(sse_parser_t *p, agent_t *agent, sse_event_cb cb) {
    memset(p, 0, sizeof(*p));
    p->agent = agent;
    p->on_event = cb;
}

void sse_parser_free(sse_parser_t *p) {
    free(p->data);
    p->data = NULL;
    p->data_cap = 0;
    p->data_len = 0;
}

static void data_append(sse_parser_t *p, const char *s, size_t n) {
    size_t need = p->data_len + n + 1;
    if (need > p->data_cap) {
        size_t cap = p->data_cap ? p->data_cap * 2 : 256;
        while (cap < need) cap *= 2;
        char *grown = realloc(p->data, cap);
        if (!grown) {
            return; /* drop on OOM */
        }
        p->data = grown;
        p->data_cap = cap;
    }
    memcpy(p->data + p->data_len, s, n);
    p->data_len += n;
    p->data[p->data_len] = '\0';
}

static void dispatch(sse_parser_t *p) {
    if (!p->saw_field) {
        return;
    }
    /* SSE: a trailing newline in the data buffer is part of the framing and is
     * stripped before dispatch. */
    if (p->data && p->data_len > 0 && p->data[p->data_len - 1] == '\n') {
        p->data[--p->data_len] = '\0';
    }
    const char *ev = p->event[0] ? p->event : "message";
    const char *data = p->data ? p->data : "";
    if (p->on_event) {
        p->on_event(p->agent, ev, data, p->data_len);
    }
    p->event[0] = '\0';
    p->data_len = 0;
    if (p->data) {
        p->data[0] = '\0';
    }
    p->saw_field = 0;
}

static void process_line(sse_parser_t *p) {
    char *line = p->line;
    size_t len = p->line_len;

    if (len == 0) {
        dispatch(p);
        return;
    }
    if (line[0] == ':') {
        return; /* comment / heartbeat */
    }

    /* split field:value */
    char *colon = memchr(line, ':', len);
    char *field;
    char *value;
    size_t value_len;
    if (colon) {
        *colon = '\0';
        field = line;
        value = colon + 1;
        if (*value == ' ') {
            value++;
        }
        value_len = (size_t)(line + len - value);
    } else {
        line[len] = '\0';
        field = line;
        value = line + len;
        value_len = 0;
    }

    p->saw_field = 1;
    if (strcmp(field, "event") == 0) {
        snprintf(p->event, sizeof(p->event), "%s", value);
    } else if (strcmp(field, "data") == 0) {
        data_append(p, value, value_len);
        data_append(p, "\n", 1);
    }
    /* "id" and "retry" fields are ignored */
}

void sse_parser_feed(sse_parser_t *p, const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r') {
            continue; /* normalize CRLF / CR */
        }
        if (c == '\n') {
            p->line[p->line_len] = '\0';
            process_line(p);
            p->line_len = 0;
            continue;
        }
        if (p->line_len < SSE_LINE_MAX - 1) {
            p->line[p->line_len++] = c;
        }
        /* overlong lines are truncated; acceptable for ZPL control payloads */
    }
}
