/* null.kmd — minimum-viable Pinecore network provider.
 *
 * Purpose: prove the .kmd load -> net_register_provider -> INT 0x80
 * dispatch -> net_unregister_provider chain in QEMU, where the r6040
 * PCI probe finds no hardware. Every socket op returns ENOSYS; the
 * dispatch path itself is what we want to exercise.
 *
 * Load via PCORE.CFG:  net_provider = null
 *
 * License: GPL-2.0
 */

#include "module.h"
#include "net.h"
#include "types.h"

extern void serial_puts(const char *s);
extern int  net_register_provider(const struct net_provider *p);
extern void net_unregister_provider(const struct net_provider *p);

static int  null_start(void) { serial_puts("null: start\n"); return 0; }
static void null_stop(void)  { serial_puts("null: stop\n"); }

static int null_create(int d, int t, int p, pcnet_sock_t *out) {
    (void)d; (void)t; (void)p; (void)out; return PCNET_ENOSYS;
}
static int null_close(pcnet_sock_t s)               { (void)s; return PCNET_OK; }
static int null_shutdown(pcnet_sock_t s, int h)     { (void)s; (void)h; return PCNET_ENOSYS; }
static int null_bind(pcnet_sock_t s, const struct sockaddr *a, pcnet_socklen_t l)
    { (void)s; (void)a; (void)l; return PCNET_ENOSYS; }
static int null_connect(pcnet_sock_t s, const struct sockaddr *a, pcnet_socklen_t l)
    { (void)s; (void)a; (void)l; return PCNET_ENETDOWN; }
static int null_listen(pcnet_sock_t s, int b)       { (void)s; (void)b; return PCNET_ENOSYS; }
static int null_accept(pcnet_sock_t s, pcnet_sock_t *n, struct sockaddr *a, pcnet_socklen_t *l)
    { (void)s; (void)n; (void)a; (void)l; return PCNET_ENOSYS; }
static int null_getsockname(pcnet_sock_t s, struct sockaddr *a, pcnet_socklen_t *l)
    { (void)s; (void)a; (void)l; return PCNET_ENOSYS; }
static int null_getpeername(pcnet_sock_t s, struct sockaddr *a, pcnet_socklen_t *l)
    { (void)s; (void)a; (void)l; return PCNET_ENOTCONN; }
static pcnet_ssize_t null_send(pcnet_sock_t s, const void *b, uint32_t l, int f)
    { (void)s; (void)b; (void)l; (void)f; return PCNET_ENETDOWN; }
static pcnet_ssize_t null_recv(pcnet_sock_t s, void *b, uint32_t l, int f)
    { (void)s; (void)b; (void)l; (void)f; return PCNET_ENETDOWN; }
static pcnet_ssize_t null_sendto(pcnet_sock_t s, const void *b, uint32_t l, int f,
                                  const struct sockaddr *to, pcnet_socklen_t tl)
    { (void)s; (void)b; (void)l; (void)f; (void)to; (void)tl; return PCNET_ENETDOWN; }
static pcnet_ssize_t null_recvfrom(pcnet_sock_t s, void *b, uint32_t l, int f,
                                    struct sockaddr *fr, pcnet_socklen_t *fl)
    { (void)s; (void)b; (void)l; (void)f; (void)fr; (void)fl; return PCNET_ENETDOWN; }
static int null_poll(pcnet_sock_t s)                { (void)s; return 0; }
static int null_setsockopt(pcnet_sock_t s, int lv, int n, const void *v, pcnet_socklen_t l)
    { (void)s; (void)lv; (void)n; (void)v; (void)l; return PCNET_ENOSYS; }
static int null_getsockopt(pcnet_sock_t s, int lv, int n, void *v, pcnet_socklen_t *l)
    { (void)s; (void)lv; (void)n; (void)v; (void)l; return PCNET_ENOSYS; }

static const struct net_provider_ops null_ops = {
    .start = null_start, .stop = null_stop,
    .sock_create = null_create, .sock_close = null_close,
    .sock_shutdown = null_shutdown,
    .sock_bind = null_bind, .sock_connect = null_connect,
    .sock_listen = null_listen, .sock_accept = null_accept,
    .sock_getsockname = null_getsockname, .sock_getpeername = null_getpeername,
    .sock_send = null_send, .sock_recv = null_recv,
    .sock_sendto = null_sendto, .sock_recvfrom = null_recvfrom,
    .sock_poll = null_poll,
    .sock_setsockopt = null_setsockopt, .sock_getsockopt = null_getsockopt,
};

static const struct net_provider null_provider = {
    .name          = "null",
    .description   = "no-op provider (chain validation only)",
    .version_major = 0,
    .version_minor = 1,
    .version_patch = 0,
    .abi_version   = NET_PROVIDER_ABI_VERSION,
    .stability     = NET_PROVIDER_EXPERIMENTAL,
    .caps          = 0,
    .ops           = &null_ops,
};

static int null_init(void) {
    serial_puts("null.kmd: registering null provider\n");
    return net_register_provider(&null_provider);
}
static void null_exit(void) {
    net_unregister_provider(&null_provider);
}

module_init(null_init);
module_exit(null_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Pinecore project");
MODULE_DESCRIPTION("Null network provider — registration smoke test");
MODULE_NAME("null");
