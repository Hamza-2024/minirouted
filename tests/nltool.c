/*
 * nltool - tiny rtnetlink helper used by the test harness, so the tests
 * don't depend on iproute2 being installed.
 *
 *   nltool up IFNAME        bring an interface up        (ip link set IF up)
 *   nltool dummy IFNAME     create a dummy interface     (ip link add IF type dummy)
 *   nltool show TABLE       list minirouted routes        (ip route show table T proto 99)
 */
#include "../src/minirouted.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fd;
static uint32_t seq;

static struct rtattr *add_attr(struct nlmsghdr *n, int type, const void *data, size_t len)
{
    struct rtattr *rta = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)RTA_LENGTH(len);
    if (len)
        memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(rta->rta_len);
    return rta;
}

static int request(struct nlmsghdr *n)
{
    n->nlmsg_seq = ++seq;
    if (send(fd, n, n->nlmsg_len, 0) < 0)
        return errno;
    uint32_t buf[2048];
    for (;;) {
        int len = (int)recv(fd, buf, sizeof buf, 0);
        if (len < 0)
            return errno;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len))
            if (h->nlmsg_seq == n->nlmsg_seq && h->nlmsg_type == NLMSG_ERROR)
                return -((struct nlmsgerr *)NLMSG_DATA(h))->error;
    }
}

struct linkreq {
    struct nlmsghdr n;
    struct ifinfomsg i;
    char attrs[256];
};

static int link_up(const char *ifname)
{
    struct linkreq req;
    memset(&req, 0, sizeof req);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.i.ifi_family = AF_UNSPEC;
    req.i.ifi_index = (int)if_nametoindex(ifname);
    req.i.ifi_flags = IFF_UP;
    req.i.ifi_change = IFF_UP;
    if (!req.i.ifi_index)
        return ENODEV;
    return request(&req.n);
}

static int add_dummy(const char *ifname)
{
    struct linkreq req;
    memset(&req, 0, sizeof req);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_NEWLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL;
    req.i.ifi_family = AF_UNSPEC;
    add_attr(&req.n, IFLA_IFNAME, ifname, strlen(ifname) + 1);
    struct rtattr *info = add_attr(&req.n, IFLA_LINKINFO, NULL, 0);
    add_attr(&req.n, IFLA_INFO_KIND, "dummy", 5);
    info->rta_len = (unsigned short)((char *)&req.n + req.n.nlmsg_len - (char *)info);
    int err = request(&req.n);
    return err ? err : link_up(ifname);
}

static int show(uint32_t table)
{
    struct {
        struct nlmsghdr n;
        struct rtmsg r;
    } req;
    memset(&req, 0, sizeof req);
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.n.nlmsg_type = RTM_GETROUTE;
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.n.nlmsg_seq = ++seq;
    req.r.rtm_family = AF_INET;
    if (send(fd, &req, req.n.nlmsg_len, 0) < 0)
        return errno;

    uint32_t buf[8192];
    for (;;) {
        int len = (int)recv(fd, buf, sizeof buf, 0);
        if (len < 0)
            return errno;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_DONE)
                return 0;
            if (h->nlmsg_type == NLMSG_ERROR)
                return -((struct nlmsgerr *)NLMSG_DATA(h))->error;
            struct rtmsg *rt = NLMSG_DATA(h);
            if (rt->rtm_protocol != RTPROT_MINIROUTED)
                continue;
            uint32_t tbl = rt->rtm_table, dst = 0, gw = 0;
            int alen = (int)RTM_PAYLOAD(h);
            for (struct rtattr *a = RTM_RTA(rt); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type == RTA_TABLE) memcpy(&tbl, RTA_DATA(a), 4);
                if (a->rta_type == RTA_DST) memcpy(&dst, RTA_DATA(a), 4);
                if (a->rta_type == RTA_GATEWAY) memcpy(&gw, RTA_DATA(a), 4);
            }
            if (tbl != table)
                continue;
            char d[INET_ADDRSTRLEN], g[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &dst, d, sizeof d);
            inet_ntop(AF_INET, &gw, g, sizeof g);
            printf("%s/%u via %s\n", d, rt->rtm_dst_len, g);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: nltool up|dummy IFNAME | nltool show TABLE\n");
        return 2;
    }
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct sockaddr_nl local = { .nl_family = AF_NETLINK };
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("netlink");
        return 1;
    }
    int err;
    if (!strcmp(argv[1], "up"))
        err = link_up(argv[2]);
    else if (!strcmp(argv[1], "dummy"))
        err = add_dummy(argv[2]);
    else if (!strcmp(argv[1], "show"))
        err = show((uint32_t)strtoul(argv[2], NULL, 10));
    else
        err = EINVAL;
    if (err) {
        fprintf(stderr, "nltool %s %s: %s\n", argv[1], argv[2], strerror(err));
        return 1;
    }
    return 0;
}
