/*
 * vhost_device_vsock: minimal vhost-user vsock back-end for cuttlefish.
 *
 * Speaks vhost-user to crosvm or QEMU on `socket=` and the Firecracker
 * hybrid protocol ("connect <port>\n" -> "OK <port>\n") to host tools
 * on `uds-path=`. Host-initiated streams only; connections the guest
 * initiates are refused with RST. Assumes a little-endian host (virtio
 * structures are copied as is) and GCC/clang __atomic builtins for the
 * ring barriers. Everything else is C99 + POSIX.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

/* vhost-user (docs/interop/vhost-user.rst). */
enum {
    VU_GET_FEATURES = 1, VU_SET_FEATURES = 2, VU_SET_OWNER = 3,
    VU_RESET_OWNER = 4, VU_SET_MEM_TABLE = 5, VU_SET_VRING_NUM = 8,
    VU_SET_VRING_ADDR = 9, VU_SET_VRING_BASE = 10, VU_GET_VRING_BASE = 11,
    VU_SET_VRING_KICK = 12, VU_SET_VRING_CALL = 13, VU_SET_VRING_ERR = 14,
    VU_GET_PROTOCOL_FEATURES = 15, VU_SET_PROTOCOL_FEATURES = 16,
    VU_GET_QUEUE_NUM = 17, VU_SET_VRING_ENABLE = 18, VU_GET_CONFIG = 24,
};
#define VU_VERSION 1
#define VU_REPLY (1u << 2)
#define VU_NEED_REPLY (1u << 3)
#define VU_FD_INVALID (1u << 8)
#define VU_MAX_REGIONS 8
#define VU_MAX_FDS 8

#define VIRTIO_F_VERSION_1 32
#define VHOST_USER_F_PROTOCOL_FEATURES 30
#define VU_PROTOCOL_F_MQ 0
#define VU_PROTOCOL_F_REPLY_ACK 3
#define VU_PROTOCOL_F_CONFIG 9

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VRING_DESC_F_INDIRECT 4

/* virtio-vsock. */
#define VSOCK_HOST_CID 2
#define VSOCK_TYPE_STREAM 1
enum {
    OP_REQUEST = 1, OP_RESPONSE, OP_RST, OP_SHUTDOWN, OP_RW,
    OP_CREDIT_UPDATE, OP_CREDIT_REQUEST,
};
#define SHUT_RCV 1
#define SHUT_SEND 2

#define NUM_QUEUES 3 /* rx, tx, event; the event queue is never served */
#define RXQ 0
#define TXQ 1
#define MAX_CONNS 64
#define MAX_SEGS 16
#define BUF_ALLOC 65536u /* guest->host backlog, advertised as buf_alloc */
#define CMD_MAX 64

struct vu_msg {
    uint32_t request, flags, size;
    union {
        uint64_t u64;
        struct { uint32_t index, num; } state;
        struct {
            uint32_t index, flags;
            uint64_t desc, used, avail, log;
        } addr;
        struct {
            uint32_t nregions, pad;
            struct { uint64_t gpa, size, uva, off; } r[VU_MAX_REGIONS];
        } mem;
        struct { uint32_t off, size, flags; uint8_t data[256]; } config;
        uint8_t raw[4096];
    } u;
} __attribute__((packed));

struct vring_desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vring_avail { uint16_t flags, idx, ring[]; };
struct vring_used_elem { uint32_t id, len; };
struct vring_used { uint16_t flags, idx; struct vring_used_elem ring[]; };

struct vsock_hdr {
    uint64_t src_cid, dst_cid;
    uint32_t src_port, dst_port, len;
    uint16_t type, op;
    uint32_t flags, buf_alloc, fwd_cnt;
} __attribute__((packed));

struct region { uint64_t gpa, size, uva, off; void *map; size_t maplen; };

struct vq {
    bool enabled, call_pending;
    uint32_t num;
    uint16_t last_avail, used_idx;
    int kick, call;
    uint64_t desc_uva, avail_uva, used_uva;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
};

enum conn_state { CS_FREE, CS_CMD, CS_REQ, CS_OPEN, CS_CLOSING };

/* Control packets waiting for an rx buffer, flushed in this order. */
enum {
    P_REQUEST = 1, P_CREDIT_UPDATE = 2, P_CREDIT_REQUEST = 4,
    P_SHUTDOWN = 8, P_RST = 16,
};

