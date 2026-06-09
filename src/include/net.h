/* Pinecore network-provider ABI.
 *
 * A "network provider" is a self-contained .kmd kernel module that drives
 * its hardware AND implements TCP/IP AND exposes BSD sockets system-wide.
 * The wifi module IS the network provider. The e1000e module IS the
 * network provider. Each provider author picks their own internal stack
 * implementation (roll minimal / port mTCP / port Watt-32 / embed lwIP /
 * write fresh) — the kernel only cares the vtable conforms.
 *
 * One active provider at a time (v1). Loading a provider while another
 * is registered fails — unload the current one first. Multi-NIC routing
 * is a future step once a second NIC appears.
 *
 * Apps in Ring-3 DPMI clients (or Ring-0 kernel callers) reach the
 * provider through a fixed syscall vector. The user-side shim
 * (libpcnet.a, eventually shipping in the DJGPP include dir as <pcnet.h>)
 * marshals into the syscall; the kernel routes to the active provider's
 * vtable.
 *
 * Boundary: kernel = GPL-2.0; provider-facing types + the registration
 * function (this header) = LGPL-2.1 so closed providers can link via
 * EXPORT_SYMBOL.
 *
 * Fd ownership: the kernel owns the user-facing socket fd namespace. The
 * provider returns an opaque cookie (pcnet_sock_t) from sock_create and
 * sees that cookie on every subsequent op. The kernel maintains the
 * {fd -> cookie} table, enforces the system-wide socket cap
 * (PCNET_FD_SETSIZE), and translates select()'s fd_set into per-cookie
 * poll calls before dispatching.
 *
 * DNS: the kernel ships its own resolver (net_resolve / NET_SYS_RESOLVE)
 * that issues DNS/UDP queries via the active provider's sockets. The
 * provider does NOT implement DNS itself. Nameserver list comes from
 * PCORE.CFG (net_dns_server).
 *
 * PCORE.CFG additions (parsed by config.c):
 *   net_provider = <name>        # provider .kmd to load at boot, e.g. e1000e
 *   net_dns_server = 1.1.1.1     # primary nameserver (one entry, v1)
 *   net_pktdrv_int = 0x60        # for PKTDRV.KMD only: INT vector of the TSR
 *   net_pktdrv_tsr = C:\NET\3C59X.COM
 *                                # for PKTDRV.KMD only: real-mode TSR to host in V86
 */
#ifndef PINECORE_NET_H
#define PINECORE_NET_H

#include "types.h"

/* ------------------------------------------------------------------
 * BSD-sockets surface — types shared by kernel, providers, and apps.
 * Values chosen to match BSD/POSIX where they're stable, so porting
 * code into a provider or onto the syscall shim is mechanical.
 * ------------------------------------------------------------------ */

/* Address families. v1 supports AF_INET only. */
#define PF_UNSPEC      0
#define PF_INET        2
#define AF_UNSPEC      PF_UNSPEC
#define AF_INET        PF_INET

/* Socket types. v1 supports SOCK_STREAM + SOCK_DGRAM. */
#define SOCK_STREAM    1   /* TCP */
#define SOCK_DGRAM     2   /* UDP */

/* IP protocols. */
#define IPPROTO_IP     0
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17

/* send/recv flags — v1 honors MSG_PEEK + MSG_DONTWAIT; others reserved. */
#define MSG_PEEK       0x0002
#define MSG_DONTWAIT   0x0040

/* setsockopt levels + names — v1 minimal. */
#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ERROR       4

/* shutdown() directions. */
#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

/* Errors — negative return codes from every syscall. Matches errno
 * values where they overlap so porting code reads naturally. */
