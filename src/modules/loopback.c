/* loopback.kmd — software-only Pinecore network provider.
 *
 * Implements UDP and TCP socket semantics in-process — no NIC, no
 * link layer, no actual IP packets. Three purposes:
 *
 *   1. Validates the full provider data path (sock_create / bind /
 *      sendto / recvfrom / poll / close) in QEMU without hardware.
 *      Sat above NULL.KMD in the M3 progression.
 *
 *   2. Synthesizes DNS A-record responses to any UDP query sent to
 *      port 53 — letting kernel-side net_resolve() be exercised
 *      end-to-end (build query → wire-encode → "send" → response
 *      "received" → parse → return IPv4) without an upstream resolver.
 *      The canned answer is 127.0.0.1 (0x7F000001).
 *
 *   3. Full in-process TCP loopback (s53.net.c). listen / connect pair
 *      two sockets in the same socket table; send writes into peer's
 *      RX ring; recv reads own ring; close half-shuts the peer. Exists
 *      to exercise select() multi-fd, accept(), and stream semantics
 *      before any real-NIC provider has to honour the same contract.
 *
 * UDP: single pending datagram per socket — no real queue. TCP: 2 KB
 * byte ring per established socket, 4-deep listener backlog. All
 * non-blocking (NET_CAP_NONBLOCK); blocking flow handled by select().
 *
 * Load via PCORE.CFG:  net_provider = loopback
 *
 * License: GPL-2.0
 */

#include "module.h"
#include "net.h"
#include "types.h"

extern void serial_puts(const char *s);
extern void serial_puthex(uint32_t v);
extern void *memset(void *d, int c, uint32_t n);
extern void *memcpy(void *d, const void *s, uint32_t n);
extern int  net_register_provider(const struct net_provider *p);
extern void net_unregister_provider(const struct net_provider *p);

/* ------------------------------------------------------------------
 * Socket table
 * ------------------------------------------------------------------ */

#define LO_MAX_SOCKETS   16
#define LO_BUF_SIZE     512    /* max UDP DNS without EDNS0 */
#define LO_TCP_RING     2048   /* per-stream RX byte ring */
#define LO_BACKLOG      4      /* per-listener pending-accept slots */

enum lo_state {
    LO_IDLE = 0,           /* allocated, not bound/listening/connected */
    LO_LISTENING,          /* TCP, bound + listen() called */
    LO_ESTABLISHED,        /* TCP, paired with peer */
    LO_PEER_GONE,          /* TCP, peer closed; reads drain then EOF */
};

struct lo_sock {
    int                     in_use;
    int                     type;           /* SOCK_STREAM or SOCK_DGRAM */
    enum lo_state           state;
    struct sockaddr_in      bound;          /* zeroed = unbound */
    int                     has_bound;

    /* UDP path: single pending RX datagram. */
    uint8_t                 rx_buf[LO_BUF_SIZE];
    uint32_t                rx_len;         /* 0 = empty */
    struct sockaddr_in      rx_from;

    /* TCP path: byte ring + peer link. */
    uint8_t                 tcp_rx[LO_TCP_RING];
    uint32_t                tcp_head;       /* next-read offset */
    uint32_t                tcp_count;      /* bytes available */
    struct lo_sock         *peer;           /* paired ESTABLISHED socket */

    /* Listener-only: ring of pending accepted-but-not-yet-claimed peers. */
    struct lo_sock         *backlog[LO_BACKLOG];
    int                     backlog_head, backlog_count;
};

static struct lo_sock g_lo[LO_MAX_SOCKETS];

/* Canned A-record reply value for DNS synthesis: 127.0.0.1. */
#define LO_DNS_ANSWER_NBO  ((uint32_t)0x0100007FUL)
#define DNS_HDR_LEN        12

static inline uint16_t rbe16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline void     wbe16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void     wbe32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

/* ------------------------------------------------------------------
 * Provider lifecycle
 * ------------------------------------------------------------------ */

static int loopback_start(void) {
    int i;
    for (i = 0; i < LO_MAX_SOCKETS; i++) g_lo[i].in_use = 0;
    serial_puts("loopback: started (16 sockets, UDP+DNS+TCP loopback)\n");
    return 0;
}

