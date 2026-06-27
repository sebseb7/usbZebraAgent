#include "app.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <poll.h>

/* ---- libusb fd <-> uv_poll bridge ---- */
typedef struct usb_fd {
    uv_poll_t poll;
    app_t *app;
    int fd;
    struct usb_fd *next;
} usb_fd_t;

static usb_fd_t *g_usb_fds;

typedef enum { PEND_ARRIVE, PEND_LEAVE } pend_kind_t;

typedef struct pending_usb {
    pend_kind_t kind;
    libusb_device *dev;          /* ref'd; arrive only                         */
    uint8_t bus;
    uint8_t addr;
    struct pending_usb *next;
} pending_usb_t;

static pending_usb_t *g_pending_usb;

static void agent_reap_ready(app_t *app);
static void usb_process_pending(app_t *app);

static void on_usb_fd_closed(uv_handle_t *h) {
    free(h->data);
}

static void usb_after_events(app_t *app) {
    usb_process_pending(app);
    agent_reap_ready(app);
}

static void usb_event_cb(uv_poll_t *handle, int status, int events) {
    (void)status;
    (void)events;
    usb_fd_t *uf = (usb_fd_t *)handle->data;
    struct timeval zero = {0, 0};
    libusb_handle_events_timeout_completed(uf->app->usb, &zero, NULL);
    usb_after_events(uf->app);
}

static int events_to_uv(short ev) {
    int uv = 0;
    if (ev & POLLIN) uv |= UV_READABLE;
    if (ev & POLLOUT) uv |= UV_WRITABLE;
    return uv;
}

static void pollfd_added(int fd, short events, void *user) {
    app_t *app = (app_t *)user;
    usb_fd_t *uf = calloc(1, sizeof(*uf));
    if (!uf) {
        return;
    }
    uf->app = app;
    uf->fd = fd;
    uv_poll_init(app->loop, &uf->poll, fd);
    uf->poll.data = uf;
    uf->next = g_usb_fds;
    g_usb_fds = uf;
    uv_poll_start(&uf->poll, events_to_uv(events), usb_event_cb);
}

static void pollfd_removed(int fd, void *user) {
    (void)user;
    usb_fd_t **pp = &g_usb_fds;
    while (*pp) {
        usb_fd_t *uf = *pp;
        if (uf->fd == fd) {
            *pp = uf->next;
            uv_poll_stop(&uf->poll);
            uf->poll.data = uf;
            uv_close((uv_handle_t *)&uf->poll, on_usb_fd_closed);
            return;
        }
        pp = &uf->next;
    }
}

/* ---- libusb timeout fallback (platforms without timerfd) ---- */
static void usb_timeout_cb(uv_timer_t *timer) {
    app_t *app = (app_t *)timer->data;
    struct timeval zero = {0, 0};
    libusb_handle_events_timeout_completed(app->usb, &zero, NULL);
    usb_after_events(app);
}

/* ---- agent lifecycle ---- */

static agent_t *agent_find(app_t *app, uint8_t bus, uint8_t addr) {
    for (agent_t *a = app->agents; a; a = a->next) {
        if (a->bus == bus && a->addr == addr) {
            return a;
        }
    }
    return NULL;
}

