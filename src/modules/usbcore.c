/* usbcore.kmd — USB core: enumeration, standard requests, registries.
 *
 * Per docs/research/50-usb-enumeration-walkthrough.md, written spec-first
 * from USB 2.0 (April 2000). USBDDOS + iPXE consulted as sanity-check
 * references only — never copied. Every cited section is in the spec
 * digest under docs/research/refs/usb-2.0/.
 *
 * License: GPL-2.0 — usbcore exports go through EXPORT_SYMBOL_GPL so
 * proprietary HCD/class modules cannot link. (We inherit GPLv2 from the
 * USBDDOS lineage even though no code was copied.)
 *
 * Module load order requirement: usbcore must load before any HCD or
 * class driver. The .kmd loader's MODULE_DEPENDS support is currently
 * aspirational — for now the autoload directory order happens to put
 * USBCORE.KMD before UHCI.KMD / HID.KMD / MSC.KMD alphabetically.
 */

#include "module.h"
#include "usbcore.h"
#include "types.h"

/* ============================================================
 * Kernel imports — must be declared extern; resolved by the .kmd
 * loader against the kernel's .kexport table at module-load time.
 * (kexports.c lists each.)
 * ============================================================ */
extern void *kmalloc(uint32_t size);
extern void  kfree  (void *p);
extern void  serial_puts (const char *s);
extern void  serial_puthex(uint32_t v);
extern void  serial_putc (char c);
extern void  pit_delay_ms(uint32_t ms);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void *memset(void *dst, int v, unsigned long n);
extern int   strcmp(const char *a, const char *b);

/* ============================================================
 * Module-private state
 * ============================================================ */

#define USBCORE_MAX_HCDS              4
#define USBCORE_MAX_CLASS_DRIVERS    16

static usb_hcd_t           *g_hcds[USBCORE_MAX_HCDS];
static int                  g_num_hcds = 0;

static usb_class_driver_t  *g_class_drivers[USBCORE_MAX_CLASS_DRIVERS];
static int                  g_num_class_drivers = 0;

/* Flat per-bus device pool. Index 0 is unused (address 0 = default). */
static usb_device_t         g_devices[USB_MAX_DEVICES + 1];
static usb_device_t        *g_device_list_head = 0;

/* ============================================================
 * Small log helpers (no printf in kernel exports)
 * ============================================================ */
static void log_str(const char *s) { serial_puts(s); }
static void log_hex(uint32_t v)    { serial_puthex(v); }

/* ============================================================
 * Device-pool helpers
 * ============================================================ */
static usb_device_t *device_alloc(usb_hcd_t *hcd, uint8_t port, usb_speed_t spd) {
    for (int i = 1; i <= USB_MAX_DEVICES; i++) {
        usb_device_t *d = &g_devices[i];
        if (d->in_use) continue;
        memset(d, 0, sizeof(*d));
        d->in_use   = 1;
        d->hcd      = hcd;
        d->port     = port;
        d->speed    = spd;
        d->address  = 0;
        /* (USB 2.0 §5.5.3, p.39) Every device guarantees ≥8 bytes in
         * a single packet on ep0 at default address; use that as the
         * bootstrap maxpacket. The real value comes from byte 7 of the
         * device descriptor. */
        d->ep0_max_packet = 8;
        d->next = g_device_list_head;
        g_device_list_head = d;
        return d;
    }
    return 0;
}

static void device_free(usb_device_t *d) {
    if (!d) return;
    /* Unlink from list */
    usb_device_t **pp = &g_device_list_head;
    while (*pp && *pp != d) pp = &(*pp)->next;
    if (*pp) *pp = d->next;
    d->in_use = 0;
}

/* Find a free address 1..127 on the given HCD bus. (USB 2.0 §9.4.6,
 * p.256: 0 is reserved as the default address.) */
