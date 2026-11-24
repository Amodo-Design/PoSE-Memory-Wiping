/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * What this machine can offer as a label sink: carved-out RAM and whole block
 * devices, discovered at startup and reported in the sink protocol's Inventory.
 * See sink_inventory.h for what counts and why.
 */

#include "sink_inventory.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Open /dev/mem for mapping carve-out RAM.  How a label reaches DRAM decides
 * the flags (sink_cache.h):
 *   x86-64, aarch64: no O_SYNC.  The mapping is cached where the kernel allows
 *     it (x86: write-back, so copies run at memory speed instead of as
 *     uncached byte writes), and sink_cache_flush at StreamEnd commits the
 *     labels (CLFLUSHOPT, or DC CIVAC from EL0).  On aarch64 the carve-out is
 *     outside the kernel's linear map and comes back as Device memory either
 *     way; a range inside it gets the same cacheable type as the kernel's own
 *     alias instead of a mismatched non-cacheable one.
 *   armv7 (the BMC): O_SYNC.  User space cannot clean the data cache by
 *     address (DCCIMVAC traps at PL0), so no flush can commit a label; the
 *     mapping must be non-cacheable, which tools/cacheprobe measured for this
 *     O_SYNC carve-out mapping.  It has not been measured without O_SYNC.
 * Used for both the inventory probe and session mappings, so a range never has
 * mappings of two memory types. */
static int open_devmem(void)
{
#if defined(__arm__)
    return open("/dev/mem", O_RDWR | O_SYNC);
#else
    return open("/dev/mem", O_RDWR);
#endif
}

/* ── carved-out RAM ──────────────────────────────────────────────────────── */

#define MAX_REGIONS  64
#define MAX_DECLARED 32
#define MAX_MANAGED  64
#define MIN_REGION   (64ULL << 20)   /* smaller gaps are firmware scraps, not a carve-out */
#define MAX_CLAIMED  1024    /* top-level /proc/iomem entries */
#define MAX_MAP      512     /* RAM map entries (declared, kernel, reserved, unusable) */

typedef struct {
    uint64_t phys_start;   /* physical byte address, page-aligned */
    uint64_t size;         /* bytes, a whole number of pages */
} region_t;

typedef struct { uint64_t start, end; } range_t;   /* inclusive */

/* A top-level /proc/iomem entry. */
typedef struct {
    uint64_t start, end;                 /* inclusive */
    char     name[SINK_NAME_MAX];        /* "System RAM", "Reserved", ... */
} claim_t;

/* One RAM map entry for the Inventory (SINK_RAM_*). */
typedef struct {
    uint8_t  kind;
    char     name[SINK_NAME_MAX];
    uint64_t start, end;                 /* inclusive */
} map_entry_t;

typedef struct {
    region_t  regions[MAX_REGIONS];      /* the carve-out: what can be labeled */
    int       count;
    uint64_t  page_size;
    /* Kept for the accounting in ram_print. */
    range_t   declared[MAX_DECLARED];    /* DRAM the firmware declares */
    int       ndeclared;
    range_t   managed[MAX_MANAGED];      /* /proc/iomem "System RAM" inside it */
    int       nmanaged;
    /* Where the rest of the declared DRAM went, reported in the Inventory. */
    map_entry_t map[MAX_MAP];
    int         nmap;
    int         map_overflow;            /* more than MAX_MAP entries: an error */
} ram_t;

static int range_cmp(const void *a, const void *b)
{
    const range_t *ra = a, *rb = b;
    return ra->start < rb->start ? -1 : ra->start > rb->start;
}

static int claim_cmp(const void *a, const void *b)
{
    const claim_t *ca = a, *cb = b;
    return ca->start < cb->start ? -1 : ca->start > cb->start;
}

/* Append [start, end] to the RAM map, merged into the previous entry when it
 * is adjacent and of the same kind and name. */
