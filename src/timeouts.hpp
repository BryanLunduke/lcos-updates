/* Shared check/install deadlines for the GUI and the privileged helper.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each apt-get run has an idle limit and a hard cap. The idle limit restarts
 * whenever apt writes output, so a slow mirror that is still transferring is
 * not killed at a fixed clock. The hard cap does not restart. The GUI must
 * wait at least as long as the hard caps added together, or it will kill a
 * check that is still inside the helper's own budget.
 *
 * The password dialog uses kAuthTimeoutSec, not the apt budget. The apt
 * budgets start when the helper announces HELPER_READY.
 */

#ifndef LCOS_UPDATES_TIMEOUTS_HPP
#define LCOS_UPDATES_TIMEOUTS_HPP

constexpr int kAuthTimeoutSec = 90;

constexpr int kUpdateIdleSec = 180;
constexpr int kUpdateHardCapSec = 900;
constexpr int kSimulateIdleSec = 180;
constexpr int kSimulateHardCapSec = 300;
constexpr int kUpgradeIdleSec = 300;
constexpr int kUpgradeHardCapSec = 7200;

/* After dpkg has started unpacking or configuring, the helper does not kill
 * apt on the idle clock or the hard cap. The window instead counts the
 * current phase and, with no apt or dpkg progress, says the install is still
 * running. A longer stall downgrades the shutdown inhibitor. */
constexpr int kStallNoticeSec = 10 * 60;
constexpr int kStallInhibitSec = 30 * 60;

/* Kept-back classification is its own pre-dpkg phase. Each probe is short.
 * The whole pass stops at kKeptClassifySec, and names still waiting are left
 * unclassified instead of being called extra packages. */
constexpr int kKeptProbeIdleSec = 8;
constexpr int kKeptProbeHardSec = 12;
constexpr int kKeptClassifySec = 45;

/* apt-get --print-uris upgrade prints the download size and does not install. */
constexpr int kDownloadSizeIdleSec = 30;
constexpr int kDownloadSizeHardSec = 60;

/* Names kept for the GUI backstop, which must cover the helper hard caps
 * plus kept-back classification and the download-size request. */
constexpr int kUpdateTimeoutSec = kUpdateHardCapSec;
constexpr int kSimulateTimeoutSec = kSimulateHardCapSec;
constexpr int kUpgradeTimeoutSec = kUpgradeHardCapSec;
constexpr int kGuiTimeoutMarginSec = 30;

constexpr int kCheckTimeoutSec = kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec +
                                 kDownloadSizeHardSec + kGuiTimeoutMarginSec;
constexpr int kInstallTimeoutSec = kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec +
                                   kUpgradeTimeoutSec + kGuiTimeoutMarginSec;

/* Polite SIGTERM window before SIGKILL of apt and its descendants.
 * Not used once a configuring dpkg is in the tree. A grace of 0 must not sleep. */
constexpr int kTermGraceMs = 2000;

#endif