static void agent_list_remove(app_t *app, agent_t *agent) {
    agent_t **pp = &app->agents;
    while (*pp) {
        if (*pp == agent) {
            *pp = agent->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void on_agent_timer_closed(uv_handle_t *h) {
    agent_t *agent = (agent_t *)h->data;
    free(agent);
}

static void agent_try_finalize(agent_t *agent) {
    if (!agent->closing || agent->in_xfer || agent->out_pending > 0) {
        return;
    }
    app_t *app = agent->app;

    if (agent->handle) {
        if (agent->iface >= 0) {
            libusb_release_interface(agent->handle, agent->iface);
        }
        libusb_close(agent->handle);
        agent->handle = NULL;
    }

    agent_list_remove(app, agent);
    log_msg("agent %s: removed", agent->id);

    agent->reconnect_timer.data = agent;
    uv_close((uv_handle_t *)&agent->reconnect_timer, on_agent_timer_closed);
}

static void agent_begin_close(agent_t *agent) {
    if (agent->closing) {
        return;
    }
    agent->closing = 1;
    curl_uv_sse_stop(agent);
    if (agent->in_xfer) {
        libusb_cancel_transfer(agent->in_xfer);
    }
}

void usb_agent_teardown(agent_t *agent) {
    agent_begin_close(agent);
    agent_try_finalize(agent);
}

/* ---- bulk IN: printer -> server ---- */

static void in_xfer_cb(struct libusb_transfer *xfer) {
    agent_t *agent = (agent_t *)xfer->user_data;

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        if (xfer->actual_length > 0) {
            curl_uv_post_output(agent, xfer->buffer, (size_t)xfer->actual_length);
        }
        if (!agent->closing) {
            if (libusb_submit_transfer(xfer) == 0) {
                return; /* keep listening */
            }
            log_err("agent %s: resubmit IN failed", agent->id);
        }
    } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        log_err("agent %s: IN transfer status %d", agent->id, xfer->status);
    }

    /* transfer is finished for good. Only do bookkeeping here; the actual
     * libusb_release_interface/libusb_close must NOT run inside a libusb event
     * callback (re-entrant control transfer deadlocks), so finalization is
     * deferred to agent_reap_ready() once libusb_handle_events returns. */
    libusb_free_transfer(xfer);
    agent->in_xfer = NULL;
    agent_begin_close(agent);
}

static int agent_start_reading(agent_t *agent) {
    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) {
        return -1;
    }
    int len = agent->ep_in_mps > 0 ? (USB_READ_BUF / agent->ep_in_mps) * agent->ep_in_mps
                                   : USB_READ_BUF;
    if (len <= 0) {
        len = USB_READ_BUF;
    }
    /* timeout 0 = wait indefinitely; completes when the printer sends data */
    libusb_fill_bulk_transfer(xfer, agent->handle, agent->ep_in, agent->in_buf,
                              len, in_xfer_cb, agent, 0);
    agent->in_xfer = xfer;
    int rc = libusb_submit_transfer(xfer);
    if (rc != 0) {
        libusb_free_transfer(xfer);
        agent->in_xfer = NULL;
        log_err("agent %s: submit IN failed: %s", agent->id, libusb_error_name(rc));
        return -1;
    }
    return 0;
}

/* ---- bulk OUT: server -> printer ---- */

static void out_xfer_cb(struct libusb_transfer *xfer) {
    agent_t *agent = (agent_t *)xfer->user_data;
    if (xfer->status != LIBUSB_TRANSFER_COMPLETED) {
        log_err("agent %s: OUT transfer status %d", agent->id, xfer->status);
    }
    free(xfer->buffer);
    libusb_free_transfer(xfer);
    agent->out_pending--;
    /* finalization deferred to agent_reap_ready(); see in_xfer_cb */
}

void usb_write(agent_t *agent, const unsigned char *data, size_t len) {
    if (agent->closing || !agent->handle || agent->ep_out == 0) {
        return;
    }
    unsigned char *buf = malloc(len);
    if (!buf) {
        return;
    }
    memcpy(buf, data, len);

    struct libusb_transfer *xfer = libusb_alloc_transfer(0);
    if (!xfer) {
        free(buf);
        return;
    }
    libusb_fill_bulk_transfer(xfer, agent->handle, agent->ep_out, buf,
                              (int)len, out_xfer_cb, agent, 5000);
    int rc = libusb_submit_transfer(xfer);
    if (rc != 0) {
        log_err("agent %s: submit OUT failed: %s", agent->id, libusb_error_name(rc));
        libusb_free_transfer(xfer);
        free(buf);
        return;
    }
    agent->out_pending++;
}

/* ---- device inspection / open ---- */

