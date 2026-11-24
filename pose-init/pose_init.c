/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * pose-init — minimal static PID 1 that prepares a bare initramfs and then
 * exec()s the daemon (sink or source), so the daemon itself becomes PID 1.
 *
 * Why this exists: the UKI images for the DPU and the x86 host boot the
 * machine's own stock kernel with an initramfs containing nothing but this
 * binary, the daemon and a handful of kernel modules.  There is no busybox,
 * no udev and no CONFIG_IP_PNP, so somebody has to mount the pseudo
 * filesystems, load the NIC driver and give the interface an address.  The
 * daemon is deliberately kept out of that job: it stays the small, audited
 * component and this file carries the environment plumbing.
 *
 * Everything is driven from the kernel command line, which in a UKI is baked
 * into the signed image and therefore trustworthy:
 *
 *   pose.mods=mlxbf_gige            comma list; bare names resolve to
 *                                   /lib/modules/<name>.ko, absolute paths
 *                                   are used as-is; loaded in order
 *   pose.addr=SEL,CIDR[,GW]         give one interface a static IPv4 address;
 *                                   repeatable, one entry per interface.  SEL picks
 *                                   the interface:
 *                                     mac:8c:91:3a:d6:08:d2  by MAC (preferred:
 *                                         kernel names like eth0 depend on probe
 *                                         order and there is no udev to rename)
 *                                     port:c1pf0  by phys_port_name, for switchdev
 *                                         representors, whose MAC is random (this is
 *                                         the raw sysfs value, not the udev name)
 *                                     iface:eth0  by kernel name
 *                                   CIDR is the address, e.g. 192.168.101.90/24.  GW,
 *                                   on at most one entry, becomes the default route,
 *                                   needed only to reach peers beyond the attached
 *                                   subnets.  No pose.addr: network left unconfigured.
 *   pose.switchdev=0000:03:00.0     set this PCI device's eswitch to switchdev
 *                                   mode before the network step (on BlueField this
 *                                   makes the host PF representor pf0hpf appear)
 *   pose.wait=20                    seconds to wait for each interface and
 *                                   then for its carrier (default 20)
 *   pose.exec=/pose-daemon          binary to exec as PID 1 (default shown)
 *   pose.env=K=V,K=V                environment handed to the daemon (e.g.
 *                                   POSE_OS_HEADROOM_MIB=512); nothing else
 *                                   is passed through
 *   pose.onfail=reboot|halt|park    what to do if any step fails
 *                                   (default reboot, after a 30 s pause so
 *                                   the console can be read)
 *
 * If the NVIDIA driver is among the loaded modules, its device nodes
 * (/dev/nvidia<N>, /dev/nvidiactl, /dev/nvidia-uvm[-tools]) are created once
 * the modules are in, as nvidia-modprobe would on a normal system.
 *
 * Not deisgned to run as a normal process (not PID 1) it will just exit.
 *
 * Build: static only (there is no loader in the initramfs).  Avoids every
 * glibc facility that dlopen()s NSS plugins (no getaddrinfo, no getpwnam).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/reboot.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/devlink.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define MAX_MODS 16
#define MAX_IFACES 32
#define MAX_ENV  8
#define MAX_ADDRS 4
#define FAIL_PAUSE_SEC 30

enum onfail { ONFAIL_REBOOT, ONFAIL_HALT, ONFAIL_PARK };

/* One pose.addr entry: how to find the interface, and what to give it. */
enum sel { SEL_MAC, SEL_PORT, SEL_IFACE };
struct net_addr {
    enum sel sel;
    char  key[32];      /* MAC (lower case), phys_port_name or kernel name */
    char  cidr[32];
    char  gw[INET_ADDRSTRLEN];   /* empty: no default route via this entry */
};

struct config {
    char  mods[MAX_MODS][64];
    int   nmods;
    struct net_addr addrs[MAX_ADDRS];
    int   naddrs;
    char  switchdev[32];
    int   wait_sec;
    char  exec_path[256];
    char  env[MAX_ENV][128];
    int   nenv;
    enum onfail onfail;
};

static int g_is_init;