static void loopback_stop(void) {
    int i;
    for (i = 0; i < LO_MAX_SOCKETS; i++) g_lo[i].in_use = 0;
    serial_puts("loopback: stopped\n");
}

/* ------------------------------------------------------------------
 * Socket allocation
 * ------------------------------------------------------------------ */

static int lo_create(int domain, int type, int proto, pcnet_sock_t *out) {
    int i;
    (void)proto;
    if (domain != AF_INET) return PCNET_EINVAL;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return PCNET_EINVAL;
    for (i = 0; i < LO_MAX_SOCKETS; i++) {
        if (!g_lo[i].in_use) {
            memset(&g_lo[i], 0, sizeof(g_lo[i]));
            g_lo[i].in_use = 1;
            g_lo[i].type   = type;
            g_lo[i].state  = LO_IDLE;
            *out = &g_lo[i];
            return PCNET_OK;
        }
    }
    return PCNET_EMFILE;
}

/* Tear the TCP pairing down without freeing either side — used by both
 * close and stop. After this returns, `sk`'s peer (if any) is in
 * LO_PEER_GONE so its readers can drain remaining bytes then see EOF. */
static void lo_tcp_disconnect(struct lo_sock *sk) {
    struct lo_sock *p = sk->peer;
    if (!p) return;
    if (p->in_use && p->state == LO_ESTABLISHED) {
        p->state = LO_PEER_GONE;
        p->peer  = 0;
    }
    sk->peer = 0;
}

static int lo_close(pcnet_sock_t s) {
    struct lo_sock *sk = (struct lo_sock *)s;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type == SOCK_STREAM) {
        if (sk->state == LO_ESTABLISHED) lo_tcp_disconnect(sk);
        /* A listener with pending accept entries: their server-side sockets
         * stay allocated (still in_use=1) until the caller claims or closes
         * them. Just clear the listener's backlog so we don't leak refs. */
    }
    sk->in_use = 0;
    return PCNET_OK;
}

static int lo_shutdown(pcnet_sock_t s, int how) {
    struct lo_sock *sk = (struct lo_sock *)s;
    (void)how;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type == SOCK_STREAM && sk->state == LO_ESTABLISHED) {
        lo_tcp_disconnect(sk);
        sk->state = LO_PEER_GONE;        /* further recvs drain then EOF */
    } else {
        sk->rx_len = 0;                  /* UDP: drop the pending datagram */
    }
    return PCNET_OK;
}

static int lo_bind(pcnet_sock_t s, const struct sockaddr *a, pcnet_socklen_t l) {
    struct lo_sock *sk = (struct lo_sock *)s;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (!a || (uint32_t)l < sizeof(struct sockaddr_in)) return PCNET_EINVAL;
    memcpy(&sk->bound, a, sizeof(struct sockaddr_in));
    sk->has_bound = 1;
    return PCNET_OK;
}

/* ------------------------------------------------------------------
 * TCP listen / accept / connect
 *
 * Loopback pairing model: connect() walks the socket table for a
 * LISTENING socket whose bound port matches the target's. If addr is
 * INADDR_ANY (0) on either side, the addr check is skipped. A free
 * socket is taken as the server-side endpoint, both are linked as peers
 * in LO_ESTABLISHED, and the server-side is queued on the listener's
 * backlog. accept() pops one.
 * ------------------------------------------------------------------ */

static struct lo_sock *lo_find_listener(const struct sockaddr_in *target) {
    int i;
    uint16_t tport = target->sin_port;          /* network-order */
    uint32_t tip   = target->sin_addr.s_addr;   /* network-order */
    for (i = 0; i < LO_MAX_SOCKETS; i++) {
        struct lo_sock *sk = &g_lo[i];
        if (!sk->in_use || sk->state != LO_LISTENING) continue;
        if (!sk->has_bound) continue;
        if (sk->bound.sin_port != tport) continue;
        if (sk->bound.sin_addr.s_addr != 0 &&
            tip != 0 &&
            sk->bound.sin_addr.s_addr != tip) continue;
        return sk;
    }
    return 0;
}

