/* Shared check/install deadlines for the GUI and the privileged helper.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The helper clocks apt-get update and the simulation separately. The GUI
 * must wait at least as long as those two limits added together, or it will
 * kill a check that is still inside the helper's own budget and show it as
 * timed out. Install uses the same 30s slack the 0.7-1 window already used
 * (600s helper, 630s GUI).
 */

#ifndef LCOS_UPDATES_TIMEOUTS_HPP
#define LCOS_UPDATES_TIMEOUTS_HPP

constexpr int kUpdateTimeoutSec = 120;
constexpr int kSimulateTimeoutSec = 120;
constexpr int kUpgradeTimeoutSec = 600;
constexpr int kGuiTimeoutMarginSec = 30;

constexpr int kCheckTimeoutSec =
    kUpdateTimeoutSec + kSimulateTimeoutSec + kGuiTimeoutMarginSec;
constexpr int kInstallTimeoutSec = kUpgradeTimeoutSec + kGuiTimeoutMarginSec;

/* Polite SIGTERM window before SIGKILL of an apt/dpkg process group. */
constexpr int kTermGraceMs = 2000;

/* How long the GUI waits for the helper to reap apt after a cancel.
 * Longer than kTermGraceMs plus the helper's post-SIGKILL wait, so a root
 * helper can finish killing dpkg before the GUI gives up on it. */
constexpr int kGuiCancelGraceMs = 6000;

#endif