#define PCNET_OK            0
#define PCNET_EBADF        -9
#define PCNET_EAGAIN      -11
#define PCNET_ENOMEM      -12
#define PCNET_EFAULT      -14
#define PCNET_EINVAL      -22
#define PCNET_EMFILE      -24
#define PCNET_ENOSYS      -38   /* op not implemented by current provider */
#define PCNET_ENOTSOCK    -88
#define PCNET_EOPNOTSUPP  -95
#define PCNET_EADDRINUSE  -98
#define PCNET_ENETDOWN   -100
#define PCNET_ECONNRESET -104
#define PCNET_EISCONN    -106
#define PCNET_ENOTCONN   -107
#define PCNET_ETIMEDOUT  -110
#define PCNET_ECONNREFUSED -111
#define PCNET_EHOSTUNREACH -113
#define PCNET_ENOPROVIDER -200  /* no provider registered */

/* Network-order IPv4 address. */
struct in_addr {
    uint32_t s_addr;          /* big-endian */
};

/* Generic sockaddr — providers must accept this and dispatch on family. */
struct sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
} __attribute__((packed));

/* IPv4 sockaddr. */
struct sockaddr_in {
    uint16_t        sin_family;   /* AF_INET */
    uint16_t        sin_port;     /* big-endian */
    struct in_addr  sin_addr;
    uint8_t         sin_zero[8];
} __attribute__((packed));

typedef int32_t pcnet_socklen_t;
typedef int32_t pcnet_ssize_t;

/* Opaque provider-side socket handle. The provider returns one from
 * sock_create and receives the same value on every subsequent op for
 * that socket. The kernel maps user-facing fd -> pcnet_sock_t in its
 * own table; providers never see the user-facing fd. */
typedef void *pcnet_sock_t;

/* select()/poll bits returned by provider's sock_poll. */
#define PCNET_POLL_RD   0x01
#define PCNET_POLL_WR   0x02
#define PCNET_POLL_EX   0x04

/* select() — bitmap of socket descriptors. v1 caps at 1024 sockets,
 * matches Linux's conventional FD_SETSIZE. 128 bytes per fd_set; 8 KB
 * total for the kernel's cookie table. Trivial at the P-MMX+ targets
 * we aim for, and puts the cap above any realistic DOS-era app's
 * needs (busy IRC bots, small web servers, MUDs). */
#define PCNET_FD_SETSIZE   1024
#define PCNET_FD_NWORDS    (PCNET_FD_SETSIZE / 64)
typedef struct {
    uint64_t mask[PCNET_FD_NWORDS];
} pcnet_fd_set;

#define PCNET_FD_ZERO(s) do {                                       \
        unsigned int _i;                                            \
        for (_i = 0; _i < PCNET_FD_NWORDS; _i++) (s)->mask[_i] = 0; \
    } while (0)
#define PCNET_FD_SET(fd,s)   ((s)->mask[(fd) >> 6] |=  (1ULL << ((fd) & 63)))
#define PCNET_FD_CLR(fd,s)   ((s)->mask[(fd) >> 6] &= ~(1ULL << ((fd) & 63)))
#define PCNET_FD_ISSET(fd,s) (((s)->mask[(fd) >> 6] >> ((fd) & 63)) & 1)

struct pcnet_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

/* ------------------------------------------------------------------
 * Provider metadata — each .kmd fills this in and registers.
 * Shown by `LOAD` and any provider-listing diagnostic. Single-slot
 * model: only one provider may be registered at a time in v1.
 * ------------------------------------------------------------------ */

enum net_provider_stability {
    NET_PROVIDER_STABLE       = 0,
    NET_PROVIDER_EXPERIMENTAL = 1,
    NET_PROVIDER_DEPRECATED   = 2
};

/* Capability flags — provider advertises what it actually supports.
 * Apps and the kernel can refuse operations the provider can't do
 * without round-tripping into the vtable. */
#define NET_CAP_TCP         0x00000001
#define NET_CAP_UDP         0x00000002
#define NET_CAP_DHCP        0x00000008   /* provider does DHCP itself */
#define NET_CAP_LISTEN      0x00000010   /* server-side sockets work */
#define NET_CAP_NONBLOCK    0x00000020   /* sock_poll + MSG_DONTWAIT work */
#define NET_CAP_HW_NATIVE   0x00010000   /* Ring-0 native NIC driver */
#define NET_CAP_HW_PKTDRV   0x00020000   /* hosts a V86 packet driver TSR */