static uint8_t address_alloc(usb_hcd_t *hcd) {
    for (uint8_t a = 1; a <= 127; a++) {
        int taken = 0;
        for (int i = 1; i <= USB_MAX_DEVICES; i++) {
            usb_device_t *d = &g_devices[i];
            if (d->in_use && d->hcd == hcd && d->address == a) { taken = 1; break; }
        }
        if (!taken) return a;
    }
    return 0;  /* exhausted — refuse to enumerate */
}

/* ============================================================
 * Standard control transfer (USB 2.0 §9.3, p.248)
 * Routes through dev->hcd->ops->submit_control synchronously.
 * ============================================================ */
int usbcore_control_transfer(usb_device_t *dev,
                             uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex,
                             void *data, uint16_t wLength,
                             uint32_t timeout_ms) {
    if (!dev || !dev->hcd || !dev->hcd->ops) return USB_EINVAL;

    usb_hcd_ops_t *ops = dev->hcd->ops;
    int have_split = (ops->submit_setup && ops->submit_data && ops->submit_status);
    if (!have_split && !ops->submit_control) return USB_ENOSYS;

    usb_xfer_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.dev               = dev;
    xfer.pipe              = dev->ep0_pipe;
    xfer.setup.bmRequestType = bmRequestType;
    xfer.setup.bRequest      = bRequest;
    xfer.setup.wValue        = wValue;
    xfer.setup.wIndex        = wIndex;
    xfer.setup.wLength       = wLength;
    xfer.data              = data;
    xfer.len               = wLength;
    xfer.dir_in            = (bmRequestType & 0x80) ? 1 : 0;
    xfer.timeout_ms        = timeout_ms;

    if (have_split) {
        int r = ops->submit_setup(dev->hcd, &xfer);
        if (r < 0) return r;
        if (wLength) {
            r = ops->submit_data(dev->hcd, &xfer);
            if (r < 0) return r;
        }
        r = ops->submit_status(dev->hcd, &xfer);
        if (r < 0) return r;
        return (int)xfer.actual;
    }

    int r = ops->submit_control(dev->hcd, &xfer);
    if (r < 0) return r;
    return (int)xfer.actual;
}

/* ============================================================
 * Standard-request wrappers (USB 2.0 §9.4)
 * Timeouts per §9.2.6.4, p.246: 50 ms for no-data requests, 5 s upper
 * bound for any request (§9.2.6.1).
 * ============================================================ */

/* (USB 2.0 §9.4.3, p.253) */
int usbcore_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t idx,
                           uint16_t langid, void *buf, uint16_t len) {
    return usbcore_control_transfer(dev,
        0x80,                                /* D2H | std | device */
        USB_REQ_GET_DESCRIPTOR,
        ((uint16_t)type << 8) | idx,
        langid,
        buf, len,
        5000);
}

/* (USB 2.0 §9.4.6, p.256) */
int usbcore_set_address(usb_device_t *dev, uint8_t addr) {
    if (addr > 127) return USB_EINVAL;
    int r = usbcore_control_transfer(dev,
        0x00, USB_REQ_SET_ADDRESS,
        addr, 0,
        0, 0,
        50);
    if (r < 0) return r;
    return USB_OK;
}

/* (USB 2.0 §9.4.7, p.257) */
int usbcore_set_configuration(usb_device_t *dev, uint8_t config) {
    int r = usbcore_control_transfer(dev,
        0x00, USB_REQ_SET_CONFIGURATION,
        config, 0,
        0, 0,
        50);
    if (r < 0) return r;
    return USB_OK;
}

/* (USB 2.0 §9.4.11, p.260 + §9.4.5, p.256) */
int usbcore_clear_halt(usb_device_t *dev, usb_endpoint_t *ep) {
    if (!ep) return USB_EINVAL;
    return usbcore_control_transfer(dev,
        0x02,                              /* H2D | std | endpoint */
        USB_REQ_CLEAR_FEATURE,
        USB_FEAT_ENDPOINT_HALT,
        ep->addr,
        0, 0,
        50);
}