static void map_add(ram_t *m, uint8_t kind, const char *name, uint64_t start, uint64_t end)
{
    if (start > end)
        return;
    if (m->nmap > 0) {
        map_entry_t *prev = &m->map[m->nmap - 1];
        if (prev->kind == kind && strcmp(prev->name, name) == 0 && prev->end + 1 == start) {
            prev->end = end;
            return;
        }
    }
    if (m->nmap >= MAX_MAP) {
        m->map_overflow = 1;
        return;
    }
    map_entry_t *e = &m->map[m->nmap++];
    e->kind  = kind;
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->start = start;
    e->end   = end;
}

/* ── what the firmware declares ──────────────────────────────────────────── */

static uint32_t read_dtb_cells(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t b[4] = {0};
    ssize_t n = read(fd, b, 4);
    close(fd);
    if (n != 4) return 0;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

/* Every memory@<addr> node under the device-tree root: its reg property is
 * (base, size) pairs of big-endian cells, the widths given by the root's
 * #address-cells and #size-cells.  /proc/device-tree and
 * /sys/firmware/devicetree/base are the same data; stop at the first that
 * yields anything. */
static int collect_dtb(range_t *out, int max)
{
    static const char *const dirs[] = { "/proc/device-tree", "/sys/firmware/devicetree/base", NULL };
    uint32_t ac = read_dtb_cells("/proc/device-tree/#address-cells");
    uint32_t sc = read_dtb_cells("/proc/device-tree/#size-cells");
    if (ac == 0) ac = 2;
    if (sc == 0) sc = 1;
    size_t entry = (ac + sc) * 4;

    int n = 0;
    for (int d = 0; dirs[d] && n == 0; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL && n < max) {
            if (strncmp(de->d_name, "memory@", 7) != 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s/reg", dirs[d], de->d_name);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            uint8_t buf[256];
            ssize_t len = read(fd, buf, sizeof(buf));
            close(fd);
            if (len <= 0 || (size_t)len % entry != 0) {
                fprintf(stderr, "inventory: %s: unexpected reg size %zd\n", de->d_name, len);
                continue;
            }
            for (size_t off = 0; off + entry <= (size_t)len && n < max; off += entry) {
                uint64_t base = 0, size = 0;
                for (uint32_t i = 0; i < ac * 4; i++) base = (base << 8) | buf[off + i];
                for (uint32_t i = 0; i < sc * 4; i++) size = (size << 8) | buf[off + ac * 4 + i];
                if (size == 0) continue;
                out[n].start = base;
                out[n].end   = base + size - 1;
                n++;
            }
        }
        closedir(dir);
    }
    if (n > 0)
        printf("  DTB: %d memory entr%s (#address-cells=%u #size-cells=%u)\n",
               n, n == 1 ? "y" : "ies", ac, sc);
    return n;
}

/* Every SMBIOS Type 19 (Memory Array Mapped Address) structure under
 * /sys/firmware/dmi/entries.  32-bit start/end in KiB at 0x04/0x08; all-ones
 * there means the 64-bit byte fields at 0x0F/0x17 apply (SMBIOS 3, 7.20). */
static int collect_smbios(range_t *out, int max)
{
    DIR *dir = opendir("/sys/firmware/dmi/entries");
    if (!dir) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < max) {
        if (strncmp(de->d_name, "19-", 3) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "/sys/firmware/dmi/entries/%s/raw", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        uint8_t buf[64];
        ssize_t len = read(fd, buf, sizeof(buf));
        close(fd);
        if (len < 0x0C) continue;
        uint32_t s32 = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
        uint32_t e32 = (uint32_t)buf[8] | (uint32_t)buf[9] << 8 | (uint32_t)buf[10] << 16 | (uint32_t)buf[11] << 24;
        uint64_t start, end;
        if (s32 == 0xFFFFFFFFu || e32 == 0xFFFFFFFFu) {
            if (len < 0x1F) continue;
            start = end = 0;
            for (int i = 7; i >= 0; i--) {
                start = (start << 8) | buf[0x0F + i];
                end   = (end   << 8) | buf[0x17 + i];
            }
        } else {
            start = (uint64_t)s32 << 10;
            end   = ((uint64_t)e32 << 10) | 0x3FF;
        }
        if (end <= start) continue;
        out[n].start = start;
        out[n].end   = end;
        n++;
    }
    closedir(dir);
    if (n > 0)
        printf("  SMBIOS: %d memory array mapped address entr%s\n", n, n == 1 ? "y" : "ies");
    return n;
}

/* ── what the kernel claims ──────────────────────────────────────────────── */

/* Every top-level /proc/iomem entry, whatever its name: System RAM, reserved,
 * ACPI tables, PCI windows.  Anything listed is not ours.  Children are
 * inside their parent and add nothing. */
static int collect_claimed(claim_t *out, int max, ram_t *m)
{
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) {
        perror("inventory: /proc/iomem");
        return -1;
    }
    int n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && n < max) {
        if (line[0] == ' ' || line[0] == '\t') continue;
        unsigned long long s, e;
        char desc[128];
        if (sscanf(line, "%llx-%llx : %127[^\n]", &s, &e, desc) != 3) continue;
        out[n].start = s;
        out[n].end   = e;
        snprintf(out[n].name, sizeof(out[n].name), "%.*s", SINK_NAME_MAX - 1, desc);   /* long names are cut */
        n++;
        if (strncmp(desc, "System RAM", 10) == 0 && m->nmanaged < MAX_MANAGED) {
            m->managed[m->nmanaged].start = s;
            m->managed[m->nmanaged].end   = e;
            m->nmanaged++;
        }
    }
    fclose(f);
    if (n == max)
        fprintf(stderr, "inventory: /proc/iomem has more than %d entries; the rest are ignored\n", max);
    return n;
}

