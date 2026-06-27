#ifndef USBPRINTAGENT_APP_H
#define USBPRINTAGENT_APP_H

#include <stddef.h>
#include <stdint.h>

#include <uv.h>
#include <curl/curl.h>
#include <libusb-1.0/libusb.h>

#define AGENT_ID_MAX 128
#define SSE_EVENT_MAX 64
#define SSE_LINE_MAX 8192
#define USB_READ_BUF 4096
#define SSE_RECONNECT_MS 2000

struct agent;

/* ---- Application-wide state (one libuv loop, one libusb ctx, one CURLM) ---- */
typedef struct app {
    uv_loop_t *loop;
    libusb_context *usb;

    CURLM *multi;
    uv_timer_t curl_timer;       /* drives curl_multi_socket_action timeouts  */
    int curl_timer_started;

    uv_timer_t usb_timer;        /* libusb timeout fallback                    */
    libusb_hotplug_callback_handle hotplug_handle;

    char server_url[512];        /* AGENT_SERVER_URL, no trailing slash        */

    struct agent *agents;        /* singly linked list of connected printers   */
    int running;                 /* cleared on SIGINT to begin shutdown        */
    int active_handles;          /* outstanding things keeping us alive        */
} app_t;

/* ---- Incremental Server-Sent-Events parser ---- */
typedef void (*sse_event_cb)(struct agent *agent, const char *event,
                             const char *data, size_t data_len);

typedef struct sse_parser {
    char event[SSE_EVENT_MAX];
    char *data;
    size_t data_len;
    size_t data_cap;
    char line[SSE_LINE_MAX];
    size_t line_len;
    int saw_field;               /* any field seen since last dispatch         */
    sse_event_cb on_event;
    struct agent *agent;
} sse_parser_t;

void sse_parser_init(sse_parser_t *p, struct agent *agent, sse_event_cb cb);
void sse_parser_feed(sse_parser_t *p, const char *buf, size_t len);
void sse_parser_free(sse_parser_t *p);

/* ---- Per-printer agent ---- */
typedef struct agent {
    app_t *app;
    char id[AGENT_ID_MAX];       /* printer serial, or "usb:001:003" fallback */
    uint8_t bus;
    uint8_t addr;

    /* USB */
    libusb_device_handle *handle;
    int iface;
    uint8_t ep_in;
    uint8_t ep_out;
    int ep_in_mps;
    struct libusb_transfer *in_xfer;
    unsigned char in_buf[USB_READ_BUF];
    int out_pending;             /* outstanding bulk OUT transfers              */

    /* SSE */
    CURL *sse;                   /* long-lived streaming GET, NULL when down    */
    sse_parser_t parser;
    uv_timer_t reconnect_timer;
    int reconnect_started;

    int closing;                 /* teardown in progress                        */
    struct agent *next;
} agent_t;

/* ---- libusb_uv.c ---- */
int usb_init(app_t *app);
void usb_agent_teardown(agent_t *agent);    /* cancel I/O, release, close       */
void usb_write(agent_t *agent, const unsigned char *data, size_t len);
void usb_shutdown(app_t *app);

/* ---- curl_uv.c ---- */
int curl_uv_init(app_t *app);
void curl_uv_sse_start(agent_t *agent);     /* (re)open SSE GET for agent       */
void curl_uv_sse_start_timer_cb(uv_timer_t *timer);
void curl_uv_sse_stop(agent_t *agent);
void curl_uv_post_output(agent_t *agent, const unsigned char *data, size_t len);
void curl_uv_shutdown(app_t *app);

#endif /* USBPRINTAGENT_APP_H */