/* ------------------------------------------------------------------
 * Provider vtable — BSD-sockets-shaped. Each .kmd implements these.
 *
 * The provider operates on its own opaque socket cookie (pcnet_sock_t).
 * Kernel-side state ({fd -> cookie} table, the global 64-socket cap,
 * fd_set translation, DNS) is all handled in net.c — providers never
 * see user-facing fds.
 *
 * Return values: 0 / byte count on success, negative PCNET_* on
 * failure. Providers MUST NOT return -1 with a separate errno; the
 * negative return IS the error. sock_create returns the cookie via
 * out-param so any pointer value (including NULL) is legal.
 * ------------------------------------------------------------------ */
struct net_provider_ops {
    /* Lifecycle — kernel calls start/stop around provider activation.
     * start() may bring up the link, allocate buffers, spawn helper
     * V86 tasks (PKTDRV.KMD case), etc. Return 0 on success. */
    int  (*start)(void);
    void (*stop)(void);

    /* Socket lifecycle. On success *out holds the provider's cookie. */
    int  (*sock_create)(int domain, int type, int protocol,
                        pcnet_sock_t *out);
    int  (*sock_close)(pcnet_sock_t s);
    int  (*sock_shutdown)(pcnet_sock_t s, int how);

    /* Naming. */
    int  (*sock_bind)(pcnet_sock_t s, const struct sockaddr *addr,
                      pcnet_socklen_t addrlen);
    int  (*sock_connect)(pcnet_sock_t s, const struct sockaddr *addr,
                         pcnet_socklen_t addrlen);
    int  (*sock_listen)(pcnet_sock_t s, int backlog);
    int  (*sock_accept)(pcnet_sock_t s, pcnet_sock_t *new_sock,
                        struct sockaddr *addr, pcnet_socklen_t *addrlen);
    int  (*sock_getsockname)(pcnet_sock_t s, struct sockaddr *addr,
                             pcnet_socklen_t *addrlen);
    int  (*sock_getpeername)(pcnet_sock_t s, struct sockaddr *addr,
                             pcnet_socklen_t *addrlen);

    /* Data path. */
    pcnet_ssize_t (*sock_send)(pcnet_sock_t s, const void *buf,
                               uint32_t len, int flags);
    pcnet_ssize_t (*sock_recv)(pcnet_sock_t s, void *buf, uint32_t len,
                               int flags);
    pcnet_ssize_t (*sock_sendto)(pcnet_sock_t s, const void *buf,
                                 uint32_t len, int flags,
                                 const struct sockaddr *to,
                                 pcnet_socklen_t tolen);
    pcnet_ssize_t (*sock_recvfrom)(pcnet_sock_t s, void *buf, uint32_t len,
                                   int flags, struct sockaddr *from,
                                   pcnet_socklen_t *fromlen);

    /* Non-blocking readiness — kernel implements select() by iterating
     * the fd_set and calling sock_poll per cookie. Returns a
     * PCNET_POLL_* bitmask, or negative PCNET_* on error. */
    int  (*sock_poll)(pcnet_sock_t s);

    /* Options. */
    int  (*sock_setsockopt)(pcnet_sock_t s, int level, int optname,
                            const void *optval, pcnet_socklen_t optlen);
    int  (*sock_getsockopt)(pcnet_sock_t s, int level, int optname,
                            void *optval, pcnet_socklen_t *optlen);
};

/* ------------------------------------------------------------------
 * Provider descriptor — the one struct each .kmd declares + passes to
 * net_register_provider() during module_init.
 * ------------------------------------------------------------------ */