static struct lo_sock *lo_alloc_pair_endpoint(void) {
    int i;
    for (i = 0; i < LO_MAX_SOCKETS; i++) {
        if (!g_lo[i].in_use) {
            memset(&g_lo[i], 0, sizeof(g_lo[i]));
            g_lo[i].in_use = 1;
            g_lo[i].type   = SOCK_STREAM;
            g_lo[i].state  = LO_IDLE;
            return &g_lo[i];
        }
    }
    return 0;
}

static int lo_listen(pcnet_sock_t s, int backlog) {
    struct lo_sock *sk = (struct lo_sock *)s;
    (void)backlog;                  /* honour LO_BACKLOG instead */
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type != SOCK_STREAM)    return PCNET_EOPNOTSUPP;
    if (!sk->has_bound)             return PCNET_EINVAL;
    sk->state = LO_LISTENING;
    return PCNET_OK;
}

static int lo_accept(pcnet_sock_t s, pcnet_sock_t *npeer,
                     struct sockaddr *a, pcnet_socklen_t *l)
{
    struct lo_sock *sk = (struct lo_sock *)s;
    struct lo_sock *peer;
    if (!sk || !sk->in_use)         return PCNET_EBADF;
    if (sk->state != LO_LISTENING)  return PCNET_EINVAL;
    if (sk->backlog_count == 0)     return PCNET_EAGAIN;
    peer = sk->backlog[sk->backlog_head];
    sk->backlog_head = (sk->backlog_head + 1) % LO_BACKLOG;
    sk->backlog_count--;
    *npeer = peer;
    /* No real peer-address surface: the connecting socket's peer addr is
     * just the listener's bound addr. Report it for getpeername parity. */
    if (a && l && (uint32_t)*l >= sizeof(struct sockaddr_in)) {
        memcpy(a, &sk->bound, sizeof(struct sockaddr_in));
        *l = sizeof(struct sockaddr_in);
    }
    return PCNET_OK;
}

static int lo_connect(pcnet_sock_t s, const struct sockaddr *a, pcnet_socklen_t l) {
    struct lo_sock *sk = (struct lo_sock *)s;
    const struct sockaddr_in *sin;
    struct lo_sock *listener, *server;
    if (!sk || !sk->in_use)         return PCNET_EBADF;
    if (!a || (uint32_t)l < sizeof(struct sockaddr_in)) return PCNET_EINVAL;
    sin = (const struct sockaddr_in *)a;
    if (sk->type != SOCK_STREAM) {
        /* UDP "connect" — record the peer in `bound`; sendto still
         * dictates per-call destination. Lightweight no-op success. */
        return PCNET_OK;
    }
    if (sk->state != LO_IDLE)       return PCNET_EISCONN;
    listener = lo_find_listener(sin);
    if (!listener)                  return PCNET_ECONNREFUSED;
    if (listener->backlog_count >= LO_BACKLOG) return PCNET_ECONNREFUSED;
    server = lo_alloc_pair_endpoint();
    if (!server)                    return PCNET_ECONNREFUSED;
    /* Wire the pair. */
    sk->peer       = server;
    sk->state      = LO_ESTABLISHED;
    server->peer   = sk;
    server->state  = LO_ESTABLISHED;
    /* Server inherits the listener's bound addr as its local addr. */
    memcpy(&server->bound, &listener->bound, sizeof(struct sockaddr_in));
    server->has_bound = 1;
    /* Enqueue server on listener backlog. */
    {
        int tail = (listener->backlog_head + listener->backlog_count) % LO_BACKLOG;
        listener->backlog[tail] = server;
        listener->backlog_count++;
    }
    return PCNET_OK;
}