static void say(const char *fmt, ...)
{
    va_list ap;
    fputs("pose-init: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ── failure handling ─────────────────────────────────────────────────────── */

static void park_forever(void)
{
    for (;;)
        pause();
}

static void __attribute__((noreturn)) fail(const struct config *c, const char *fmt, ...)
{
    va_list ap;
    fputs("pose-init: FATAL: ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);

    if (!g_is_init)
        exit(1);

    switch (c->onfail) {
    case ONFAIL_PARK:
        say("parking (pose.onfail=park)");
        park_forever();
        break;
    case ONFAIL_HALT:
        say("halting in %d s (pose.onfail=halt)", FAIL_PAUSE_SEC);
        sleep(FAIL_PAUSE_SEC);
        sync();
        reboot(RB_HALT_SYSTEM);
        break;
    case ONFAIL_REBOOT:
    default:
        say("rebooting in %d s (pose.onfail=reboot)", FAIL_PAUSE_SEC);
        sleep(FAIL_PAUSE_SEC);
        sync();
        reboot(RB_AUTOBOOT);
        break;
    }
    /* reboot(2) only returns on error; never let PID 1 exit. */
    park_forever();
    exit(1);
}

/* ── command line parsing ─────────────────────────────────────────────────── */

static void set_default_config(struct config *c)
{
    memset(c, 0, sizeof(*c));
    c->wait_sec = 20;
    strcpy(c->exec_path, "/pose-daemon");
    c->onfail = ONFAIL_REBOOT;
}

static void copy_val(char *dst, size_t cap, const char *val)
{
    size_t n = strlen(val);
    if (n >= cap) n = cap - 1;
    memcpy(dst, val, n);
    dst[n] = '\0';
}

static int parse_cidr(const char *s, struct in_addr *addr, struct in_addr *mask);
static const char *sel_name(enum sel s)
{
    return s == SEL_MAC ? "mac" : s == SEL_PORT ? "port" : "iface";
}

/* pose.addr=SEL,CIDR[,GW]; returns -1 on any malformed field. */
static int parse_addr(struct net_addr *a, const char *val)
{
    char buf[128];
    copy_val(buf, sizeof(buf), val);
    char *sel = buf, *cidr = strchr(buf, ','), *gw = NULL;
    if (!cidr) return -1;
    *cidr++ = '\0';
    if ((gw = strchr(cidr, ',')) != NULL) *gw++ = '\0';

    const char *k;
    if      (strncmp(sel, "mac:", 4) == 0)   { a->sel = SEL_MAC;   k = sel + 4; }
    else if (strncmp(sel, "port:", 5) == 0)  { a->sel = SEL_PORT;  k = sel + 5; }
    else if (strncmp(sel, "iface:", 6) == 0) { a->sel = SEL_IFACE; k = sel + 6; }
    else return -1;
    if (*k == '\0' || strlen(k) >= sizeof(a->key)) return -1;
    if (a->sel == SEL_MAC && strlen(k) != 17) return -1;
    if (a->sel == SEL_IFACE && strlen(k) >= IFNAMSIZ) return -1;
    copy_val(a->key, sizeof(a->key), k);
    if (a->sel == SEL_MAC)
        for (char *p = a->key; *p; p++)
            if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');

    struct in_addr x, y;
    if (parse_cidr(cidr, &x, &y) != 0) return -1;
    copy_val(a->cidr, sizeof(a->cidr), cidr);
    a->gw[0] = '\0';
    if (gw) {
        if (inet_pton(AF_INET, gw, &x) != 1) return -1;
        copy_val(a->gw, sizeof(a->gw), gw);
    }
    return 0;
}

static int apply_key(struct config *c, const char *key, const char *val)
{
    if (strcmp(key, "pose.mods") == 0) {
        char buf[1024];
        copy_val(buf, sizeof(buf), val);
        char *save = NULL;
        for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            if (*t == '\0') continue;
            if (c->nmods >= MAX_MODS) return -1;
            copy_val(c->mods[c->nmods++], sizeof(c->mods[0]), t);
        }
    } else if (strcmp(key, "pose.addr") == 0) {
        if (c->naddrs >= MAX_ADDRS) return -1;
        if (parse_addr(&c->addrs[c->naddrs], val) != 0) return -1;
        int ngw = 0;
        for (int i = 0; i <= c->naddrs; i++) ngw += c->addrs[i].gw[0] != '\0';
        if (ngw > 1) return -1;          /* one default route only */
        c->naddrs++;
    } else if (strcmp(key, "pose.switchdev") == 0) {
        const char *v = val;
        if (strncmp(v, "pci/", 4) == 0) v += 4;   /* accept "pci/BDF" or "BDF" */
        copy_val(c->switchdev, sizeof(c->switchdev), v);
    } else if (strcmp(key, "pose.wait") == 0) {
        char *end = NULL;
        long v = strtol(val, &end, 10);
        if (!end || *end || v < 0 || v > 3600) return -1;
        c->wait_sec = (int)v;
    } else if (strcmp(key, "pose.exec") == 0) {
        copy_val(c->exec_path, sizeof(c->exec_path), val);
    } else if (strcmp(key, "pose.env") == 0) {
        char buf[1024];
        copy_val(buf, sizeof(buf), val);
        char *save = NULL;
        for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            if (*t == '\0' || !strchr(t, '=')) return -1;
            if (c->nenv >= MAX_ENV) return -1;
            copy_val(c->env[c->nenv++], sizeof(c->env[0]), t);
        }
    } else if (strcmp(key, "pose.onfail") == 0) {
        if (strcmp(val, "reboot") == 0)     c->onfail = ONFAIL_REBOOT;
        else if (strcmp(val, "halt") == 0)  c->onfail = ONFAIL_HALT;
        else if (strcmp(val, "park") == 0)  c->onfail = ONFAIL_PARK;
        else return -1;
    } else {
        return 1;   /* not ours */
    }
    return 0;
}

/* Returns 0 on success, -1 if a pose.* key had a bad value (name in *bad). */
static int parse_cmdline(struct config *c, const char *cmdline, char *bad, size_t badcap)
{
    char buf[4096];
    copy_val(buf, sizeof(buf), cmdline);

    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (strncmp(tok, "pose.", 5) != 0) continue;
        char *eq = strchr(tok, '=');
        if (!eq) { copy_val(bad, badcap, tok); return -1; }
        *eq = '\0';
        if (apply_key(c, tok, eq + 1) < 0) { copy_val(bad, badcap, tok); return -1; }
    }
    return 0;
}