struct net_provider {
    /* Display + identification — shown by `LOAD`. */
    const char *name;             /* short name, e.g. "e1000e" */
    const char *description;      /* one-line human-readable */
    uint16_t    version_major;
    uint16_t    version_minor;
    uint16_t    version_patch;
    uint16_t    abi_version;      /* must equal NET_PROVIDER_ABI_VERSION */
    enum net_provider_stability stability;
    uint32_t    caps;             /* NET_CAP_* bitmap */

    const struct net_provider_ops *ops;
};

/* Bumped any time the vtable layout or enum values change in a way
 * that breaks compiled providers. v1 starts at 1. */
#define NET_PROVIDER_ABI_VERSION  1

/* ------------------------------------------------------------------
 * Kernel-exported API — providers call these. Both are EXPORT_SYMBOL
 * (LGPL-friendly) so closed-source providers can link.
 * ------------------------------------------------------------------ */

/* Register this .kmd as THE active network provider. Called from the
 * provider's module_init. Returns 0 on success, negative PCNET_* on
 * failure. Fails with PCNET_EADDRINUSE if another provider is already
 * registered (single-slot v1). On success the kernel calls ops->start()
 * before returning. */
int  net_register_provider(const struct net_provider *p);

/* Unregister and tear down. Called from the provider's module_exit
 * (also called automatically by the kernel if the .kmd is unloaded
 * without doing so). Kernel calls ops->stop() before clearing the
 * slot. */
void net_unregister_provider(const struct net_provider *p);

/* Query — for diagnostics, `lsmod`-style display, the load banner. */
const struct net_provider *net_active_provider(void);

/* Kernel DNS resolver — sits above the provider, uses the active
 * provider's UDP socket. Reads nameserver from PCORE.CFG net_dns_server.
 * Returns 0 + IPv4 (network order) via *out on success; negative
 * PCNET_* on failure (ETIMEDOUT, ENOPROVIDER, EHOSTUNREACH). Backs
 * NET_SYS_RESOLVE; apps can also call directly from kernel-side code. */
int  net_resolve(const char *hostname, struct in_addr *out);

/* ------------------------------------------------------------------
 * Kernel-internal — boot wiring + the syscall dispatch path. NOT
 * exported to providers. The user-side shim issues the syscall vector;
 * the kernel ISR converges here.
 * ------------------------------------------------------------------ */

/* Initialize the net subsystem: clear active-provider slot, install the
 * syscall vector. Call once at boot before any module load and before
 * config_init() loads a provider per PCORE.CFG. */
void net_init(void);

/* Internal dispatch — invoked by the syscall ISR. Routes a single
 * sockets call into the active provider's vtable, or returns
 * PCNET_ENOPROVIDER. Exposed for the ISR; not part of the provider
 * ABI. */
struct net_syscall_frame {
    uint32_t op;       /* NET_SYS_* */
    uint32_t a0, a1, a2, a3, a4, a5;
    int32_t  ret;
};

/* Syscall op numbers — stable, append-only. */
#define NET_SYS_SOCKET       1
#define NET_SYS_CLOSE        2
#define NET_SYS_SHUTDOWN     3
#define NET_SYS_BIND         4
#define NET_SYS_CONNECT      5
#define NET_SYS_LISTEN       6
#define NET_SYS_ACCEPT       7
#define NET_SYS_GETSOCKNAME  8
#define NET_SYS_GETPEERNAME  9
#define NET_SYS_SEND        10
#define NET_SYS_RECV        11
#define NET_SYS_SENDTO      12
#define NET_SYS_RECVFROM    13
#define NET_SYS_SELECT      14
#define NET_SYS_SETSOCKOPT  15
#define NET_SYS_GETSOCKOPT  16
#define NET_SYS_RESOLVE     17

/* `ds_base` is the linear-address base of the caller's data segment.
 * The kernel adds it to any pointer-typed arg in `frame` before passing
 * to the provider — clients pass offsets in DS, not linear addresses.
 * Kernel-internal callers pass ds_base = 0 (identity translation). */
void net_dispatch(struct net_syscall_frame *frame, uint32_t ds_base);

#endif