/* ── the carve-out ───────────────────────────────────────────────────────── */

/* Add the unclaimed [start, end] as a carve-out region if, page-aligned and
 * trimmed, it is at least MIN_REGION and one page of it maps through
 * /dev/mem.  Whatever of it is not carved out goes into the RAM map as
 * unusable, with the reason as its name. */
static void add_gap(ram_t *m, int devmem, uint64_t start, uint64_t end)
{
    uint64_t ps = m->page_size;
    uint64_t a  = (start + ps - 1) & ~(ps - 1);
    uint64_t size = a > end ? 0 : ((end - a + 1) / ps) * ps;
    if (size < MIN_REGION) {
        map_add(m, SINK_RAM_UNUSABLE, "too small", start, end);
        return;
    }

    void *p = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE, MAP_SHARED, devmem, (off_t)a);
    if (p == MAP_FAILED) {
        fprintf(stderr, "inventory: gap 0x%llx+%llu MiB not mappable through /dev/mem: %s\n",
                (unsigned long long)a, (unsigned long long)(size >> 20), strerror(errno));
        map_add(m, SINK_RAM_UNUSABLE, "not mappable", start, end);
        return;
    }
    munmap(p, (size_t)ps);

    if (m->count >= MAX_REGIONS) {
        fprintf(stderr, "inventory: region limit (%d) reached\n", MAX_REGIONS);
        map_add(m, SINK_RAM_UNUSABLE, "region limit", start, end);
        return;
    }
    /* The page-alignment trim at either end. */
    if (a > start)
        map_add(m, SINK_RAM_UNUSABLE, "too small", start, a - 1);
    m->regions[m->count].phys_start = a;
    m->regions[m->count].size       = size;
    m->count++;
    printf("  carve-out: 0x%llx - 0x%llx (%llu MiB)\n",
           (unsigned long long)a, (unsigned long long)(a + size - 1),
           (unsigned long long)(size >> 20));
    if (a + size <= end)
        map_add(m, SINK_RAM_UNUSABLE, "too small", a + size, end);
}

/* Sort the declared ranges and merge any that overlap or touch (DTB and
 * SMBIOS may both describe the same DRAM), so no byte is mapped twice.
 * Returns the new count. */