static int lo_getsockname(pcnet_sock_t s, struct sockaddr *a, pcnet_socklen_t *l) {
    struct lo_sock *sk = (struct lo_sock *)s;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (!a || !l || (uint32_t)*l < sizeof(struct sockaddr_in)) return PCNET_EINVAL;
    if (sk->has_bound) memcpy(a, &sk->bound, sizeof(struct sockaddr_in));
    else               memset(a, 0, sizeof(struct sockaddr_in));
    *l = sizeof(struct sockaddr_in);
    return PCNET_OK;
}
static int lo_getpeername(pcnet_sock_t s, struct sockaddr *a, pcnet_socklen_t *l) {
    struct lo_sock *sk = (struct lo_sock *)s;
    if (!sk || !sk->in_use)         return PCNET_EBADF;
    if (sk->type != SOCK_STREAM)    return PCNET_ENOTCONN;
    if (sk->state != LO_ESTABLISHED && sk->state != LO_PEER_GONE) return PCNET_ENOTCONN;
    if (!a || !l || (uint32_t)*l < sizeof(struct sockaddr_in))    return PCNET_EINVAL;
    /* Peer's local address; for a server-side socket that's the listener's
     * bound addr, for a client-side it's whatever the server's been
     * assigned (which we set to the listener's bound addr above). */
    if (sk->peer) memcpy(a, &sk->peer->bound, sizeof(struct sockaddr_in));
    else          memset(a, 0, sizeof(struct sockaddr_in));
    *l = sizeof(struct sockaddr_in);
    return PCNET_OK;
}

/* ------------------------------------------------------------------
 * DNS synthesis — turn a single-question A query into a one-answer
 * response with rdata = LO_DNS_ANSWER_NBO. Returns response length or
 * 0 if the query doesn't look like a well-formed A query we can answer.
 * ------------------------------------------------------------------ */

static uint32_t loopback_dns_synth(const uint8_t *qbuf, uint32_t qlen,
                                    uint8_t *rbuf, uint32_t rcap)
{
    uint32_t p, qend;
    uint16_t qtype, qclass;
    if (qlen < DNS_HDR_LEN + 5) return 0;
    if (rcap < qlen + 16)        return 0;
    /* Walk the question's QNAME to find its end. */
    p = DNS_HDR_LEN;
    while (p < qlen) {
        uint8_t len = qbuf[p];
        if (len == 0) { p++; break; }
        if ((len & 0xC0) || len > 63) return 0;       /* malformed for our purpose */
        p += 1 + len;
    }
    if (p + 4 > qlen) return 0;
    qtype  = rbe16(qbuf + p);
    qclass = rbe16(qbuf + p + 2);
    qend   = p + 4;
    if (qtype != 1 || qclass != 1) return 0;           /* only A IN */

    /* Copy the request verbatim — header + question — as the response
     * prefix, then patch the flags and counts and append one A answer.
     * Compression pointer to the original QNAME at offset DNS_HDR_LEN. */
    memcpy(rbuf, qbuf, qend);
    /* flags: QR=1, OPCODE=0, AA=0, TC=0, RD=copy, RA=1, RCODE=0. */
    rbuf[2] = 0x81;     /* QR=1, RD=1 */
    rbuf[3] = 0x80;     /* RA=1, RCODE=NOERROR */
    wbe16(rbuf + 6, 1);     /* ANCOUNT = 1 */
    wbe16(rbuf + 8, 0);     /* NSCOUNT */
    wbe16(rbuf + 10, 0);    /* ARCOUNT */

    /* Answer: NAME (pointer 0xC00C → question QNAME), TYPE A, CLASS IN,
     * TTL=60, RDLENGTH=4, RDATA = canned IPv4. */
    p = qend;
    rbuf[p++] = 0xC0;
    rbuf[p++] = 0x0C;
    wbe16(rbuf + p, 1);     p += 2;            /* TYPE A */
    wbe16(rbuf + p, 1);     p += 2;            /* CLASS IN */
    wbe32(rbuf + p, 60);    p += 4;            /* TTL */
    wbe16(rbuf + p, 4);     p += 2;            /* RDLENGTH */
    memcpy(rbuf + p, &(uint32_t){LO_DNS_ANSWER_NBO}, 4);
    p += 4;
    return p;
}

/* ------------------------------------------------------------------
 * UDP data path
 * ------------------------------------------------------------------ */

