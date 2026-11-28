/* BIOS grub-pc boot disk: decide whether install_devices must be set.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Calamares installs GRUB and leaves grub-pc/install_devices empty. A later
 * non-interactive grub-pc configure then fails, and Repair repeats that
 * configure. These functions decide the disk and the debconf line. They do
 * not run commands and they never mention install_devices_empty.
 */

#ifndef LCOS_UPDATES_GRUB_PC_HPP
#define LCOS_UPDATES_GRUB_PC_HPP

#include <string>
#include <utility>
#include <vector>

/* True when dpkg status has grub-pc on the system (installed, or not yet
 * fully configured). config-files and not-installed are false. */
bool grub_pc_package_present(const std::string& status_text);

/* Value of grub-pc/install_devices in debconf config.dat, trimmed. Empty
 * when the question is missing or the value is only whitespace. */
std::string grub_pc_install_devices_value(const std::string& config_dat);

/* Kernel name of the whole disk for a partition, or empty when `name` is
 * not a partition. sda1 -> sda, nvme0n1p2 -> nvme0n1, mmcblk0p1 -> mmcblk0,
 * vda2 -> vda. sda and nvme0n1 return empty. */
std::string partition_parent_name(const std::string& name);

/* True for a whole disk grub can be installed on: sdX, vdX, hdX, xvdX,
 * nvmeXnY, mmcblkN. Partitions and device-mapper names are false. */
bool is_whole_disk_name(const std::string& name);

/* Slaves are kernel names (dm-0 -> sda2). alias maps a mapper name to the
 * dm name it points at. by_id maps a /dev/disk/by-id link name to the kernel
 * name that link points at (the link's last path component). */
struct DiskTopology {
  std::vector<std::pair<std::string, std::vector<std::string>>> slaves;
  std::vector<std::pair<std::string, std::string>> alias;
  std::vector<std::pair<std::string, std::string>> by_id;
};

/* Whole-disk debconf paths for `grub-probe -t disk` output. Partitions are
 * reduced to the disk. Device-mapper and RAID names are walked through
 * slaves to the underlying disks. Several probe devices become several
 * disks. Each disk is /dev/disk/by-id/<name> when a by-id link points at
 * that whole disk, otherwise /dev/<disk>. Empty when nothing usable was
 * found. Never a partition. */
std::vector<std::string> grub_boot_disk_devices(const std::string& probe_output,
                                               const DiskTopology& topo);

enum class GrubPcAction {
  Skip,    /* not BIOS grub-pc, or install_devices is already set */
  Set,     /* write these devices */
  Unknown, /* BIOS grub-pc, empty install_devices, disk not known */
};

struct GrubPcDecision {
  GrubPcAction action = GrubPcAction::Skip;
  std::vector<std::string> devices;
  std::string log;
};

struct GrubPcFacts {
  bool package_present = false;
  bool bios_core_img = false;
  std::string install_devices;
  bool probe_ok = false;
  std::string probe_output;
  DiskTopology topo;
};

/* Skip when grub-pc is absent, when /boot/grub/i386-pc/core.img is absent
 * (UEFI-only), or when install_devices is already set. Set when the probe
 * names a whole disk. Unknown when the disk cannot be determined. The log
 * is empty when there is nothing to do. */
GrubPcDecision decide_grub_pc(const GrubPcFacts& facts);

/* One debconf-set-selections record. Empty when devices is empty or any
 * path is not a safe /dev path. Never mentions install_devices_empty. */
std::string grub_pc_debconf_line(const std::vector<std::string>& devices);

/* Shown when Repair cannot find the boot disk. Does not offer Repair again. */
std::string grub_pc_manual_message();

/* True when dpkg output is a failed configure of package grub-pc, and not
 * of grub-pc-bin or grub-efi. */
bool grub_pc_configure_failed(const std::string& text);

#endif
