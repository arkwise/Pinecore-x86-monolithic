/* hid.kmd — USB HID Boot Protocol class driver for pinecore-x86.
 *
 * Spec-grounded from docs/research/52-hid-boot-protocol-mapping.md.
 * Primary references:
 *   - USB HID 1.11 (USB-IF, 2001)
 *       §4 class/subclass/protocol  §6 HID descriptor
 *       §7 class requests           §8 report layout
 *       Appendix B Boot reports     Appendix C diff algorithm
 *       Appendix F legacy host impl Appendix G request requirements
 *   - HID Usage Tables 1.22 §10 Keyboard/Keypad (page 0x07)
 *
 * Cross-references consulted (NOT sources):
 *   - USBDDOS CLASS/hid.c (GPLv2)
 *   - iPXE drivers/usb/usbkbd.c (GPL2_OR_LATER_OR_UBDL)
 * Original code written from the spec.
 *
 * License: GPL-2.0.
 *
 * v1 scope:
 *   - Boot keyboard (8-byte report) — modifier bitmap + 6-key array, diff
 *     against previous report to derive make / break events; feeds the
 *     existing keyboard_inject_scancode_sequence sink.
 *   - Boot mouse (3-byte report, optional 4th for wheel) — feeds mouse_inject.
 *   - Phantom-state filter (HID 1.11 §F.3, p.74).
 *
 * Out of v1: typematic auto-repeat (deferred — most software handles it
 * itself), LED sync, dead-key composition, alt-layout remap (kbmap layer
 * already covers this from US scancodes), Report-Protocol parsing.
 */
#include "module.h"
#include "types.h"
#include "usbcore.h"

extern void *kmalloc(uint32_t size);
extern void  kfree  (void *p);
extern void *dma_alloc(uint32_t size, uint32_t align);
extern void  dma_free (void *p);
extern void *memset(void *dst, int v, unsigned long n);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void  serial_puts(const char *s);
extern void  serial_puthex(uint32_t v);