static pcnet_ssize_t lo_sendto(pcnet_sock_t s, const void *buf, uint32_t len,
                                int flags, const struct sockaddr *to,
                                pcnet_socklen_t tolen)
{
    struct lo_sock *sk = (struct lo_sock *)s;
    const struct sockaddr_in *sin;
    uint16_t dest_port;
    (void)flags;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type != SOCK_DGRAM) return PCNET_EOPNOTSUPP;
    if (!buf || !to || (uint32_t)tolen < sizeof(struct sockaddr_in))
        return PCNET_EINVAL;
    sin = (const struct sockaddr_in *)to;
    /* sin_port is network-order; high byte first in the wire layout but
     * stored as a 16-bit value — pull both bytes. */
    dest_port = (uint16_t)((sin->sin_port & 0xFF) << 8) |
                (uint16_t)((sin->sin_port >> 8) & 0xFF);

    if (dest_port == 53) {
        uint32_t rlen = loopback_dns_synth((const uint8_t *)buf, len,
                                            sk->rx_buf, LO_BUF_SIZE);
        if (rlen == 0) {
            /* Malformed query — drop silently (mimics a real resolver). */
            return (pcnet_ssize_t)len;
        }
        sk->rx_len = rlen;
        sk->rx_from = *sin;        /* "reply" appears to come from server */
        return (pcnet_ssize_t)len;
    }

    /* Non-DNS UDP: nothing to do — silently consume (no peer to receive). */
    return (pcnet_ssize_t)len;
}

static pcnet_ssize_t lo_recvfrom(pcnet_sock_t s, void *buf, uint32_t len,
                                  int flags, struct sockaddr *from,
                                  pcnet_socklen_t *fromlen)
{
    struct lo_sock *sk = (struct lo_sock *)s;
    uint32_t n;
    (void)flags;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type == SOCK_STREAM) {
        /* Allow recvfrom on a stream — just delegate to recv semantics
         * and don't fill `from`. */
        if (sk->tcp_count == 0) {
            return (sk->state == LO_PEER_GONE) ? 0 : PCNET_EAGAIN;
        }
        n = sk->tcp_count; if (n > len) n = len;
        {
            /* Copy out from ring head, handling wrap. */
            uint32_t first = LO_TCP_RING - sk->tcp_head;
            if (first > n) first = n;
            memcpy(buf, sk->tcp_rx + sk->tcp_head, first);
            if (n > first) memcpy((uint8_t *)buf + first, sk->tcp_rx, n - first);
            sk->tcp_head  = (sk->tcp_head + n) % LO_TCP_RING;
            sk->tcp_count -= n;
        }
        if (from && fromlen) *fromlen = 0;
        return (pcnet_ssize_t)n;
    }
    if (sk->rx_len == 0)    return PCNET_EAGAIN;
    n = sk->rx_len;
    if (n > len) n = len;
    memcpy(buf, sk->rx_buf, n);
    if (from && fromlen && (uint32_t)*fromlen >= sizeof(struct sockaddr_in)) {
        memcpy(from, &sk->rx_from, sizeof(struct sockaddr_in));
        *fromlen = sizeof(struct sockaddr_in);
    }
    sk->rx_len = 0;
    return (pcnet_ssize_t)n;
}

/* ------------------------------------------------------------------
 * TCP stream data path
 *
 * Wire model: send(s) deposits bytes into the PEER's RX ring. recv(s)
 * drains its OWN RX ring. Ring full → EAGAIN (caller should select()
 * for WR before retrying). Ring empty + peer gone → 0 (EOF).
 * ------------------------------------------------------------------ */

static pcnet_ssize_t lo_send(pcnet_sock_t s, const void *buf, uint32_t len, int flags) {
    struct lo_sock *sk = (struct lo_sock *)s;
    struct lo_sock *p;
    uint32_t space, first, n;
    (void)flags;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type != SOCK_STREAM) return PCNET_EOPNOTSUPP;
    if (sk->state == LO_PEER_GONE) return PCNET_ECONNRESET;
    if (sk->state != LO_ESTABLISHED) return PCNET_ENOTCONN;
    p = sk->peer;
    if (!p || !p->in_use) {
        sk->state = LO_PEER_GONE;
        sk->peer  = 0;
        return PCNET_ECONNRESET;
    }
    space = LO_TCP_RING - p->tcp_count;
    if (space == 0) return PCNET_EAGAIN;
    n = (len < space) ? len : space;
    {
        uint32_t tail = (p->tcp_head + p->tcp_count) % LO_TCP_RING;
        first = LO_TCP_RING - tail;
        if (first > n) first = n;
        memcpy(p->tcp_rx + tail, buf, first);
        if (n > first) memcpy(p->tcp_rx, (const uint8_t *)buf + first, n - first);
        p->tcp_count += n;
    }
    return (pcnet_ssize_t)n;
}