/* (USB 2.0 §9.4.10, p.259) */
int usbcore_set_interface(usb_device_t *dev, uint8_t iface, uint8_t alt) {
    return usbcore_control_transfer(dev,
        0x01,                              /* H2D | std | interface */
        USB_REQ_SET_INTERFACE,
        alt, iface,
        0, 0,
        50);
}

/* (USB 2.0 §9.6.7, p.273) String descriptors are UTF-16LE without
 * NUL. We render to ASCII by dropping the high byte; replace
 * non-ASCII with '?'. langid = 0x0409 (US English) by default. */
int usbcore_get_string(usb_device_t *dev, uint8_t idx,
                       uint16_t langid, char *buf, uint32_t buflen) {
    if (!buf || buflen == 0) return USB_EINVAL;
    buf[0] = 0;
    if (idx == 0) return USB_OK;

    uint8_t raw[256];
    int n = usbcore_get_descriptor(dev, USB_DT_STRING, idx,
                                   langid ? langid : 0x0409,
                                   raw, sizeof(raw));
    if (n < 2) return n < 0 ? n : USB_EIO;

    uint8_t blen = raw[0];
    if (blen < 2 || blen > n) return USB_EIO;

    uint32_t out = 0;
    for (uint32_t i = 2; i + 1 < blen && out + 1 < buflen; i += 2) {
        uint16_t cp = (uint16_t)raw[i] | ((uint16_t)raw[i + 1] << 8);
        buf[out++] = (cp < 0x80) ? (char)cp : '?';
    }
    buf[out] = 0;
    return (int)out;
}

/* ============================================================
 * Configuration-chain parser (USB 2.0 §9.5, p.260 + §9.4.3, p.253)
 * ============================================================ */

static void attach_endpoint(usb_interface_t *iface,
                            const struct usb_endpoint_descriptor *ed) {
    if (iface->num_endpoints >= USB_MAX_ENDPOINTS_PER_IF) return;
    usb_endpoint_t *ep = &iface->endpoints[iface->num_endpoints++];
    memset(ep, 0, sizeof(*ep));
    ep->desc       = *ed;
    ep->addr       = ed->bEndpointAddress;
    ep->type       = ed->bmAttributes & 0x03;
    /* wMaxPacketSize bits [10:0] = bytes per packet; [12:11] = HS
     * additional transactions. Mask to the size for v1. */
    ep->max_packet = ed->wMaxPacketSize & 0x07FF;
    ep->interval   = ed->bInterval;
}

static void append_class_desc(usb_interface_t *iface,
                              const uint8_t *p, uint8_t blen) {
    if (iface->class_desc_len + blen > USB_CLASS_DESC_BLOB_MAX) return;
    memcpy(iface->class_desc_blob + iface->class_desc_len, p, blen);
    iface->class_desc_len += blen;
}

int usbcore_parse_config_descriptor(usb_device_t *dev,
                                    const uint8_t *buf, uint16_t total) {
    if (!dev || !buf || total < 9) return USB_EINVAL;

    usb_configuration_t *cfg = &dev->configs[0];
    memset(cfg, 0, sizeof(*cfg));

    usb_interface_t *cur = 0;
    uint16_t pos = 0;

    while (pos + 2 <= total) {
        uint8_t blen  = buf[pos];
        uint8_t btype = buf[pos + 1];
        if (blen < 2 || pos + blen > total) return USB_EINVAL;

        switch (btype) {
        case USB_DT_CONFIG: {
            uint32_t n = blen;
            if (n > sizeof(cfg->desc)) n = sizeof(cfg->desc);
            memcpy(&cfg->desc, buf + pos, n);
            break;
        }
        case USB_DT_INTERFACE: {
            const struct usb_interface_descriptor *id =
                (const struct usb_interface_descriptor *)(buf + pos);
            if (id->bInterfaceNumber >= USB_MAX_INTERFACES_PER_CFG)
                return USB_EINVAL;
            cur = &cfg->interfaces[id->bInterfaceNumber];
            cur->desc          = *id;
            cur->num_endpoints = 0;
            cur->class_desc_len = 0;
            if (id->bInterfaceNumber + 1 > cfg->num_interfaces)
                cfg->num_interfaces = id->bInterfaceNumber + 1;
            break;
        }
        case USB_DT_ENDPOINT: {
            if (!cur) return USB_EINVAL;
            const struct usb_endpoint_descriptor *ed =
                (const struct usb_endpoint_descriptor *)(buf + pos);
            attach_endpoint(cur, ed);
            break;
        }
        default:
            /* class-specific (HID descriptor, MSC pipe-usage, etc.) —
             * preserve as opaque blob for the class driver. */
            if (cur) append_class_desc(cur, buf + pos, blen);
            break;
        }
        pos += blen;
    }
    return USB_OK;
}

