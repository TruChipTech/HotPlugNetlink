/*
 * main.c — HotPlugNetlink: monitor Linux kernel events in real-time
 *           using Netlink sockets multiplexed via epoll(7).
 *
 * Three Netlink families are used:
 *   NETLINK_KOBJECT_UEVENT — hotplug/udev device events
 *   NETLINK_ROUTE          — network link and address events
 *   NETLINK_CONNECTOR      — process lifecycle events (root only)
 *
 * Usage:
 *   sudo ./hotplug_monitor [-u] [-r] [-p] [-h]
 *
 * Options
 *   -u   disable uevent (hotplug) monitoring
 *   -r   disable rtnetlink (network) monitoring
 *   -p   disable process-event monitoring
 *   -h   show help and exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>

#include <sys/epoll.h>

#include "color.h"
#include "uevent.h"
#include "rtnetlink.h"
#include "proc_events.h"

#define MAX_EVENTS 8

static volatile int running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void print_banner(int do_uevent, int do_rtnl, int do_proc)
{
    printf(COL_BOLD COL_CYAN
           "╔══════════════════════════════════════════════════════╗\n"
           "║          HotPlugNetlink — Kernel Event Monitor       ║\n"
           "╚══════════════════════════════════════════════════════╝\n"
           COL_RESET);
    printf("Monitoring: ");
    if (do_uevent) printf(COL_GREEN  "uevent " COL_RESET);
    if (do_rtnl)   printf(COL_YELLOW "rtnetlink " COL_RESET);
    if (do_proc)   printf(COL_MAGENTA "proc_events " COL_RESET);
    printf("\nPress " COL_BOLD "Ctrl+C" COL_RESET " to stop.\n\n");
    fflush(stdout);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-u] [-r] [-p] [-h]\n"
        "  -u   disable uevent (hotplug) monitor\n"
        "  -r   disable rtnetlink (network) monitor\n"
        "  -p   disable process-event monitor\n"
        "  -h   show this help\n",
        prog);
}

int main(int argc, char *argv[])
{
    int do_uevent = 1;
    int do_rtnl   = 1;
    int do_proc   = 1;

    int opt;
    while ((opt = getopt(argc, argv, "hurp")) != -1) {
        switch (opt) {
        case 'u': do_uevent = 0; break;
        case 'r': do_rtnl   = 0; break;
        case 'p': do_proc   = 0; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    print_banner(do_uevent, do_rtnl, do_proc);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    typedef void (*handler_fn)(int);

    struct {
        int        fd;
        handler_fn fn;
    } monitors[3];
    int nmon = 0;

    if (do_uevent) {
        int fd = uevent_open();
        if (fd >= 0) {
            monitors[nmon].fd = fd;
            monitors[nmon].fn = uevent_handle;
            nmon++;
        }
    }

    if (do_rtnl) {
        int fd = rtnetlink_open();
        if (fd >= 0) {
            monitors[nmon].fd = fd;
            monitors[nmon].fn = rtnetlink_handle;
            nmon++;
        }
    }

    if (do_proc) {
        int fd = proc_events_open();
        if (fd >= 0) {
            monitors[nmon].fd = fd;
            monitors[nmon].fn = proc_events_handle;
            nmon++;
        }
    }

    if (nmon == 0) {
        fprintf(stderr, "No monitors could be opened. Exiting.\n");
        close(epfd);
        return 1;
    }

    /* Register all open fds with epoll */
    for (int i = 0; i < nmon; i++) {
        struct epoll_event ev = {
            .events  = EPOLLIN,
            .data.fd = monitors[i].fd,
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, monitors[i].fd, &ev) < 0) {
            perror("epoll_ctl ADD");
            return 1;
        }
    }

    struct epoll_event events[MAX_EVENTS];

    while (running) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 500 /* ms */);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int efd = events[i].data.fd;
            for (int j = 0; j < nmon; j++) {
                if (monitors[j].fd == efd) {
                    monitors[j].fn(efd);
                    break;
                }
            }
        }
    }

    printf("\n" COL_BOLD "Shutting down…" COL_RESET "\n");

    for (int i = 0; i < nmon; i++)
        close(monitors[i].fd);
    close(epfd);

    return 0;
}
