/* pcnet.c — DJGPP user-side shim for the Pinecore net-provider ABI.
 *
 * Each call packs its arguments into a struct net_syscall_frame on the
 * caller's stack, loads EBX with the frame address, and triggers
 * INT 0x80 — the kernel ISR (net_dispatch in src/kernel/net.c) routes
 * to the active provider's vtable and writes the result into frame.ret.
 *
 * Convention is fixed in src/include/net.h: op number in frame.op,
 * args left-to-right in a0..a5, signed int32_t return in frame.ret.
 *
 * Compiled into libpcnet.a (see Makefile in this directory). Installs
 * to $(DJGPP)/lib/libpcnet.a alongside the header at $(DJGPP)/include/pcnet.h
 * so any DJGPP app can `#include <pcnet.h>` + `-lpcnet`.
 */

#include "pcnet.h"

/* Op numbers — mirror src/include/net.h NET_SYS_*. */
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

struct pcnet_frame {
    uint32_t op;
    uint32_t a0, a1, a2, a3, a4, a5;
    int32_t  ret;
};

/* The INT 0x80 gate is DPL=3 so a PM Ring-3 caller can invoke it
 * directly. We pass EBX = &frame; everything else is in the frame.
 * No registers besides memory are clobbered on the kernel side beyond
 * what `int $imm8` already does (flags, etc.). */
static inline int32_t pcnet_call(struct pcnet_frame *f) {
    __asm__ __volatile__("int $0x80" : : "b"(f) : "memory");
    return f->ret;
}

/* Small helper to keep each wrapper terse. */
#define PCNET_PTR(p)  ((uint32_t)(unsigned long)(p))

int pcnet_socket(int domain, int type, int protocol) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SOCKET;
    f.a0 = (uint32_t)domain;
    f.a1 = (uint32_t)type;
    f.a2 = (uint32_t)protocol;
    return pcnet_call(&f);
}

int pcnet_close(int fd) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_CLOSE;
    f.a0 = (uint32_t)fd;
    return pcnet_call(&f);
}

int pcnet_shutdown(int fd, int how) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SHUTDOWN;
    f.a0 = (uint32_t)fd;
    f.a1 = (uint32_t)how;
    return pcnet_call(&f);
}

int pcnet_bind(int fd, const struct pcnet_sockaddr *addr, pcnet_socklen_t len) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_BIND;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(addr);
    f.a2 = (uint32_t)len;
    return pcnet_call(&f);
}

int pcnet_connect(int fd, const struct pcnet_sockaddr *addr, pcnet_socklen_t len) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_CONNECT;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(addr);
    f.a2 = (uint32_t)len;
    return pcnet_call(&f);
}

int pcnet_listen(int fd, int backlog) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_LISTEN;
    f.a0 = (uint32_t)fd;
    f.a1 = (uint32_t)backlog;
    return pcnet_call(&f);
}

int pcnet_accept(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_ACCEPT;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(addr);
    f.a2 = PCNET_PTR(len);
    return pcnet_call(&f);
}

int pcnet_getsockname(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_GETSOCKNAME;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(addr);
    f.a2 = PCNET_PTR(len);
    return pcnet_call(&f);
}

int pcnet_getpeername(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_GETPEERNAME;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(addr);
    f.a2 = PCNET_PTR(len);
    return pcnet_call(&f);
}

pcnet_ssize_t pcnet_send(int fd, const void *buf, uint32_t len, int flags) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SEND;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(buf);
    f.a2 = len;
    f.a3 = (uint32_t)flags;
    return pcnet_call(&f);
}

pcnet_ssize_t pcnet_recv(int fd, void *buf, uint32_t len, int flags) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_RECV;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(buf);
    f.a2 = len;
    f.a3 = (uint32_t)flags;
    return pcnet_call(&f);
}

pcnet_ssize_t pcnet_sendto(int fd, const void *buf, uint32_t len, int flags,
                           const struct pcnet_sockaddr *to, pcnet_socklen_t tolen)
{
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SENDTO;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(buf);
    f.a2 = len;
    f.a3 = (uint32_t)flags;
    f.a4 = PCNET_PTR(to);
    f.a5 = (uint32_t)tolen;
    return pcnet_call(&f);
}

pcnet_ssize_t pcnet_recvfrom(int fd, void *buf, uint32_t len, int flags,
                             struct pcnet_sockaddr *from, pcnet_socklen_t *fromlen)
{
    struct pcnet_frame f = {0};
    f.op = NET_SYS_RECVFROM;
    f.a0 = (uint32_t)fd;
    f.a1 = PCNET_PTR(buf);
    f.a2 = len;
    f.a3 = (uint32_t)flags;
    f.a4 = PCNET_PTR(from);
    f.a5 = PCNET_PTR(fromlen);
    return pcnet_call(&f);
}

int pcnet_select(int nfds, pcnet_fd_set *rd, pcnet_fd_set *wr,
                 pcnet_fd_set *ex, struct pcnet_timeval *tv)
{
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SELECT;
    f.a0 = (uint32_t)nfds;
    f.a1 = PCNET_PTR(rd);
    f.a2 = PCNET_PTR(wr);
    f.a3 = PCNET_PTR(ex);
    f.a4 = PCNET_PTR(tv);
    return pcnet_call(&f);
}

int pcnet_setsockopt(int fd, int level, int optname,
                     const void *optval, pcnet_socklen_t optlen)
{
    struct pcnet_frame f = {0};
    f.op = NET_SYS_SETSOCKOPT;
    f.a0 = (uint32_t)fd;
    f.a1 = (uint32_t)level;
    f.a2 = (uint32_t)optname;
    f.a3 = PCNET_PTR(optval);
    f.a4 = (uint32_t)optlen;
    return pcnet_call(&f);
}

int pcnet_getsockopt(int fd, int level, int optname,
                     void *optval, pcnet_socklen_t *optlen)
{
    struct pcnet_frame f = {0};
    f.op = NET_SYS_GETSOCKOPT;
    f.a0 = (uint32_t)fd;
    f.a1 = (uint32_t)level;
    f.a2 = (uint32_t)optname;
    f.a3 = PCNET_PTR(optval);
    f.a4 = PCNET_PTR(optlen);
    return pcnet_call(&f);
}

int pcnet_resolve(const char *hostname, struct pcnet_in_addr *out) {
    struct pcnet_frame f = {0};
    f.op = NET_SYS_RESOLVE;
    f.a0 = PCNET_PTR(hostname);
    f.a1 = PCNET_PTR(out);
    return pcnet_call(&f);
}
