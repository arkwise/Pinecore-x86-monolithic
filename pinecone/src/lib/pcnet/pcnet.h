/* pcnet.h — DJGPP user-side BSD-sockets shim for Pinecore.
 *
 * Marshals into the kernel net-provider ABI via INT 0x80 (EBX = &frame).
 * Mirrors the surface declared in src/include/net.h. Apps include this
 * header and link against pcnet.c (today: unity-included; eventually:
 * libpcnet.a installed to the DJGPP lib/ dir as <pcnet.h> + -lpcnet).
 *
 * The kernel does NOT check whether the active provider supports an op
 * before dispatching — callers must consult net_active_provider->caps if
 * they care to avoid the round-trip cost.
 */
#ifndef PINECONE_PCNET_H
#define PINECONE_PCNET_H

#include <stdint.h>

/* ------------------------------------------------------------------
 * Sockets surface — values match src/include/net.h. Re-declared here
 * so user-side code can include this header standalone without
 * pulling the kernel header in.
 * ------------------------------------------------------------------ */

#define PF_INET        2
#define AF_INET        PF_INET
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define IPPROTO_IP     0
#define IPPROTO_TCP    6
#define IPPROTO_UDP   17

#define MSG_PEEK       0x0002
#define MSG_DONTWAIT   0x0040

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ERROR       4

#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

#define PCNET_OK            0
#define PCNET_EBADF        -9
#define PCNET_EAGAIN      -11
#define PCNET_ENOMEM      -12
#define PCNET_EFAULT      -14
#define PCNET_EINVAL      -22
#define PCNET_EMFILE      -24
#define PCNET_ENOSYS      -38
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
#define PCNET_ENOPROVIDER -200

struct pcnet_in_addr {
    uint32_t s_addr;       /* network order */
};

struct pcnet_sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
} __attribute__((packed));

struct pcnet_sockaddr_in {
    uint16_t              sin_family;     /* AF_INET */
    uint16_t              sin_port;       /* network order */
    struct pcnet_in_addr  sin_addr;
    uint8_t               sin_zero[8];
} __attribute__((packed));

typedef int32_t pcnet_socklen_t;
typedef int32_t pcnet_ssize_t;

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
 * Big-endian conversion helpers — DJGPP doesn't ship <arpa/inet.h>
 * out of the box. The macros are byte-swaps on i386 (LE host).
 * ------------------------------------------------------------------ */

static inline uint16_t pcnet_htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t pcnet_ntohs(uint16_t v) { return pcnet_htons(v); }

static inline uint32_t pcnet_htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
static inline uint32_t pcnet_ntohl(uint32_t v) { return pcnet_htonl(v); }

/* ------------------------------------------------------------------
 * BSD-sockets calls. Return values: 0 / byte count on success;
 * negative PCNET_E* on error.
 * ------------------------------------------------------------------ */

int  pcnet_socket(int domain, int type, int protocol);
int  pcnet_close(int fd);
int  pcnet_shutdown(int fd, int how);
int  pcnet_bind(int fd, const struct pcnet_sockaddr *addr, pcnet_socklen_t len);
int  pcnet_connect(int fd, const struct pcnet_sockaddr *addr, pcnet_socklen_t len);
int  pcnet_listen(int fd, int backlog);
int  pcnet_accept(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len);
int  pcnet_getsockname(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len);
int  pcnet_getpeername(int fd, struct pcnet_sockaddr *addr, pcnet_socklen_t *len);

pcnet_ssize_t pcnet_send(int fd, const void *buf, uint32_t len, int flags);
pcnet_ssize_t pcnet_recv(int fd, void *buf, uint32_t len, int flags);
pcnet_ssize_t pcnet_sendto(int fd, const void *buf, uint32_t len, int flags,
                           const struct pcnet_sockaddr *to, pcnet_socklen_t tolen);
pcnet_ssize_t pcnet_recvfrom(int fd, void *buf, uint32_t len, int flags,
                             struct pcnet_sockaddr *from, pcnet_socklen_t *fromlen);

int  pcnet_select(int nfds, pcnet_fd_set *rd, pcnet_fd_set *wr,
                  pcnet_fd_set *ex, struct pcnet_timeval *tv);

int  pcnet_setsockopt(int fd, int level, int optname,
                      const void *optval, pcnet_socklen_t optlen);
int  pcnet_getsockopt(int fd, int level, int optname,
                      void *optval, pcnet_socklen_t *optlen);

/* Kernel-side DNS resolver. *out gets the network-order IPv4 on success. */
int  pcnet_resolve(const char *hostname, struct pcnet_in_addr *out);

#endif
