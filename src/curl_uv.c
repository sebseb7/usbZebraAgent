#include "app.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---- transfer identity stored in CURLOPT_PRIVATE ---- */
typedef enum { CURL_SSE, CURL_POST } curl_kind_t;

typedef struct {
    curl_kind_t kind;
    agent_t *agent;          /* SSE only */
    char *body;              /* POST only: owned request body */
    struct curl_slist *hdrs; /* POST only */
} curl_priv_t;

/* ---- per-socket context bound to a uv_poll_t ---- */
typedef struct {
    uv_poll_t poll;
    app_t *app;
    curl_socket_t sockfd;
    int poll_init;
} sock_ctx_t;

static void check_multi_info(app_t *app);

/* ---------- libuv callbacks ---------- */

static void on_poll_close(uv_handle_t *h) {
    free(h->data);
}

static void curl_perform(uv_poll_t *req, int status, int events) {
    sock_ctx_t *ctx = (sock_ctx_t *)req->data;
    app_t *app = ctx->app;

    int flags = 0;
    if (status < 0) {
        flags |= CURL_CSELECT_ERR;
    }
    if (events & UV_READABLE) {
        flags |= CURL_CSELECT_IN;
    }
    if (events & UV_WRITABLE) {
        flags |= CURL_CSELECT_OUT;
    }

    int still_running = 0;
    curl_multi_socket_action(app->multi, ctx->sockfd, flags, &still_running);
    check_multi_info(app);
}

static void on_curl_timeout(uv_timer_t *timer) {
    app_t *app = (app_t *)timer->data;
    int still_running = 0;
    curl_multi_socket_action(app->multi, CURL_SOCKET_TIMEOUT, 0, &still_running);
    check_multi_info(app);
}

/* ---------- CURLM callbacks ---------- */

static int handle_socket(CURL *easy, curl_socket_t s, int action,
                         void *userp, void *socketp) {
    (void)easy;
    app_t *app = (app_t *)userp;
    sock_ctx_t *ctx = (sock_ctx_t *)socketp;

    switch (action) {
        case CURL_POLL_IN:
        case CURL_POLL_OUT:
        case CURL_POLL_INOUT: {
            if (!ctx) {
                ctx = calloc(1, sizeof(*ctx));
                if (!ctx) {
                    return -1;
                }
                ctx->app = app;
                ctx->sockfd = s;
                uv_poll_init_socket(app->loop, &ctx->poll, s);
                ctx->poll.data = ctx;
                ctx->poll_init = 1;
                curl_multi_assign(app->multi, s, ctx);
            }
            int events = 0;
            if (action != CURL_POLL_IN) events |= UV_WRITABLE;
            if (action != CURL_POLL_OUT) events |= UV_READABLE;
            uv_poll_start(&ctx->poll, events, curl_perform);
            break;
        }
        case CURL_POLL_REMOVE:
        default:
            if (ctx) {
                uv_poll_stop(&ctx->poll);
                curl_multi_assign(app->multi, s, NULL);
                if (ctx->poll_init) {
                    ctx->poll.data = ctx;
                    uv_close((uv_handle_t *)&ctx->poll, on_poll_close);
                } else {
                    free(ctx);
                }
            }
            break;
    }
    return 0;
}

static int start_timeout(CURLM *multi, long timeout_ms, void *userp) {
    (void)multi;
    app_t *app = (app_t *)userp;
    if (timeout_ms < 0) {
        uv_timer_stop(&app->curl_timer);
        return 0;
    }
    if (timeout_ms == 0) {
        timeout_ms = 1; /* defer to loop iteration, avoid recursion */
    }
    uv_timer_start(&app->curl_timer, on_curl_timeout, (uint64_t)timeout_ms, 0);
    return 0;
}

/* ---------- completion handling ---------- */

static void priv_free(curl_priv_t *priv) {
    if (!priv) {
        return;
    }
    if (priv->hdrs) {
        curl_slist_free_all(priv->hdrs);
    }
    free(priv->body);
    free(priv);
}

static void check_multi_info(app_t *app) {
    CURLMsg *msg;
    int pending;
    while ((msg = curl_multi_info_read(app->multi, &pending)) != NULL) {
        if (msg->msg != CURLMSG_DONE) {
            continue;
        }
        CURL *easy = msg->easy_handle;
        curl_priv_t *priv = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, &priv);
        CURLcode result = msg->data.result;

        curl_multi_remove_handle(app->multi, easy);

        if (priv && priv->kind == CURL_SSE) {
            agent_t *agent = priv->agent;
            log_err("agent %s: SSE closed (%s)", agent->id,
                    curl_easy_strerror(result));
            curl_easy_cleanup(easy);
            priv_free(priv);
            agent->sse = NULL;
            /* reconnect while the printer is still present */
            if (app->running && !agent->closing) {
                uv_timer_start(&agent->reconnect_timer,
                               curl_uv_sse_start_timer_cb,
                               SSE_RECONNECT_MS, 0);
                agent->reconnect_started = 1;
            }
        } else {
            if (priv && result != CURLE_OK) {
                log_err("agent %s: POST output failed (%s)", priv->agent->id,
                        curl_easy_strerror(result));
            }
            curl_easy_cleanup(easy);
            priv_free(priv);
        }
        app->active_handles--;
    }
}

/* ---------- SSE ---------- */

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    agent_t *agent = (agent_t *)userdata;
    size_t total = size * nmemb;
    sse_parser_feed(&agent->parser, ptr, total);
    return total;
}

