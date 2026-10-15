/*
 * router.c - config loading, the event loop, neighbor discovery (HELLO)
 * and LSA flooding.
 *
 * The daemon is single-threaded: one select() loop handles packet input and
 * timers. Everything that changes the topology just sets spf_dirty, and SPF
 * runs at most once per loop iteration, so a burst of LSAs costs one SPF.
 */
#include "minirouted.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

/* ---------- small helpers ---------- */

double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void logmsg(const struct router *r, const char *fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "%02ld.%03ld R%u: ", (long)(ts.tv_sec % 60), ts.tv_nsec / 1000000, r->id);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int parse_prefix(const char *s, struct prefix *p)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%s", s);
    char *slash = strchr(buf, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    struct in_addr a;
    char *end;
    long len = strtol(slash + 1, &end, 10);
    if (inet_pton(AF_INET, buf, &a) != 1 || *end || len < 0 || len > 32)
        return -1;
    uint32_t mask = len ? 0xFFFFFFFFu << (32 - len) : 0;
    p->addr = ntohl(a.s_addr) & mask;
    p->len = (uint8_t)len;
    return 0;
}

void format_prefix(const struct prefix *p, char *buf, size_t n)
{
    struct in_addr a = { .s_addr = htonl(p->addr) };
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    snprintf(buf, n, "%s/%u", ip, p->len);
}

static const char *state_name(enum nbr_state s)
{
    return s == NBR_TWOWAY ? "2-WAY" : s == NBR_INIT ? "INIT" : "DOWN";
}

/* ---------- config ----------
 *   router-id 1
 *   port 5001
 *   prefix 10.0.1.0/24
 *   neighbor <router-id> <udp-port> <cost>
 */
static int load_config(struct router *r, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';

        char kw[32], arg[64];
        unsigned a, b, c;
        if (sscanf(line, "%31s", kw) != 1)
            continue;

        if (!strcmp(kw, "router-id") && sscanf(line, "%*s %u", &a) == 1 && a > 0) {
            r->id = a;
        } else if (!strcmp(kw, "port") && sscanf(line, "%*s %u", &a) == 1 && a > 0 && a < 65536) {
            r->port = (uint16_t)a;
        } else if (!strcmp(kw, "prefix") && sscanf(line, "%*s %63s", arg) == 1 &&
                   r->npfx < MAX_PFX && parse_prefix(arg, &r->pfx[r->npfx]) == 0) {
            r->npfx++;
        } else if (!strcmp(kw, "neighbor") && sscanf(line, "%*s %u %u %u", &a, &b, &c) == 3 &&
                   r->nnbrs < MAX_NBRS && a > 0 && b > 0 && b < 65536 && c > 0) {
            struct neighbor *nb = &r->nbrs[r->nnbrs++];
            memset(nb, 0, sizeof *nb);
            nb->id = a;
            nb->port = (uint16_t)b;
            nb->cost = c;
        } else {
            fprintf(stderr, "%s:%d: cannot parse: %s", path, lineno, line);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    if (!r->id || !r->port) {
        fprintf(stderr, "%s: router-id and port are required\n", path);
        return -1;
    }
    return 0;
}

/* ---------- sockets ---------- */

static int open_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    return s;
}

static void send_to(const struct router *r, uint16_t port, const uint8_t *buf, size_t n)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (sendto(r->sock, buf, n, 0, (struct sockaddr *)&a, sizeof a) < 0 && errno != ECONNREFUSED)
        logmsg(r, "sendto port %u: %s", port, strerror(errno));
}

/* ---------- link-state database ---------- */

static struct neighbor *find_nbr(struct router *r, uint32_t id)
{
    for (int i = 0; i < r->nnbrs; i++)
        if (r->nbrs[i].id == id)
            return &r->nbrs[i];
    return NULL;
}

static struct lsa *lsdb_find(struct router *r, uint32_t origin)
{
    for (int i = 0; i < r->nlsa; i++)
        if (r->lsdb[i].origin == origin)
            return &r->lsdb[i];
    return NULL;
}

static struct lsa *lsdb_get_or_add(struct router *r, uint32_t origin)
{
    struct lsa *l = lsdb_find(r, origin);
    if (l || r->nlsa >= MAX_ROUTERS)
        return l;
    l = &r->lsdb[r->nlsa++];
    memset(l, 0, sizeof *l);
    l->origin = origin;
    return l;
}

