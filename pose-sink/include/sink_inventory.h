/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SINK_INVENTORY_H
#define POSE_SINK_INVENTORY_H

/*
 * This machine's targets: the physical RAM and the block devices it can offer
 * as a label sink.  Shared by sink-daemon, which serves only these, and
 * source-daemon, which serves these alongside its downstream sinks'.
 *
 * RAM means carved-out RAM only: DRAM the firmware declares (DTB memory@
 * nodes, SMBIOS Type 19 entries) that the kernel does not claim in
 * /proc/iomem, because mem= or memmap= hid it from the allocator.  Such a
 * range has no struct pages, so the kernel can never hand any of it out:
 * it is TOCTOU-safe and needs no exclusion scanning.  Kernel-managed RAM is
 * never labeled.  The algorithm is declared DRAM minus every top-level
 * /proc/iomem entry (System RAM, reserved, ACPI tables, ...), page-aligned,
 * at least 64 MiB, and confirmed reachable by mapping one page of it through
 * /dev/mem.
 *
 * Block devices are whole disks from /sys/block, sized by ioctl.
 */

#include "sink_proto.h"

#include <stdint.h>

/* Fill inv with this machine's labelable targets (carved-out RAM regions,
 * whole block devices) followed by its RAM map (sink_proto.h, Flow 1: the
 * declared DRAM and what claimed or ruled out the rest of it), all with node
 * "".  RAM is discovered on the first call, which also prints a DRAM
 * accounting (exact only as root), and cached; block devices are enumerated
 * afresh on every call, because they can appear after startup (NVMe and MMC
 * probe asynchronously), so call it for every InventoryRequest.  No RAM is
 * not an error by itself (a sink may serve only disks); the caller decides
 * what an inventory without targets means.  Returns the number of entries,
 * or -1 (message printed) if they do not fit in SINK_MAX_TARGETS. */
int      sink_inventory_local(sink_inventory_t *inv);

/* Append one entry (a target or a map entry) to inv.  The source-daemon adds
 * its GPU targets this way.  Returns 0, or -1 (message printed) if full. */
int      sink_inventory_add(sink_inventory_t *inv, uint8_t kind, const char *name,
                            uint64_t base, uint64_t size, uint32_t sector);

/* How many of an inventory's entries are targets (not map entries). */
uint32_t sink_inventory_targets(const sink_inventory_t *inv);

/* Print an inventory's targets, one per line, with the node where there is
 * one; the RAM map entries are only counted. */
void     sink_inventory_print(const sink_inventory_t *inv);

/* Map [phys_start, phys_start+size) of a RAM target read-write through
 * /dev/mem (MAP_SHARED; O_SYNC on armv7 only, see open_devmem in
 * sink_inventory.c).  Both must be page-aligned.  NULL on failure. */
void    *sink_inventory_map_ram(uint64_t phys_start, uint64_t size);
void     sink_inventory_unmap_ram(void *p, uint64_t size);

#endif /* POSE_SINK_INVENTORY_H */
