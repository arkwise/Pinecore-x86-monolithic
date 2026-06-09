/* usbcore.h — USB core API, shared by usbcore.kmd and its consumers
 * (HCD modules like uhci.kmd, class drivers like hid.kmd / msc.kmd).
 *
 * All facts in this header are cited against the USB 2.0 specification
 * (April 2000) — see docs/research/refs/usb-2.0/ + docs/research/50-usb-
 * enumeration-walkthrough.md for the rationale per byte.
 *
 * License: GPL-2.0 (matches usbcore.kmd; consumers must be GPL-family to
 * resolve EXPORT_SYMBOL_GPL surface).
 */
#ifndef PINECORE_USBCORE_H
#define PINECORE_USBCORE_H

#include "types.h"

/* ============================================================
 * 1. Standard descriptor types (USB 2.0 §9.6, Table 9-5, p.251)
 * ============================================================ */
#define USB_DT_DEVICE                 1
#define USB_DT_CONFIG                 2
#define USB_DT_STRING                 3
#define USB_DT_INTERFACE              4
#define USB_DT_ENDPOINT               5
#define USB_DT_DEVICE_QUALIFIER       6
#define USB_DT_OTHER_SPEED_CONFIG     7
#define USB_DT_INTERFACE_POWER        8

/* Standard request codes (USB 2.0 Table 9-4, p.251) */
#define USB_REQ_GET_STATUS            0
#define USB_REQ_CLEAR_FEATURE         1
#define USB_REQ_SET_FEATURE           3
#define USB_REQ_SET_ADDRESS           5
#define USB_REQ_GET_DESCRIPTOR        6
#define USB_REQ_SET_DESCRIPTOR        7
#define USB_REQ_GET_CONFIGURATION     8
#define USB_REQ_SET_CONFIGURATION     9
#define USB_REQ_GET_INTERFACE        10
#define USB_REQ_SET_INTERFACE        11
#define USB_REQ_SYNCH_FRAME          12

/* Standard feature selectors (USB 2.0 Table 9-6, p.252) */
#define USB_FEAT_DEVICE_REMOTE_WAKEUP 1
#define USB_FEAT_ENDPOINT_HALT        0
#define USB_FEAT_TEST_MODE            2

/* Endpoint transfer types (encoded in bmAttributes[1:0], §9.6.6, p.270) */
#define USB_EP_TYPE_CONTROL           0
#define USB_EP_TYPE_ISOC              1
#define USB_EP_TYPE_BULK              2
#define USB_EP_TYPE_INTERRUPT         3

/* Endpoint direction bit (bEndpointAddress[7], §9.6.6, p.269) */
#define USB_DIR_OUT                   0x00
#define USB_DIR_IN                    0x80

/* Class codes — interface bInterfaceClass (USB-IF assigned) */
#define USB_CLASS_HID                 0x03
#define USB_CLASS_MSC                 0x08
#define USB_CLASS_HUB                 0x09

/* Bus speeds (USB 2.0 §4.2.1, p.13) */
typedef enum {
    USB_SPEED_LOW   = 1,  /* 1.5 Mb/s */
    USB_SPEED_FULL  = 2,  /* 12 Mb/s */
    USB_SPEED_HIGH  = 3,  /* 480 Mb/s */
} usb_speed_t;

/* Errno-style returns. Match the pcnet/posix scheme used elsewhere. */
#define USB_OK            0
#define USB_EIO          -5
#define USB_ENOMEM      -12
#define USB_EBUSY       -16
#define USB_EINVAL      -22
#define USB_ENODEV      -19
#define USB_ETIMEDOUT  -110
#define USB_ENOSYS      -38
#define USB_ESTALL      -42   /* endpoint STALL — recoverable via CLEAR_FEATURE
                                  + toggle reset. Distinct from generic USB_EIO
                                  (doc 55 §6, USB 2.0 §8.4.5). */

/* Flags passed to `usb_xfer_done_cb_t`. Set by the HCD at completion time so
 * the class driver knows the calling context. Required by xHCI/EHCI Event-Ring
 * processing where completion runs from the HCD's IRQ handler with no task
 * context available (doc 55 §3, §9 A3). */
#define USB_CB_IN_ISR  (1u << 0)

/* ============================================================
 * 2. Setup packet + standard descriptor byte layouts
 *    Every struct here is the **on-wire** layout — packed,
 *    little-endian (USB transports LE on the wire).
 * ============================================================ */