struct conn {
    int fd;
    enum conn_state st;
    uint32_t lport, pport;
    uint32_t tx_cnt, fwd_cnt, last_fwd_cnt, peer_buf_alloc, peer_fwd_cnt;
    unsigned pend, peer_shut;
    bool host_eof, credit_req, shut_wr;
    size_t head, len;
    uint8_t buf[BUF_ALLOC];
};

struct dev {
    uint64_t cid;
    int vhost_listen, vhost, uds;
    int nreg;
    struct region reg[VU_MAX_REGIONS];
    struct vq vq[NUM_QUEUES];
    uint32_t next_port;
    struct conn conn[MAX_CONNS];
};

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("vhost_device_vsock: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static int read_full(int fd, void *buf, size_t len)
{
    for (size_t got = 0; got < len;) {
        ssize_t r = read(fd, (uint8_t *)buf + got, len - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    for (size_t done = 0; done < len;) {
        ssize_t r = write(fd, (const uint8_t *)buf + done, len - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

/* Guest memory. */

static void *gpa_ptr(struct dev *d, uint64_t gpa, uint64_t len)
{
    for (int i = 0; i < d->nreg; i++) {
        struct region *r = &d->reg[i];
        if (r->map && gpa >= r->gpa && len <= r->size &&
            gpa - r->gpa <= r->size - len)
            return (uint8_t *)r->map + (gpa - r->gpa);
    }
    return NULL;
}

static void *uva_ptr(struct dev *d, uint64_t uva, uint64_t len)
{
    for (int i = 0; i < d->nreg; i++) {
        struct region *r = &d->reg[i];
        if (r->map && uva >= r->uva && len <= r->size &&
            uva - r->uva <= r->size - len)
            return (uint8_t *)r->map + (uva - r->uva);
    }
    return NULL;
}

static void unmap_all(struct dev *d)
{
    for (int i = 0; i < d->nreg; i++)
        if (d->reg[i].map) munmap(d->reg[i].map, d->reg[i].maplen);
    d->nreg = 0;
}

/* Virtqueues. */

static void vq_map(struct dev *d, struct vq *q)
{
    q->desc = NULL;
    if (!q->num || !q->desc_uva) return;
    struct vring_avail *a = uva_ptr(d, q->avail_uva, 6 + 2 * q->num);
    struct vring_used *u = uva_ptr(d, q->used_uva, 6 + 8 * q->num);
    struct vring_desc *dsc = uva_ptr(d, q->desc_uva, 16 * q->num);
    if (!a || !u || !dsc) {
        say("vring addresses outside the memory table");
        return;
    }
    q->avail = a;
    q->used = u;
    q->desc = dsc;
    q->used_idx = u->idx;
}

static bool vq_ready(struct vq *q)
{
    return q->enabled && q->desc && q->kick >= 0;
}

static bool vq_has_avail(struct vq *q)
{
    return vq_ready(q) &&
           q->last_avail != __atomic_load_n(&q->avail->idx, __ATOMIC_ACQUIRE);
}

/*
 * Pop the next chain and gather its descriptors of one direction.
 * Returns the segment count, 0 if nothing is available, or -1 for a
 * malformed chain, which the caller must still complete with len 0.
 */
static int vq_pop(struct dev *d, struct vq *q, uint16_t *head,
                  struct iovec *iov, bool writable)
{
    if (!vq_has_avail(q)) return 0;
    *head = q->avail->ring[q->last_avail++ % q->num];
    int n = 0;
    uint16_t i = *head;
    for (uint32_t hops = 0; hops < q->num; hops++) {
        if (i >= q->num) return -1;
        struct vring_desc dsc = q->desc[i];
        if (dsc.flags & VRING_DESC_F_INDIRECT) return -1;
        if (!!(dsc.flags & VRING_DESC_F_WRITE) == writable) {
            void *p = gpa_ptr(d, dsc.addr, dsc.len);
            if (!p || n == MAX_SEGS) return -1;
            iov[n].iov_base = p;
            iov[n].iov_len = dsc.len;
            n++;
        }
        if (!(dsc.flags & VRING_DESC_F_NEXT)) return n;
        i = dsc.next;
    }
    return -1;
}

static void vq_push(struct vq *q, uint16_t head, uint32_t len)
{
    struct vring_used_elem *e = &q->used->ring[q->used_idx % q->num];
    e->id = head;
    e->len = len;
    __atomic_store_n(&q->used->idx, ++q->used_idx, __ATOMIC_RELEASE);
    q->call_pending = true;
}

static void vq_call(struct vq *q)
{
    uint64_t one = 1;
    if (q->call_pending && q->call >= 0 && write(q->call, &one, 8) < 0)
        say("call fd: %s", strerror(errno));
    q->call_pending = false;
}

/* Scatter/gather helpers over a descriptor chain. */

static size_t iov_size(const struct iovec *iov, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++) total += iov[i].iov_len;
    return total;
}

/* Copy len bytes between buf and the chain at offset off. */
static size_t iov_xfer(const struct iovec *iov, int n, size_t off, void *buf,
                       size_t len, bool to_iov)
{
    size_t done = 0;
    for (int i = 0; i < n && done < len; i++) {
        if (off >= iov[i].iov_len) {
            off -= iov[i].iov_len;
            continue;
        }
        size_t k = iov[i].iov_len - off;
        if (k > len - done) k = len - done;
        uint8_t *p = (uint8_t *)iov[i].iov_base + off;
        if (to_iov) memcpy(p, (uint8_t *)buf + done, k);
        else memcpy((uint8_t *)buf + done, p, k);
        done += k;
        off = 0;
    }
    return done;
}

/* View [off, off+len) of a chain as a new iovec array. */
static int iov_slice(const struct iovec *iov, int n, size_t off, size_t len,
                     struct iovec *out)
{
    int m = 0;
    for (int i = 0; i < n && len; i++) {
        if (off >= iov[i].iov_len) {
            off -= iov[i].iov_len;
            continue;
        }
        size_t k = iov[i].iov_len - off;
        if (k > len) k = len;
        out[m].iov_base = (uint8_t *)iov[i].iov_base + off;
        out[m].iov_len = k;
        m++;
        len -= k;
        off = 0;
    }
    return m;
}

/* Connections. */

static struct conn *find_conn(struct dev *d, uint32_t lport, uint32_t pport)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &d->conn[i];
        if (c->st >= CS_REQ && c->lport == lport && c->pport == pport)
            return c;
    }
    return NULL;
}

static uint32_t alloc_port(struct dev *d)
{
    for (;;) {
        uint32_t p = d->next_port++;
        if (d->next_port < 1024) d->next_port = 1024;
        bool used = false;
        for (int i = 0; i < MAX_CONNS && !used; i++)
            used = d->conn[i].st >= CS_REQ && d->conn[i].lport == p;
        if (!used) return p;
    }
}

static void conn_free(struct conn *c)
{
    if (c->fd >= 0) close(c->fd);
    say("conn %u->%u closed", c->lport, c->pport);
    memset(c, 0, offsetof(struct conn, buf));
    c->fd = -1;
}

/* The host side failed: tell the guest and forget the socket. */
static void conn_abort(struct conn *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->st = CS_CLOSING;
    c->pend = P_RST;
}

static void init_hdr(struct dev *d, struct vsock_hdr *h, uint32_t lport,
                     uint32_t pport, uint32_t fwd_cnt, uint16_t op)
{
    memset(h, 0, sizeof(*h));
    h->src_cid = VSOCK_HOST_CID;
    h->dst_cid = d->cid;
    h->src_port = lport;
    h->dst_port = pport;
    h->type = VSOCK_TYPE_STREAM;
    h->op = op;
    h->buf_alloc = BUF_ALLOC;
    h->fwd_cnt = fwd_cnt;
}

/* Put a control packet in the next rx buffer; false if there is none. */
static bool rx_ctl(struct dev *d, const struct vsock_hdr *h)
{
    struct vq *q = &d->vq[RXQ];
    struct iovec iov[MAX_SEGS];
    uint16_t head;
    for (;;) {
        int n = vq_pop(d, q, &head, iov, true);
        if (n == 0) return false;
        if (n < 0 || iov_size(iov, n) < sizeof(*h)) {
            vq_push(q, head, 0);
            continue;
        }
        iov_xfer(iov, n, 0, (void *)h, sizeof(*h), true);
        vq_push(q, head, sizeof(*h));
        return true;
    }
}

static bool send_ctl(struct dev *d, struct conn *c, uint16_t op,
                     uint32_t flags)
{
    struct vsock_hdr h;
    init_hdr(d, &h, c->lport, c->pport, c->fwd_cnt, op);
    h.flags = flags;
    if (!rx_ctl(d, &h)) return false;
    c->last_fwd_cnt = c->fwd_cnt;
    return true;
}

static void flush_pending(struct dev *d, struct conn *c)
{
    static const struct { unsigned bit; uint16_t op; uint32_t flags; } ops[] = {
        { P_REQUEST, OP_REQUEST, 0 },
        { P_CREDIT_UPDATE, OP_CREDIT_UPDATE, 0 },
        { P_CREDIT_REQUEST, OP_CREDIT_REQUEST, 0 },
        { P_SHUTDOWN, OP_SHUTDOWN, SHUT_RCV | SHUT_SEND },
        { P_RST, OP_RST, 0 },
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]) && c->pend; i++) {
        if (!(c->pend & ops[i].bit)) continue;
        if (!send_ctl(d, c, ops[i].op, ops[i].flags)) return;
        c->pend &= ~ops[i].bit;
        if (ops[i].bit == P_RST) conn_free(c);
    }
}

/*
 * Credit the guest for bytes that reached the host socket. Like the
 * rust-vmm daemon, announce it once the guest's view of our free space
 * could have dropped below a quarter; sooner is chatter, later stalls.
 */
static void conn_wrote(struct conn *c, size_t n)
{
    c->fwd_cnt += n;
    if (BUF_ALLOC - (c->fwd_cnt - c->last_fwd_cnt) < BUF_ALLOC / 4)
        c->pend |= P_CREDIT_UPDATE;
}

/* Apply a guest SHUTDOWN once nothing is left to deliver to the host. */
static void conn_check_shut(struct conn *c)
{
    if (c->len || c->st != CS_OPEN) return;
    if (c->peer_shut & SHUT_RCV) c->host_eof = true;
    if ((c->peer_shut & (SHUT_RCV | SHUT_SEND)) == (SHUT_RCV | SHUT_SEND)) {
        conn_abort(c);
    } else if ((c->peer_shut & SHUT_SEND) && !c->shut_wr) {
        shutdown(c->fd, SHUT_WR);
        c->shut_wr = true;
    }
}

static void conn_flush(struct conn *c)
{
    while (c->len) {
        struct iovec v[2];
        int vn = 1;
        v[0].iov_base = c->buf + c->head;
        v[0].iov_len = c->len < BUF_ALLOC - c->head ? c->len
                                                     : BUF_ALLOC - c->head;
        if (v[0].iov_len < c->len) {
            v[1].iov_base = c->buf;
            v[1].iov_len = c->len - v[0].iov_len;
            vn = 2;
        }
        ssize_t r = writev(c->fd, v, vn);
        if (r < 0) {
            if (errno != EAGAIN && errno != EINTR) conn_abort(c);
            return;
        }
        c->head = (c->head + r) % BUF_ALLOC;
        c->len -= r;
        conn_wrote(c, r);
    }
    conn_check_shut(c);
}

/* Deliver guest payload to the host socket, queueing what does not fit. */
static void guest_to_host(struct conn *c, const struct iovec *iov, int n,
                          size_t off, size_t len)
{
    size_t done = 0;
    if (c->len == 0) {
        struct iovec v[MAX_SEGS];
        int vn = iov_slice(iov, n, off, len, v);
        ssize_t r = writev(c->fd, v, vn);
        if (r < 0 && errno != EAGAIN && errno != EINTR) {
            conn_abort(c);
            return;
        }
        if (r > 0) {
            done = r;
            conn_wrote(c, r);
        }
    }
    /* An honest guest never exceeds our buf_alloc, so this fits. */
    while (done < len && c->len < BUF_ALLOC) {
        size_t at = (c->head + c->len) % BUF_ALLOC;
        size_t k = len - done;
        if (k > BUF_ALLOC - at) k = BUF_ALLOC - at;
        if (k > BUF_ALLOC - c->len) k = BUF_ALLOC - c->len;
        iov_xfer(iov, n, off + done, c->buf + at, k, false);
        c->len += k;
        done += k;
    }
    if (done < len) say("guest overran credit, %zu bytes dropped", len - done);
}

/* Move host bytes into rx buffers until EAGAIN, credit or buffers run out. */
static void host_to_guest(struct dev *d, struct conn *c)
{
    struct vq *q = &d->vq[RXQ];
    for (int batch = 0; batch < 64; batch++) {
        uint32_t credit = c->peer_buf_alloc - (c->tx_cnt - c->peer_fwd_cnt);
        if (credit == 0) {
            if (!c->credit_req) c->pend |= P_CREDIT_REQUEST;
            c->credit_req = true;
            return;
        }
        struct iovec iov[MAX_SEGS], data[MAX_SEGS];
        uint16_t head;
        int n = vq_pop(d, q, &head, iov, true);
        if (n == 0) return;
        size_t space = iov_size(iov, n);
        if (n < 0 || space <= sizeof(struct vsock_hdr)) {
            vq_push(q, head, 0);
            continue;
        }
        size_t want = space - sizeof(struct vsock_hdr);
        if (want > credit) want = credit;
        int dn = iov_slice(iov, n, sizeof(struct vsock_hdr), want, data);
        ssize_t r = readv(c->fd, data, dn);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
            vq_push(q, head, 0);
            return;
        }
        struct vsock_hdr h;
        init_hdr(d, &h, c->lport, c->pport, c->fwd_cnt, OP_RW);
        if (r > 0) {
            h.len = r;
            c->tx_cnt += r;
        } else if (r == 0) {
            h.op = OP_SHUTDOWN;
            h.flags = SHUT_RCV | SHUT_SEND;
            c->host_eof = true;
        } else {
            h.op = OP_RST;
        }
        iov_xfer(iov, n, 0, &h, sizeof(h), true);
        vq_push(q, head, sizeof(h) + h.len);
        c->last_fwd_cnt = c->fwd_cnt;
        if (r < 0) {
            conn_free(c);
            return;
        }
        if (r == 0 || (size_t)r < want) return;
    }
}

