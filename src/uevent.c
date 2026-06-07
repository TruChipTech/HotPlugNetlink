/*
 * uevent.c — Capture kernel hotplug/uevent messages via NETLINK_KOBJECT_UEVENT.
 *
 * The kernel sends null-separated "key=value" strings for every udev/kobject
 * event (device add/remove/change/bind/unbind).  The very first field is the
 * action@devpath header rather than a key=value pair.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <linux/netlink.h>

#include "color.h"
#include "uevent.h"

#define UEVENT_BUF  4096

int uevent_open(void)
{
    struct sockaddr_nl addr = {
        .nl_family = AF_NETLINK,
        .nl_pid    = 0,          /* kernel assigns unique pid */
        .nl_groups = 1,          /* multicast group 1 = uevents */
    };

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        perror("uevent socket");
        return -1;
    }

    /* Increase receive buffer so we don't drop events under burst load */
    int rcvbuf = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("uevent bind");
        close(fd);
        return -1;
    }

    return fd;
}

static const char *action_color(const char *action)
{
    if (!action)                   return COL_WHITE;
    if (!strcmp(action, "add"))    return COL_GREEN;
    if (!strcmp(action, "remove")) return COL_RED;
    if (!strcmp(action, "change")) return COL_YELLOW;
    if (!strcmp(action, "bind"))   return COL_CYAN;
    if (!strcmp(action, "unbind")) return COL_MAGENTA;
    return COL_WHITE;
}

void uevent_handle(int fd)
{
    char buf[UEVENT_BUF];

    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        if (errno != EAGAIN && errno != EINTR)
            perror("uevent recv");
        return;
    }
    buf[n] = '\0';

    /* Timestamp */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    /* The first string is "ACTION@DEVPATH", not a key=value pair */
    const char *header  = buf;
    const char *action  = NULL;
    const char *devpath = NULL;
    const char *subsystem = NULL;
    const char *devtype = NULL;
    const char *devname = NULL;

    /* Parse the header: "add@/devices/..." */
    char action_buf[64] = {0};
    const char *at = strchr(header, '@');
    if (at) {
        size_t alen = (size_t)(at - header);
        if (alen < sizeof(action_buf)) {
            memcpy(action_buf, header, alen);
            action_buf[alen] = '\0';
            action  = action_buf;
            devpath = at + 1;
        }
    }

    /* Walk the remaining null-separated key=value pairs */
    const char *p = buf;
    while (p < buf + n) {
        size_t len = strlen(p);
        if (len == 0) { p++; continue; }

        const char *eq = strchr(p, '=');
        if (eq) {
            const char *key = p;
            const char *val = eq + 1;
            size_t klen = (size_t)(eq - key);

            if      (!strncmp(key, "SUBSYSTEM", klen) && klen == 9)  subsystem = val;
            else if (!strncmp(key, "DEVTYPE",   klen) && klen == 7)  devtype   = val;
            else if (!strncmp(key, "DEVNAME",   klen) && klen == 7)  devname   = val;
        }
        p += len + 1;
    }

    const char *col = action_color(action);

    printf(COL_BOLD "[%s] " COL_RESET, ts);
    printf(COL_CYAN "UEVENT " COL_RESET);
    printf("%s%-8s" COL_RESET " ", col, action ? action : "?");

    if (subsystem) printf("subsystem=%-12s ", subsystem);
    if (devtype)   printf("devtype=%-10s ",   devtype);
    if (devname)   printf("devname=%-12s ",   devname);
    if (devpath)   printf(COL_BOLD "path=%s" COL_RESET, devpath);

    printf("\n");
    fflush(stdout);
}
