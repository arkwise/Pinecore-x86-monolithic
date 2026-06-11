/*
 * lib/v86mt/v86mt.c — V86MT vendor-API client wrapper implementation.
 *
 * Unity-included from src/main.c after pine_trace_*() is defined, so the
 * probe can log to COM1 alongside kernel-side diagnostics.
 */

#include "lib/v86mt/v86mt.h"
#include <dpmi.h>

static __dpmi_paddr g_v86mt_entry;
static int          g_v86mt_present = 0;

int v86mt_is_present(void) { return g_v86mt_present; }

int v86mt_probe(void)
{
    char sig[] = V86MT_VENDOR_SIGNATURE;   /* writable copy in DS */
    __dpmi_paddr api;

    pine_trace("V86MT: probe INT 31h AX=0x0A00 sig=\""); pine_trace(sig); pine_trace("\"\n");

    int r = __dpmi_get_vendor_specific_api_entry_point(sig, &api);
    if (r != 0) {
        pine_trace("V86MT: probe CF=1 (host not present)\n");
        g_v86mt_present = 0;
        return -1;
    }

    g_v86mt_entry  = api;
    g_v86mt_present = 1;
    pine_trace("V86MT: entry sel=");
    pine_trace_hex(api.selector);
    pine_trace(" off=");
    pine_trace_hex(api.offset32);
    pine_trace("\n");
    return 0;
}

int v86mt_get_caps(uint16_t *caps_lo, uint16_t *caps_hi,
                   uint32_t *max_vts, uint16_t *api_minor)
{
    if (!g_v86mt_present) return -1;

    uint32_t eax_in = VMT_GET_CAPS;
    uint32_t eax_out, ebx_out, ecx_out, edx_out;
    int cf;

    /* Far call to the host's V86MT entry-point. The stub at linear 0x530
     * (see kernel/dpmi.c dpmi_init_client) sets CF on error / clears on
     * success, returns AX/BX/ECX/DX per V86MT-API.md Discovery section.
     * `sbbl %%edi, %%edi` after the lcall captures CF as 0 / -1. */
    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%edi, %%edi"
        : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx_out),
          "=D"(cf)
        : "a"(eax_in), [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );

    if (cf) {
        pine_trace("V86MT: get_caps CF=1 err=");
        pine_trace_hex(eax_out & 0xFFFF);
        pine_trace("\n");
        return (int)(eax_out & 0xFFFF);
    }
    if (caps_lo)   *caps_lo   = (uint16_t)(eax_out & 0xFFFF);
    if (caps_hi)   *caps_hi   = (uint16_t)(ebx_out & 0xFFFF);
    if (max_vts)   *max_vts   = ecx_out;
    if (api_minor) *api_minor = (uint16_t)(edx_out & 0xFFFF);
    pine_trace("V86MT: caps=");
    pine_trace_hex(eax_out & 0xFFFF);
    pine_trace(" max_vts=");
    pine_trace_hex(ecx_out);
    pine_trace(" minor=");
    pine_trace_hex(edx_out & 0xFFFF);
    pine_trace("\n");
    return 0;
}

int v86mt_vt_alloc(uint8_t cols, uint8_t rows,
                   uint16_t *handle, uint16_t *char_sel,
                   uint16_t *attr_sel, uint16_t *kbd_sel)
{
    if (!g_v86mt_present) return -1;

    uint32_t eax_in = VMT_VT_ALLOC;
    uint32_t ebx_in = ((uint32_t)rows << 8) | cols;
    uint32_t eax_out, ebx_out, ecx_out, edx_out;
    int cf;

    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%edi, %%edi"
        : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx_out),
          "=D"(cf)
        : "a"(eax_in), "b"(ebx_in), [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );

    if (cf) {
        pine_trace("V86MT: vt_alloc CF=1 err=");
        pine_trace_hex(eax_out & 0xFFFF);
        pine_trace("\n");
        return (int)(eax_out & 0xFFFF);
    }
    if (handle)   *handle   = (uint16_t)(eax_out & 0xFFFF);
    if (char_sel) *char_sel = (uint16_t)(ebx_out & 0xFFFF);
    if (attr_sel) *attr_sel = (uint16_t)(ecx_out & 0xFFFF);
    if (kbd_sel)  *kbd_sel  = (uint16_t)(edx_out & 0xFFFF);
    pine_trace("V86MT: vt_alloc handle=");
    pine_trace_hex(eax_out & 0xFFFF);
    pine_trace(" char_sel=");
    pine_trace_hex(ebx_out & 0xFFFF);
    pine_trace(" attr_sel=");
    pine_trace_hex(ecx_out & 0xFFFF);
    pine_trace(" kbd_sel=");
    pine_trace_hex(edx_out & 0xFFFF);
    pine_trace("\n");
    return 0;
}

