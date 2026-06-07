/*
 * proc_events.c — Capture process lifecycle events via NETLINK_CONNECTOR
 *                 using the kernel's CN_IDX_PROC connector.
 *
 * Requires root (or CAP_NET_ADMIN) to subscribe.
 * Events: fork, exec, exit, uid/gid change, coredump, ptrace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>

#include "color.h"
#include "proc_events.h"

#define PROC_BUF 4096

/* Build a subscribe/unsubscribe control message.
 * We use a flat byte buffer to avoid -Wpedantic complaints about
 * embedding structs that contain flexible (zero-length) array members. */
static int send_proc_subscribe(int fd, int subscribe)
{
    enum proc_cn_mcast_op op = subscribe
        ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;

    size_t total = NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(op));
    char buf[256];
    memset(buf, 0, total);

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len   = (uint32_t)total;
    nlh->nlmsg_type  = NLMSG_DONE;
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_seq   = 1;
    nlh->nlmsg_pid   = (uint32_t)getpid();

    struct cn_msg *cn = NLMSG_DATA(nlh);
    cn->id.idx = CN_IDX_PROC;
    cn->id.val = CN_VAL_PROC;
    cn->seq    = 1;
    cn->ack    = 0;
    cn->len    = sizeof(op);
    memcpy(cn->data, &op, sizeof(op));

    if (send(fd, buf, total, 0) < 0) {
        perror("proc_events send subscribe");
        return -1;
    }
    return 0;
}

int proc_events_open(void)
{
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid    = (uint32_t)getpid();
    addr.nl_groups = CN_IDX_PROC;

    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (fd < 0) {
        perror("proc_events socket");
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        /* EPERM means no CAP_NET_ADMIN; skip gracefully */
        if (errno == EPERM || errno == EACCES) {
            fprintf(stderr,
                COL_YELLOW "WARNING: proc_events requires root/CAP_NET_ADMIN — "
                "process events disabled.\n" COL_RESET);
        } else {
            perror("proc_events bind");
        }
        close(fd);
        return -1;
    }

    if (send_proc_subscribe(fd, 1) < 0) {
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

void proc_events_handle(int fd)
{
    char buf[PROC_BUF];

    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (errno != EAGAIN && errno != EINTR)
            perror("proc_events recv");
        return;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    if (!NLMSG_OK(nlh, (unsigned int)n)) return;

    struct cn_msg *cn = NLMSG_DATA(nlh);
    if (cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC) return;

    struct proc_event *ev = (struct proc_event *)cn->data;

    print_ts();
    printf(COL_MAGENTA "PROC_EVENT " COL_RESET);

    switch (ev->what) {
    case PROC_EVENT_FORK:
        printf(COL_GREEN "FORK   " COL_RESET
               "parent pid=%-6d tgid=%-6d  -> child pid=%-6d tgid=%-6d\n",
               ev->event_data.fork.parent_pid,
               ev->event_data.fork.parent_tgid,
               ev->event_data.fork.child_pid,
               ev->event_data.fork.child_tgid);
        break;

    case PROC_EVENT_EXEC:
        printf(COL_CYAN "EXEC   " COL_RESET
               "pid=%-6d tgid=%-6d\n",
               ev->event_data.exec.process_pid,
               ev->event_data.exec.process_tgid);
        break;

    case PROC_EVENT_EXIT:
        printf(COL_RED "EXIT   " COL_RESET
               "pid=%-6d tgid=%-6d  exit_code=%d\n",
               ev->event_data.exit.process_pid,
               ev->event_data.exit.process_tgid,
               ev->event_data.exit.exit_code);
        break;

    case PROC_EVENT_UID:
        printf(COL_YELLOW "UID    " COL_RESET
               "pid=%-6d  uid %u -> %u\n",
               ev->event_data.id.process_pid,
               ev->event_data.id.r.ruid,
               ev->event_data.id.e.euid);
        break;

    case PROC_EVENT_GID:
        printf(COL_YELLOW "GID    " COL_RESET
               "pid=%-6d  gid %u -> %u\n",
               ev->event_data.id.process_pid,
               ev->event_data.id.r.rgid,
               ev->event_data.id.e.egid);
        break;

    case PROC_EVENT_COREDUMP:
        printf(COL_RED COL_BOLD "COREDUMP " COL_RESET
               "pid=%-6d tgid=%-6d\n",
               ev->event_data.coredump.process_pid,
               ev->event_data.coredump.process_tgid);
        break;

    case PROC_EVENT_PTRACE:
        printf(COL_YELLOW "PTRACE " COL_RESET
               "pid=%-6d tracer_pid=%-6d\n",
               ev->event_data.ptrace.process_pid,
               ev->event_data.ptrace.tracer_pid);
        break;

    case PROC_EVENT_NONE:
        /* ACK from the kernel — ignore */
        break;

    default:
        printf("what=0x%x\n", ev->what);
        break;
    }

    fflush(stdout);
}
