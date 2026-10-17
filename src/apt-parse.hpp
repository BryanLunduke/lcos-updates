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
  /* New packages a safe kept-back upgrade will install. Not upgrades. */
  std::vector<PackageUpgrade> new_packages;
  /* Kept-back names adopted because the probe only added packages. */
  std::vector<std::string> adopted_kept;
  /* Upgrades taken from those probes (the kept-back package and any others). */
  std::vector<PackageUpgrade> kept_upgrades;
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
  /* Round 15 (U-2): packages apt-get autoremove would remove (no longer
   * needed). Offered as "Remove Unused Packages", never removed silently. */
  std::vector<std::string> unused;
  /* Round 15 (U-2): a cleanup run removed this many packages. -1 if none ran. */
  int removed_unused = -1;
  /* The install log had no apt summary line, so kept-back packages are unknown. */
  bool summary_missing = false;
  /* From the apt summary line. -1 when that line was not present. */
  int upgraded_count = -1;
  int not_upgraded_count = -1;
  /* Round 12 (U-1): reviewed packages that use debconf and may ask setup
   * questions. The install runs non-interactively, so defaults are used. */
  std::vector<std::string> debconf_pkgs;
  struct DebconfAnswer {
    std::string package;
    std::string question;
    std::string value;
    /* Round 13 (U-3): the question's text from its template ("Pick a colour"). */
    std::string text;
  };
  /* Questions the install answered without asking (not marked seen). */
  std::vector<DebconfAnswer> debconf_answers;
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

/* Leading "28%" or a trailing "Reading package lists... 59%".
 * A status-fd line uses the float percent (dlstatus:1:17.5000:…). -1 when absent. */
int progress_percent(const std::string& line);

/* Package named by a progress line. Empty for apt placeholders ([Working],
 * [Connecting …], [Waiting …]), Hit/Get/Ign/Err prefixes, dlstatus text,
 * dpkg-exec, and error lines. */
std::string progress_package_name(const std::string& line);

/* True when the line must not replace a status that already names a package. */
bool progress_is_download_noise(const std::string& line);

/* Unpacking, Setting up, or a pmstatus line that names a real package. */
bool progress_is_configure_line(const std::string& line);

/* How one progress segment updates the line on screen.
 * A placeholder does not replace a line that already names a package.
 * The percent is still taken from the new segment. */
struct AptProgressNote {
  bool replace_shown = false;
  std::string shown;
  bool have_percent = false;
  int percent = -1;
};

AptProgressNote apt_progress_note(const std::string& line, const std::string& current_shown);

/* Sentence for the line currently on screen. Placeholders never become a name. */
std::string friendly_progress_text(const std::string& line, const std::string& phase);

/* Percent-decode apt's filename version (%3a is the epoch colon). */
std::string canonical_deb_version(const std::string& version);

/* True when two versions are the same after filename decoding.
 * An epoch (N:ver) is part of the version. */
bool deb_versions_match(const std::string& a, const std::string& b);

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
/* Field escaping for that protocol. token also escapes a space. */
std::string protocol_escape(const std::string& value, bool token);
std::string protocol_unescape(const std::string& value);
/* True when an error says dpkg/apt is left interrupted or broken and the
 * Repair action (dpkg --configure -a, then apt-get -f install) applies. */
bool error_suggests_repair(const std::string& msg);

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

/* One apt-get -s install simulation: removals, new packages, downgrades, Inst lines. */
struct InstallPlan {
  std::vector<std::string> removed;
  std::vector<std::string> newly;
  std::vector<std::string> downgraded;
  std::vector<PackageUpgrade> inst;
  /* From the summary line. -1 when that phrase was absent. */
  int remove_count = -1;
  int downgrade_count = -1;
};

InstallPlan parse_install_plan(const std::string& text);

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
/* Round 15 (U-2): result of "Remove Unused Packages". */
JobOutcome outcome_cleanup(const SimulateResult& result, int exit_code);
/* Round 15 (U-1): packages whose install needs a restart even when nothing
 * wrote /var/run/reboot-required: kernels, firmware and CPU microcode. */
bool needs_reboot_package(const std::string& name);
std::vector<std::string> reboot_trigger_packages(const std::vector<std::string>& names);
/* 0.9.5-2: the system's restart marker (/var/run/reboot-required and its
 * .pkgs list), read whenever the window opens or a job ends. */
struct RebootMarker {
  bool present = false;
  std::vector<std::string> pkgs;
};
/* Package names from reboot-required.pkgs: trimmed, blank lines skipped,
 * duplicates dropped, first-seen order kept. */
std::vector<std::string> parse_reboot_marker_pkgs(const std::string& text);
/* The notice for a marker; empty when no marker exists. */
std::string reboot_marker_sentence(const RebootMarker& marker);
/* Offers Restart for a marker unless the outcome already offers it. lead puts
 * the notice first (above a long error) instead of after the status. Returns
 * true when the marker added the notice. */