static int merge_declared(range_t *d, int n)
{
    qsort(d, (size_t)n, sizeof(range_t), range_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k > 0 && d[i].start <= d[k - 1].end + 1) {
            if (d[i].end > d[k - 1].end)
                d[k - 1].end = d[i].end;
        } else {
            d[k++] = d[i];
        }
    }
    return k;
}

/* Find the carve-out.  Returns 0 with at least one region, or -1 when there
 * is none: no firmware DRAM declaration, no gap of MIN_REGION, or /dev/mem
 * cannot be opened (it is needed to label, so there is no point reporting
 * what cannot be reached). */
static int ram_discover(ram_t *m)
{
    memset(m, 0, sizeof(*m));
    long ps = sysconf(_SC_PAGE_SIZE);
    m->page_size = ps > 0 ? (uint64_t)ps : 4096;

    range_t *declared = m->declared;
    int nd = collect_dtb(declared, MAX_DECLARED);
    nd += collect_smbios(declared + nd, MAX_DECLARED - nd);
    nd = merge_declared(declared, nd);
    m->ndeclared = nd;
    if (nd == 0) {
        fprintf(stderr, "inventory: no DTB or SMBIOS memory ranges found; cannot tell declared DRAM from the kernel's\n");
        return -1;
    }
    for (int d = 0; d < nd; d++)
        printf("    declared DRAM: 0x%llx - 0x%llx (%llu MiB)\n",
               (unsigned long long)declared[d].start, (unsigned long long)declared[d].end,
               (unsigned long long)((declared[d].end - declared[d].start + 1) >> 20));

    static claim_t claimed[MAX_CLAIMED];
    int nc = collect_claimed(claimed, MAX_CLAIMED, m);
    if (nc < 0)
        return -1;
    qsort(claimed, (size_t)nc, sizeof(claim_t), claim_cmp);

    int devmem = open_devmem();
    if (devmem < 0) {
        perror("inventory: /dev/mem");
        return -1;
    }

    /* For each declared range, the gaps between the claimed entries that
     * overlap it are the carve-out.  The claims themselves, clipped to the
     * declared range, go into the RAM map. */
    for (int d = 0; d < nd; d++) {
        map_add(m, SINK_RAM_DECLARED, "dram", declared[d].start, declared[d].end);
        uint64_t cursor = declared[d].start;
        for (int c = 0; c < nc; c++) {
            if (claimed[c].end < declared[d].start || claimed[c].start > declared[d].end)
                continue;
            if (claimed[c].start > cursor)
                add_gap(m, devmem, cursor, claimed[c].start - 1 < declared[d].end
                                              ? claimed[c].start - 1 : declared[d].end);
            uint64_t cs = claimed[c].start > cursor ? claimed[c].start : cursor;
            uint64_t ce = claimed[c].end < declared[d].end ? claimed[c].end : declared[d].end;
            if (cs <= ce) {
                int kernel = strncmp(claimed[c].name, "System RAM", 10) == 0;
                map_add(m, kernel ? SINK_RAM_KERNEL : SINK_RAM_RESERVED,
                        kernel ? "System RAM" : claimed[c].name, cs, ce);
            }
            if (claimed[c].end + 1 > cursor)
                cursor = claimed[c].end + 1;
        }
        if (cursor <= declared[d].end)
            add_gap(m, devmem, cursor, declared[d].end);
    }
    close(devmem);
    if (m->map_overflow)
        fprintf(stderr, "inventory: more than %d RAM map entries\n", MAX_MAP);

    if (m->count == 0) {
        fprintf(stderr, "inventory: no carved-out RAM (declared DRAM is all claimed by the kernel; "
                        "hide some with mem= or memmap=)\n");
        return -1;
    }
    return 0;
}

