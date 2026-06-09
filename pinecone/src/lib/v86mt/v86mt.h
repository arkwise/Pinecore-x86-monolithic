/*
 * lib/v86mt/v86mt.h — V86MT vendor-API client wrapper (libv86mt).
 *
 * Spec: docs/design/V86MT-API.md.
 *
 * v1 surface (Phase 4.7 M1):
 *   - v86mt_probe()      — INT 31h AX=0x0A00 with signature "V86MT v1";
 *                          caches the returned entry-point.
 *   - v86mt_get_caps()   — sub-function 0x0000 through the cached entry.
 *
 * M2–M7 will add vt_alloc / vt_spawn / vt_kbd_inject / poll / focus /
 * resize / kill / free wrappers using the same cached entry-point.
 *
 * Host-agnostic: works against pinecore-x86 today and (eventually)
 * CWSDPMIX or any other DPMI host that advertises the V86MT vendor
 * signature.
 */
#ifndef PINECONE_LIB_V86MT_H
#define PINECONE_LIB_V86MT_H

#include <stdint.h>

/* Vendor-API signature per V86MT-API.md "Discovery" section. */
#define V86MT_VENDOR_SIGNATURE  "V86MT v1"

/* Sub-function numbers (passed in AX through the entry-point). */
#define VMT_GET_CAPS     0x0000
#define VMT_VT_ALLOC     0x0001
#define VMT_VT_SPAWN     0x0002
#define VMT_KBD_INJECT   0x0003
#define VMT_POLL         0x0004
#define VMT_FOCUS        0x0005
#define VMT_VT_RESIZE    0x0006
#define VMT_VT_KILL      0x0007
#define VMT_VT_FREE      0x0008

/* Error codes (returned in AX with CF=1). */
#define V86MT_E_NO_FREE_VT      0x0001
#define V86MT_E_BAD_DIMENSIONS  0x0002
#define V86MT_E_NO_LDT          0x0008
#define V86MT_E_BAD_HANDLE      0x0010
#define V86MT_E_TASK_LIVE       0x0011
#define V86MT_E_EXEC_FAILED     0x0012
#define V86MT_E_BAD_ARGV        0x0013
#define V86MT_E_BUSY            0x0014
#define V86MT_E_NO_TASK         0x0015
#define V86MT_E_RING_FULL       0x0020
#define V86MT_E_BAD_BUFFER      0x0030
#define DPMI_E_UNSUPPORTED      0x8001

/* Capability bits (returned by get_caps in EAX|EBX, low 32 in caps_lo). */
#define V86MT_CAP_TEXT_VT       (1u << 0)
#define V86MT_CAP_EXT_SCANCODES (1u << 1)
#define V86MT_CAP_RESIZE        (1u << 2)
#define V86MT_CAP_MULTI_VT      (1u << 3)

/* vt_state struct passed to vt_poll. Exactly 32 bytes, packed,
 * little-endian. Matches V86MT-API.md layout byte-for-byte. */
#pragma pack(push, 1)
struct v86mt_vt_state {
    uint16_t task_running;     /* +0  */
    uint16_t exit_code;        /* +2  */
    uint16_t exited;           /* +4  */
    uint16_t screen_dirty;     /* +6  */
    uint16_t cursor_x;         /* +8  */
    uint16_t cursor_y;         /* +10 */
    uint16_t cursor_visible;   /* +12 */
    uint16_t cols;             /* +14 */
    uint16_t rows;             /* +16 */
    uint16_t kbd_drops;        /* +18 */
    uint16_t reserved_a;       /* +20 */
    uint32_t ticks_consumed;   /* +22 */
    uint16_t reserved_b;       /* +26 */
    uint16_t reserved_c;       /* +28 */
    uint16_t api_version;      /* +30 */
};
#pragma pack(pop)

/* Probe the host for V86MT support. Returns 0 on success (entry-point
 * cached internally), -1 if the host doesn't advertise V86MT v1. */
int v86mt_probe(void);

/* Returns 1 iff v86mt_probe() previously succeeded. */
int v86mt_is_present(void);

/* Get host capabilities (caps_lo / max_vts / api_minor).
 * Returns 0 on success, -1 if not probed, error code otherwise. */
int v86mt_get_caps(uint16_t *caps_lo, uint16_t *caps_hi,
                   uint32_t *max_vts, uint16_t *api_minor);

/* Allocate a headless VT. cols/rows = 80/25 for v1.0 baseline.
 * On success: *handle = 1..max_vts; char_sel / attr_sel / kbd_sel are
 * RO/RO/RW LDT selectors (0 in M2, real in M3+). Any out-ptr may be NULL.
 * Returns 0 on success, -1 if not probed, error code otherwise. */
int v86mt_vt_alloc(uint8_t cols, uint8_t rows,
                   uint16_t *handle, uint16_t *char_sel,
                   uint16_t *attr_sel, uint16_t *kbd_sel);

/* Release a VT handle. Returns 0 on success, -1 if not probed,
 * error code otherwise. */
int v86mt_vt_free(uint16_t handle);

/* Launch a DOS program inside a VT. argv = packed string "name\0arg1\0…\0\0"
 * (filename + tail args, double-NUL terminates). env = packed environment
 * block in DOS format, or NULL to inherit the PM client's environment.
 * Returns 0 on success, -1 if not probed, error code otherwise. */
int v86mt_vt_spawn(uint16_t handle, const char *argv, const char *env);

/* Poll a VT's lifecycle + dirty counter. The host writes a 32-byte
 * struct v86mt_vt_state into *out. Returns 0 on success, -1 if not
 * probed, error code otherwise. */
int v86mt_poll(uint16_t handle, struct v86mt_vt_state *out);

#endif