static int read_cmdline(char *out, size_t cap)
{
    const char *env = getenv("POSE_INIT_CMDLINE");
    if (env && !g_is_init) {
        copy_val(out, cap, env);
        return 0;
    }
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
}

/* ── filesystems ──────────────────────────────────────────────────────────── */

static void mount_one(const struct config *c, const char *src, const char *dir,
                      const char *type, unsigned long flags)
{
    mkdir(dir, 0755);
    if (mount(src, dir, type, flags, NULL) != 0 && errno != EBUSY)
        fail(c, "mount %s on %s: %s", type, dir, strerror(errno));
}

/* ── kernel modules ───────────────────────────────────────────────────────── */

static void load_module(const struct config *c, const char *name)
{
    char path[256];
    if (name[0] == '/')
        copy_val(path, sizeof(path), name);
    else
        snprintf(path, sizeof(path), "/lib/modules/%s.ko", name);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        fail(c, "module %s: open: %s", path, strerror(errno));

    /* finit_module(2): the kernel reads the ELF straight from the fd.  No
     * compressed-module support here on purpose — the image builder ships
     * plain .ko files so this does not depend on CONFIG_MODULE_DECOMPRESS. */
    long rc = syscall(SYS_finit_module, fd, "", 0);
    int err = errno;
    close(fd);
    if (rc != 0 && err != EEXIST)
        fail(c, "module %s: finit_module: %s", path, strerror(err));
    say("module %s loaded%s", path, (rc != 0) ? " (already present)" : "");
}

/* ── NVIDIA device nodes ──────────────────────────────────────────────────── */

/* Major number of the character device registered under exactly `name` in
 * /proc/devices, or -1 if there is none. */
static int char_major(const char *name)
{
    FILE *f = fopen("/proc/devices", "r");
    if (!f) return -1;
    char line[128];
    int in_char = 0, major = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Character devices:", 18) == 0) { in_char = 1; continue; }
        if (strncmp(line, "Block devices:", 14) == 0)     { in_char = 0; continue; }
        int m;
        char n[64];
        if (in_char && sscanf(line, " %d %63s", &m, n) == 2 && strcmp(n, name) == 0) {
            major = m;
            break;
        }
    }
    fclose(f);
    return major;
}

static void make_char_node(const struct config *c, const char *name, int major, int minor)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name);
    if (mknod(path, S_IFCHR | 0666, makedev(major, minor)) != 0 && errno != EEXIST)
        fail(c, "mknod %s (%d:%d): %s", path, major, minor, strerror(errno));
    chmod(path, 0666);   /* mknod's mode is filtered by the umask */
}