int v86mt_poll(uint16_t handle, struct v86mt_vt_state *out)
{
    if (!g_v86mt_present || !out) return -1;

    /* Host writes the 32-byte vt_state into ES:EDI per spec. In DJGPP's
     * flat-ish PM model ES = DS = the program's data segment, so we pass
     * the struct's flat address as EDI and the host's selector lookup
     * resolves to the same linear address. */
    uint32_t eax_in = VMT_POLL;
    uint32_t ebx_in = handle;
    uint32_t eax_out;
    int cf;

    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%ecx, %%ecx"
        : "=a"(eax_out), "=c"(cf)
        : "a"(eax_in), "b"(ebx_in), "D"((uint32_t)out),
          [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );

    if (cf) return (int)(eax_out & 0xFFFF);
    return 0;
}

int v86mt_vt_spawn(uint16_t handle, const char *argv, const char *env)
{
    if (!g_v86mt_present) return -1;

    /* Per V86MT-API.md: DS:ESI = argv, ES:EDI = env (or NULL=inherit).
     * Our DS = the COFF data segment; argv & env strings already live there
     * (caller passed pointers into our own data). Load ESI/EDI from those
     * pointers. ES = DS in DJGPP's flat-ish PM model, so the kernel will
     * read env via ES:EDI just fine. */
    uint32_t eax_in = VMT_VT_SPAWN;
    uint32_t ebx_in = handle;
    uint32_t esi_in = (uint32_t)argv;
    uint32_t edi_in = (uint32_t)env;       /* may be 0 = inherit */
    uint32_t eax_out;
    int cf;

    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%ecx, %%ecx"
        : "=a"(eax_out), "=c"(cf)
        : "a"(eax_in), "b"(ebx_in), "S"(esi_in), "D"(edi_in),
          [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );

    if (cf) {
        pine_trace("V86MT: vt_spawn CF=1 err=");
        pine_trace_hex(eax_out & 0xFFFF);
        pine_trace("\n");
        return (int)(eax_out & 0xFFFF);
    }
    pine_trace("V86MT: vt_spawn OK handle=");
    pine_trace_hex(handle);
    pine_trace("\n");
    return 0;
}

int v86mt_kbd_inject(uint16_t handle, uint8_t scancode, uint8_t ascii)
{
    if (!g_v86mt_present) return -1;

    uint32_t eax_in = VMT_KBD_INJECT;
    uint32_t ebx_in = handle;
    uint32_t ecx_in = ((uint32_t)scancode << 8) | ascii;
    uint32_t eax_out;
    int cf;

    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%edi, %%edi"
        : "=a"(eax_out), "=D"(cf)
        : "a"(eax_in), "b"(ebx_in), "c"(ecx_in),
          [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );

    if (cf) return (int)(eax_out & 0xFFFF);
    return 0;
}

int v86mt_vt_free(uint16_t handle)
{
    if (!g_v86mt_present) return -1;

    uint32_t eax_in = VMT_VT_FREE;
    uint32_t ebx_in = handle;
    uint32_t eax_out, ebx_out, ecx_out, edx_out;
    int cf;

    asm volatile (
        "lcalll *%[entry]\n\t"
        "sbbl   %%edi, %%edi"
        : "=a"(eax_out), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx_out),
          "=D"(cf)
        : "a"(eax_in), "b"(ebx_in), [entry] "m"(g_v86mt_entry)
        : "memory", "cc"
    );
    (void)ebx_out; (void)ecx_out; (void)edx_out;

    if (cf) {
        pine_trace("V86MT: vt_free CF=1 err=");
        pine_trace_hex(eax_out & 0xFFFF);
        pine_trace("\n");
        return (int)(eax_out & 0xFFFF);
    }
    pine_trace("V86MT: vt_free OK handle=");
    pine_trace_hex(handle);
    pine_trace("\n");
    return 0;
}