usb_endpoint_t *usbcore_find_endpoint(usb_interface_t *iface, uint8_t addr) {
    if (!iface) return 0;
    for (uint8_t i = 0; i < iface->num_endpoints; i++) {
        if (iface->endpoints[i].addr == addr) return &iface->endpoints[i];
    }
    return 0;
}

/* ============================================================
 * Class-driver match + probe (doc 50 §3 step 8 tail)
 * ============================================================ */
static void probe_class_drivers(usb_device_t *dev) {
    usb_configuration_t *cfg = &dev->configs[0];
    for (uint8_t i = 0; i < cfg->num_interfaces; i++) {
        usb_interface_t *iface = &cfg->interfaces[i];
        if (iface->desc.bLength == 0) continue;   /* not populated */
        for (int j = 0; j < g_num_class_drivers; j++) {
            usb_class_driver_t *drv = g_class_drivers[j];
            if (!drv || !drv->match) continue;
            if (drv->match(iface)) {
                if (drv->probe && drv->probe(dev, iface) == 0)
                    iface->driver = drv;
                break;   /* first match wins */
            }
        }
    }
}

/* ============================================================
 * Enumeration — 8-step recipe (USB 2.0 §9.1.2, p.243).
 * Steps 1-4 are the HCD's job (port reset + speed detect);
 * usbcore picks up at step 5.
 * ============================================================ */
