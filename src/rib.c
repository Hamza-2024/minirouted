/*
 * rib.c - the Routing Information Base.
 *
 * After every SPF run we have the routes we *want*. We compare them with the
 * routes we *installed* last time and apply only the difference (add,
 * replace, delete). Pushing the whole table on every change would hammer the
 * kernel and cause traffic blips; diffing is how real routing stacks
 * reconcile their state with the kernel's forwarding table.
 *
 * The current table is also written to <outdir>/R<id>.routes so tests and
 * humans can see it without needing kernel access.
 */
#include "minirouted.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_route(const void *a, const void *b)
{
    const struct route *x = a, *y = b;
    if (x->dst.addr != y->dst.addr)
        return x->dst.addr < y->dst.addr ? -1 : 1;
    return (int)x->dst.len - (int)y->dst.len;
}

static const struct route *find_dst(const struct route *tab, int n, const struct prefix *p)
{
    for (int i = 0; i < n; i++)
        if (tab[i].dst.addr == p->addr && tab[i].dst.len == p->len)
            return &tab[i];
    return NULL;
}

static void kernel_apply(struct router *r, int add, const struct route *rt)
{
    if (!r->kernel_if)
        return;
    int err = kroute_apply(add, r->kernel_if, KERNEL_TABLE_BASE + r->id, rt);
    if (err) {
        char p[INET_ADDRSTRLEN + 4];
        format_prefix(&rt->dst, p, sizeof p);
        logmsg(r, "kernel %s %s failed: %s", add ? "add" : "delete", p, strerror(err));
    }
}

static void write_routes_file(const struct router *r)
{
    char path[512], tmp[520];
    snprintf(path, sizeof path, "%s/R%u.routes", r->outdir, r->id);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        logmsg(r, "cannot write %s", tmp);
        return;
    }
    for (int i = 0; i < r->ninstalled; i++) {
        char p[INET_ADDRSTRLEN + 4];
        format_prefix(&r->installed[i].dst, p, sizeof p);
        fprintf(f, "%s R%u %u\n", p, r->installed[i].nexthop, r->installed[i].metric);
    }
    fclose(f);
    rename(tmp, path); /* atomic: readers never see a half-written file */
}

void rib_sync(struct router *r)
{
    static struct route want[MAX_ROUTES];
    int nw = spf_compute(r, want, MAX_ROUTES);
    qsort(want, (size_t)nw, sizeof *want, cmp_route);

    int changes = 0;
    char p[INET_ADDRSTRLEN + 4];

    for (int i = 0; i < nw; i++) {
        const struct route *old = find_dst(r->installed, r->ninstalled, &want[i].dst);
        format_prefix(&want[i].dst, p, sizeof p);
        if (!old) {
            logmsg(r, "route + %s via R%u metric %u", p, want[i].nexthop, want[i].metric);
            kernel_apply(r, 1, &want[i]);
            changes++;
        } else if (old->nexthop != want[i].nexthop || old->metric != want[i].metric) {
            logmsg(r, "route ~ %s via R%u metric %u (was via R%u metric %u)", p,
                   want[i].nexthop, want[i].metric, old->nexthop, old->metric);
            kernel_apply(r, 1, &want[i]); /* NLM_F_REPLACE swaps it in place */
            changes++;
        }
    }
    for (int i = 0; i < r->ninstalled; i++) {
        if (!find_dst(want, nw, &r->installed[i].dst)) {
            format_prefix(&r->installed[i].dst, p, sizeof p);
            logmsg(r, "route - %s (unreachable)", p);
            kernel_apply(r, 0, &r->installed[i]);
            changes++;
        }
    }

    memcpy(r->installed, want, (size_t)nw * sizeof *want);
    r->ninstalled = nw;
    write_routes_file(r);
    if (changes)
        logmsg(r, "SPF done: %d routes, %d changed", nw, changes);
}

void rib_shutdown(struct router *r)
{
    /* Withdraw our routes so a stopped daemon leaves no stale state behind. */
    for (int i = 0; i < r->ninstalled; i++)
        kernel_apply(r, 0, &r->installed[i]);
    r->ninstalled = 0;
    write_routes_file(r);
}