/* The NVIDIA driver registers its character devices but, unlike most drivers,
 * creates no device nodes: on a normal system nvidia-modprobe (or udev) makes
 * them.  The UKI has neither, so once the modules are loaded do what
 * nvidia-modprobe does:
 *   /dev/nvidiactl           <nvidia>:255
 *   /dev/nvidia<N>           <nvidia>:N for each GPU, N being the "Device Minor"
 *                            in /proc/driver/nvidia/gpus/<bdf>/information
 *   /dev/nvidia-uvm[-tools]  <nvidia-uvm>:0 and :1, if nvidia_uvm is loaded
 * Majors come from /proc/devices: nvidia's is 195 in practice, nvidia-uvm's is
 * assigned at load.  Does nothing if the nvidia driver is not loaded. */
static void create_nvidia_nodes(const struct config *c)
{
    int major = char_major("nvidia");
    if (major < 0)
        return;

    int ngpu = 0;
    DIR *d = opendir("/proc/driver/nvidia/gpus");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[512], line[128];
            snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%s/information", e->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            int minor = -1;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, "Device Minor: %d", &minor) == 1) break;
            fclose(f);
            if (minor < 0 || minor > 254) {
                say("nvidia: no Device Minor for GPU %s, skipping it", e->d_name);
                continue;
            }
            char node[32];
            snprintf(node, sizeof(node), "nvidia%d", minor);
            make_char_node(c, node, major, minor);
            say("nvidia: /dev/%s for GPU %s", node, e->d_name);
            ngpu++;
        }
        closedir(d);
    }
    make_char_node(c, "nvidiactl", major, 255);

    int uvm = char_major("nvidia-uvm");
    if (uvm >= 0) {
        make_char_node(c, "nvidia-uvm", uvm, 0);
        make_char_node(c, "nvidia-uvm-tools", uvm, 1);
    }
    say("nvidia: created /dev/nvidiactl (%d:255)%s, %d GPU node(s)",
        major, uvm >= 0 ? " and /dev/nvidia-uvm{,-tools}" : "", ngpu);
    if (ngpu == 0)
        say("nvidia: WARNING: driver loaded but no GPUs listed in /proc/driver/nvidia/gpus");
}

/* ── interface lookup ─────────────────────────────────────────────────────── */

static int read_sysfs_line(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* Find the interface whose /sys/class/net/<if>/address matches mac. */
static int find_iface_by_mac(const char *mac, char *out, size_t cap)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[300], addr[64];
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", e->d_name);
        if (read_sysfs_line(path, addr, sizeof(addr)) != 0) continue;
        if (strcmp(addr, mac) == 0) {
            copy_val(out, cap, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Find the interface whose /sys/class/net/<if>/phys_port_name matches portname.
 * Switchdev representors (pf0hpf, en3f0pf0sf0, ...) keep a stable phys_port_name
 * but get a random MAC and a probe-order-dependent ethN name, so this is the
 * only reliable way to pick one without udev. */
static int find_iface_by_portname(const char *portname, char *out, size_t cap)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[300], name[64];
        snprintf(path, sizeof(path), "/sys/class/net/%s/phys_port_name", e->d_name);
        if (read_sysfs_line(path, name, sizeof(name)) != 0) continue;
        if (strcmp(name, portname) == 0) {
            copy_val(out, cap, e->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

static int iface_exists(const char *name)
{
    char path[300];
    snprintf(path, sizeof(path), "/sys/class/net/%s", name);
    struct stat st;
    return stat(path, &st) == 0;
}

/* ── interface enumeration ────────────────────────────────────────────────── */

struct iface_entry {
    char     name[IFNAMSIZ];
    unsigned ifindex;
};

/* Read one sysfs attribute of an interface into out, or "-" if unreadable
 * (carrier, for one, returns EINVAL while the interface is down). */
static void iface_attr(const char *ifname, const char *attr, char *out, size_t cap)
{
    char path[300];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, attr);
    if (read_sysfs_line(path, out, cap) != 0)
        copy_val(out, cap, "-");
}

/* Print every interface in /sys/class/net, sorted by ifindex, with what is
 * needed to see why the wanted NIC did not come up: MAC, ARPHRD type,
 * operstate and carrier from sysfs, UP/RUNNING flags and the IPv4 address
 * via ioctl.  Purely informational: never fails the boot. */
static void list_interfaces(const char *title)
{
    struct iface_entry list[MAX_IFACES];
    int n = 0;

    DIR *d = opendir("/sys/class/net");
    if (!d) {
        say("%s: /sys/class/net: %s", title, strerror(errno));
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAX_IFACES) {
        if (e->d_name[0] == '.') continue;
        char val[32];
        iface_attr(e->d_name, "ifindex", val, sizeof(val));
        copy_val(list[n].name, IFNAMSIZ, e->d_name);
        list[n].ifindex = (unsigned)strtoul(val, NULL, 10);
        n++;
    }
    closedir(d);

    for (int i = 1; i < n; i++) {           /* insertion sort by ifindex */
        struct iface_entry tmp = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].ifindex > tmp.ifindex) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = tmp;
    }

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    say("%s: %d interface(s)", title, n);
    for (int i = 0; i < n; i++) {
        const char *ifname = list[i].name;
        char mac[32], type[16], oper[16], carrier[8], mtu[16];
        iface_attr(ifname, "address",   mac,     sizeof(mac));
        iface_attr(ifname, "type",      type,    sizeof(type));
        iface_attr(ifname, "operstate", oper,    sizeof(oper));
        iface_attr(ifname, "carrier",   carrier, sizeof(carrier));
        iface_attr(ifname, "mtu",       mtu,     sizeof(mtu));

        const char *type_name = type;
        if (strcmp(type, "1") == 0)        type_name = "ether";
        else if (strcmp(type, "772") == 0) type_name = "loopback";

        char flags[16] = "-";
        char ip[INET_ADDRSTRLEN] = "-";
        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            copy_val(ifr.ifr_name, IFNAMSIZ, ifname);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
                int up      = (ifr.ifr_flags & IFF_UP) != 0;
                int running = (ifr.ifr_flags & IFF_RUNNING) != 0;
                snprintf(flags, sizeof(flags), "%s%s%s",
                         up ? "UP" : "down",
                         (up && running) ? "," : "",
                         running ? "RUNNING" : "");
            }
            memset(&ifr, 0, sizeof(ifr));
            copy_val(ifr.ifr_name, IFNAMSIZ, ifname);
            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            }
        }

        say("  %-15s idx=%-3u mac=%-17s %-8s state=%-7s carrier=%s "
            "flags=%s mtu=%s ip=%s",
            ifname, list[i].ifindex, mac, type_name, oper, carrier,
            flags, mtu, ip);
    }

    if (sock >= 0) close(sock);
}

