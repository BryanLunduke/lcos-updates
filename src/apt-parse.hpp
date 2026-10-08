/* lcos-updates — parse apt-get -s Inst lines and the helper protocol.
 * Copyright (C) 2026 LCOS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef LCOS_UPDATES_APT_PARSE_HPP
#define LCOS_UPDATES_APT_PARSE_HPP

#include <string>
#include <vector>

struct PackageUpgrade {
  std::string name;
  std::string old_version;
  std::string new_version;
};

struct SimulateResult {
  enum Status { UpToDate, Upgrades, Error, Success, KeptBack };
  Status status = UpToDate;
  std::string error_msg;
  std::string warning;
  std::vector<PackageUpgrade> packages;
  /* Packages apt will not upgrade because they need extra packages. */
  std::vector<std::string> kept_back;
  /* Updates apt deferred because of phasing. Offered again later. */
  std::vector<std::string> phased;
  /* Packages kept back because dpkg has them on hold. */
  std::vector<std::string> held;
  /* Conffiles dpkg left at the locally modified contents. */
  std::vector<std::string> conffiles_kept;
  /* Packages named in reboot-required.pkgs after a finished install. */
  std::vector<std::string> reboot_pkgs;
  /* Post-update simulation did not match the reviewed set, so nothing was installed. */
  bool install_skipped = false;
  /* /var/run/reboot-required exists after the install. */
  bool reboot_required = false;
  /* The install log had no apt summary line, so kept-back packages are unknown. */
  bool summary_missing = false;
  /* From the apt summary line. -1 when that line was not present. */
  int upgraded_count = -1;
  int not_upgraded_count = -1;
};

/* Apt output retained by the helper and the window. The buffer stays within
 * about 1 MiB. The first E: line, the summary line, kept-back / phasing /
 * held sections, and conffile notes are kept even when the head is dropped. */
constexpr std::size_t kAptCaptureCap = 1024 * 1024;

struct CaptureBuf {
  std::string data;
  std::string first_error;
  std::string pending;
  std::string preserved;
  bool in_preserved_list = false;
};

void capture_append(CaptureBuf& cap, const char* data, std::size_t n);
std::string capture_text(const CaptureBuf& cap);

/* Parse one apt-get -s "Inst name [old] (new ...)" or "Inst name (new ...)" line. */
bool parse_inst_line(const std::string& line, PackageUpgrade& out);

/* Parse full apt-get -s upgrade text. E: lines become Error.
 * A kept-back section or a non-zero "not upgraded" summary is not up to date. */
SimulateResult parse_apt_simulate(const std::string& text);

/* How apt-get update's combined stdout/stderr should be treated.
 * Total: every source failed, or nothing was fetched. Do not simulate.
 * Partial: at least one Hit: or Get:, and another index failed. Keep going. */
enum class UpdateFetchKind { Ok, Partial, Total };

struct UpdateFetch {
  UpdateFetchKind kind = UpdateFetchKind::Ok;
  std::string detail;
};

UpdateFetch classify_apt_update(const std::string& text);

/* True only for a partial failure: some indexes were fetched and one failed. */
bool apt_index_failure_is_partial(const std::string& text);

/* Notice lines to show for a partial index failure. */
std::string apt_index_warning(const std::string& text);

/* Move kept-back names whose dpkg status is "hold" into result.held.
 * status_text is the text of a dpkg status file, not a path. */
void apply_held_packages(SimulateResult& result, const std::string& status_text);

/* Stable helper stdout protocol. */
std::string format_protocol(const SimulateResult& result);
SimulateResult parse_protocol(const std::string& text);

/* Check and install failures are worded separately. Resolve failures and an
 * unreachable network use the offline sentence. "unable to connect" and
 * "connection timed out" say that a proxy has to be set in apt's
 * configuration. A partial index failure (some indexes were skipped) is not
 * rewritten into either sentence. Other apt lines (404, hash mismatch,
 * NO_PUBKEY) are left as apt wrote them. A multi-line message is mapped one
 * line at a time so one timeout does not discard the other lines. */
enum class JobKind { Check, Install };
std::string friendly_job_error(const std::string& msg, JobKind kind);

#endif
