# HotPlugNetlink

Real-time Linux kernel event monitor using **Netlink sockets**.  
No external dependencies — pure C11, standard Linux kernel headers only.

---

## What it monitors

| Channel | Netlink family | Events captured |
|---|---|---|
| **uevent** | `NETLINK_KOBJECT_UEVENT` | Device add/remove/change/bind/unbind (USB, block, PCI, …) |
| **rtnetlink** | `NETLINK_ROUTE` | Network interface up/down, IPv4/IPv6 address add/remove |
| **proc_events** | `NETLINK_CONNECTOR` | Process fork, exec, exit, UID/GID change, coredump, ptrace |

All three channels are multiplexed on a single `epoll` loop.  
Each event is printed with a timestamp and ANSI color coding.

---

## How Netlink works (brief)

Netlink is a socket-based IPC mechanism between the Linux kernel and user-space.
Unlike `ioctl` or `/proc`, Netlink supports **async, multicast notifications** — the
kernel pushes events to subscribed sockets without polling.

```
┌────────────────────────────────┐
│         Linux Kernel           │
│  kobject / rtnetlink / cn_proc │
│         subsystems             │
└──────────┬─────────────────────┘
           │  Netlink multicast
           ▼
┌────────────────────────────────┐
│     hotplug_monitor (you)      │
│  epoll → recv → parse → print  │
└────────────────────────────────┘
```

Each Netlink family uses a different socket:

```c
// Hotplug / udev events
socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT)

// Network link + address changes
socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)

// Process lifecycle (requires root)
socket(AF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR)
```

---

## Project structure

```
HotPlugNetlink/
├── include/
│   ├── color.h          ANSI escape code macros
│   ├── uevent.h         uevent open/handle declarations
│   ├── rtnetlink.h      rtnetlink open/handle declarations
│   └── proc_events.h    proc_events open/handle declarations
├── src/
│   ├── main.c           epoll multiplexer, CLI flags, signal handling
│   ├── uevent.c         NETLINK_KOBJECT_UEVENT parser & printer
│   ├── rtnetlink.c      NETLINK_ROUTE parser & printer
│   └── proc_events.c    NETLINK_CONNECTOR parser & printer
├── Makefile
└── README.md
```

---

## Build

### Requirements

- GCC (any version supporting C11)
- Linux kernel headers (standard on any Linux distro)
- `make`

```bash
# Install build tools on Ubuntu/Debian if needed
sudo apt-get install build-essential linux-headers-$(uname -r)
```

### Compile

```bash
make
```

The binary `hotplug_monitor` is produced in the project root.

```bash
make clean    # remove build artefacts
make install  # copy binary to /usr/local/bin (optional)
```

---

## Run

```
Usage: ./hotplug_monitor [-u] [-r] [-p] [-h]

  -u   disable uevent (hotplug) monitor
  -r   disable rtnetlink (network) monitor
  -p   disable process-event monitor
  -h   show help
```

### All monitors (requires root for proc_events)

```bash
sudo ./hotplug_monitor
```

### Network events only (no root needed)

```bash
./hotplug_monitor -p
```

### Hotplug only

```bash
sudo ./hotplug_monitor -r -p
```

Press **Ctrl+C** to stop cleanly.

---

## Sample output

```
╔══════════════════════════════════════════════════════╗
║          HotPlugNetlink — Kernel Event Monitor       ║
╚══════════════════════════════════════════════════════╝
Monitoring: uevent rtnetlink proc_events
Press Ctrl+C to stop.

[14:02:11] UEVENT add      subsystem=usb        devtype=usb_device  devname=bus/usb/003/004  path=/devices/pci0000:00/…/3-1
[14:02:11] UEVENT add      subsystem=usb        devtype=usb_interface                        path=/devices/pci0000:00/…/3-1:1.0
[14:02:11] RTNETLINK LINK NEW  iface=enp3s0      idx=2    flags=[UP RUNNING BROADCAST MULTICAST ]
[14:02:13] PROC_EVENT FORK   parent pid=12345  tgid=12345  -> child pid=12402  tgid=12402
[14:02:13] PROC_EVENT EXEC   pid=12402  tgid=12402
[14:02:14] PROC_EVENT EXIT   pid=12402  tgid=12402  exit_code=0
[14:02:15] RTNETLINK ADDR ADD  IPv4 iface=enp3s0      addr=192.168.1.42/24
[14:02:20] UEVENT remove   subsystem=usb        devtype=usb_device  devname=bus/usb/003/004  path=/devices/pci0000:00/…/3-1
```

---

## How to trigger events for testing

### 1. Hotplug / uevent events