/* Find the interface an entry selects, waiting up to wait_sec for it to appear
 * (module probe is asynchronous relative to finit_module returning). */
static void resolve_iface(const struct config *c, const struct net_addr *a,
                          char *name, size_t cap)
{
    for (int i = 0; ; i++) {
        int found = a->sel == SEL_MAC  ? find_iface_by_mac(a->key, name, cap) == 0
                  : a->sel == SEL_PORT ? find_iface_by_portname(a->key, name, cap) == 0
                  : iface_exists(a->key);
        if (found) {
            if (a->sel == SEL_IFACE) copy_val(name, cap, a->key);
            say("interface %s matches %s:%s", name, sel_name(a->sel), a->key);
            return;
        }
        if (i >= c->wait_sec) break;
        sleep(1);
    }
    list_interfaces("interfaces present");
    fail(c, "no interface matching %s:%s after %d s", sel_name(a->sel), a->key, c->wait_sec);
}

/* ── IPv4 configuration ───────────────────────────────────────────────────── */

static int parse_cidr(const char *s, struct in_addr *addr, struct in_addr *mask)
{
    char buf[32];
    copy_val(buf, sizeof(buf), s);
    char *slash = strchr(buf, '/');
    if (!slash) return -1;
    *slash = '\0';
    char *end = NULL;
    long plen = strtol(slash + 1, &end, 10);
    if (!end || *end || plen < 0 || plen > 32) return -1;
    if (inet_pton(AF_INET, buf, addr) != 1) return -1;
    mask->s_addr = plen == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - plen));
    return 0;
}

static void ioctl_sockaddr(const struct config *c, int sock, unsigned long req,
                           const char *what, const char *ifname, struct in_addr a)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    copy_val(ifr.ifr_name, IFNAMSIZ, ifname);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr   = a;
    if (ioctl(sock, req, &ifr) != 0)
        fail(c, "%s %s: %s", what, ifname, strerror(errno));
}

static void iface_up(const struct config *c, int sock, const char *ifname)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    copy_val(ifr.ifr_name, IFNAMSIZ, ifname);
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) != 0)
        fail(c, "SIOCGIFFLAGS %s: %s", ifname, strerror(errno));
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) != 0)
        fail(c, "SIOCSIFFLAGS %s: %s", ifname, strerror(errno));
}