static int enumerate_new_device(usb_hcd_t *hcd, uint8_t port,
                                usb_speed_t speed) {
    usb_device_t *dev = device_alloc(hcd, port, speed);
    if (!dev) {
        log_str("usbcore: enumerate: device pool full\n");
        return USB_ENOMEM;
    }

    /* Bootstrap step 6: read first 8 bytes of device descriptor at
     * default address to discover real bMaxPacketSize0. (USB 2.0 §5.5.3,
     * p.39 — every device returns ≥8 bytes in one packet; byte 7 is
     * the answer.) */
    uint8_t head[8];
    int n = usbcore_get_descriptor(dev, USB_DT_DEVICE, 0, 0, head, 8);
    if (n < 8) {
        log_str("usbcore: enumerate: short device-desc read (n=");
        log_hex((uint32_t)n); log_str(")\n");
        goto fail;
    }
    dev->ep0_max_packet = head[7];

    /* Step 5: assign address. */
    uint8_t addr = address_alloc(hcd);
    if (addr == 0) { log_str("usbcore: enumerate: addr exhausted\n"); goto fail; }
    if (usbcore_set_address(dev, addr) < 0) {
        log_str("usbcore: enumerate: SET_ADDRESS failed\n"); goto fail;
    }
    dev->address = addr;

    /* (USB 2.0 §9.2.6.3, p.246) 2 ms recovery interval. */
    pit_delay_ms(2);

    /* Step 6: read full 18-byte device descriptor. */
    struct usb_device_descriptor dd;
    n = usbcore_get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dd, 18);
    if (n < 18) { log_str("usbcore: enumerate: short full-dev-desc\n"); goto fail; }
    dev->vid                = dd.idVendor;
    dev->pid                = dd.idProduct;
    dev->bcd_device         = dd.bcdDevice;
    dev->num_configurations = dd.bNumConfigurations;

    log_str("usbcore: new device vid="); log_hex(dev->vid);
    log_str(" pid="); log_hex(dev->pid);
    log_str(" speed="); log_hex((uint32_t)dev->speed);
    log_str(" addr="); log_hex(dev->address);
    log_str("\n");

    /* Step 7: read configuration descriptor — two-pass to discover
     * wTotalLength. (USB 2.0 §9.6.3, Table 9-10 byte 2-3.) */
    uint8_t cd_head[9];
    n = usbcore_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cd_head, 9);
    if (n < 9) { log_str("usbcore: enumerate: short config-head\n"); goto fail; }
    uint16_t total_len = (uint16_t)cd_head[2] | ((uint16_t)cd_head[3] << 8);
    if (total_len < 9 || total_len > 4096) {
        log_str("usbcore: enumerate: implausible wTotalLength\n"); goto fail;
    }

    uint8_t *cd = (uint8_t *)kmalloc(total_len);
    if (!cd) { log_str("usbcore: enumerate: kmalloc(cfg) failed\n"); goto fail; }
    n = usbcore_get_descriptor(dev, USB_DT_CONFIG, 0, 0, cd, total_len);
    if ((uint32_t)n < total_len) {
        log_str("usbcore: enumerate: short cfg chain\n");
        kfree(cd); goto fail;
    }
    if (usbcore_parse_config_descriptor(dev, cd, total_len) < 0) {
        log_str("usbcore: enumerate: cfg parse failed\n");
        kfree(cd); goto fail;
    }
    kfree(cd);

    /* Step 8: select the configuration. v1 always picks index 0. */
    uint8_t cfg_value = dev->configs[0].desc.bConfigurationValue;
    if (usbcore_set_configuration(dev, cfg_value) < 0) {
        log_str("usbcore: enumerate: SET_CONFIGURATION failed\n"); goto fail;
    }
    dev->current_config = cfg_value;

    /* Class-driver probe. */
    probe_class_drivers(dev);
    return USB_OK;

fail:
    device_free(dev);
    return USB_EIO;
}

/* ============================================================
 * HCD registry (doc 50 §8)
 * ============================================================ */
int usbcore_register_hcd(usb_hcd_t *hcd) {
    if (!hcd || !hcd->ops) return USB_EINVAL;
    if (g_num_hcds >= USBCORE_MAX_HCDS) return USB_ENOMEM;
    /* Refuse duplicates. */
    for (int i = 0; i < g_num_hcds; i++)
        if (g_hcds[i] == hcd) return USB_EBUSY;
    g_hcds[g_num_hcds++] = hcd;
    log_str("usbcore: HCD registered: ");
    log_str(hcd->name ? hcd->name : "(unnamed)");
    log_str(" ports="); log_hex(hcd->num_ports); log_str("\n");
    return USB_OK;
}

int usbcore_unregister_hcd(usb_hcd_t *hcd) {
    for (int i = 0; i < g_num_hcds; i++) {
        if (g_hcds[i] != hcd) continue;
        /* Disconnect any device on this HCD. */
        for (int j = 1; j <= USB_MAX_DEVICES; j++) {
            usb_device_t *d = &g_devices[j];
            if (d->in_use && d->hcd == hcd) {
                /* Notify class drivers of disconnect. */
                usb_configuration_t *cfg = &d->configs[0];
                for (uint8_t k = 0; k < cfg->num_interfaces; k++) {
                    usb_interface_t *iface = &cfg->interfaces[k];
                    if (iface->driver && iface->driver->disconnect)
                        iface->driver->disconnect(d, iface);
                }
                device_free(d);
            }
        }
        /* Shift the array down. */
        for (int k = i; k < g_num_hcds - 1; k++) g_hcds[k] = g_hcds[k + 1];
        g_hcds[--g_num_hcds] = 0;
        log_str("usbcore: HCD unregistered\n");
        return USB_OK;
    }
    return USB_ENODEV;
}

