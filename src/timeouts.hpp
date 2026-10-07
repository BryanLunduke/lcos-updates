/* Shared check/install deadlines for the GUI and the privileged helper.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The helper clocks apt-get update and the simulation separately. The GUI
 * must wait at least as long as those two limits added together, or it will
 * kill a check that is still inside the helper's own budget and show it as
 * timed out. Install refreshes package lists and then upgrades, so its
 * budget is update plus upgrade plus the same 30s slack.
 *
 * Both budgets start when the helper announces HELPER_READY, not when the
 * password dialog opens.
 */

#ifndef LCOS_UPDATES_TIMEOUTS_HPP
#define LCOS_UPDATES_TIMEOUTS_HPP

constexpr int kUpdateTimeoutSec = 120;
constexpr int kSimulateTimeoutSec = 120;
constexpr int kUpgradeTimeoutSec = 600;
constexpr int kGuiTimeoutMarginSec = 30;

constexpr int kCheckTimeoutSec =
    kUpdateTimeoutSec + kSimulateTimeoutSec + kGuiTimeoutMarginSec;
constexpr int kInstallTimeoutSec =
    kUpdateTimeoutSec + kUpgradeTimeoutSec + kGuiTimeoutMarginSec;

/* Polite SIGTERM window before SIGKILL of an apt process group.
 * Not used once dpkg is in the tree. A grace of 0 must not sleep. */
constexpr int kTermGraceMs = 2000;

#endif