static int find_printer_iface(struct libusb_config_descriptor *cfg,
                              int *iface_out, uint8_t *ep_in, uint8_t *ep_out,
                              int *ep_in_mps) {
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
            if (alt->bInterfaceClass != LIBUSB_CLASS_PRINTER) {
                continue;
            }
            uint8_t in = 0, out = 0;
            int in_mps = 0;
            for (int e = 0; e < alt->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                    LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                    in = ep->bEndpointAddress;
                    in_mps = ep->wMaxPacketSize;
                } else {
                    out = ep->bEndpointAddress;
                }
            }
            if (out != 0) {
                *iface_out = alt->bInterfaceNumber;
                *ep_in = in;
                *ep_out = out;
                *ep_in_mps = in_mps;
                return 1;
            }
        }
    }
    return 0;
}

static int device_is_printer(libusb_device *dev) {
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return 0;
    }
    int iface, mps;
    uint8_t in, out;
    int yes = find_printer_iface(cfg, &iface, &in, &out, &mps);
    libusb_free_config_descriptor(cfg);
    return yes;
}

static void agent_open(app_t *app, libusb_device *dev, uint8_t bus, uint8_t addr) {
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || !cfg) {
        return;
    }
    int iface = -1, mps = 0;
    uint8_t ep_in = 0, ep_out = 0;
    int found = find_printer_iface(cfg, &iface, &ep_in, &ep_out, &mps);
    libusb_free_config_descriptor(cfg);
    if (!found) {
        return;
    }

    libusb_device_handle *handle = NULL;
    int rc = libusb_open(dev, &handle);
    if (rc != 0) {
        log_err("usb:%03u:%03u open failed: %s", bus, addr, libusb_error_name(rc));
        return;
    }

    libusb_set_auto_detach_kernel_driver(handle, 1);
    rc = libusb_claim_interface(handle, iface);
    if (rc != 0) {
        log_err("usb:%03u:%03u claim iface %d failed: %s", bus, addr, iface,
                libusb_error_name(rc));
        libusb_close(handle);
        return;
    }

    agent_t *agent = calloc(1, sizeof(*agent));
    if (!agent) {
        libusb_release_interface(handle, iface);
        libusb_close(handle);
        return;
    }
    agent->app = app;
    agent->bus = bus;
    agent->addr = addr;
    agent->handle = handle;
    agent->iface = iface;
    agent->ep_in = ep_in;
    agent->ep_out = ep_out;
    agent->ep_in_mps = mps;
    snprintf(agent->id, sizeof(agent->id), "usb:%03u:%03u", bus, addr);

    uv_timer_init(app->loop, &agent->reconnect_timer);
    agent->reconnect_timer.data = agent;

    agent->next = app->agents;
    app->agents = agent;

    log_msg("agent %s: opened printer iface=%d ep_in=0x%02x ep_out=0x%02x",
            agent->id, iface, ep_in, ep_out);

    if (ep_in != 0) {
        agent_start_reading(agent);
    }
    curl_uv_sse_start(agent);
}

/* ---- hotplug (deferred open/teardown outside libusb callbacks) ---- */

static void pending_usb_push(pend_kind_t kind, libusb_device *dev,
                             uint8_t bus, uint8_t addr) {
    pending_usb_t *p = calloc(1, sizeof(*p));
    if (!p) {
        if (kind == PEND_ARRIVE && dev) {
            libusb_unref_device(dev);
        }
        return;
    }
    p->kind = kind;
    p->dev = dev;
    p->bus = bus;
    p->addr = addr;
    p->next = g_pending_usb;
    g_pending_usb = p;
}

static int LIBUSB_CALL hotplug_cb(libusb_context *ctx, libusb_device *dev,
                                  libusb_hotplug_event event, void *user_data) {
    (void)ctx;
    app_t *app = (app_t *)user_data;
    if (!app->running) {
        return 0;
    }

    uint8_t bus = libusb_get_bus_number(dev);
    uint8_t addr = libusb_get_device_address(dev);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        libusb_ref_device(dev);
        pending_usb_push(PEND_ARRIVE, dev, bus, addr);
    } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        pending_usb_push(PEND_LEAVE, NULL, bus, addr);
    }
    return 0;
}