int usbcore_port_connect(usb_hcd_t *hcd, uint8_t port, usb_speed_t spd) {
    if (!hcd) return USB_EINVAL;
    log_str("usbcore: port_connect port="); log_hex(port);
    log_str(" speed="); log_hex((uint32_t)spd); log_str("\n");
    return enumerate_new_device(hcd, port, spd);
}

int usbcore_port_disconnect(usb_hcd_t *hcd, uint8_t port) {
    int found = 0;
    for (int i = 1; i <= USB_MAX_DEVICES; i++) {
        usb_device_t *d = &g_devices[i];
        if (!d->in_use || d->hcd != hcd || d->port != port) continue;
        usb_configuration_t *cfg = &d->configs[0];
        for (uint8_t k = 0; k < cfg->num_interfaces; k++) {
            usb_interface_t *iface = &cfg->interfaces[k];
            if (iface->driver && iface->driver->disconnect)
                iface->driver->disconnect(d, iface);
        }
        device_free(d);
        found++;
    }
    return found ? USB_OK : USB_ENODEV;
}

/* ============================================================
 * Class-driver registry
 * ============================================================ */
int usbcore_register_class_driver(usb_class_driver_t *drv) {
    if (!drv || !drv->match) return USB_EINVAL;
    if (g_num_class_drivers >= USBCORE_MAX_CLASS_DRIVERS) return USB_ENOMEM;
    for (int i = 0; i < g_num_class_drivers; i++)
        if (g_class_drivers[i] == drv) return USB_EBUSY;
    g_class_drivers[g_num_class_drivers++] = drv;
    log_str("usbcore: class driver registered: ");
    log_str(drv->name ? drv->name : "(unnamed)");
    log_str("\n");

    /* Probe against already-enumerated devices. */
    for (int i = 1; i <= USB_MAX_DEVICES; i++) {
        usb_device_t *d = &g_devices[i];
        if (!d->in_use) continue;
        usb_configuration_t *cfg = &d->configs[0];
        for (uint8_t k = 0; k < cfg->num_interfaces; k++) {
            usb_interface_t *iface = &cfg->interfaces[k];
            if (iface->driver) continue;
            if (drv->match(iface) && drv->probe && drv->probe(d, iface) == 0)
                iface->driver = drv;
        }
    }
    return USB_OK;
}

int usbcore_unregister_class_driver(usb_class_driver_t *drv) {
    for (int i = 0; i < g_num_class_drivers; i++) {
        if (g_class_drivers[i] != drv) continue;
        /* Disconnect any interface this driver owns. */
        for (int j = 1; j <= USB_MAX_DEVICES; j++) {
            usb_device_t *d = &g_devices[j];
            if (!d->in_use) continue;
            usb_configuration_t *cfg = &d->configs[0];
            for (uint8_t k = 0; k < cfg->num_interfaces; k++) {
                usb_interface_t *iface = &cfg->interfaces[k];
                if (iface->driver == drv) {
                    if (drv->disconnect) drv->disconnect(d, iface);
                    iface->driver = 0;
                }
            }
        }
        for (int k = i; k < g_num_class_drivers - 1; k++)
            g_class_drivers[k] = g_class_drivers[k + 1];
        g_class_drivers[--g_num_class_drivers] = 0;
        return USB_OK;
    }
    return USB_ENODEV;
}

/* ============================================================
 * Async transfer dispatcher (passes straight through to HCD)
 * ============================================================ */
