/*
 * minirouted - a small OSPF-style link-state routing daemon.
 *
 * Each router process:
 *   1. discovers neighbors with HELLO packets (2-way check, dead interval),
 *   2. describes its own links in a Link-State Advertisement (LSA),
 *   3. floods LSAs so every router ends up with the same link-state database,
 *   4. runs Dijkstra (SPF) over that database to pick a next hop per prefix,
 *   5. diffs the result against what it installed last time and pushes only
 *      the changes into the Linux kernel routing table over rtnetlink.
 *
 * Routers talk over UDP on 127.0.0.1 so a whole network can run on one box.
 */
#ifndef MINIROUTED_H
#define MINIROUTED_H

#include <stddef.h>
#include <stdint.h>

#define MAX_NBRS     16
#define MAX_ROUTERS  64
#define MAX_PFX      8
#define MAX_ROUTES   (MAX_ROUTERS * MAX_PFX)
#define MAX_PKT      1500

/* Timers, in seconds. Real OSPF defaults are 10s / 40s / 30min / 60min. */
#define HELLO_INTERVAL 1.0
#define DEAD_INTERVAL  4.0
#define LSA_REFRESH    10.0
#define LSA_MAX_AGE    40.0

/* Kernel integration: router N installs into table 100+N, and next-hop
 * router M is reached at gateway 10.255.0.M on the given interface. */
#define KERNEL_TABLE_BASE 100u
#define KERNEL_GW_BASE    0x0AFF0000u /* 10.255.0.0 */
#define RTPROT_MINIROUTED 99          /* tags our routes in the kernel */

#define PROTO_VERSION 1
enum msg_type { MSG_HELLO = 1, MSG_LSA = 2 };
enum nbr_state { NBR_DOWN = 0, NBR_INIT, NBR_TWOWAY };

struct prefix {
    uint32_t addr; /* host byte order, host bits zeroed */
    uint8_t len;
};

struct neighbor {
    uint32_t id;
    uint16_t port;
    uint32_t cost;
    enum nbr_state state;
    double last_hello;
};

struct lsa_link {
    uint32_t nbr;
    uint32_t cost;
};

struct lsa {
    uint32_t origin; /* router that generated it */
    uint32_t seq;    /* higher = newer */
    uint16_t nlinks;
    struct lsa_link links[MAX_NBRS];
    uint16_t npfx;
    struct prefix pfx[MAX_PFX];
    double installed; /* local time we accepted it, for aging */
};

struct route {
    struct prefix dst;
    uint32_t nexthop; /* router id of the neighbor to forward to */
    uint32_t metric;
};

struct router {
    uint32_t id;
    uint16_t port;
    int sock;
    const char *outdir;
    const char *kernel_if; /* NULL = don't touch the kernel */

    struct prefix pfx[MAX_PFX];
    int npfx;
    struct neighbor nbrs[MAX_NBRS];
    int nnbrs;

    uint32_t my_seq;
    double last_originate;
    struct lsa lsdb[MAX_ROUTERS];
    int nlsa;

    struct route installed[MAX_ROUTES];
    int ninstalled;

    int spf_dirty;
    int hello_now;
    unsigned long bad_packets;
};

/* router.c */
double now(void);
void logmsg(const struct router *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int parse_prefix(const char *s, struct prefix *p);
void format_prefix(const struct prefix *p, char *buf, size_t n);

/* proto.c - wire format (all fields big-endian) */
size_t encode_hello(uint8_t *buf, size_t cap, const struct router *r);
size_t encode_lsa(uint8_t *buf, size_t cap, uint32_t sender, const struct lsa *l);
int decode_header(const uint8_t *buf, size_t n, uint8_t *type, uint32_t *sender);
int decode_hello(const uint8_t *buf, size_t n, uint32_t *seen, int max, int *nseen);
int decode_lsa(const uint8_t *buf, size_t n, struct lsa *out);

/* spf.c */
int spf_compute(const struct router *r, struct route *out, int max);

/* rib.c */
void rib_sync(struct router *r);
void rib_shutdown(struct router *r);

/* netlink.c */
int kroute_init(void);
int kroute_apply(int add, const char *ifname, uint32_t table, const struct route *rt);

#endif