/* (USB 2.0 §9.3, Table 9-2, p.248) */
struct usb_setup_packet {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* (USB 2.0 §9.6.1, Table 9-8, p.262) */
struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/* (USB 2.0 §9.6.3, Table 9-10, p.265) */
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* (USB 2.0 §9.6.5, Table 9-12, p.268) */
struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* (USB 2.0 §9.6.6, Table 9-13, p.269) */
struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* ============================================================
 * 3. Runtime structures — usbcore-internal but exposed so HCD
 *    and class drivers can read them. (doc 50 §7.)
 * ============================================================ */

#define USB_MAX_ENDPOINTS_PER_IF      16
#define USB_MAX_INTERFACES_PER_CFG    16
#define USB_CLASS_DESC_BLOB_MAX       256
#define USB_MAX_DEVICES               16  /* flat per-bus device table */

struct usb_hcd;          /* forward */
struct usb_device;       /* forward */
struct usb_class_driver; /* forward */
struct usb_xfer;         /* forward */

typedef struct usb_endpoint {
    struct usb_endpoint_descriptor desc;
    uint8_t  addr;            /* raw bEndpointAddress */
    uint8_t  type;            /* USB_EP_TYPE_* */
    uint16_t max_packet;      /* wMaxPacketSize[10:0] */
    uint8_t  interval;        /* bInterval as reported */
    void    *hcd_priv;        /* HCD's QH/qTD/TRB chain handle */
} usb_endpoint_t;

typedef struct usb_interface {
    struct usb_interface_descriptor desc;
    usb_endpoint_t endpoints[USB_MAX_ENDPOINTS_PER_IF];
    uint8_t  num_endpoints;
    uint8_t  class_desc_blob[USB_CLASS_DESC_BLOB_MAX];
    uint16_t class_desc_len;
    struct usb_class_driver *driver;
    void    *driver_priv;
} usb_interface_t;

typedef struct usb_configuration {
    struct usb_config_descriptor desc;
    usb_interface_t interfaces[USB_MAX_INTERFACES_PER_CFG];
    uint8_t  num_interfaces;
} usb_configuration_t;

typedef struct usb_device {
    struct usb_hcd      *hcd;
    uint8_t              port;
    uint8_t              address;     /* 0..127; 0 = default address */
    usb_speed_t          speed;
    uint16_t             vid, pid, bcd_device;
    uint8_t              ep0_max_packet;
    void                *ep0_pipe;   /* HCD default-control-pipe handle */
    usb_configuration_t  configs[1]; /* v1: single config */
    uint8_t              num_configurations;
    uint8_t              current_config;
    uint8_t              in_use;
    struct usb_device   *next;
} usb_device_t;

/* The HCD ORs USB_CB_IN_ISR into `flags` when invoking the callback from its
 * IRQ handler. Class drivers that touch a scheduler / take a mutex must check
 * this. v1 sinks (keyboard_inject_scancode_sequence, mouse_inject) are
 * IRQ-safe so HID ignores `flags`. */
typedef int (*usb_xfer_done_cb_t)(struct usb_xfer *xfer, void *ctx,
                                  uint32_t flags);

typedef struct usb_xfer {
    struct usb_device   *dev;             /* owning device — set by usbcore */
    struct usb_endpoint *ep;              /* endpoint for non-control xfers */
    void                *pipe;            /* HCD pipe handle (ep->hcd_priv) */
    struct usb_setup_packet setup;        /* control only */
    void                *data;            /* DMA-mapped data buffer */
    uint32_t             len;             /* requested length */
    uint32_t             actual;          /* set by HCD on completion */
    uint32_t             dir_in;          /* 1 = D2H */
    uint32_t             timeout_ms;
    int                  status;          /* USB_OK or USB_E* */
    usb_xfer_done_cb_t   done;            /* async only */
    void                *ctx;
} usb_xfer_t;

/* HCD operations — uhci/ehci/xhci modules implement. (doc 50 §8.)
 *
 * Control-transfer split (doc 55 §9 A5):
 *   - UHCI implements only `submit_control` (bundled 3-TD chain).
 *   - EHCI/xHCI MAY implement `submit_setup` + `submit_data` + `submit_status`
 *     to expose per-stage submission. When all three are present,
 *     `usbcore_control_transfer` walks them in sequence. Otherwise it falls
 *     back to `submit_control`. Each stage op blocks until that stage
 *     completes (synchronous from the caller's view; the HCD may poll its
 *     own async machinery internally). Setup carries `xfer->setup` populated
 *     and `xfer->len = wLength`; Data uses `xfer->data` + `xfer->len` +
 *     `xfer->dir_in`; Status is opposite-direction zero-length.
 *
 * DMA address fields (pinecore-x86 is 32-bit, doc 55 §7):
 *   - All HCD-facing pointers go through `dma_alloc` and `dma_virt_to_phys`
 *     which return values in [0x200000, 0x240000) — all < 4 GB.
 *   - HCDs whose hardware registers expose 64-bit address fields
 *     (EHCI CTRLDSSEGMENT, xHCI 64-bit TRB/Context pointers) MUST explicitly
 *     zero the high dword. Don't rely on register-power-on reset for this. */
typedef struct usb_hcd_ops {
    int  (*submit_control)(struct usb_hcd *hcd, usb_xfer_t *xfer);
    int  (*submit_xfer)   (struct usb_hcd *hcd, usb_xfer_t *xfer);
    int  (*ep_open)       (struct usb_hcd *hcd, usb_device_t *dev,
                           usb_endpoint_t *ep);
    void (*ep_close)      (struct usb_hcd *hcd, usb_device_t *dev,
                           usb_endpoint_t *ep);
    int  (*port_reset)    (struct usb_hcd *hcd, uint8_t port);
    int  (*port_status)   (struct usb_hcd *hcd, uint8_t port, uint32_t *status);
    int  (*port_enable)   (struct usb_hcd *hcd, uint8_t port);
    int  (*set_address)   (struct usb_hcd *hcd, usb_device_t *dev, uint8_t addr);

    /* Optional — see comment above. NULL means "use submit_control". */
    int  (*submit_setup)  (struct usb_hcd *hcd, usb_xfer_t *xfer);
    int  (*submit_data)   (struct usb_hcd *hcd, usb_xfer_t *xfer);
    int  (*submit_status) (struct usb_hcd *hcd, usb_xfer_t *xfer);
} usb_hcd_ops_t;

typedef struct usb_hcd {
    const char    *name;        /* e.g. "uhci@0x3000" */
    void          *mmio_or_io;
    uint8_t        num_ports;
    usb_hcd_ops_t *ops;
    void          *priv;
} usb_hcd_t;

typedef struct usb_class_driver {
    const char *name;
    int  (*match)     (usb_interface_t *iface);
    int  (*probe)     (usb_device_t *dev, usb_interface_t *iface);
    void (*disconnect)(usb_device_t *dev, usb_interface_t *iface);
} usb_class_driver_t;

/* ============================================================
 * 4. usbcore API surface
 *    EXPORT_SYMBOL_GPL — consumers must declare GPL-family license.
 * ============================================================ */

/* HCD → usbcore */
int  usbcore_register_hcd      (usb_hcd_t *hcd);
int  usbcore_unregister_hcd    (usb_hcd_t *hcd);
int  usbcore_port_connect      (usb_hcd_t *hcd, uint8_t port, usb_speed_t spd);
int  usbcore_port_disconnect   (usb_hcd_t *hcd, uint8_t port);

/* class driver ↔ usbcore */
int  usbcore_register_class_driver   (usb_class_driver_t *drv);
int  usbcore_unregister_class_driver (usb_class_driver_t *drv);

/* Standard control transfer — synchronous, blocks until done or timeout.
 * Returns bytes transferred for IN, or 0 on success for OUT. <0 on error. */
int  usbcore_control_transfer(usb_device_t *dev,
                              uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              void *data, uint16_t wLength,
                              uint32_t timeout_ms);

/* Standard-request convenience wrappers */
int  usbcore_get_descriptor   (usb_device_t *dev, uint8_t type, uint8_t idx,
                               uint16_t langid, void *buf, uint16_t len);
int  usbcore_set_address      (usb_device_t *dev, uint8_t addr);
int  usbcore_set_configuration(usb_device_t *dev, uint8_t config);
int  usbcore_get_string       (usb_device_t *dev, uint8_t idx,
                               uint16_t langid, char *buf, uint32_t buflen);
int  usbcore_clear_halt       (usb_device_t *dev, usb_endpoint_t *ep);
int  usbcore_set_interface    (usb_device_t *dev, uint8_t iface, uint8_t alt);

/* Generic async transfer — bulk + interrupt. Provider returns immediately;
 * `done` callback fires from HCD completion path (typically IRQ ctx). */
int  usbcore_submit_xfer      (usb_device_t *dev, usb_endpoint_t *ep,
                               void *data, uint32_t len,
                               uint32_t timeout_ms,
                               usb_xfer_done_cb_t done, void *ctx);

/* Endpoint open / close — forwards to the HCD's ep_open/ep_close ops so
 * class drivers don't have to reach into dev->hcd. ep->hcd_priv is set
 * by the HCD on success. */
int  usbcore_open_endpoint    (usb_device_t *dev, usb_endpoint_t *ep);
void usbcore_close_endpoint   (usb_device_t *dev, usb_endpoint_t *ep);

/* Descriptor cache helpers */
usb_endpoint_t *usbcore_find_endpoint(usb_interface_t *iface, uint8_t addr);
int  usbcore_parse_config_descriptor(usb_device_t *dev,
                                     const uint8_t *buf, uint16_t total);

/* Diagnostic — list devices on the bus. Used by `lsusb`-style shell cmds. */
usb_device_t *usbcore_device_list(void);

#endif /* PINECORE_USBCORE_H */