static int agent_build_url(char *url, size_t url_len, const char *server_url,
                           const char *agent_id, CURL *easy) {
    char *escaped = curl_easy_escape(easy, agent_id, 0);
    if (!escaped) {
        return -1;
    }
    int n = snprintf(url, url_len, "%s/agent/%s", server_url, escaped);
    curl_free(escaped);
    if (n < 0 || (size_t)n >= url_len) {
        return -1;
    }
    return 0;
}

static void on_sse_event(agent_t *agent, const char *event,
                         const char *data, size_t data_len) {
    (void)data_len;
    if (strcmp(event, "zpl") != 0) {
        return; /* connected/hs/zpl-result/etc. are not relayed to the printer */
    }
    size_t zpl_len = 0;
    char *zpl = json_get_string(data, "zpl", &zpl_len);
    if (!zpl || zpl_len == 0) {
        free(zpl);
        return;
    }
    if (zpl_len >= 4) {
        log_msg("agent %s: zpl -> printer (%zu bytes)", agent->id, zpl_len);
    }
    usb_write(agent, (const unsigned char *)zpl, zpl_len);
    free(zpl);
}

void curl_uv_sse_start(agent_t *agent) {
    app_t *app = agent->app;
    if (agent->sse || agent->closing || !app->running) {
        return;
    }

    sse_parser_init(&agent->parser, agent, on_sse_event);

    CURL *easy = curl_easy_init();
    if (!easy) {
        log_err("agent %s: curl_easy_init failed", agent->id);
        return;
    }

    char url[768];
    if (agent_build_url(url, sizeof(url), app->server_url, agent->id, easy) != 0) {
        log_err("agent %s: failed to build SSE URL", agent->id);
        curl_easy_cleanup(easy);
        return;
    }

    curl_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        curl_easy_cleanup(easy);
        return;
    }
    priv->kind = CURL_SSE;
    priv->agent = agent;

    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: text/event-stream");
    priv->hdrs = hdrs;

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, agent);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, priv);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);

    agent->sse = easy;
    curl_multi_add_handle(app->multi, easy);
    app->active_handles++;
    log_msg("agent %s: SSE connecting %s", agent->id, url);
}

void curl_uv_sse_start_timer_cb(uv_timer_t *timer) {
    agent_t *agent = (agent_t *)timer->data;
    agent->reconnect_started = 0;
    curl_uv_sse_start(agent);
}

void curl_uv_sse_stop(agent_t *agent) {
    app_t *app = agent->app;
    if (agent->reconnect_started) {
        uv_timer_stop(&agent->reconnect_timer);
        agent->reconnect_started = 0;
    }
    if (agent->sse) {
        curl_priv_t *priv = NULL;
        curl_easy_getinfo(agent->sse, CURLINFO_PRIVATE, &priv);
        curl_multi_remove_handle(app->multi, agent->sse);
        curl_easy_cleanup(agent->sse);
        priv_free(priv);
        agent->sse = NULL;
        app->active_handles--;
    }
    sse_parser_free(&agent->parser);
}

/* ---------- POST printer output ---------- */

void curl_uv_post_output(agent_t *agent, const unsigned char *data, size_t len) {
    app_t *app = agent->app;
    if (!app->running) {
        return;
    }

    char *escaped = json_escape(data, len);
    if (!escaped) {
        return;
    }
    size_t body_cap = strlen(escaped) + 16;
    char *body = malloc(body_cap);
    if (!body) {
        free(escaped);
        return;
    }
    snprintf(body, body_cap, "{\"output\":\"%s\"}", escaped);
    free(escaped);

    CURL *easy = curl_easy_init();
    if (!easy) {
        free(body);
        return;
    }

    char url[768];
    if (agent_build_url(url, sizeof(url), app->server_url, agent->id, easy) != 0) {
        curl_easy_cleanup(easy);
        free(body);
        return;
    }

    curl_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        curl_easy_cleanup(easy);
        free(body);
        return;
    }
    priv->kind = CURL_POST;
    priv->agent = agent;
    priv->body = body;
    priv->hdrs = curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, priv->hdrs);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, priv->body);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(priv->body));
    curl_easy_setopt(easy, CURLOPT_PRIVATE, priv);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);

    curl_multi_add_handle(app->multi, easy);
    app->active_handles++;
    if (len >= 100) {
        log_msg("agent %s: printer -> server (%zu bytes)", agent->id, len);
    }
}

/* ---------- lifecycle ---------- */

int curl_uv_init(app_t *app) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        log_err("curl_global_init failed");
        return -1;
    }
    app->multi = curl_multi_init();
    if (!app->multi) {
        log_err("curl_multi_init failed");
        return -1;
    }
    uv_timer_init(app->loop, &app->curl_timer);
    app->curl_timer.data = app;
    app->curl_timer_started = 1;

    curl_multi_setopt(app->multi, CURLMOPT_SOCKETFUNCTION, handle_socket);
    curl_multi_setopt(app->multi, CURLMOPT_SOCKETDATA, app);
    curl_multi_setopt(app->multi, CURLMOPT_TIMERFUNCTION, start_timeout);
    curl_multi_setopt(app->multi, CURLMOPT_TIMERDATA, app);
    return 0;
}

void curl_uv_shutdown(app_t *app) {
    if (app->multi) {
        curl_multi_cleanup(app->multi);
        app->multi = NULL;
    }
    curl_global_cleanup();
}
