/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SOURCE_GPU_TARGETS_H
#define POSE_SOURCE_GPU_TARGETS_H
/*
 * The source-daemon's GPU targets: every NVIDIA device's HBM, reported in the
 * inventory as a SINK_TARGET_GPU entry "gpuN" plus the map entries that tile
 * its whole HBM (sink_proto.h, protocol version 3).  GPUs are discovered, not
 * configured; POSE_GPUS=<n> in the environment says how many to expect, and
 * fewer (including none, or a CUDA runtime that will not initialise) is then
 * an error instead of a quiet omission.
 *
 * Built without TARGET_GPU, every call reports no GPUs, and POSE_GPUS > 0 is
 * an error ("built without GPU support").
 */

#include "sink_inventory.h"

#include <stddef.h>

/* Initialise CUDA and create a context on every device (so free HBM is
 * measured with them), read POSE_GPUS.  Returns the device count (0 if none,
 * or if the runtime is unavailable and POSE_GPUS is unset or 0), or -1 with
 * the reason in why.  Call once at startup, before the first inventory. */
int gpu_targets_init(char *why, size_t why_cap);

/* Number of devices gpu_targets_init found. */
int gpu_targets_count(void);

/* Append each GPU's entries to inv (node ""): SINK_GPU_DECLARED "gpuN" (whole
 * HBM), SINK_GPU_RESERVED "driver" and "headroom", and the target
 * SINK_TARGET_GPU "gpuN" sized free minus headroom.  Free HBM is re-read each
 * time.  Returns 0, or -1 with the reason in why. */
int gpu_targets_append(sink_inventory_t *inv, char *why, size_t why_cap);

/* Parse "gpuN" -> N, or -1 if the name is not a GPU target of this daemon. */
int gpu_targets_parse(const char *name);

#endif /* POSE_SOURCE_GPU_TARGETS_H */