/* Bytes of [start, end] that fall inside any of the declared ranges. */
static uint64_t within_declared(const ram_t *m, uint64_t start, uint64_t end)
{
    uint64_t n = 0;
    for (int d = 0; d < m->ndeclared; d++) {
        uint64_t s = start > m->declared[d].start ? start : m->declared[d].start;
        uint64_t e = end   < m->declared[d].end   ? end   : m->declared[d].end;
        if (s <= e) n += e - s + 1;
    }
    return n;
}

/* Free (buddy) bytes in the managed ranges, from /proc/kpageflags.  -1 if
 * unreadable (needs root). */
#define KPF_BUDDY (1ULL << 10)
static int64_t managed_free_bytes(const ram_t *m)
{
    int fd = open("/proc/kpageflags", O_RDONLY);
    if (fd < 0) return -1;
    uint64_t ps = m->page_size, free_pages = 0;
    for (int i = 0; i < m->nmanaged; i++) {
        uint64_t pfn = m->managed[i].start / ps;
        uint64_t end = m->managed[i].end / ps;             /* inclusive */
        uint64_t flags[1024];
        while (pfn <= end) {
            uint64_t want = end - pfn + 1;
            if (want > 1024) want = 1024;
            ssize_t n = pread(fd, flags, (size_t)(want * 8), (off_t)(pfn * 8));
            if (n <= 0) break;
            uint64_t got = (uint64_t)n / 8;
            for (uint64_t j = 0; j < got; j++)
                if (flags[j] & KPF_BUDDY) free_pages++;
            pfn += got;
        }
    }
    close(fd);
    return (int64_t)(free_pages * ps);
}

/* Bytes of this process's resident pages that lie in the managed ranges,
 * from /proc/self/pagemap.  -1 if unreadable (needs root). */
static int64_t self_bytes(const ram_t *m)
{
    int pm = open("/proc/self/pagemap", O_RDONLY);
    if (pm < 0) return -1;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) { close(pm); return -1; }
    uint64_t ps = m->page_size, pages = 0;
    char line[256];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long long vs, ve;
        if (sscanf(line, "%llx-%llx", &vs, &ve) != 2) continue;
        if (strstr(line, "/dev/mem")) continue;          /* a mapped carve-out is not our page */
        for (uint64_t v = vs; v < ve; v += ps) {
            uint64_t entry = 0;
            if (pread(pm, &entry, 8, (off_t)((v / ps) * 8)) != 8) continue;
            if (!(entry & (1ULL << 63))) continue;         /* not present */
            uint64_t phys = (entry & ((1ULL << 55) - 1)) * ps;
            for (int i = 0; i < m->nmanaged; i++)
                if (phys >= m->managed[i].start && phys <= m->managed[i].end) { pages++; break; }
        }
    }
    fclose(maps);
    close(pm);
    return (int64_t)(pages * ps);
}

static void print_mib(const char *label, uint64_t bytes)
{
    printf("  %-40s %8llu MiB\n", label, (unsigned long long)(bytes >> 20));
}

/* Account for every byte of declared DRAM: the carve-out (labelable), the
 * kernel-managed System RAM split into free pages, pages this process owns
 * and everything else, which is allocated by the kernel (read from
 * /proc/kpageflags and /proc/self/pagemap, so it needs root), and the
 * remainder claimed by firmware or too small to use. */