int usbcore_submit_xfer(usb_device_t *dev, usb_endpoint_t *ep,
                        void *data, uint32_t len,
                        uint32_t timeout_ms,
                        usb_xfer_done_cb_t done, void *ctx) {
    if (!dev || !dev->hcd || !ep) return USB_EINVAL;
    if (!dev->hcd->ops->submit_xfer) return USB_ENOSYS;

    usb_xfer_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.dev        = dev;
    xfer.ep         = ep;
    xfer.pipe       = ep->hcd_priv;
    xfer.data       = data;
    xfer.len        = len;
    xfer.dir_in     = (ep->addr & USB_DIR_IN) ? 1 : 0;
    xfer.timeout_ms = timeout_ms;
    xfer.done       = done;
    xfer.ctx        = ctx;
    return dev->hcd->ops->submit_xfer(dev->hcd, &xfer);
}

usb_device_t *usbcore_device_list(void) {
    return g_device_list_head;
}

int usbcore_open_endpoint(usb_device_t *dev, usb_endpoint_t *ep) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !ep) return USB_EINVAL;
    if (!dev->hcd->ops->ep_open) return USB_ENOSYS;
    return dev->hcd->ops->ep_open(dev->hcd, dev, ep);
}

void usbcore_close_endpoint(usb_device_t *dev, usb_endpoint_t *ep) {
    if (!dev || !dev->hcd || !dev->hcd->ops || !ep) return;
    if (dev->hcd->ops->ep_close)
        dev->hcd->ops->ep_close(dev->hcd, dev, ep);
}

/* ============================================================
 * Module init / exit
 * ============================================================ */
static int usbcore_module_init(void) {
    log_str("usbcore.kmd: init\n");
    g_num_hcds = 0;
    g_num_class_drivers = 0;
    g_device_list_head = 0;
    memset(g_devices, 0, sizeof(g_devices));
    memset(g_hcds, 0, sizeof(g_hcds));
    memset(g_class_drivers, 0, sizeof(g_class_drivers));
    return 0;
}

static void usbcore_module_exit(void) {
    log_str("usbcore.kmd: exit\n");
    /* Tear down all HCDs in reverse order (each disconnects its devices). */
    while (g_num_hcds > 0)
        usbcore_unregister_hcd(g_hcds[g_num_hcds - 1]);
    g_num_class_drivers = 0;
}

module_init(usbcore_module_init);
module_exit(usbcore_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("USB core — enumeration, standard requests, registries");
MODULE_NAME("usbcore");

/* ============================================================
 * Module-side exports — consumed by HCD modules (uhci/ehci/xhci.kmd)
 * and class drivers (hid/msc.kmd). GPL-family only.
 * ============================================================ */
EXPORT_SYMBOL_GPL(usbcore_register_hcd);
EXPORT_SYMBOL_GPL(usbcore_unregister_hcd);
EXPORT_SYMBOL_GPL(usbcore_port_connect);
EXPORT_SYMBOL_GPL(usbcore_port_disconnect);
EXPORT_SYMBOL_GPL(usbcore_register_class_driver);
EXPORT_SYMBOL_GPL(usbcore_unregister_class_driver);
EXPORT_SYMBOL_GPL(usbcore_control_transfer);
EXPORT_SYMBOL_GPL(usbcore_get_descriptor);
EXPORT_SYMBOL_GPL(usbcore_set_address);
EXPORT_SYMBOL_GPL(usbcore_set_configuration);
EXPORT_SYMBOL_GPL(usbcore_clear_halt);
EXPORT_SYMBOL_GPL(usbcore_set_interface);
EXPORT_SYMBOL_GPL(usbcore_get_string);
EXPORT_SYMBOL_GPL(usbcore_submit_xfer);
EXPORT_SYMBOL_GPL(usbcore_find_endpoint);
EXPORT_SYMBOL_GPL(usbcore_parse_config_descriptor);
EXPORT_SYMBOL_GPL(usbcore_device_list);
EXPORT_SYMBOL_GPL(usbcore_open_endpoint);
EXPORT_SYMBOL_GPL(usbcore_close_endpoint);