static int lsa_same(const struct lsa *a, const struct lsa *b)
{
    if (a->nlinks != b->nlinks || a->npfx != b->npfx)
        return 0;
    for (int i = 0; i < a->nlinks; i++)
        if (a->links[i].nbr != b->links[i].nbr || a->links[i].cost != b->links[i].cost)
            return 0;
    for (int i = 0; i < a->npfx; i++)
        if (a->pfx[i].addr != b->pfx[i].addr || a->pfx[i].len != b->pfx[i].len)
            return 0;
    return 1;
}

static void send_lsa(const struct router *r, const struct neighbor *nb, const struct lsa *l)
{
    uint8_t buf[MAX_PKT];
    size_t n = encode_lsa(buf, sizeof buf, r->id, l);
    if (n)
        send_to(r, nb->port, buf, n);
}

/* Send an LSA to every 2-way neighbor except the one we got it from. */
static void flood_lsa(const struct router *r, const struct lsa *l, uint32_t except)
{
    for (int i = 0; i < r->nnbrs; i++)
        if (r->nbrs[i].state == NBR_TWOWAY && r->nbrs[i].id != except)
            send_lsa(r, &r->nbrs[i], l);
}

/* Build a fresh LSA describing our current 2-way links and flood it. */
static void originate_lsa(struct router *r, double t, const char *why)
{
    struct lsa *l = lsdb_get_or_add(r, r->id);
    if (!l)
        return;
    l->seq = ++r->my_seq;
    l->nlinks = 0;
    for (int i = 0; i < r->nnbrs; i++) {
        const struct neighbor *nb = &r->nbrs[i];
        if (nb->state == NBR_TWOWAY)
            l->links[l->nlinks++] = (struct lsa_link){ nb->id, nb->cost };
    }
    l->npfx = (uint16_t)r->npfx;
    memcpy(l->pfx, r->pfx, sizeof(struct prefix) * (size_t)r->npfx);
    l->installed = t;

    r->last_originate = t;
    r->spf_dirty = 1;
    logmsg(r, "originate LSA seq=%u links=%u (%s)", l->seq, l->nlinks, why);
    flood_lsa(r, l, 0);
}

/* ---------- packet handlers ---------- */

static void handle_hello(struct router *r, struct neighbor *nb, const uint8_t *buf, size_t n, double t)
{
    uint32_t seen[MAX_NBRS];
    int nseen;
    if (decode_hello(buf, n, seen, MAX_NBRS, &nseen) < 0) {
        r->bad_packets++;
        return;
    }

    int sees_me = 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == r->id)
            sees_me = 1;

    enum nbr_state old = nb->state;
    nb->last_hello = t;
    nb->state = sees_me ? NBR_TWOWAY : NBR_INIT;
    if (old == nb->state)
        return;

    logmsg(r, "neighbor R%u %s -> %s", nb->id, state_name(old), state_name(nb->state));
    r->hello_now = 1; /* answer right away so the other side reaches 2-way fast */

    if (nb->state == NBR_TWOWAY) {
        /* Database sync (simplified): hand the new neighbor everything we know. */
        for (int i = 0; i < r->nlsa; i++)
            send_lsa(r, nb, &r->lsdb[i]);
    }
    if (old == NBR_TWOWAY || nb->state == NBR_TWOWAY)
        originate_lsa(r, t, nb->state == NBR_TWOWAY ? "adjacency up" : "adjacency down");
}

static void handle_lsa(struct router *r, uint32_t sender, const uint8_t *buf, size_t n, double t)
{
    struct lsa in;
    if (decode_lsa(buf, n, &in) < 0) {
        r->bad_packets++;
        return;
    }
    in.installed = t;

    if (in.origin == r->id) {
        /* A copy of our own LSA from a previous run (before a restart) can
         * still be in the network. If its sequence number is higher, or the
         * same but with different contents, everyone would treat our new LSA
         * as old news and ignore it, so jump past it. (Same seq and same
         * contents is just our current LSA flooded back around a loop.) */
        const struct lsa *mine = lsdb_find(r, r->id);
        if (in.seq > r->my_seq || (in.seq == r->my_seq && mine && !lsa_same(&in, mine))) {
            logmsg(r, "saw own LSA seq=%u from a previous run, jumping ahead", in.seq);
            r->my_seq = in.seq;
            originate_lsa(r, t, "seq recovery");
        }
        return;
    }

    struct lsa *cur = lsdb_find(r, in.origin);
    if (cur && in.seq <= cur->seq)
        return; /* duplicate or older: already flooded, stop here */

    struct lsa *slot = cur ? cur : lsdb_get_or_add(r, in.origin);
    if (!slot)
        return;
    *slot = in;
    flood_lsa(r, slot, sender);
    r->spf_dirty = 1;
}