static void ram_print(const ram_t *m)
{
    uint64_t declared = 0, carved = 0, managed = 0;
    for (int d = 0; d < m->ndeclared; d++) declared += m->declared[d].end - m->declared[d].start + 1;
    for (int i = 0; i < m->count; i++)     carved   += m->regions[i].size;
    for (int i = 0; i < m->nmanaged; i++)  managed  += within_declared(m, m->managed[i].start, m->managed[i].end);
    uint64_t other = declared - carved - managed;     /* firmware-claimed + gaps too small to use */

    printf("DRAM accounting:\n");
    print_mib("declared by firmware", declared);
    print_mib("carved out (labelable)", carved);
    for (int i = 0; i < m->count; i++) {
        const region_t *r = &m->regions[i];
        printf("    [%d] 0x%012llx + 0x%llx (%llu MiB)\n", i,
               (unsigned long long)r->phys_start, (unsigned long long)r->size,
               (unsigned long long)(r->size >> 20));
    }
    print_mib("kernel-managed (System RAM)", managed);
    int64_t fr = managed_free_bytes(m), self = self_bytes(m);
    if (fr >= 0 && self >= 0) {
        uint64_t kernel = managed > (uint64_t)fr + (uint64_t)self ? managed - (uint64_t)fr - (uint64_t)self : 0;
        print_mib("  free (buddy pages)", (uint64_t)fr);
        print_mib("  allocated (kernel)", kernel);
        printf("  %-40s %8llu KiB\n", "  this process", (unsigned long long)((uint64_t)self >> 10));
    } else {
        printf("  %-40s %8s\n", "  free / kernel / this process", "n/a (needs root)");
    }
    print_mib("firmware-claimed or unusable", other);
}

/* ── block devices ───────────────────────────────────────────────────────── */

typedef struct {
    char     name[32];     /* "nvme0n1" */
    char     path[48];     /* "/dev/nvme0n1" */
    uint64_t size;         /* bytes, BLKGETSIZE64 */
    uint32_t sector;       /* logical sector size, BLKSSZGET */
} blockdev_t;

static int has_prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static int is_wanted(const char *name)
{
    static const char *const skip[] = { "loop", "ram", "zram", "nbd", "dm-", "md", "fd", "sr", NULL };
    for (int i = 0; skip[i]; i++)
        if (has_prefix(name, skip[i]))
            return 0;
    if (has_prefix(name, "mmcblk") && (strstr(name, "boot") || strstr(name, "rpmb")))
        return 0;
    return 1;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const blockdev_t *)a)->name, ((const blockdev_t *)b)->name);
}

/* Whole disks only (nvme*, sd*, mmcblk*, vd*, ...); loop, ram, zram, nbd,
 * device-mapper, md, floppy, optical and the eMMC boot/RPMB areas are
 * skipped.  Fills out[0..max) sorted by name and returns the count, or -1 if
 * /sys/block is unreadable.  A device that cannot be opened or sized is
 * reported on stderr and not counted. */
static int blockdev_enumerate(blockdev_t *out, int max)
{
    DIR *d = opendir("/sys/block");
    if (!d) {
        fprintf(stderr, "inventory: /sys/block: %s\n", strerror(errno));
        return -1;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (e->d_name[0] == '.' || !is_wanted(e->d_name))
            continue;
        blockdev_t *b = &out[n];
        memset(b, 0, sizeof(*b));
        if (strlen(e->d_name) >= sizeof(b->name)) {
            fprintf(stderr, "inventory: %s: name too long, skipped\n", e->d_name);
            continue;
        }
        strcpy(b->name, e->d_name);
        snprintf(b->path, sizeof(b->path), "/dev/%s", b->name);

        int fd = open(b->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "inventory: %s: %s, skipped\n", b->path, strerror(errno));
            continue;
        }
        int ssz = 0;
        if (ioctl(fd, BLKGETSIZE64, &b->size) != 0 || ioctl(fd, BLKSSZGET, &ssz) != 0) {
            fprintf(stderr, "inventory: %s: ioctl: %s, skipped\n", b->path, strerror(errno));
            close(fd);
            continue;
        }
        close(fd);
        b->sector = (uint32_t)ssz;
        if (b->size == 0)
            continue;                       /* no medium */
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(*out), cmp_name);
    return n;
}

/* ── the inventory ───────────────────────────────────────────────────────── */

/* Append one entry; -1 if the inventory is full. */
int sink_inventory_add(sink_inventory_t *inv, uint8_t kind, const char *name,
                       uint64_t base, uint64_t size, uint32_t sector)
{
    if (inv->count >= SINK_MAX_TARGETS) {
        fprintf(stderr, "inventory: more than %d entries\n", SINK_MAX_TARGETS);
        return -1;
    }
    sink_target_t *t = &inv->targets[inv->count++];
    t->kind = kind;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->base = base;
    t->size = size;
    t->sector = sector;
    return 0;
}