static void handle_tx_pkt(struct dev *d, const struct vsock_hdr *h,
                          const struct iovec *iov, int n)
{
    struct conn *c = find_conn(d, h->dst_port, h->src_port);
    if (!c || c->st == CS_CLOSING) {
        /* Guest-initiated or stale. Dropped if no rx buffer is free,
         * in which case the guest times out instead. */
        if (h->op != OP_RST) {
            struct vsock_hdr rst;
            init_hdr(d, &rst, h->dst_port, h->src_port, 0, OP_RST);
            rx_ctl(d, &rst);
        }
        return;
    }
    c->peer_buf_alloc = h->buf_alloc;
    c->peer_fwd_cnt = h->fwd_cnt;
    c->credit_req = false;
    switch (h->op) {
    case OP_RESPONSE: {
        if (c->st != CS_REQ) break;
        c->st = CS_OPEN;
        char ok[32];
        int k = snprintf(ok, sizeof(ok), "OK %u\n", c->pport);
        if (write_full(c->fd, ok, k) < 0) conn_abort(c);
        else say("conn %u->%u open", c->lport, c->pport);
        break;
    }
    case OP_RW: {
        if (c->st != CS_OPEN) break;
        size_t avail = iov_size(iov, n) - sizeof(*h);
        guest_to_host(c, iov, n, sizeof(*h),
                      h->len < avail ? h->len : avail);
        break;
    }
    case OP_CREDIT_REQUEST:
        c->pend |= P_CREDIT_UPDATE;
        break;
    case OP_SHUTDOWN:
        c->peer_shut |= h->flags;
        conn_check_shut(c);
        break;
    case OP_RST:
        conn_free(c);
        break;
    default:
        break;
    }
}

