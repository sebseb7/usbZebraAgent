#include "app.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static app_t g_app;

static void on_walk_close(uv_handle_t *handle, void *arg) {
    (void)arg;
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

static void on_sigint(uv_signal_t *sig, int signum) {
    app_t *app = (app_t *)sig->data;
    if (!app->running) {
        return;
    }
    log_msg("signal %d: shutting down", signum);
    app->running = 0;

    /* stop accepting new work and cancel in-flight USB/curl */
    uv_timer_stop(&app->usb_timer);
    uv_signal_stop(sig);

    usb_shutdown(app);   /* cancels USB transfers, tears down agents          */

    /* close everything still attached to the loop; pending close callbacks
     * (USB transfer cancellations, curl socket removals) run as the loop winds
     * down, after which uv_run returns. */
    uv_close((uv_handle_t *)sig, NULL);
    uv_walk(app->loop, on_walk_close, NULL);
}

int main(void) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.loop = uv_default_loop();
    g_app.running = 1;
    g_app.agents = NULL;

    util_load_config(&g_app);

    log_msg("usbprintagent (C) starting");
    log_msg("  agent server: %s", g_app.server_url);

    if (curl_uv_init(&g_app) != 0) {
        return 1;
    }
    if (usb_init(&g_app) != 0) {
        curl_uv_shutdown(&g_app);
        return 1;
    }

    uv_signal_t sigint, sigterm;
    uv_signal_init(g_app.loop, &sigint);
    sigint.data = &g_app;
    uv_signal_start(&sigint, on_sigint, SIGINT);
    uv_signal_init(g_app.loop, &sigterm);
    sigterm.data = &g_app;
    uv_signal_start(&sigterm, on_sigint, SIGTERM);

    uv_run(g_app.loop, UV_RUN_DEFAULT);

    curl_uv_shutdown(&g_app);
    if (g_app.usb) {
        libusb_exit(g_app.usb);
        g_app.usb = NULL;
    }
    uv_loop_close(g_app.loop);
    log_msg("usbprintagent stopped");
    return 0;
}