static void usb_process_pending(app_t *app) {
    while (g_pending_usb) {
        pending_usb_t *p = g_pending_usb;
        g_pending_usb = p->next;

        if (p->kind == PEND_ARRIVE) {
            if (device_is_printer(p->dev) && !agent_find(app, p->bus, p->addr)) {
                agent_open(app, p->dev, p->bus, p->addr);
            }
            libusb_unref_device(p->dev);
        } else {
            agent_t *a = agent_find(app, p->bus, p->addr);
            if (a && !a->closing) {
                log_msg("agent %s: disconnected", a->id);
                usb_agent_teardown(a);
            }
        }
        free(p);
    }
}

/* ---- init / shutdown ---- */

int usb_init(app_t *app) {
    int rc = libusb_init(&app->usb);
    if (rc != 0) {
        log_err("libusb_init failed: %s", libusb_error_name(rc));
        return -1;
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        log_err("libusb hotplug not supported on this platform");
        libusb_exit(app->usb);
        app->usb = NULL;
        return -1;
    }

    rc = libusb_hotplug_register_callback(
        app->usb,
        LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_cb, app, &app->hotplug_handle);
    if (rc != 0) {
        log_err("libusb_hotplug_register_callback failed: %s",
                libusb_error_name(rc));
        libusb_exit(app->usb);
        app->usb = NULL;
        return -1;
    }
    log_msg("USB hotplug enabled");

    const struct libusb_pollfd **pfds = libusb_get_pollfds(app->usb);
    if (pfds) {
        for (int i = 0; pfds[i]; i++) {
            pollfd_added(pfds[i]->fd, pfds[i]->events, app);
        }
        libusb_free_pollfds(pfds);
    }
    libusb_set_pollfd_notifiers(app->usb, pollfd_added, pollfd_removed, app);

    uv_timer_init(app->loop, &app->usb_timer);
    app->usb_timer.data = app;
    if (!libusb_pollfds_handle_timeouts(app->usb)) {
        uv_timer_start(&app->usb_timer, usb_timeout_cb, 100, 100);
    }

    /* LIBUSB_HOTPLUG_ENUMERATE queues already-connected devices; pump once so
     * they are opened before the main loop runs. */
    struct timeval zero = {0, 0};
    libusb_handle_events_timeout_completed(app->usb, &zero, NULL);
    usb_after_events(app);
    return 0;
}

/* Finalize agents whose in-flight transfers have drained. MUST be called from
 * loop context (after libusb_handle_events returns), never from inside a libusb
 * transfer callback, because agent_try_finalize() does synchronous libusb calls
 * (release_interface/close) that deadlock if re-entered during event handling. */
static void agent_reap_ready(app_t *app) {
    agent_t *a = app->agents;
    while (a) {
        agent_t *next = a->next;
        if (a->closing) {
            agent_try_finalize(a);
        }
        a = next;
    }
}

void usb_shutdown(app_t *app) {
    if (app->hotplug_handle) {
        libusb_hotplug_deregister_callback(app->usb, app->hotplug_handle);
        app->hotplug_handle = 0;
    }

    /* tear down all agents */
    agent_t *a = app->agents;
    while (a) {
        agent_t *next = a->next;
        usb_agent_teardown(a);
        a = next;
    }

    /* pump libusb so cancelled transfers complete and handles get released */
    struct timeval tv = {0, 50000};
    for (int i = 0; i < 40 && app->agents; i++) {
        libusb_handle_events_timeout_completed(app->usb, &tv, NULL);
        usb_after_events(app);
    }

    /* close remaining fd pollers */
    usb_fd_t *uf = g_usb_fds;
    while (uf) {
        usb_fd_t *next = uf->next;
        uv_poll_stop(&uf->poll);
        uf->poll.data = uf;
        uv_close((uv_handle_t *)&uf->poll, on_usb_fd_closed);
        uf = next;
    }
    g_usb_fds = NULL;

    if (app->usb) {
        libusb_set_pollfd_notifiers(app->usb, NULL, NULL, NULL);
    }
}