#define MAX_DISKS 64

int sink_inventory_local(sink_inventory_t *inv)
{
    memset(inv, 0, sizeof(*inv));

    /* The carve-out is fixed by the kernel command line at boot: find it once.
     * Disks are not: NVMe and MMC drivers find their devices asynchronously
     * after the module loads, so a disk may appear after the daemon started.
     * Enumerated afresh on every call, so a late disk is never left out. */
    static ram_t map;      /* large: keep it off the stack */
    static int   ram_known;
    if (!ram_known) {
        if (ram_discover(&map) != 0) {
            fprintf(stderr, "inventory: no labelable RAM on this machine\n");
            map.count = 0;
        } else {
            ram_print(&map);
        }
        ram_known = 1;
    }
    for (int i = 0; i < map.count; i++)
        if (sink_inventory_add(inv, SINK_TARGET_RAM, "ram", map.regions[i].phys_start,
                    map.regions[i].size, 0) != 0)
            return -1;

    blockdev_t disks[MAX_DISKS];
    int nd = blockdev_enumerate(disks, MAX_DISKS);
    for (int i = 0; i < nd; i++)
        if (sink_inventory_add(inv, SINK_TARGET_DISK, disks[i].path, 0, disks[i].size, disks[i].sector) != 0)
            return -1;

    /* The RAM map, even when nothing could be carved out: then it shows
     * where all the DRAM went. */
    if (map.map_overflow)
        return -1;
    for (int i = 0; i < map.nmap; i++) {
        const map_entry_t *e = &map.map[i];
        if (sink_inventory_add(inv, e->kind, e->name, e->start, e->end - e->start + 1, 0) != 0)
            return -1;
    }
    return (int)inv->count;
}

uint32_t sink_inventory_targets(const sink_inventory_t *inv)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < inv->count; i++)
        n += (uint32_t)sink_kind_is_target(inv->targets[i].kind);
    return n;
}

void sink_inventory_print(const sink_inventory_t *inv)
{
    uint32_t targets = sink_inventory_targets(inv);
    printf("Inventory: %u target(s), %u RAM map entr%s\n", targets, inv->count - targets,
           inv->count - targets == 1 ? "y" : "ies");
    for (uint32_t i = 0; i < inv->count; i++) {
        const sink_target_t *t = &inv->targets[i];
        if (!sink_kind_is_target(t->kind))
            continue;
        char where[SINK_NODE_MAX + 2] = "";
        if (t->node[0])
            snprintf(where, sizeof(where), "%s:", t->node);
        if (t->kind == SINK_TARGET_RAM)
            printf("  ram   %-8s base=0x%012llx size=%llu MiB\n", where,
                   (unsigned long long)t->base, (unsigned long long)(t->size >> 20));
        else if (t->kind == SINK_TARGET_GPU)
            printf("  gpu   %-8s %-20s size=%llu MiB\n", where, t->name,
                   (unsigned long long)(t->size >> 20));
        else
            printf("  disk  %-8s %-20s size=%llu MiB sector=%u\n", where, t->name,
                   (unsigned long long)(t->size >> 20), t->sector);
    }
    fflush(stdout);
}

/* ── mapping ─────────────────────────────────────────────────────────────── */

void *sink_inventory_map_ram(uint64_t phys_start, uint64_t size)
{
    int fd = open_devmem();
    if (fd < 0) {
        perror("inventory: /dev/mem");
        return NULL;
    }
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)phys_start);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "inventory: mmap 0x%llx+0x%llx: %s\n",
                (unsigned long long)phys_start, (unsigned long long)size, strerror(errno));
        return NULL;
    }
    return p;
}

void sink_inventory_unmap_ram(void *p, uint64_t size)
{
    if (p)
        munmap(p, (size_t)size);
}
