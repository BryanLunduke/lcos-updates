/* Stop a process and every descendant, including separate process groups.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LCOS_UPDATES_PROC_GROUP_HPP
#define LCOS_UPDATES_PROC_GROUP_HPP

#include <sys/types.h>

/* SIGTERM leader and its descendants, wait term_grace_ms, then SIGKILL.
 * Waits until the leader is reaped (when it is our child) and tracked
 * members are gone. Returns false if any tracked process is still alive
 * (for example a root child this process is not allowed to signal).
 * Never signals this process or its own process group.
 */
bool terminate_process_tree(pid_t leader, int term_grace_ms);

#endif