extern int  keyboard_inject_scancode_sequence(const uint8_t *seq, int n);
extern void mouse_inject(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

extern int  usbcore_register_class_driver  (usb_class_driver_t *drv);
extern int  usbcore_unregister_class_driver(usb_class_driver_t *drv);
extern int  usbcore_control_transfer(usb_device_t *dev,
                                     uint8_t bmRequestType, uint8_t bRequest,
                                     uint16_t wValue, uint16_t wIndex,
                                     void *data, uint16_t wLength,
                                     uint32_t timeout_ms);
extern int  usbcore_open_endpoint   (usb_device_t *dev, usb_endpoint_t *ep);
extern void usbcore_close_endpoint  (usb_device_t *dev, usb_endpoint_t *ep);
extern int  usbcore_submit_xfer     (usb_device_t *dev, usb_endpoint_t *ep,
                                     void *data, uint32_t len,
                                     uint32_t timeout_ms,
                                     usb_xfer_done_cb_t done, void *ctx);

/* ============================================================
 * HID class request codes (HID 1.11 §7.2)
 * ============================================================ */
#define HID_REQ_GET_REPORT    0x01
#define HID_REQ_GET_IDLE      0x02
#define HID_REQ_GET_PROTOCOL  0x03
#define HID_REQ_SET_REPORT    0x09
#define HID_REQ_SET_IDLE      0x0A
#define HID_REQ_SET_PROTOCOL  0x0B

#define HID_BMREQ_H2D_CLASS_IF  0x21
#define HID_BMREQ_D2H_CLASS_IF  0xA1

/* Boot subclass + protocol (HID 1.11 §4.2 + §4.3) */
#define HID_SUBCLASS_BOOT      1
#define HID_PROTOCOL_KBD       1
#define HID_PROTOCOL_MOUSE     2

#define HID_PROTO_BOOT         0  /* SET_PROTOCOL wValue */
#define HID_PROTO_REPORT       1

/* ============================================================
 * HID Usage Page 0x07 → AT Set 1 (US, base codes; E0-prefix flag)
 * Per docs/research/52 §10. Entries default to {0, 0} = no map → key
 * silently dropped.
 * ============================================================ */
typedef struct { uint8_t code; uint8_t e0; } hid_at_t;

static const hid_at_t hid_to_at[256] = {
    /* Letters a..z (HID 0x04..0x1D) */
    [0x04]={0x1E,0},[0x05]={0x30,0},[0x06]={0x2E,0},[0x07]={0x20,0},
    [0x08]={0x12,0},[0x09]={0x21,0},[0x0A]={0x22,0},[0x0B]={0x23,0},
    [0x0C]={0x17,0},[0x0D]={0x24,0},[0x0E]={0x25,0},[0x0F]={0x26,0},
    [0x10]={0x32,0},[0x11]={0x31,0},[0x12]={0x18,0},[0x13]={0x19,0},
    [0x14]={0x10,0},[0x15]={0x13,0},[0x16]={0x1F,0},[0x17]={0x14,0},
    [0x18]={0x16,0},[0x19]={0x2F,0},[0x1A]={0x11,0},[0x1B]={0x2D,0},
    [0x1C]={0x15,0},[0x1D]={0x2C,0},
    /* Digits 1..9 (HID 0x1E..0x26), then 0 (HID 0x27) */
    [0x1E]={0x02,0},[0x1F]={0x03,0},[0x20]={0x04,0},[0x21]={0x05,0},
    [0x22]={0x06,0},[0x23]={0x07,0},[0x24]={0x08,0},[0x25]={0x09,0},
    [0x26]={0x0A,0},[0x27]={0x0B,0},
    /* Enter/Esc/BS/Tab/Space */
    [0x28]={0x1C,0},[0x29]={0x01,0},[0x2A]={0x0E,0},[0x2B]={0x0F,0},
    [0x2C]={0x39,0},
    /* - = [ ] \ ; ' ` , . / */
    [0x2D]={0x0C,0},[0x2E]={0x0D,0},[0x2F]={0x1A,0},[0x30]={0x1B,0},
    [0x31]={0x2B,0},                /* 0x32 non-US # */
    [0x33]={0x27,0},[0x34]={0x28,0},[0x35]={0x29,0},[0x36]={0x33,0},
    [0x37]={0x34,0},[0x38]={0x35,0},
    /* CapsLock + F1..F12 */
    [0x39]={0x3A,0},
    [0x3A]={0x3B,0},[0x3B]={0x3C,0},[0x3C]={0x3D,0},[0x3D]={0x3E,0},
    [0x3E]={0x3F,0},[0x3F]={0x40,0},[0x40]={0x41,0},[0x41]={0x42,0},
    [0x42]={0x43,0},[0x43]={0x44,0},[0x44]={0x57,0},[0x45]={0x58,0},
    /* Navigation cluster (E0-prefix) */
    [0x46]={0x37,1},                /* PrtSc — simplified single byte */
    [0x47]={0x46,0},                /* ScrollLock */
                                    /* 0x48 Pause — needs E1 1D 45, dropped */
    [0x49]={0x52,1},                /* Insert */
    [0x4A]={0x47,1},                /* Home */
    [0x4B]={0x49,1},                /* PgUp */
    [0x4C]={0x53,1},                /* Delete */
    [0x4D]={0x4F,1},                /* End */
    [0x4E]={0x51,1},                /* PgDn */
    [0x4F]={0x4D,1},                /* Right arrow */
    [0x50]={0x4B,1},                /* Left arrow */
    [0x51]={0x50,1},                /* Down arrow */
    [0x52]={0x48,1},                /* Up arrow */
    /* Keypad */
    [0x53]={0x45,0},                /* NumLock */
    [0x54]={0x35,1},                /* KP / */
    [0x55]={0x37,0},                /* KP * */
    [0x56]={0x4A,0},                /* KP - */
    [0x57]={0x4E,0},                /* KP + */
    [0x58]={0x1C,1},                /* KP Enter */
    [0x59]={0x4F,0},                /* KP 1 */
    [0x5A]={0x50,0},                /* KP 2 */
    [0x5B]={0x51,0},                /* KP 3 */
    [0x5C]={0x4B,0},                /* KP 4 */
    [0x5D]={0x4C,0},                /* KP 5 */
    [0x5E]={0x4D,0},                /* KP 6 */
    [0x5F]={0x47,0},                /* KP 7 */
    [0x60]={0x48,0},                /* KP 8 */
    [0x61]={0x49,0},                /* KP 9 */
    [0x62]={0x52,0},                /* KP 0 */
    [0x63]={0x53,0},                /* KP . */
    /* Modifiers (also delivered via report byte 0 bitmap) */
    [0xE0]={0x1D,0},                /* LCtrl  */
    [0xE1]={0x2A,0},                /* LShift */
    [0xE2]={0x38,0},                /* LAlt   */
    [0xE3]={0x5B,1},                /* LGUI   */
    [0xE4]={0x1D,1},                /* RCtrl  */
    [0xE5]={0x36,0},                /* RShift */
    [0xE6]={0x38,1},                /* RAlt   */
    [0xE7]={0x5C,1},                /* RGUI   */
};

/* Inject a make or break event for a single HID usage. */
static void inject_hid(uint8_t hid, int make) {
    hid_at_t a = hid_to_at[hid];
    if (a.code == 0 && a.e0 == 0) return;  /* unmapped */

    uint8_t seq[2];
    int n = 0;
    if (a.e0) seq[n++] = 0xE0;
    seq[n++] = make ? a.code : (uint8_t)(a.code | 0x80);
    keyboard_inject_scancode_sequence(seq, n);
}

/* Is `kc` present in `set[6]`? */
static int in_set(const uint8_t *set, uint8_t kc) {
    for (int i = 0; i < 6; i++) if (set[i] == kc) return 1;
    return 0;
}

/* ============================================================
 * Per-instance state
 * ============================================================ */
#define HID_KBD_REPORT_LEN   8

typedef struct hid_priv {
    usb_device_t   *dev;
    usb_interface_t *iface;
    usb_endpoint_t *ep_in;
    int             is_kbd;
    uint8_t         report_size;
    uint8_t        *buf;       /* DMA-allocated; sized to report_size */
    /* keyboard diff state */
    uint8_t         last_mods;
    uint8_t         last_kc[6];
} hid_priv_t;

/* Forward */
static int hid_submit(hid_priv_t *priv);

/* ============================================================
 * Completion callbacks (run from HCD's IRQ context)
 * ============================================================ */

static int hid_kbd_complete(usb_xfer_t *xfer, void *ctx, uint32_t flags) {
    (void)flags;  /* keyboard_inject_scancode_sequence is IRQ-safe */
    hid_priv_t *priv = (hid_priv_t *)ctx;
    if (xfer->status != USB_OK || xfer->actual < HID_KBD_REPORT_LEN)
        goto resubmit;

    const uint8_t *r = priv->buf;

    /* Phantom state — all six keycodes = ErrorRollOver (0x01). Drop. */
    if (r[2] == 0x01 && r[3] == 0x01 && r[4] == 0x01 &&
        r[5] == 0x01 && r[6] == 0x01 && r[7] == 0x01)
        goto resubmit;

    /* Modifier diff (byte 0 bitmap → HID usages 0xE0..0xE7). */
    uint8_t mod_now = r[0];
    uint8_t diff    = mod_now ^ priv->last_mods;
    for (int b = 0; b < 8; b++) {
        if (!(diff & (1u << b))) continue;
        inject_hid((uint8_t)(0xE0 + b), (mod_now & (1u << b)) ? 1 : 0);
    }
    priv->last_mods = mod_now;

    /* Keycode array diff — order is arbitrary per HID 1.11 Appendix C. */
    uint8_t now_kc[6] = { r[2], r[3], r[4], r[5], r[6], r[7] };
    for (int i = 0; i < 6; i++)
        if (now_kc[i] && !in_set(priv->last_kc, now_kc[i]))
            inject_hid(now_kc[i], 1);
    for (int i = 0; i < 6; i++)
        if (priv->last_kc[i] && !in_set(now_kc, priv->last_kc[i]))
            inject_hid(priv->last_kc[i], 0);
    memcpy(priv->last_kc, now_kc, 6);

resubmit:
    hid_submit(priv);
    return 0;
}

static int hid_mouse_complete(usb_xfer_t *xfer, void *ctx, uint32_t flags) {
    (void)flags;  /* mouse_inject is IRQ-safe */
    hid_priv_t *priv = (hid_priv_t *)ctx;
    if (xfer->status != USB_OK || xfer->actual < 3) goto resubmit;

    const uint8_t *r = priv->buf;
    uint8_t  btn   = r[0] & 0x07;
    int8_t   dx    = (int8_t)r[1];
    int8_t   dy    = (int8_t)r[2];
    int8_t   wheel = (xfer->actual >= 4) ? (int8_t)r[3] : 0;
    mouse_inject(btn, dx, dy, wheel);

resubmit:
    hid_submit(priv);
    return 0;
}

static int hid_submit(hid_priv_t *priv) {
    return usbcore_submit_xfer(priv->dev, priv->ep_in,
                               priv->buf, priv->report_size,
                               0,   /* IRQ-driven, no timeout */
                               priv->is_kbd ? hid_kbd_complete
                                            : hid_mouse_complete,
                               priv);
}

/* ============================================================
 * Class-driver match + probe + disconnect
 * ============================================================ */
static int hid_match(usb_interface_t *iface) {
    if (!iface) return 0;
    if (iface->desc.bInterfaceClass    != USB_CLASS_HID)    return 0;
    if (iface->desc.bInterfaceSubClass != HID_SUBCLASS_BOOT) return 0;
    uint8_t p = iface->desc.bInterfaceProtocol;
    return (p == HID_PROTOCOL_KBD || p == HID_PROTOCOL_MOUSE);
}

static int hid_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (!dev || !iface) return USB_EINVAL;
    int is_kbd = (iface->desc.bInterfaceProtocol == HID_PROTOCOL_KBD);

    /* Find first Interrupt IN endpoint. */
    usb_endpoint_t *ep_in = 0;
    for (uint8_t i = 0; i < iface->num_endpoints; i++) {
        usb_endpoint_t *e = &iface->endpoints[i];
        if (e->type == USB_EP_TYPE_INTERRUPT && (e->addr & USB_DIR_IN)) {
            ep_in = e;
            break;
        }
    }
    if (!ep_in) {
        serial_puts("hid: no Interrupt IN endpoint\n");
        return USB_ENODEV;
    }

    hid_priv_t *priv = (hid_priv_t *)kmalloc(sizeof(*priv));
    if (!priv) return USB_ENOMEM;
    memset(priv, 0, sizeof(*priv));
    priv->dev    = dev;
    priv->iface  = iface;
    priv->ep_in  = ep_in;
    priv->is_kbd = is_kbd;
    priv->report_size = is_kbd ? HID_KBD_REPORT_LEN
                               : (ep_in->max_packet ? ep_in->max_packet : 4);
    if (priv->report_size < (is_kbd ? 8 : 3)) {
        kfree(priv);
        return USB_EINVAL;
    }

    priv->buf = (uint8_t *)dma_alloc(priv->report_size, 16);
    if (!priv->buf) { kfree(priv); return USB_ENOMEM; }
    memset(priv->buf, 0, priv->report_size);

    /* SET_PROTOCOL(Boot) — devices default to Report Protocol after
     * reset (HID 1.11 §7.2.6, p.54). Failure isn't fatal — many low-cost
     * Boot devices return STALL on this and stay in Boot anyway. */
    usbcore_control_transfer(dev, HID_BMREQ_H2D_CLASS_IF, HID_REQ_SET_PROTOCOL,
                             HID_PROTO_BOOT,
                             iface->desc.bInterfaceNumber, 0, 0, 100);

    /* SET_IDLE(0) — only report on change. (HID 1.11 §7.2.4, p.53) */
    usbcore_control_transfer(dev, HID_BMREQ_H2D_CLASS_IF, HID_REQ_SET_IDLE,
                             0x0000,
                             iface->desc.bInterfaceNumber, 0, 0, 100);

    /* Open the Interrupt IN endpoint on the HCD. */
    int rc = usbcore_open_endpoint(dev, ep_in);
    if (rc < 0) {
        dma_free(priv->buf); kfree(priv);
        return rc;
    }

    iface->driver_priv = priv;
    serial_puts("hid: bound ");
    serial_puts(is_kbd ? "keyboard" : "mouse");
    serial_puts(" (vid="); serial_puthex(dev->vid);
    serial_puts(" pid=");  serial_puthex(dev->pid);
    serial_puts(" ep=");   serial_puthex(ep_in->addr);
    serial_puts(")\n");

    /* Post first IN transfer; subsequent ones are chained from the
     * completion callback. */
    hid_submit(priv);
    return USB_OK;
}

static void hid_disconnect(usb_device_t *dev, usb_interface_t *iface) {
    if (!iface || !iface->driver_priv) return;
    hid_priv_t *priv = (hid_priv_t *)iface->driver_priv;
    usbcore_close_endpoint(dev, priv->ep_in);
    if (priv->buf) dma_free(priv->buf);
    kfree(priv);
    iface->driver_priv = 0;
}

static usb_class_driver_t hid_driver = {
    .name       = "hid_boot",
    .match      = hid_match,
    .probe      = hid_probe,
    .disconnect = hid_disconnect,
};

/* ============================================================
 * Module init / exit
 * ============================================================ */
static int hid_module_init(void) {
    serial_puts("hid.kmd: registering Boot Protocol class driver\n");
    return usbcore_register_class_driver(&hid_driver);
}

static void hid_module_exit(void) {
    usbcore_unregister_class_driver(&hid_driver);
}

module_init(hid_module_init);
module_exit(hid_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("USB HID Boot Protocol (keyboard + mouse)");
MODULE_NAME("hid");