**USB device** — plug in or unplug any USB device (mouse, flash drive, phone).

**Simulate with `udevadm`** (no physical hardware needed):

```bash
# Trigger a change event on the first block device
sudo udevadm trigger --action=change --subsystem-match=block

# Trigger on all USB devices
sudo udevadm trigger --action=add --subsystem-match=usb

# Watch raw uevent stream (independent tool for comparison)
sudo udevadm monitor --kernel --udev
```

**Loop-back device** (generates block + partition uevents):

```bash
dd if=/dev/zero of=/tmp/test.img bs=1M count=64
sudo losetup /dev/loop10 /tmp/test.img          # triggers add
sudo losetup -d /dev/loop10                     # triggers remove
```

---

### 2. Network (rtnetlink) events

```bash
# Bring an interface down and back up
sudo ip link set eth0 down    # RTM_NEWLINK with flags changed
sudo ip link set eth0 up

# Add and remove an IP address
sudo ip addr add 10.0.0.1/24 dev lo    # RTM_NEWADDR
sudo ip addr del 10.0.0.1/24 dev lo    # RTM_DELADDR

# Create and delete a dummy interface
sudo ip link add dummy0 type dummy     # RTM_NEWLINK
sudo ip link del dummy0                # RTM_DELLINK

# Toggle a network namespace (advanced)
sudo ip netns add testns
sudo ip netns del testns
```

---

### 3. Process events (requires root)

Any process activity generates events. Convenient ways to trigger them:

```bash
# Lots of fork+exec+exit events
find /usr/bin -name 'ls' -exec ls {} \; &

# Single fork/exec/exit
bash -c 'echo hello'

# UID/GID change
sudo -u nobody true

# Watch a specific process (generates PTRACE events)
strace -p $(pgrep bash | head -1) &
```

**Filter by event type while running** — restart with flags:

```bash
# Only show fork and exec (disable network and uevent)
sudo ./hotplug_monitor -u -r
```

---

## Permissions

| Monitor | Needs root? | Why |
|---|---|---|
| uevent | No | `NETLINK_KOBJECT_UEVENT` group 1 is world-readable |
| rtnetlink | No | `NETLINK_ROUTE` multicast groups are world-readable |
| proc_events | **Yes** | `NETLINK_CONNECTOR` `CN_IDX_PROC` bind requires `CAP_NET_ADMIN` |

Running without root will automatically disable proc_events with a warning and continue monitoring the other two channels.

---

## Key concepts

### Netlink message structure

Every Netlink message starts with `struct nlmsghdr` followed by family-specific payload:

```
┌──────────────────────────────────────────┐
│ nlmsghdr (16 bytes)                      │
│   nlmsg_len   nlmsg_type                │
│   nlmsg_flags nlmsg_seq  nlmsg_pid      │
├──────────────────────────────────────────┤
│ Family-specific header                   │
│   (ifinfomsg / ifaddrmsg / cn_msg / …)  │
├──────────────────────────────────────────┤
│ rtattr chain  (type + len + data) …     │
└──────────────────────────────────────────┘
```

### uevent wire format

A raw uevent is a sequence of **null-terminated strings**:

```
add@/devices/pci0000:00/...\0
ACTION=add\0
DEVPATH=/devices/pci0000:00/...\0
SUBSYSTEM=usb\0
DEVTYPE=usb_device\0
DEVNAME=bus/usb/003/004\0
\0
```

### epoll multiplexing

Three Netlink sockets are registered in a single `epoll` instance.
`epoll_wait` blocks until any socket has data, then dispatches to the right handler:

```c
epoll_ctl(epfd, EPOLL_CTL_ADD, uevent_fd,      &ev);
epoll_ctl(epfd, EPOLL_CTL_ADD, rtnetlink_fd,   &ev);
epoll_ctl(epfd, EPOLL_CTL_ADD, proc_event_fd,  &ev);

while (running) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, 500 /*ms timeout*/);
    for (int i = 0; i < n; i++)
        dispatch(events[i].data.fd);
}
```

---

## Extending the project

| Idea | Where to change |
|---|---|
| Filter events by subsystem | `uevent_handle()` in `src/uevent.c` |
| Log events to a file | Add `freopen` in `main.c` or `fprintf(logfile, …)` in handlers |
| Add NETLINK_AUDIT events | Add `audit.c` / `audit.h`, follow same pattern |
| Add JSON output | Replace `printf` with `json_object` or hand-roll JSON strings |
| Watch only specific interface | Compare `ifname` in `rtnetlink.c:handle_link()` |
| Alert on USB device insert | Hook `uevent_handle` with an action == "add" check |

---

## License

MIT — use freely, attribution appreciated.
