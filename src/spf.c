/*
 * spf.c - Shortest Path First (Dijkstra) over the link-state database.
 *
 * Nodes are routers (one per LSA). An edge u->v is used only if u's LSA
 * lists v AND v's LSA lists u. This "bidirectional check" is what makes a
 * crashed router drop out of the topology even though its old LSA is still
 * sitting in the database: its neighbors stop advertising the link to it.
 *
 * We track the first hop (the neighbor of *this* router) for every node,
 * which is all a router needs to forward. With equal-cost paths we pick the
 * neighbor with the lowest router id so results are deterministic.
 *
 * N is at most MAX_ROUTERS, so the simple O(N^2) array version is fine;
 * a binary heap would make it O(E log N) for large networks.
 */
#include "minirouted.h"

#include <stdint.h>
#include <string.h>

#define INF UINT32_MAX

static int lsdb_index(const struct router *r, uint32_t origin)
{
    for (int i = 0; i < r->nlsa; i++)
        if (r->lsdb[i].origin == origin)
            return i;
    return -1;
}

static int lsa_lists(const struct lsa *l, uint32_t nbr)
{
    for (int i = 0; i < l->nlinks; i++)
        if (l->links[i].nbr == nbr)
            return 1;
    return 0;
}

static int is_local_prefix(const struct router *r, const struct prefix *p)
{
    for (int i = 0; i < r->npfx; i++)
        if (r->pfx[i].addr == p->addr && r->pfx[i].len == p->len)
            return 1;
    return 0;
}

int spf_compute(const struct router *r, struct route *out, int max)
{
    uint32_t dist[MAX_ROUTERS], nexthop[MAX_ROUTERS];
    int done[MAX_ROUTERS];
    int n = r->nlsa;

    int self = lsdb_index(r, r->id);
    if (self < 0)
        return 0;

    for (int i = 0; i < n; i++) {
        dist[i] = INF;
        nexthop[i] = 0;
        done[i] = 0;
    }
    dist[self] = 0;

    for (;;) {
        /* Pick the closest router we haven't finalised yet. */
        int u = -1;
        for (int i = 0; i < n; i++)
            if (!done[i] && dist[i] != INF && (u < 0 || dist[i] < dist[u]))
                u = i;
        if (u < 0)
            break;
        done[u] = 1;

        const struct lsa *lu = &r->lsdb[u];
        for (int k = 0; k < lu->nlinks; k++) {
            int v = lsdb_index(r, lu->links[k].nbr);
            if (v < 0 || done[v] || !lsa_lists(&r->lsdb[v], lu->origin))
                continue;

            uint64_t d64 = (uint64_t)dist[u] + lu->links[k].cost;
            uint32_t d = d64 >= INF ? INF - 1 : (uint32_t)d64;
            uint32_t hop = (u == self) ? r->lsdb[v].origin : nexthop[u];

            if (d < dist[v] || (d == dist[v] && hop < nexthop[v])) {
                dist[v] = d;
                nexthop[v] = hop;
            }
        }
    }

    /* Turn reachable routers into prefix routes. If two routers advertise
     * the same prefix, the closer one wins. */
    int cnt = 0;
    for (int v = 0; v < n; v++) {
        if (v == self || dist[v] == INF)
            continue;
        const struct lsa *lv = &r->lsdb[v];
        for (int k = 0; k < lv->npfx; k++) {
            const struct prefix *p = &lv->pfx[k];
            if (is_local_prefix(r, p))
                continue; /* directly connected, nothing to install */

            int j;
            for (j = 0; j < cnt; j++)
                if (out[j].dst.addr == p->addr && out[j].dst.len == p->len)
                    break;
            if (j < cnt) {
                if (dist[v] < out[j].metric) {
                    out[j].nexthop = nexthop[v];
                    out[j].metric = dist[v];
                }
            } else if (cnt < max) {
                out[cnt].dst = *p;
                out[cnt].nexthop = nexthop[v];
                out[cnt].metric = dist[v];
                cnt++;
            }
        }
    }
    return cnt;
}
