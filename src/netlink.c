/*
 * netlink.c - install and remove routes in the Linux kernel via rtnetlink.
 *
 * This is the same interface `ip route` uses. A request is a netlink header,
 * an rtmsg describing the route, then a list of type-length-value
 * attributes (destination, gateway, output interface, table). We ask for an
 * ACK on every request and return the kernel's errno, so failures are seen.
 *
 * Routes are tagged with our own protocol number (RTPROT_MINIROUTED) so they
 * are easy to tell apart from routes added by anything else.
 */
#include "minirouted.h"

#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

static int nl_fd = -1;
static uint32_t nl_seq;

int kroute_init(void)
{
    nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nl_fd < 0)
        return -1;
    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    if (bind(nl_fd, (struct sockaddr *)&local, sizeof local) < 0) {
        close(nl_fd);
        nl_fd = -1;
        return -1;
    }
    return 0;
}

static void add_attr(struct nlmsghdr *n, int type, const void *data, size_t len)
{
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

/* Send one request and wait for its ACK. Returns 0 or a positive errno. */
static int nl_request(struct nlmsghdr *req)
{
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
    if (sendto(nl_fd, req, req->nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof kernel) < 0)
        return errno;

    uint32_t buf[2048]; /* uint32_t keeps the buffer aligned for nlmsghdr */
    for (;;) {
        ssize_t got = recv(nl_fd, buf, sizeof buf, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        int len = (int)got;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_seq != req->nlmsg_seq || h->nlmsg_type != NLMSG_ERROR)
                continue;
            const struct nlmsgerr *e = NLMSG_DATA(h);
            return -e->error; /* kernel sends 0 on success, -errno on failure */
        }
    }
}

int kroute_apply(int add, const char *ifname, uint32_t table, const struct route *rt)
{
    if (nl_fd < 0)
        return EBADF;

    struct {
        struct nlmsghdr n;
        struct rtmsg r;
        char attrs[256];
    } req;
    memset(&req, 0, sizeof req);

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.n.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (add)
        req.n.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    req.n.nlmsg_seq = ++nl_seq;

    req.r.rtm_family = AF_INET;
    req.r.rtm_dst_len = rt->dst.len;
    req.r.rtm_table = RT_TABLE_UNSPEC; /* real table id goes in RTA_TABLE */
    req.r.rtm_protocol = RTPROT_MINIROUTED;
    req.r.rtm_scope = RT_SCOPE_UNIVERSE;
    req.r.rtm_type = RTN_UNICAST;

    uint32_t dst = htonl(rt->dst.addr);
    add_attr(&req.n, RTA_DST, &dst, 4);
    add_attr(&req.n, RTA_TABLE, &table, 4);

    if (add) {
        unsigned oif = if_nametoindex(ifname);
        if (!oif)
            return ENODEV;
        uint32_t gw = htonl(KERNEL_GW_BASE | (rt->nexthop & 0xff));
        /* ONLINK: treat the gateway as directly reachable on this interface */
        req.r.rtm_flags = RTNH_F_ONLINK;
        add_attr(&req.n, RTA_GATEWAY, &gw, 4);
        add_attr(&req.n, RTA_OIF, &oif, 4);
    }
    return nl_request(&req.n);
}
