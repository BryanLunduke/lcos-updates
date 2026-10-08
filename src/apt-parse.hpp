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
  /* Archive token from the Inst line, such as "Debian-Security:12/stable-security". */
  std::string archive;
  /* Display size from apt's --print-uris line. Empty when apt did not name one. */
  std::string size;
  /* True when the archive or a Release file marks a security origin or suite. */
  bool security = false;
};

struct SimulateResult {
  enum Status { UpToDate, Upgrades, Error, Success, KeptBack };
  Status status = UpToDate;
  std::string error_msg;
  std::string warning;
  std::vector<PackageUpgrade> packages;
  /* Packages apt will not upgrade because they need extra packages. */
  std::vector<std::string> kept_back;
  /* Classic kept-back packages that would remove another package (Breaks). */
  struct KeptRemoval {
    std::string package;
    std::vector<std::string> removes;
  };
  std::vector<KeptRemoval> kept_removals;
  /* Kept-back names a probe did not classify. Not "needs extra packages". */
  std::vector<std::string> unclassified;
  /* Apt's "Need to get … of archives." line for the reviewed set. Empty if unknown. */
  std::string download_need;
  /* Apt's "After this operation, …" line. Empty when that line was absent. */
  std::string disk_use;
  /* Updates apt deferred because of phasing. Offered again later. */
  std::vector<std::string> phased;
  /* Packages kept back because dpkg has them on hold. */
  std::vector<std::string> held;
  /* Conffiles dpkg left at the locally modified contents. */
  std::vector<std::string> conffiles_kept;
  /* Packages this install added to reboot-required.pkgs. */
  std::vector<std::string> reboot_pkgs;
  /* Packages already listed before this install. */
  std::vector<std::string> reboot_pending_pkgs;
  /* Post-update simulation did not match the reviewed set, so nothing was installed. */
  bool install_skipped = false;
  /* This install created or grew reboot-required. */
  bool reboot_required = false;
  /* reboot-required was already present and this install did not add packages. */
  bool reboot_already = false;
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

/* Parse one apt-get -s "Inst name [old] (new ...)" or "Inst name (new ...)" line.
 * A security archive token on that line sets PackageUpgrade::security. */
bool parse_inst_line(const std::string& line, PackageUpgrade& out);

/* An apt progress segment that is empty or only whitespace is the clear-line
 * apt writes before the next percent. It is not a status. */
bool progress_line_visible(const std::string& line);

/* Leading "28%" or a trailing "Reading package lists... 59%". -1 when absent. */
int progress_percent(const std::string& line);

/* True for a Release/InRelease body whose Origin is Debian-Security, or whose
 * Label, Suite, or Codename ends in -security. */
bool release_text_is_security(const std::string& text);

/* Security suite, codename, origin, and label values from Release text. */
void collect_security_ids(const std::string& text, std::vector<std::string>& ids);

/* Mark packages whose Inst archive is a security pocket, or names an id. */
void apply_security_ids(std::vector<PackageUpgrade>& packages, const std::vector<std::string>& ids);

/* Fill download_need, disk_use, and per-package sizes from --print-uris text.
 * download_need stays apt's own line. */
void apply_download_details(SimulateResult& result, const std::string& text);

/* The sentence shown for apt's "Need to get" line. A cached "0 B/N" download
 * says the packages are already downloaded and names N. */
std::string present_download_need(const std::string& text);

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

/* Check and install failures are worded separately. Apt's Err:, W:, and E:
 * lines are kept, including the URL. A connect timeout or refusal adds the
 * proxy sentence as a second hint. A resolve failure or an unreachable
 * network adds the offline sentence and still shows the hostname. A partial
 * index failure (some indexes were skipped) is not given either hint. A
 * helper timeout sentence is still mapped to the check or install wording.
 * A multi-line message is mapped one line at a time so one timeout does not
 * discard the other lines. */
enum class JobKind { Check, Install };
std::string friendly_job_error(const std::string& msg, JobKind kind);

/* REMOVED and NEW package sections from apt-get -s install. */
void parse_removal_plan(const std::string& text, std::vector<std::string>& removed,
                        std::vector<std::string>& newly);

/* Apt's "Need to get … of archives." line, or empty when that line is absent.
 * --print-uris prints it; apt-get -s upgrade does not. */
std::string parse_download_need(const std::string& text);

/* Sentences Check and Install share for phased, held, removal, and extra packages. */
std::string describe_remaining(const SimulateResult& result);
std::string kept_headline(const SimulateResult& result);

/* One model for the text the window and the notification show, and for
 * whether Check and Install are available afterwards. */
enum class PackageListAction { Hide, Show, Keep };

struct JobOutcome {
  std::string status;
  bool check_enabled = true;
  bool install_enabled = false;
  bool offer_restart = false;
  PackageListAction packages = PackageListAction::Keep;
};

JobOutcome outcome_check(const SimulateResult& result, int exit_code, bool have_rows);
JobOutcome outcome_install(const SimulateResult& result, int exit_code, bool have_rows);
JobOutcome outcome_timeout(bool installing, bool helper_ready, bool have_rows);

#endif
