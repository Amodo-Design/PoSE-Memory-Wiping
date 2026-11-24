/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#include "gpu_targets.h"
#include "sink_inventory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_count;      /* devices found */
static int g_expected;   /* POSE_GPUS, -1 = unset */

static int read_expected(char *why, size_t why_cap)
{
    const char *e = getenv("POSE_GPUS");
    if (!e || !*e) {
        g_expected = -1;
        return 0;
    }
    char *end = NULL;
    long v = strtol(e, &end, 10);
    if (!end || *end || v < 0 || v > 64) {
        snprintf(why, why_cap, "POSE_GPUS=%s is not a count of GPUs", e);
        return -1;
    }
    g_expected = (int)v;
    return 0;
}

static int check_expected(int found, const char *detail, char *why, size_t why_cap)
{
    if (g_expected >= 0 && found < g_expected) {
        snprintf(why, why_cap, "POSE_GPUS=%d but %d GPU(s) usable%s%s", g_expected, found,
                 detail ? ": " : "", detail ? detail : "");
        return -1;
    }
    return 0;
}

int gpu_targets_parse(const char *name)
{
    if (strncmp(name, "gpu", 3) != 0 || name[3] == '\0')
        return -1;
    char *end = NULL;
    long v = strtol(name + 3, &end, 10);
    if (!end || *end || v < 0 || v >= 64 || (name[3] == '0' && name[4] != '\0'))
        return -1;
    return (int)v;
}

int gpu_targets_count(void)
{
    return g_count;
}

#ifdef TARGET_GPU

#include "gpu_session.h"

int gpu_targets_init(char *why, size_t why_cap)
{
    if (read_expected(why, why_cap) != 0)
        return -1;
    char cuda_why[256] = "";
    int n = pose_gpu_init(cuda_why, sizeof(cuda_why));
    if (n < 0) {
        /* No driver, no libcuda.so.1, firmware failure, ...: report it, and
         * carry on without GPUs unless some were expected. */
        fprintf(stderr, "gpu: CUDA is unavailable: %s\n", cuda_why);
        if (check_expected(0, cuda_why, why, why_cap) != 0)
            return -1;
        g_count = 0;
        return 0;
    }
    g_count = n;
    for (int d = 0; d < n; d++) {
        pose_gpu_info_t info;
        if (pose_gpu_info(d, &info) == 0)
            printf("gpu%d: %s at %s, %llu MiB HBM (%llu MiB free), %d SMs\n", d, info.name,
                   info.pci_bus_id, (unsigned long long)(info.total_bytes >> 20),
                   (unsigned long long)(info.free_bytes >> 20), info.sm_count);
    }
    if (n == 0)
        printf("gpu: no CUDA devices\n");
    fflush(stdout);
    return check_expected(n, NULL, why, why_cap) != 0 ? -1 : n;
}

int gpu_targets_append(sink_inventory_t *inv, char *why, size_t why_cap)
{
    for (int d = 0; d < g_count; d++) {
        pose_gpu_info_t info;
        if (pose_gpu_info(d, &info) != 0) {
            snprintf(why, why_cap, "gpu%d: cannot query the device", d);
            return -1;
        }
        char name[16];
        snprintf(name, sizeof(name), "gpu%d", d);
        /* The map tiles the device: declared = driver + target + headroom.
         * Bases are offsets into the device's HBM, so the entries can be
         * shown in order like a RAM map; only the target's base (0) matters
         * to the protocol. */
        uint64_t held   = info.total_bytes > info.free_bytes ? info.total_bytes - info.free_bytes : 0;
        uint64_t avail  = info.free_bytes > POSE_GPU_HEADROOM_BYTES ? info.free_bytes - POSE_GPU_HEADROOM_BYTES : 0;
        uint64_t target = avail - avail % POSE_HASH_BYTES;
        uint64_t head   = info.total_bytes - held - target;   /* headroom + rounding */
        if (sink_inventory_add(inv, SINK_GPU_DECLARED, name, 0, info.total_bytes, 0) != 0 ||
            (target > 0 && sink_inventory_add(inv, SINK_TARGET_GPU, name, 0, target, 0) != 0) ||
            (head > 0 && sink_inventory_add(inv, SINK_GPU_RESERVED, "headroom", target, head, 0) != 0) ||
            (held > 0 && sink_inventory_add(inv, SINK_GPU_RESERVED, "driver", target + head, held, 0) != 0)) {
            snprintf(why, why_cap, "inventory full while adding gpu%d", d);
            return -1;
        }
        if (target == 0)
            fprintf(stderr, "gpu%d: only %llu MiB free, nothing labelable after the %llu MiB headroom\n",
                    d, (unsigned long long)(info.free_bytes >> 20),
                    (unsigned long long)(POSE_GPU_HEADROOM_BYTES >> 20));
    }
    return 0;
}

#else /* !TARGET_GPU */

int gpu_targets_init(char *why, size_t why_cap)
{
    if (read_expected(why, why_cap) != 0)
        return -1;
    g_count = 0;
    return check_expected(0, "built without GPU support (TARGET_GPU=OFF)", why, why_cap) != 0 ? -1 : 0;
}

int gpu_targets_append(sink_inventory_t *inv, char *why, size_t why_cap)
{
    (void)inv; (void)why; (void)why_cap;
    return 0;
}

#endif