static pcnet_ssize_t lo_recv(pcnet_sock_t s, void *b, uint32_t l, int f) {
    return lo_recvfrom(s, b, l, f, 0, 0);
}

/* ------------------------------------------------------------------
 * poll — feed select() the right RD/WR bits per socket state.
 *
 *   LO_IDLE / unconnected stream: writable but never readable.
 *   LO_LISTENING:   RD when backlog non-empty (accept will succeed).
 *   LO_ESTABLISHED: RD if my ring has bytes, WR if peer's ring has space.
 *   LO_PEER_GONE:   RD always (drain remaining + return 0 for EOF).
 *   UDP DGRAM:      RD if pending datagram, WR always.
 * ------------------------------------------------------------------ */

static int lo_poll(pcnet_sock_t s) {
    struct lo_sock *sk = (struct lo_sock *)s;
    int r = 0;
    if (!sk || !sk->in_use) return PCNET_EBADF;
    if (sk->type == SOCK_STREAM) {
        switch (sk->state) {
        case LO_LISTENING:
            if (sk->backlog_count > 0) r |= PCNET_POLL_RD;
            break;
        case LO_ESTABLISHED:
            if (sk->tcp_count > 0)                       r |= PCNET_POLL_RD;
            if (sk->peer && sk->peer->tcp_count < LO_TCP_RING) r |= PCNET_POLL_WR;
            break;
        case LO_PEER_GONE:
            r |= PCNET_POLL_RD;     /* drain or read EOF immediately */
            break;
        case LO_IDLE:
        default:
            r |= PCNET_POLL_WR;
            break;
        }
        return r;
    }
    /* UDP / SOCK_DGRAM */
    r = PCNET_POLL_WR;
    if (sk->rx_len > 0) r |= PCNET_POLL_RD;
    return r;
}

static int lo_setsockopt(pcnet_sock_t s, int lv, int n, const void *v, pcnet_socklen_t l)
    { (void)s; (void)lv; (void)n; (void)v; (void)l; return PCNET_OK; }
static int lo_getsockopt(pcnet_sock_t s, int lv, int n, void *v, pcnet_socklen_t *l)
    { (void)s; (void)lv; (void)n; (void)v; (void)l; return PCNET_OK; }

/* ------------------------------------------------------------------
 * Provider descriptor
 * ------------------------------------------------------------------ */

static const struct net_provider_ops loopback_ops = {
    .start = loopback_start, .stop = loopback_stop,
    .sock_create = lo_create, .sock_close = lo_close,
    .sock_shutdown = lo_shutdown,
    .sock_bind = lo_bind, .sock_connect = lo_connect,
    .sock_listen = lo_listen, .sock_accept = lo_accept,
    .sock_getsockname = lo_getsockname, .sock_getpeername = lo_getpeername,
    .sock_send = lo_send, .sock_recv = lo_recv,
    .sock_sendto = lo_sendto, .sock_recvfrom = lo_recvfrom,
    .sock_poll = lo_poll,
    .sock_setsockopt = lo_setsockopt, .sock_getsockopt = lo_getsockopt,
};

static const struct net_provider loopback_provider = {
    .name          = "loopback",
    .description   = "software UDP+TCP loopback + DNS synthesis (no NIC)",
    .version_major = 0,
    .version_minor = 2,
    .version_patch = 0,
    .abi_version   = NET_PROVIDER_ABI_VERSION,
    .stability     = NET_PROVIDER_EXPERIMENTAL,
    .caps          = NET_CAP_UDP | NET_CAP_TCP | NET_CAP_LISTEN | NET_CAP_NONBLOCK,
    .ops           = &loopback_ops,
};

static int loopback_init(void) {
    serial_puts("loopback.kmd: registering loopback provider\n");
    return net_register_provider(&loopback_provider);
}
static void loopback_exit(void) {
    net_unregister_provider(&loopback_provider);
}

module_init(loopback_init);
module_exit(loopback_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("Loopback network provider (UDP+TCP+DNS synthesis)");
MODULE_NAME("loopback");