bool apply_reboot_marker(JobOutcome& out, const RebootMarker& marker, bool lead);
/* Round 15 (U-2): package names from "Remv" lines of apt-get -s autoremove. */
std::vector<std::string> parse_autoremove(const std::string& text);
JobOutcome outcome_timeout(bool installing, bool helper_ready, bool have_rows);
/* Round 11: a Repair timeout is worded as a repair, not as an install. */
JobOutcome outcome_repair_timeout(bool helper_ready);

/* Round 11 (U-1, U-5): one stanza of /var/lib/dpkg/status. state is the
 * third word of Status: (installed, unpacked, half-configured, ...). */
struct DpkgPackageState {
  std::string name;
  std::string state;
  std::string version;
};
std::vector<DpkgPackageState> parse_dpkg_status(const std::string& status_text);
/* Names dpkg --audit would report as not fully installed: half-installed,
 * unpacked, half-configured, triggers-awaited, triggers-pending. Sorted. */
std::vector<std::string> unfinished_packages(const std::string& status_text);
/* The Check/Install error for that state. It always says "Use Repair" so the
 * window offers the Repair button. journal is true when dpkg's update journal
 * (/var/lib/dpkg/updates) is not empty, i.e. dpkg itself was killed. */
std::string unfinished_message(const std::vector<std::string>& names, bool journal);
/* Round 11 (U-2): dpkg or apt could not take its lock (another program is
 * installing). */
bool error_says_locked(const std::string& text);
extern const char kAnotherInstaller[];
/* Round 11 (U-4): every remote source failed to resolve or connect. */
extern const char kServerUnreachable[];

/* Round 12 (U-1). Names of packages in apt-cache show output whose Depends
 * or Pre-Depends name debconf or debconf-2.0. Sorted, unique. */
std::vector<std::string> packages_depending_on_debconf(const std::string& apt_cache_show);
/* Owners of any question in debconf's config.dat. Sorted, unique. */
std::vector<std::string> debconf_question_owners(const std::string& config_dat);
/* Questions owned by one of pkgs that were not shown to anyone (no "seen"
 * flag), with their stored value. Only asking types (string, boolean,
 * select, multiselect, password); password values are never returned. */
std::vector<SimulateResult::DebconfAnswer> unseen_debconf_answers(const std::string& config_dat,
                                                                  const std::string& templates_dat,
                                                                  const std::vector<std::string>& pkgs);
/* Review-dialog notice for packages that may ask setup questions. Empty when none. */
std::string debconf_review_notice(const std::vector<std::string>& pkgs);
/* After-install sentence listing auto-answered questions and how to change them. */
std::string debconf_answer_sentence(const SimulateResult& result);

/* Round 13. What debconf held for one question before or after an install.
 * Password values are never kept: only whether one was set. */
struct DebconfQuestion {
  std::string name;
  std::string value;
  bool has_value = false;
  bool seen = false;
  bool password = false;
  std::string owner; /* first Owners entry */
};
/* config.dat plus passwords.dat. passwords.dat entries keep has_value only. */
std::vector<DebconfQuestion> debconf_snapshot(const std::string& config_dat, const std::string& passwords_dat);
/* One "<-- INPUT priority question" line from DEBCONF_DEBUG=developer, with
 * the package named by the latest "package name is" line. */
struct DebconfInput {
  std::string package;
  std::string priority;
  std::string question;
};
std::vector<DebconfInput> debconf_inputs(const std::string& apt_output);
/* Round 13b: the install runs with DEBCONF_NONINTERACTIVE_SEEN=true and no
 * DEBCONF_DEBUG. Debconf itself applies the priority and seen checks and marks
 * each question it would have shown, so a question that turned seen during
 * the install is one a normal install would have asked. Nothing here reads a
 * protocol trace, so no answer value is ever written to apt's term.log. */
std::vector<DebconfInput> debconf_newly_seen(const std::vector<DebconfQuestion>& before,
                                             const std::vector<DebconfQuestion>& after);
/* Drops every "debconf (developer):" line. That trace can carry values,
 * including a password a maintainer script reads back, so it never leaves
 * the helper, in the log or in an error message. */
std::string strip_debconf_trace(const std::string& text);
bool is_debconf_trace_line(const std::string& line);
/* debconf/priority from config.dat; "high" when unset or unknown. */
std::string debconf_priority(const std::string& config_dat);
/* Questions a normal (interactive) install at the configured priority would
 * have asked, that were not answered before (no "seen" flag before), and
 * that are new or changed since the snapshot taken before the install, so a
 * later upgrade does not list the same answers again. Text comes from
 * templates.dat. Passwords give "password left blank" or "password set,
 * not shown", never the value. */
std::vector<SimulateResult::DebconfAnswer> debconf_auto_answers(const std::vector<DebconfInput>& inputs,
                                                                const std::vector<DebconfQuestion>& before,
                                                                const std::vector<DebconfQuestion>& after,
                                                                const std::string& templates_dat,
                                                                const std::string& priority);

#endif