static void wait_carrier(const struct config *c, const char *ifname)
{
    char path[300], val[8];
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    for (int i = 0; i <= c->wait_sec; i++) {
        if (read_sysfs_line(path, val, sizeof(val)) == 0 && val[0] == '1') {
            say("%s: carrier up", ifname);
            return;
        }
        sleep(1);
    }
    /* Not fatal: some drivers never report carrier before traffic flows, and
     * the daemon's listener does not need link to bind. */
    say("%s: no carrier after %d s — continuing anyway", ifname, c->wait_sec);
}

/* Add a default route via rtnetlink.  Equivalent to `ip route add default
 * via <gw> dev <if>`; written out by hand to avoid a libnl dependency. */
static void add_default_route(const struct config *c, const char *ifname,
                              struct in_addr gw)
{
    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0)
        fail(c, "if_nametoindex %s: %s", ifname, strerror(errno));

    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nl < 0)
        fail(c, "netlink socket: %s", strerror(errno));

    struct {
        struct nlmsghdr nh;
        struct rtmsg    rt;
        char            attrs[64];
    } req;
    memset(&req, 0, sizeof(req));

    req.nh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nh.nlmsg_type  = RTM_NEWROUTE;
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req.nh.nlmsg_seq   = 1;
    req.rt.rtm_family   = AF_INET;
    req.rt.rtm_dst_len  = 0;            /* 0.0.0.0/0 */
    req.rt.rtm_table    = RT_TABLE_MAIN;
    req.rt.rtm_protocol = RTPROT_BOOT;
    req.rt.rtm_scope    = RT_SCOPE_UNIVERSE;
    req.rt.rtm_type     = RTN_UNICAST;

    struct rtattr *rta = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = RTA_GATEWAY;
    rta->rta_len  = RTA_LENGTH(sizeof(gw));
    memcpy(RTA_DATA(rta), &gw, sizeof(gw));
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    rta = (struct rtattr *)((char *)&req + NLMSG_ALIGN(req.nh.nlmsg_len));
    rta->rta_type = RTA_OIF;
    rta->rta_len  = RTA_LENGTH(sizeof(int));
    int oif = (int)ifindex;
    memcpy(RTA_DATA(rta), &oif, sizeof(oif));
    req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (sendto(nl, &req, req.nh.nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        fail(c, "netlink send RTM_NEWROUTE: %s", strerror(errno));

    char resp[512];
    ssize_t n = recv(nl, resp, sizeof(resp), 0);
    close(nl);
    if (n < (ssize_t)sizeof(struct nlmsghdr))
        fail(c, "netlink recv: %s", n < 0 ? strerror(errno) : "short read");
    struct nlmsghdr *rh = (struct nlmsghdr *)resp;
    if (rh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(rh);
        if (e->error != 0)
            fail(c, "RTM_NEWROUTE rejected: %s", strerror(-e->error));
    }
    say("default route via %s dev %s", inet_ntoa(gw), ifname);
}

/* --- generic netlink: set a PCI device's eswitch to switchdev mode -------- */

/* Append one attribute to a netlink request being built in a fixed buffer. */
static void nla_put(struct nlmsghdr *nh, size_t cap, int type,
                    const void *data, size_t len)
{
    size_t off  = NLMSG_ALIGN(nh->nlmsg_len);
    size_t need = off + NLA_ALIGN(NLA_HDRLEN + len);
    if (need > cap) {                       /* never happens with our fixed attrs */
        fputs("pose-init: nla_put overflow\n", stdout);
        _exit(1);
    }
    struct nlattr *na = (struct nlattr *)((char *)nh + off);
    na->nla_type = (unsigned short)type;
    na->nla_len  = (unsigned short)(NLA_HDRLEN + len);
    memcpy((char *)na + NLA_HDRLEN, data, len);
    nh->nlmsg_len = (unsigned int)(off + NLA_ALIGN(na->nla_len));
}

/* Resolve a generic-netlink family id by name; returns -1 if not registered. */
static int genl_resolve_family(int sock, const char *name)
{
    char buf[1024];
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    memset(buf, 0, sizeof(buf));
    nh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
    nh->nlmsg_type  = GENL_ID_CTRL;
    nh->nlmsg_flags = NLM_F_REQUEST;
    nh->nlmsg_seq   = 1;
    struct genlmsghdr *gh = (struct genlmsghdr *)NLMSG_DATA(nh);
    gh->cmd     = CTRL_CMD_GETFAMILY;
    gh->version = 1;
    nla_put(nh, sizeof(buf), CTRL_ATTR_FAMILY_NAME, name, strlen(name) + 1);

    if (send(sock, buf, nh->nlmsg_len, 0) < 0)
        return -1;
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n < (ssize_t)sizeof(struct nlmsghdr) || nh->nlmsg_type == NLMSG_ERROR)
        return -1;

    int len = (int)nh->nlmsg_len - (int)NLMSG_LENGTH(GENL_HDRLEN);
    struct nlattr *na = (struct nlattr *)((char *)NLMSG_DATA(nh) + GENL_HDRLEN);
    while (len >= (int)NLA_HDRLEN) {
        int alen = na->nla_len;
        if (alen < (int)NLA_HDRLEN || alen > len) break;
        if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
            uint16_t id;
            memcpy(&id, (char *)na + NLA_HDRLEN, sizeof(id));
            return id;
        }
        int step = NLA_ALIGN(alen);
        len -= step;
        na = (struct nlattr *)((char *)na + step);
    }
    return -1;
}

