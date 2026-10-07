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
  /* Conffiles dpkg left at the locally modified contents. */
  std::vector<std::string> conffiles_kept;
  /* Post-update simulation did not match the reviewed set, so nothing was installed. */
  bool install_skipped = false;
  /* From the apt summary line. -1 when that line was not present. */
  int upgraded_count = -1;
  int not_upgraded_count = -1;
};

/* Apt output retained by the helper and the window. The buffer stays within
 * about 1 MiB. The first E: line is kept even when the head is dropped. */
constexpr std::size_t kAptCaptureCap = 1024 * 1024;

struct CaptureBuf {
  std::string data;
  std::string first_error;
  std::string pending;
};

void capture_append(CaptureBuf& cap, const char* data, std::size_t n);
std::string capture_text(const CaptureBuf& cap);

/* Parse one apt-get -s "Inst name [old] (new ...)" or "Inst name (new ...)" line. */
bool parse_inst_line(const std::string& line, PackageUpgrade& out);

/* Parse full apt-get -s upgrade text. E: lines become Error.
 * A kept-back section or a non-zero "not upgraded" summary is not up to date. */
SimulateResult parse_apt_simulate(const std::string& text);

/* apt-get update exits non-zero for "some index files failed to download"
 * even when other indexes were refreshed. That is a partial failure. */
bool apt_index_failure_is_partial(const std::string& text);

/* E: lines (and key-signing / hash lines) to show for a partial index failure. */
std::string apt_index_warning(const std::string& text);

/* Stable helper stdout protocol. */
std::string format_protocol(const SimulateResult& result);
SimulateResult parse_protocol(const std::string& text);

/* Check and install failures are worded separately. The offline sentence is
 * only for resolve failures, an unreachable network, "unable to connect",
 * and "connection timed out". Other apt lines (404, hash mismatch, NO_PUBKEY)
 * are left as apt wrote them. A multi-line message is mapped one line at a
 * time so one timeout does not discard the other lines. */
enum class JobKind { Check, Install };
std::string friendly_job_error(const std::string& msg, JobKind kind);

#endif