static void handle_packet(struct router *r, const uint8_t *buf, size_t n, double t)
{
    uint8_t type;
    uint32_t sender;
    struct neighbor *nb;

    if (decode_header(buf, n, &type, &sender) < 0 || !(nb = find_nbr(r, sender))) {
        if (r->bad_packets++ % 100 == 0) /* rate-limit the log */
            logmsg(r, "dropped bad packet (%zu bytes, %lu dropped so far)", n, r->bad_packets);
        return;
    }
    if (type == MSG_HELLO)
        handle_hello(r, nb, buf, n, t);
    else if (type == MSG_LSA)
        handle_lsa(r, sender, buf, n, t);
    else
        r->bad_packets++;
}

/* ---------- timers ---------- */

static void send_hellos(const struct router *r)
{
    uint8_t buf[MAX_PKT];
    size_t n = encode_hello(buf, sizeof buf, r);
    for (int i = 0; i < r->nnbrs; i++)
        send_to(r, r->nbrs[i].port, buf, n);
}

static void check_dead_neighbors(struct router *r, double t)
{
    int lost_adjacency = 0;
    for (int i = 0; i < r->nnbrs; i++) {
        struct neighbor *nb = &r->nbrs[i];
        if (nb->state != NBR_DOWN && t - nb->last_hello > DEAD_INTERVAL) {
            logmsg(r, "neighbor R%u %s -> DOWN (no hello for %.0fs)", nb->id,
                   state_name(nb->state), DEAD_INTERVAL);
            if (nb->state == NBR_TWOWAY)
                lost_adjacency = 1;
            nb->state = NBR_DOWN;
        }
    }
    if (lost_adjacency)
        originate_lsa(r, t, "neighbor dead");
}

static void age_lsdb(struct router *r, double t)
{
    for (int i = 0; i < r->nlsa;) {
        struct lsa *l = &r->lsdb[i];
        if (l->origin != r->id && t - l->installed > LSA_MAX_AGE) {
            logmsg(r, "LSA from R%u reached max age, removing", l->origin);
            r->lsdb[i] = r->lsdb[--r->nlsa];
            r->spf_dirty = 1;
        } else {
            i++;
        }
    }
}

/* ---------- main ---------- */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s -c CONFIG [-o OUTDIR] [-k IFNAME]\n"
                    "  -o  directory for R<id>.routes (default .)\n"
                    "  -k  install routes in kernel table 100+<id> via IFNAME\n", prog);
}

int main(int argc, char **argv)
{
    static struct router r;
    const char *cfg = NULL;
    r.outdir = ".";

    int opt;
    while ((opt = getopt(argc, argv, "c:o:k:h")) != -1) {
        switch (opt) {
        case 'c': cfg = optarg; break;
        case 'o': r.outdir = optarg; break;
        case 'k': r.kernel_if = optarg; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!cfg) {
        usage(argv[0]);
        return 2;
    }
    if (load_config(&r, cfg) < 0)
        return 1;
    if (r.kernel_if && kroute_init() < 0) {
        perror("netlink");
        return 1;
    }
    if ((r.sock = open_socket(r.port)) < 0)
        return 1;

    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    logmsg(&r, "up on port %u, %d neighbors, %d prefixes%s%s", r.port, r.nnbrs, r.npfx,
           r.kernel_if ? ", kernel via " : "", r.kernel_if ? r.kernel_if : "");

    double t = now();
    double next_hello = t;
    originate_lsa(&r, t, "startup");

    while (!stop_flag) {
        t = now();
        if (t >= next_hello || r.hello_now) {
            send_hellos(&r);
            next_hello = t + HELLO_INTERVAL;
            r.hello_now = 0;
        }
        check_dead_neighbors(&r, t);
        if (t - r.last_originate >= LSA_REFRESH)
            originate_lsa(&r, t, "refresh");
        age_lsdb(&r, t);
        if (r.spf_dirty) {
            r.spf_dirty = 0;
            rib_sync(&r);
        }

        double wait = next_hello - now();
        if (wait > 0.1)
            wait = 0.1;
        if (wait < 0)
            wait = 0;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(r.sock, &rf);
        struct timeval tv = { 0, (long)(wait * 1e6) };
        int rc = select(r.sock + 1, &rf, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (rc > 0) {
            uint8_t buf[MAX_PKT];
            ssize_t n;
            while ((n = recv(r.sock, buf, sizeof buf, 0)) > 0)
                handle_packet(&r, buf, (size_t)n, now());
        }
    }

    logmsg(&r, "shutting down, withdrawing %d routes", r.ninstalled);
    rib_shutdown(&r);
    close(r.sock);
    return 0;
}