static void process_tx(struct dev *d)
{
    struct vq *q = &d->vq[TXQ];
    struct iovec iov[MAX_SEGS];
    struct vsock_hdr h;
    uint16_t head;
    int n;
    while ((n = vq_pop(d, q, &head, iov, false)) != 0) {
        if (n > 0 && iov_xfer(iov, n, 0, &h, sizeof(h), false) == sizeof(h))
            handle_tx_pkt(d, &h, iov, n);
        vq_push(q, head, 0);
    }
}

static void accept_host(struct dev *d)
{
    int fd = accept(d->uds, NULL, NULL);
    if (fd < 0) return;
    for (int i = 0; i < MAX_CONNS; i++) {
        struct conn *c = &d->conn[i];
        if (c->st != CS_FREE) continue;
        set_nonblock(fd);
        c->fd = fd;
        c->st = CS_CMD;
        return;
    }
    say("too many connections");
    close(fd);
}

/* Parse "connect <port>\n"; case-insensitive like the rust-vmm daemon. */
static void read_cmd(struct dev *d, struct conn *c)
{
    ssize_t r = read(c->fd, c->buf + c->len, CMD_MAX - 1 - c->len);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {
        conn_free(c);
        return;
    }
    c->len += r;
    c->buf[c->len] = 0;
    if (!memchr(c->buf, '\n', c->len)) {
        if (c->len >= CMD_MAX - 1) conn_free(c);
        return;
    }
    char word[8];
    unsigned port;
    if (sscanf((char *)c->buf, "%7s %u", word, &port) != 2 ||
        strcasecmp(word, "connect") != 0) {
        say("bad command from host: %s", c->buf);
        conn_free(c);
        return;
    }
    c->len = 0;
    c->pport = port;
    c->lport = alloc_port(d);
    c->st = CS_REQ;
    c->pend = P_REQUEST;
}

