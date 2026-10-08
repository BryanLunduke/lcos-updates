/* Stop a process and its descendants.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LCOS_UPDATES_PROC_GROUP_HPP
#define LCOS_UPDATES_PROC_GROUP_HPP

#include <sys/types.h>
#include <vector>

struct DpkgNote {
  pid_t pid = 0;
  unsigned long long starttime = 0;
};

/* Remember configuring dpkg descendants so a later check can see them after
 * they are reparented. A pid is recorded only while it is still in the tree. */
void remember_configuring_dpkg(pid_t leader, std::vector<DpkgNote>& notes);

/* True when a noted dpkg is still the same live process. */
bool configuring_dpkg_alive(const std::vector<DpkgNote>& notes);

/* True when a non-zombie descendant is configuring packages.
 * That is a cmdline containing --unpack, --configure, --install, --remove,
 * or --triggers-only. dpkg-query and dpkg queries (--print-architecture,
 * --print-foreign-architectures, --assert-multi-arch, --compare-versions)
 * do not count. Zombies do not count. comm alone is not enough. */
bool process_tree_contains_dpkg(pid_t leader);

/* Clock-tick start time from /proc/pid/stat, or 0 if it cannot be read. */
unsigned long long proc_starttime(pid_t pid);

/* SIGTERM leader and its descendants, wait term_grace_ms, then SIGKILL.
 * Each signal is sent to a descendant pid only when its start time still
 * matches the snapshot. Process groups are not signalled: kill(-pgid) would
 * hit processes that merely share a group.
 * Returns false if a configuring dpkg is in the tree. If it is already
 * present, nothing is signalled. If it appears after SIGTERM, SIGKILL is
 * not sent. When signaled is non-null it is set true if a signal was sent.
 * term_grace_ms == 0 does not sleep.
 * Never signals this process.
 */
bool terminate_process_tree(pid_t leader, int term_grace_ms, bool* signaled = nullptr);

#endif
