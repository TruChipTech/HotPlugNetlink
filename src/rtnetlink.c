/*
 * rtnetlink.c — Capture network interface events via NETLINK_ROUTE.
 *
 * Subscribes to:
 *   RTMGRP_LINK         — interface up/down, add/del
 *   RTMGRP_IPV4_IFADDR  — IPv4 address add/del
 *   RTMGRP_IPV6_IFADDR  — IPv6 address add/del
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <arpa/inet.h>

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>

#include "color.h"
#include "rtnetlink.h"

#define RTNL_BUF 8192

int rtnetlink_open(void)
{
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_pid    = 0,
        .nl_groups = RTMGRP_LINK
                   | RTMGRP_IPV4_IFADDR
                   | RTMGRP_IPV6_IFADDR,
    };

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) {
        perror("rtnetlink socket");
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rtnetlink bind");
        close(fd);
        return -1;
    }

    return fd;
}

static void print_ts(void)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);
    printf(COL_BOLD "[%s] " COL_RESET, ts);
}

static const char *ifi_flags_str(unsigned int flags, char *buf, size_t len)
{
    buf[0] = '\0';
    if (flags & IFF_UP)        strncat(buf, "UP ",        len - strlen(buf) - 1);
    if (flags & IFF_RUNNING)   strncat(buf, "RUNNING ",   len - strlen(buf) - 1);
    if (flags & IFF_LOOPBACK)  strncat(buf, "LOOPBACK ",  len - strlen(buf) - 1);
    if (flags & IFF_BROADCAST) strncat(buf, "BROADCAST ",  len - strlen(buf) - 1);
    if (flags & IFF_MULTICAST) strncat(buf, "MULTICAST ", len - strlen(buf) - 1);
    if (flags & IFF_PROMISC)   strncat(buf, "PROMISC ",   len - strlen(buf) - 1);
    return buf;
}

static void handle_link(struct nlmsghdr *nlh)
{
    struct ifinfomsg *ifi = NLMSG_DATA(nlh);
    int rta_len = IFLA_PAYLOAD(nlh);

    const char *ifname = NULL;
    for (struct rtattr *rta = IFLA_RTA(ifi); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFLA_IFNAME) {
            ifname = RTA_DATA(rta);
            break;
        }
    }

    char flags_buf[128];
    ifi_flags_str(ifi->ifi_flags, flags_buf, sizeof(flags_buf));

    const char *op     = (nlh->nlmsg_type == RTM_NEWLINK) ? "NEW" : "DEL";
    const char *op_col = (nlh->nlmsg_type == RTM_NEWLINK) ? COL_GREEN : COL_RED;

    print_ts();
    printf(COL_YELLOW "RTNETLINK LINK " COL_RESET);
    printf("%s%-4s" COL_RESET " ", op_col, op);
    printf("iface=%-12s ", ifname ? ifname : "?");
    printf("idx=%-4d ", ifi->ifi_index);
    printf("flags=[%s]\n", flags_buf);
    fflush(stdout);
}

static void handle_addr(struct nlmsghdr *nlh)
{
    struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
    int rta_len = IFA_PAYLOAD(nlh);

    char addr_str[INET6_ADDRSTRLEN] = {0};
    char ifname[IF_NAMESIZE]        = {0};

    for (struct rtattr *rta = IFA_RTA(ifa); RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
            if (ifa->ifa_family == AF_INET)
                inet_ntop(AF_INET,  RTA_DATA(rta), addr_str, sizeof(addr_str));
            else
                inet_ntop(AF_INET6, RTA_DATA(rta), addr_str, sizeof(addr_str));
        }
        if (rta->rta_type == IFA_LABEL) {
            strncpy(ifname, RTA_DATA(rta), IF_NAMESIZE - 1);
        }
    }

    if (!ifname[0])
        if_indextoname(ifa->ifa_index, ifname);

    const char *op     = (nlh->nlmsg_type == RTM_NEWADDR) ? "ADD" : "DEL";
    const char *op_col = (nlh->nlmsg_type == RTM_NEWADDR) ? COL_GREEN : COL_RED;
    const char *family = (ifa->ifa_family == AF_INET) ? "IPv4" : "IPv6";

    print_ts();
    printf(COL_YELLOW "RTNETLINK ADDR " COL_RESET);
    printf("%s%-4s" COL_RESET " ", op_col, op);
    printf("%-4s ", family);
    printf("iface=%-12s ", ifname[0] ? ifname : "?");
    printf("addr=%s/%d\n", addr_str[0] ? addr_str : "?", ifa->ifa_prefixlen);
    fflush(stdout);
}

void rtnetlink_handle(int fd)
{
    char buf[RTNL_BUF];

    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (errno != EAGAIN && errno != EINTR)
            perror("rtnetlink recv");
        return;
    }

    for (struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
         NLMSG_OK(nlh, (unsigned int)n);
         nlh = NLMSG_NEXT(nlh, n))
    {
        if (nlh->nlmsg_type == NLMSG_DONE)  break;
        if (nlh->nlmsg_type == NLMSG_ERROR) continue;

        switch (nlh->nlmsg_type) {
        case RTM_NEWLINK:
        case RTM_DELLINK:
            handle_link(nlh);
            break;
        case RTM_NEWADDR:
        case RTM_DELADDR:
            handle_addr(nlh);
            break;
        default:
            break;
        }
    }
}