/* vhost-user. */

static int vu_recv(int fd, struct vu_msg *m, int *fds, int *nfds)
{
    struct iovec iov = { m, 12 };
    union {
        struct cmsghdr h;
        char b[CMSG_SPACE(sizeof(int) * VU_MAX_FDS)];
    } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctl.b;
    mh.msg_controllen = sizeof(ctl.b);
    ssize_t r = recvmsg(fd, &mh, 0);
    if (r <= 0) return -1;
    if (r < 12 && read_full(fd, (uint8_t *)m + r, 12 - r) < 0) return -1;
    *nfds = 0;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm;
         cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
            continue;
        int k = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (k > VU_MAX_FDS - *nfds) k = VU_MAX_FDS - *nfds;
        memcpy(fds + *nfds, CMSG_DATA(cm), k * sizeof(int));
        *nfds += k;
    }
    if (m->size > sizeof(m->u)) return -1;
    return read_full(fd, &m->u, m->size);
}

static void vq_set_fd(int *slot, int fd)
{
    if (*slot >= 0) close(*slot);
    *slot = fd;
}

static int vu_handle(struct dev *d, struct vu_msg *m, int *fds, int nfds)
{
    bool ack = m->flags & VU_NEED_REPLY, send = false;
    int used_fds = 0;
    uint32_t idx = m->u.state.index;
    struct vq *q = idx < NUM_QUEUES ? &d->vq[idx] : NULL;
    m->flags = VU_VERSION | VU_REPLY;
    switch (m->request) {
    case VU_GET_FEATURES:
        m->u.u64 = (1ull << VIRTIO_F_VERSION_1) |
                   (1ull << VHOST_USER_F_PROTOCOL_FEATURES);
        m->size = 8;
        send = true;
        break;
    case VU_GET_PROTOCOL_FEATURES:
        m->u.u64 = (1ull << VU_PROTOCOL_F_MQ) |
                   (1ull << VU_PROTOCOL_F_REPLY_ACK) |
                   (1ull << VU_PROTOCOL_F_CONFIG);
        m->size = 8;
        send = true;
        break;
    case VU_GET_QUEUE_NUM:
        m->u.u64 = NUM_QUEUES;
        m->size = 8;
        send = true;
        break;
    case VU_SET_FEATURES:
        if (!(m->u.u64 & (1ull << VIRTIO_F_VERSION_1)))
            say("front-end did not accept VIRTIO_F_VERSION_1");
        break;
    case VU_SET_MEM_TABLE:
        unmap_all(d);
        d->nreg = m->u.mem.nregions < VU_MAX_REGIONS ? m->u.mem.nregions
                                                     : VU_MAX_REGIONS;
        for (int i = 0; i < d->nreg; i++) {
            struct region *r = &d->reg[i];
            r->gpa = m->u.mem.r[i].gpa;
            r->size = m->u.mem.r[i].size;
            r->uva = m->u.mem.r[i].uva;
            r->off = m->u.mem.r[i].off;
            r->map = NULL;
            if (i >= nfds) continue;
            r->maplen = r->size;
            r->map = mmap(NULL, r->maplen, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fds[i], r->off);
            if (r->map == MAP_FAILED) {
                r->map = NULL;
                say("mmap region %d: %s", i, strerror(errno));
            }
            used_fds = i + 1;
        }
        for (int i = 0; i < NUM_QUEUES; i++) vq_map(d, &d->vq[i]);
        break;
    case VU_SET_VRING_NUM:
        if (q) q->num = m->u.state.num;
        break;
    case VU_SET_VRING_ADDR:
        if (!q) break;
        q->desc_uva = m->u.addr.desc;
        q->avail_uva = m->u.addr.avail;
        q->used_uva = m->u.addr.used;
        vq_map(d, q);
        break;
    case VU_SET_VRING_BASE:
        if (q) q->last_avail = m->u.state.num;
        break;
    case VU_GET_VRING_BASE:
        /* Stops the ring; the front-end re-sends the kick fd on resume. */
        if (q) {
            q->enabled = false;
            vq_set_fd(&q->kick, -1);
            m->u.state.num = q->last_avail;
        }
        m->size = 8;
        send = true;
        break;
    case VU_SET_VRING_KICK:
    case VU_SET_VRING_CALL:
    case VU_SET_VRING_ERR: {
        int fd = -1;
        if (!(m->u.u64 & VU_FD_INVALID) && nfds > 0) {
            fd = fds[0];
            used_fds = 1;
        }
        q = (m->u.u64 & 0xff) < NUM_QUEUES ? &d->vq[m->u.u64 & 0xff] : NULL;
        if (!q || m->request == VU_SET_VRING_ERR) {
            if (fd >= 0) close(fd);
        } else if (m->request == VU_SET_VRING_KICK) {
            vq_set_fd(&q->kick, fd);
        } else {
            vq_set_fd(&q->call, fd);
        }
        break;
    }
    case VU_SET_VRING_ENABLE:
        if (q) q->enabled = m->u.state.num;
        break;
    case VU_GET_CONFIG: {
        uint8_t cfg[8];
        uint32_t off = m->u.config.off, size = m->u.config.size;
        if (size > sizeof(m->u.config.data)) size = sizeof(m->u.config.data);
        memcpy(cfg, &d->cid, 8);
        for (uint32_t i = 0; i < size; i++)
            m->u.config.data[i] = off + i < 8 ? cfg[off + i] : 0;
        m->u.config.size = size;
        m->size = 12 + size;
        send = true;
        break;
    }
    case VU_SET_OWNER:
    case VU_RESET_OWNER:
    case VU_SET_PROTOCOL_FEATURES:
        break;
    default:
        /* Nothing a front-end sends without a negotiated feature needs
         * action here, and an error reply makes it abort the VM. */
        say("ignoring vhost-user request %u", m->request);
        break;
    }
    for (int i = used_fds; i < nfds; i++) close(fds[i]);
    if (!send && ack) {
        m->u.u64 = 0;
        m->size = 8;
        send = true;
    }
    return send ? write_full(d->vhost, m, 12 + m->size) : 0;
}