/* devlink DEVLINK_CMD_ESWITCH_SET, mode switchdev, on pci/<bdf>.  This is the
 * one thing mlnx_bf_configure does at stock boot that the host PF depends on:
 * without it the eswitch stays legacy, pf0hpf never appears, and the host NIC
 * has no path to this device.  Standard devlink, so no NVIDIA mlxdevm needed. */
static void set_eswitch_switchdev(const struct config *c, const char *bdf)
{
    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (sock < 0)
        fail(c, "genl socket: %s", strerror(errno));
    int fam = genl_resolve_family(sock, DEVLINK_GENL_NAME);
    if (fam < 0) {
        close(sock);
        fail(c, "devlink netlink family not found (is the NIC driver loaded?)");
    }

    char buf[256];
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    memset(buf, 0, sizeof(buf));
    nh->nlmsg_len   = NLMSG_LENGTH(GENL_HDRLEN);
    nh->nlmsg_type  = (unsigned short)fam;
    nh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nh->nlmsg_seq   = 2;
    struct genlmsghdr *gh = (struct genlmsghdr *)NLMSG_DATA(nh);
    gh->cmd     = DEVLINK_CMD_ESWITCH_SET;
    gh->version = 1;
    nla_put(nh, sizeof(buf), DEVLINK_ATTR_BUS_NAME, "pci", 4);
    nla_put(nh, sizeof(buf), DEVLINK_ATTR_DEV_NAME, bdf, strlen(bdf) + 1);
    uint16_t mode = DEVLINK_ESWITCH_MODE_SWITCHDEV;
    nla_put(nh, sizeof(buf), DEVLINK_ATTR_ESWITCH_MODE, &mode, sizeof(mode));

    if (send(sock, buf, nh->nlmsg_len, 0) < 0) {
        close(sock);
        fail(c, "genl send ESWITCH_SET: %s", strerror(errno));
    }
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    close(sock);
    if (n < (ssize_t)sizeof(struct nlmsghdr))
        fail(c, "genl recv: %s", n < 0 ? strerror(errno) : "short read");
    if (nh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(nh);
        if (e->error != 0)
            fail(c, "eswitch set switchdev on pci/%s rejected: %s",
                 bdf, strerror(-e->error));
    }
    say("eswitch pci/%s -> switchdev", bdf);
}

