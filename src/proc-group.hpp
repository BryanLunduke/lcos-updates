/* Stop a process and every descendant, including separate process groups.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LCOS_UPDATES_PROC_GROUP_HPP
#define LCOS_UPDATES_PROC_GROUP_HPP

#include <sys/types.h>

/* True when leader or any descendant is dpkg (or dpkg-*). Once package
 * configuration has started, callers must not signal that tree. */
bool process_tree_contains_dpkg(pid_t leader);

/* SIGTERM leader and its descendants, wait term_grace_ms, then SIGKILL.
 * Waits until the leader is reaped (when it is our child) and tracked
 * members are gone. Returns false if any tracked process is still alive
 * (for example a root child this process is not allowed to signal), or if
 * dpkg is in the tree. dpkg is never signaled: if it is already present,
 * nothing in the tree is signaled; if it appears after SIGTERM, SIGKILL
 * is not sent.
 * term_grace_ms == 0 does not sleep (no grace wait, no post-SIGKILL poll).
 * Never signals this process or its own process group.
 */
bool terminate_process_tree(pid_t leader, int term_grace_ms);

#endif