static void dev_reset(struct dev *d)
{
    for (int i = 0; i < NUM_QUEUES; i++) {
        vq_set_fd(&d->vq[i].kick, -1);
        vq_set_fd(&d->vq[i].call, -1);
        memset(&d->vq[i], 0, sizeof(d->vq[i]));
        d->vq[i].kick = d->vq[i].call = -1;
    }
    unmap_all(d);
    for (int i = 0; i < MAX_CONNS; i++)
        if (d->conn[i].st != CS_FREE) conn_free(&d->conn[i]);
}

static int listen_unix(const char *path, int backlog)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        say("path too long: %s", path);
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, backlog) < 0) {
        say("listen on %s: %s", path, strerror(errno));
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void drain_kick(int fd)
{
    uint64_t v;
    if (read(fd, &v, 8) < 0 && errno != EAGAIN)
        say("kick: %s", strerror(errno));
}

static void serve(struct dev *d)
{
    struct pollfd p[4 + MAX_CONNS];
    struct conn *pc[4 + MAX_CONNS];
    for (;;) {
        bool rx_space = vq_has_avail(&d->vq[RXQ]);
        int n = 0;
        p[n].fd = d->vhost >= 0 ? d->vhost : d->vhost_listen;
        p[n++].events = POLLIN;
        p[n].fd = d->vq[RXQ].kick;
        p[n++].events = POLLIN;
        p[n].fd = d->vq[TXQ].kick;
        p[n++].events = POLLIN;
        p[n].fd = d->uds;
        p[n++].events = POLLIN;
        for (int i = 0; i < MAX_CONNS; i++) {
            struct conn *c = &d->conn[i];
            if (c->pend && rx_space) flush_pending(d, c);
            if (c->st == CS_FREE || c->fd < 0) continue;
            short ev = 0;
            if (c->st == CS_CMD) ev = POLLIN;
            if (c->st == CS_OPEN) {
                uint32_t credit =
                    c->peer_buf_alloc - (c->tx_cnt - c->peer_fwd_cnt);
                if (!c->host_eof && credit && rx_space) ev |= POLLIN;
                if (c->len) ev |= POLLOUT;
            }
            if (!ev) continue;
            p[n].fd = c->fd;
            p[n].events = ev;
            pc[n++] = c;
        }
        vq_call(&d->vq[RXQ]);
        vq_call(&d->vq[TXQ]);
        if (poll(p, n, -1) < 0) {
            if (errno == EINTR) continue;
            say("poll: %s", strerror(errno));
            return;
        }
        if (p[0].revents) {
            if (d->vhost < 0) {
                d->vhost = accept(d->vhost_listen, NULL, NULL);
                if (d->vhost >= 0) say("front-end connected");
            } else {
                struct vu_msg m;
                int fds[VU_MAX_FDS], nfds;
                if (vu_recv(d->vhost, &m, fds, &nfds) < 0 ||
                    vu_handle(d, &m, fds, nfds) < 0) {
                    say("front-end disconnected");
                    close(d->vhost);
                    d->vhost = -1;
                    dev_reset(d);
                    continue;
                }
            }
        }
        if (p[1].revents) drain_kick(p[1].fd);
        if (p[2].revents) {
            drain_kick(p[2].fd);
            process_tx(d);
        }
        if (p[3].revents) accept_host(d);
        for (int i = 4; i < n; i++) {
            struct conn *c = pc[i];
            short re = p[i].revents;
            if (c->st == CS_CMD) {
                if (re) read_cmd(d, c);
                continue;
            }
            if ((re & (POLLIN | POLLERR | POLLHUP)) &&
                (p[i].events & POLLIN) && c->st == CS_OPEN)
                host_to_guest(d, c);
            if ((re & (POLLOUT | POLLERR | POLLHUP)) && c->st == CS_OPEN &&
                c->len)
                conn_flush(c);
        }
    }
}

static int parse_vm(struct dev *d, char *arg, char **sock, char **uds)
{
    for (char *tok = strtok(arg, ","); tok; tok = strtok(NULL, ",")) {
        if (!strncmp(tok, "guest-cid=", 10)) d->cid = strtoull(tok + 10, 0, 10);
        else if (!strncmp(tok, "socket=", 7)) *sock = tok + 7;
        else if (!strncmp(tok, "uds-path=", 9)) *uds = tok + 9;
        else say("ignoring --vm option %s", tok);
    }
    return *sock && *uds && d->cid > VSOCK_HOST_CID ? 0 : -1;
}

int main(int argc, char **argv)
{
    static struct dev d;
    char *sock = NULL, *uds = NULL;
    int nvm = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vm") && i + 1 < argc && nvm++ == 0 &&
            parse_vm(&d, argv[++i], &sock, &uds) == 0)
            continue;
        fprintf(stderr, "usage: %s --vm guest-cid=<cid>,socket=<path>,"
                "uds-path=<path>  (one --vm only)\n", argv[0]);
        return 1;
    }
    if (!nvm) {
        fprintf(stderr, "usage: %s --vm ...\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    d.vhost = -1;
    d.next_port = 1024;
    for (int i = 0; i < MAX_CONNS; i++) d.conn[i].fd = -1;
    for (int i = 0; i < NUM_QUEUES; i++) d.vq[i].kick = d.vq[i].call = -1;
    /* run_cvd waits for vm.vsock first, then for vhost.socket. */
    d.uds = listen_unix(uds, SOMAXCONN);
    d.vhost_listen = listen_unix(sock, 1);
    if (d.uds < 0 || d.vhost_listen < 0) return 1;
    say("cid %llu, vhost-user on %s, host sockets on %s",
         (unsigned long long)d.cid, sock, uds);
    serve(&d);
    return 1;
}