static void configure_network(const struct config *c)
{
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        fail(c, "socket: %s", strerror(errno));

    /* Loopback is harmless and keeps anything that binds 127.0.0.1 happy. */
    iface_up(c, sock, "lo");

    char names[MAX_ADDRS][IFNAMSIZ];
    for (int n = 0; n < c->naddrs; n++) {
        const struct net_addr *a = &c->addrs[n];
        resolve_iface(c, a, names[n], sizeof(names[n]));

        struct in_addr addr, mask;
        parse_cidr(a->cidr, &addr, &mask);      /* validated when parsed */
        ioctl_sockaddr(c, sock, SIOCSIFADDR,    "SIOCSIFADDR",    names[n], addr);
        ioctl_sockaddr(c, sock, SIOCSIFNETMASK, "SIOCSIFNETMASK", names[n], mask);
        iface_up(c, sock, names[n]);

        char ip[INET_ADDRSTRLEN], m[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip, sizeof(ip));
        inet_ntop(AF_INET, &mask, m, sizeof(m));
        say("%s: %s netmask %s up", names[n], ip, m);
    }
    close(sock);

    for (int n = 0; n < c->naddrs; n++)
        wait_carrier(c, names[n]);

    for (int n = 0; n < c->naddrs; n++) {
        struct in_addr gw;
        if (c->addrs[n].gw[0] && inet_pton(AF_INET, c->addrs[n].gw, &gw) == 1)
            add_default_route(c, names[n], gw);
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */

/* Wait up to wait_sec for hvc0 to register, then point fds 0-2 at
 * /dev/console.  /dev/console resolves to the preferred (last console=) console
 * that is registered, so opening it before hvc0 exists would give the UART
 * instead.  Not fatal: if hvc0 never appears we keep whatever we had. */
static void reopen_console(int wait_sec)
{
    struct stat st;
    int i;
    for (i = 0; i <= wait_sec * 10; i++) {
        if (stat("/sys/class/tty/hvc0", &st) == 0) break;
        usleep(100 * 1000);
    }
    if (i > wait_sec * 10)
        return;

    int fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return;
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
    say("console attached to hvc0 after %d.%d s", i / 10, i % 10);
}

static void print_plan(const struct config *c)
{
    say("plan:");
    for (int i = 0; i < c->nmods; i++)
        say("  module  %s", c->mods[i]);
    for (int i = 0; i < c->naddrs; i++)
        say("  addr    %s:%s %s%s%s", sel_name(c->addrs[i].sel), c->addrs[i].key,
            c->addrs[i].cidr, c->addrs[i].gw[0] ? " via " : "", c->addrs[i].gw);
    if (c->naddrs == 0)
        say("  addr    (none — network step skipped)");
    if (c->switchdev[0]) say("  eswitch %s -> switchdev", c->switchdev);
    say("  wait    %d s", c->wait_sec);
    say("  exec    %s", c->exec_path);
    for (int i = 0; i < c->nenv; i++)
        say("  env     %s", c->env[i]);
    say("  onfail  %s", c->onfail == ONFAIL_REBOOT ? "reboot" :
                         c->onfail == ONFAIL_HALT   ? "halt" : "park");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_is_init = (getpid() == 1);

    struct config c;
    set_default_config(&c);

    if (!g_is_init) {
        say("not PID 1, exiting");
        return 0;
    }

    /* /proc must exist before the command line can be read.  Mount it first;
     * the other two follow once we know we are not about to bail out. */
    mount_one(&c, "proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV);

    char cmdline[4096];
    if (read_cmdline(cmdline, sizeof(cmdline)) != 0)
        fail(&c, "cannot read kernel command line: %s", strerror(errno));

    char bad[64] = "";
    if (parse_cmdline(&c, cmdline, bad, sizeof(bad)) != 0)
        fail(&c, "bad or unknown value for %s", bad);

    mount_one(&c, "sysfs",    "/sys", "sysfs",    MS_NOSUID | MS_NOEXEC | MS_NODEV);
    /* No MS_NODEV here: the daemon needs /dev/mem and /dev/pose_probe. */
    mount_one(&c, "devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NOEXEC);

    for (int i = 0; i < c.nmods; i++)
        load_module(&c, c.mods[i]);

    /* The kernel opened /dev/console for us before any module was loaded.  If
     * the preferred console is a module-backed one (hvc0 on the BlueField DPU,
     * which needs mlxbf_tmfifo), that open went nowhere and everything printed
     * so far was lost.  Now that the modules are in, wait for hvc0 and
     * re-attach fds 0-2 so this process and the exec'd daemon reach it. */
    if (strstr(cmdline, "console=hvc0"))
        reopen_console(5);

    create_nvidia_nodes(&c);

    struct utsname u;
    uname(&u);
    say("pid=%d kernel=%s %s", getpid(), u.release, u.machine);
    say("%d module(s) loaded", c.nmods);
    print_plan(&c);

    struct stat st;
    if (stat(c.exec_path, &st) != 0)
        fail(&c, "%s: %s", c.exec_path, strerror(errno));

    if (c.switchdev[0])
        set_eswitch_switchdev(&c, c.switchdev);

    if (c.naddrs)
        configure_network(&c);
    else
        say("no pose.addr — leaving network unconfigured");

    list_interfaces("network interfaces");

    say("exec %s as PID 1", c.exec_path);
    char *args[] = { c.exec_path, NULL };
    char *envp[MAX_ENV + 1];
    for (int i = 0; i < c.nenv; i++) envp[i] = c.env[i];
    envp[c.nenv] = NULL;
    execve(c.exec_path, args, envp);
    fail(&c, "execve %s: %s", c.exec_path, strerror(errno));
}
