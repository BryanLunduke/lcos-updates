/* lcos-updates — parse apt-get -s Inst lines and the helper protocol.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "apt-parse.hpp"

#include <cctype>
#include <sstream>

static std::string trim_cr(std::string line)
{
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}

static std::string lower_copy(std::string text)
{
  for (char& ch : text)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

static bool has(const std::string& text, const char* needle)
{
  return text.find(needle) != std::string::npos;
}

static void append_tokens(const std::string& line, std::vector<std::string>& names)
{
  std::istringstream in(line);
  std::string tok;
  while (in >> tok) {
    if (!tok.empty())
      names.push_back(tok);
  }
}

bool parse_inst_line(const std::string& raw, PackageUpgrade& out)
{
  const std::string line = trim_cr(raw);
  const std::string prefix = "Inst ";
  if (line.compare(0, prefix.size(), prefix) != 0)
    return false;

  std::string rest = line.substr(prefix.size());
  const std::string::size_type name_end = rest.find(' ');
  if (name_end == std::string::npos || name_end == 0)
    return false;

  out.name = rest.substr(0, name_end);
  rest = rest.substr(name_end + 1);
  while (!rest.empty() && rest[0] == ' ')
    rest.erase(0, 1);
  if (rest.empty())
    return false;

  if (rest[0] == '[') {
    const std::string::size_type close = rest.find(']');
    if (close == std::string::npos || close < 2)
      return false;
    out.old_version = rest.substr(1, close - 1);
    rest = rest.substr(close + 1);
    while (!rest.empty() && rest[0] == ' ')
      rest.erase(0, 1);
  } else {
    out.old_version = "-";
  }

  if (rest.empty() || rest[0] != '(')
    return false;
  rest.erase(0, 1);
  const std::string::size_type ver_end = rest.find_first_of(" )");
  if (ver_end == std::string::npos || ver_end == 0)
    return false;
  out.new_version = rest.substr(0, ver_end);
  return !out.name.empty() && !out.new_version.empty();
}

static int integer_before(const std::string& line, std::string::size_type word)
{
  std::string::size_type i = word;
  while (i > 0 && line[i - 1] == ' ')
    --i;
  const std::string::size_type end = i;
  while (i > 0 && std::isdigit(static_cast<unsigned char>(line[i - 1])))
    --i;
  if (i == end)
    return -1;
  int value = 0;
  for (; i < end; ++i)
    value = value * 10 + (line[i] - '0');
  return value;
}

/* "N upgraded, ... and M not upgraded." The first " upgraded" is the
 * upgraded count; " not upgraded" is the kept-back count. */
static bool parse_summary_counts(const std::string& line, int& upgraded, int& not_upgraded)
{
  const std::string::size_type not_pos = line.find(" not upgraded");
  if (not_pos == std::string::npos)
    return false;
  const std::string::size_type up_pos = line.find(" upgraded");
  if (up_pos == std::string::npos || up_pos >= not_pos)
    return false;
  const int up = integer_before(line, up_pos);
  const int kept = integer_before(line, not_pos);
  if (up < 0 || kept < 0)
    return false;
  upgraded = up;
  not_upgraded = kept;
  return true;
}

static bool is_kept_back_header(const std::string& line)
{
  const std::string header = "The following packages have been kept back:";
  return line.compare(0, header.size(), header) == 0;
}

static bool is_phasing_header(const std::string& line)
{
  const std::string header = "The following upgrades have been deferred due to phasing:";
  return line.compare(0, header.size(), header) == 0;
}

static bool is_held_changed_header(const std::string& line)
{
  const std::string header = "The following held packages will be changed:";
  return line.compare(0, header.size(), header) == 0;
}

static bool starts_with(const std::string& line, const char* prefix)
{
  const std::size_t n = std::char_traits<char>::length(prefix);
  return line.compare(0, n, prefix) == 0;
}

static std::string quoted_path(const std::string& line)
{
  const std::string::size_type open = line.find('\'');
  if (open == std::string::npos)
    return {};
  const std::string::size_type close = line.find('\'', open + 1);
  if (close == std::string::npos || close == open + 1)
    return {};
  return line.substr(open + 1, close - open - 1);
}

SimulateResult parse_apt_simulate(const std::string& text)
{
  SimulateResult result;
  result.status = SimulateResult::UpToDate;

  std::istringstream in(text);
  std::string line;
  enum class Section { None, Kept, Phased };
  Section section = Section::None;
  std::string current_conffile;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (section != Section::None) {
      if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
        append_tokens(line, section == Section::Phased ? result.phased : result.kept_back);
        continue;
      }
      section = Section::None;
    }
    if (line.compare(0, 19, "Configuration file ") == 0) {
      const std::string path = quoted_path(line);
      if (!path.empty())
        current_conffile = path;
      continue;
    }
    if (line.find("Keeping old config file") != std::string::npos) {
      std::string path = quoted_path(line);
      if (path.empty())
        path = current_conffile;
      if (!path.empty()) {
        bool seen = false;
        for (const auto& have : result.conffiles_kept) {
          if (have == path)
            seen = true;
        }
        if (!seen)
          result.conffiles_kept.push_back(path);
      }
      continue;
    }
    if (line.compare(0, 2, "E:") == 0) {
      result.status = SimulateResult::Error;
      if (result.error_msg.empty())
        result.error_msg = line;
      continue;
    }
    if (is_phasing_header(line)) {
      section = Section::Phased;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), result.phased);
      continue;
    }
    if (is_kept_back_header(line)) {
      section = Section::Kept;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), result.kept_back);
      continue;
    }
    /* Dist-upgrade warns that a hold will be changed. That is not a package
     * this tool left un-upgraded, so the names are not a held list. */
    if (is_held_changed_header(line)) {
      section = Section::None;
      continue;
    }
    int upgraded = -1;
    int not_upgraded = -1;
    if (parse_summary_counts(line, upgraded, not_upgraded)) {
      result.upgraded_count = upgraded;
      result.not_upgraded_count = not_upgraded;
      continue;
    }
    PackageUpgrade pkg;
    if (parse_inst_line(line, pkg))
      result.packages.push_back(pkg);
  }

  if (result.status == SimulateResult::Error)
    return result;
  const bool kept = !result.kept_back.empty() || !result.phased.empty() || !result.held.empty() ||
                    result.not_upgraded_count > 0;
  if (!result.packages.empty())
    result.status = SimulateResult::Upgrades;
  else if (kept)
    result.status = SimulateResult::KeptBack;
  else
    result.status = SimulateResult::UpToDate;
  return result;
}

static const char kProxyHint[] =
    "Could not connect to the update server. If this computer uses a proxy, set it in "
    "/etc/apt/apt.conf.d.";
static const char kOfflineHint[] =
    "No network connection. Connect to the Internet and try again.";

static bool is_hit_or_get(const std::string& line)
{
  return starts_with(line, "Hit:") || starts_with(line, "Get:");
}

static bool is_fetch_notice_line(const std::string& line, const std::string& lower)
{
  if (starts_with(line, "E:") || starts_with(line, "W:") || starts_with(line, "Err:"))
    return has(lower, "fail") || has(lower, "timed out") || has(lower, "unable to connect") ||
           has(lower, "could not connect") || has(lower, "could not resolve") ||
           has(lower, "temporary failure resolving") || has(lower, "name or service not known") ||
           has(lower, "network is unreachable") || has(lower, "hash sum mismatch") ||
           has(lower, "no_pubkey") || has(lower, "404") || has(lower, "index files");
  return has(lower, "some index files failed to download") || has(lower, "old ones used instead") ||
         has(lower, "hash sum mismatch") || has(lower, "no_pubkey");
}

UpdateFetch classify_apt_update(const std::string& text)
{
  UpdateFetch out;
  bool hit = false;
  bool fetch_fail = false;
  bool connect = false;
  bool resolve = false;
  std::string notices;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty())
      continue;
    if (is_hit_or_get(line))
      hit = true;
    const std::string lower = lower_copy(line);
    const bool index_note = has(lower, "some index files failed to download") ||
                            has(lower, "old ones used instead");
    const bool connect_line = has(lower, "connection timed out") || has(lower, "unable to connect") ||
                              has(lower, "could not connect");
    const bool resolve_line = has(lower, "temporary failure resolving") || has(lower, "could not resolve") ||
                              has(lower, "name or service not known") || has(lower, "network is unreachable");
    const bool fetch_line = has(lower, "failed to fetch") || has(lower, "hash sum mismatch") ||
                            has(lower, "no_pubkey") || starts_with(line, "Err:") || index_note ||
                            connect_line || resolve_line;
    if (fetch_line)
      fetch_fail = true;
    if (connect_line)
      connect = true;
    if (resolve_line)
      resolve = true;
    if (!is_fetch_notice_line(line, lower))
      continue;
    if (!notices.empty())
      notices += "\n";
    notices += line;
  }

  if (!fetch_fail)
    return out;
  /* No Hit: or Get: means nothing was fetched. That includes a dead mirror
   * whose apt still exits 0, and a resolve failure of every source. */
  if (!hit) {
    out.kind = UpdateFetchKind::Total;
    if (connect && !resolve)
      out.detail = kProxyHint;
    else if (resolve)
      out.detail = kOfflineHint;
    else if (!notices.empty())
      out.detail = notices;
    else
      out.detail = "The package lists could not be refreshed.";
    return out;
  }
  out.kind = UpdateFetchKind::Partial;
  out.detail = notices;
  return out;
}

bool apt_index_failure_is_partial(const std::string& text)
{
  return classify_apt_update(text).kind == UpdateFetchKind::Partial;
}

static bool is_index_warning_line(const std::string& line)
{
  if (line.compare(0, 2, "E:") == 0)
    return true;
  const std::string lower = lower_copy(line);
  return has(lower, "no_pubkey") || has(lower, "hash sum mismatch");
}

std::string apt_index_warning(const std::string& text)
{
  std::istringstream in(text);
  std::string line;
  std::string warning;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (!is_index_warning_line(line))
      continue;
    if (!warning.empty())
      warning += "\n";
    warning += line;
  }
  return warning;
}

static void emit_lines(std::ostringstream& out, const char* tag, const std::string& text)
{
  std::istringstream lines(text);
  std::string line;
  bool any = false;
  while (std::getline(lines, line)) {
    line = trim_cr(line);
    if (line.empty())
      continue;
    any = true;
    out << tag << " " << line << "\n";
  }
  if (!any)
    out << tag << " unknown error\n";
}

static void emit_extra(std::ostringstream& out, const SimulateResult& result)
{
  if (result.not_upgraded_count > 0)
    out << "NOT_UPGRADED " << result.not_upgraded_count << "\n";
  for (const auto& name : result.kept_back)
    out << "KEPT " << name << "\n";
  for (const auto& name : result.phased)
    out << "PHASED " << name << "\n";
  for (const auto& name : result.held)
    out << "HELD " << name << "\n";
  if (result.reboot_required) {
    if (result.reboot_pkgs.empty())
      out << "REBOOT\n";
    for (const auto& name : result.reboot_pkgs)
      out << "REBOOT " << name << "\n";
  }
  if (result.summary_missing)
    out << "SUMMARY_MISSING\n";
  for (const auto& path : result.conffiles_kept)
    out << "CONFKEPT " << path << "\n";
  if (result.install_skipped)
    out << "SKIPPED\n";
  if (result.warning.empty())
    return;
  std::istringstream lines(result.warning);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim_cr(line);
    if (!line.empty())
      out << "WARN " << line << "\n";
  }
}

std::string format_protocol(const SimulateResult& result)
{
  std::ostringstream out;
  switch (result.status) {
  case SimulateResult::UpToDate:
    out << "STATUS up-to-date\n";
    break;
  case SimulateResult::Upgrades:
    out << "STATUS upgrades\n";
    out << "COUNT " << result.packages.size() << "\n";
    for (const auto& pkg : result.packages)
      out << "PKG " << pkg.name << " " << pkg.old_version << " " << pkg.new_version
          << "\n";
    break;
  case SimulateResult::Success:
    out << "STATUS success\n";
    break;
  case SimulateResult::KeptBack:
    out << "STATUS kept-back\n";
    break;
  case SimulateResult::Error:
    out << "STATUS error\n";
    emit_lines(out, "MSG", result.error_msg);
    break;
  }
  emit_extra(out, result);
  return out.str();
}

SimulateResult parse_protocol(const std::string& text)
{
  SimulateResult result;
  result.status = SimulateResult::Error;
  result.error_msg = "No STATUS from helper";
  bool saw_status = false;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.compare(0, 7, "STATUS ") == 0) {
      const std::string st = line.substr(7);
      saw_status = true;
      if (st == "up-to-date") {
        result.status = SimulateResult::UpToDate;
        result.error_msg.clear();
      } else if (st == "upgrades") {
        result.status = SimulateResult::Upgrades;
        result.error_msg.clear();
      } else if (st == "success") {
        result.status = SimulateResult::Success;
        result.error_msg.clear();
      } else if (st == "kept-back") {
        result.status = SimulateResult::KeptBack;
        result.error_msg.clear();
      } else if (st == "error") {
        result.status = SimulateResult::Error;
        result.error_msg.clear();
      } else {
        result.status = SimulateResult::Error;
        result.error_msg = "Unknown STATUS " + st;
      }
    } else if (line.compare(0, 4, "MSG ") == 0) {
      const std::string msg = line.substr(4);
      if (result.error_msg.empty())
        result.error_msg = msg;
      else
        result.error_msg += "\n" + msg;
    } else if (line.compare(0, 9, "CONFKEPT ") == 0) {
      const std::string path = line.substr(9);
      if (!path.empty())
        result.conffiles_kept.push_back(path);
    } else if (line == "SKIPPED") {
      result.install_skipped = true;
    } else if (line.compare(0, 5, "WARN ") == 0) {
      const std::string msg = line.substr(5);
      if (result.warning.empty())
        result.warning = msg;
      else
        result.warning += "\n" + msg;
    } else if (line.compare(0, 5, "KEPT ") == 0) {
      const std::string name = line.substr(5);
      if (!name.empty())
        result.kept_back.push_back(name);
    } else if (line.compare(0, 7, "PHASED ") == 0) {
      const std::string name = line.substr(7);
      if (!name.empty())
        result.phased.push_back(name);
    } else if (line.compare(0, 5, "HELD ") == 0) {
      const std::string name = line.substr(5);
      if (!name.empty())
        result.held.push_back(name);
    } else if (line == "REBOOT") {
      result.reboot_required = true;
    } else if (line.compare(0, 7, "REBOOT ") == 0) {
      result.reboot_required = true;
      const std::string name = line.substr(7);
      if (!name.empty())
        result.reboot_pkgs.push_back(name);
    } else if (line == "SUMMARY_MISSING") {
      result.summary_missing = true;
    } else if (line.compare(0, 13, "NOT_UPGRADED ") == 0) {
      int value = 0;
      const std::string digits = line.substr(13);
      if (!digits.empty()) {
        for (char ch : digits) {
          if (!std::isdigit(static_cast<unsigned char>(ch)))
            break;
          value = value * 10 + (ch - '0');
        }
        result.not_upgraded_count = value;
      }
    } else if (line.compare(0, 4, "PKG ") == 0) {
      std::istringstream ps(line.substr(4));
      PackageUpgrade pkg;
      if (ps >> pkg.name >> pkg.old_version >> pkg.new_version)
        result.packages.push_back(pkg);
    }
  }

  if (!saw_status) {
    result.status = SimulateResult::Error;
    if (result.error_msg.empty())
      result.error_msg = "No STATUS from helper";
  }
  return result;
}

static std::string friendly_one_line(const std::string& msg, JobKind kind, bool leave_fetch)
{
  const std::string lower = lower_copy(msg);
  /* A partial refresh already says some indexes were skipped. One timed-out
   * mirror in that text is not "the computer is offline". */
  if (!leave_fetch && (has(lower, "connection timed out") || has(lower, "unable to connect") ||
                       has(lower, "could not connect")))
    return kProxyHint;
  if (!leave_fetch && (has(lower, "temporary failure resolving") || has(lower, "could not resolve") ||
                       has(lower, "name or service not known") || has(lower, "network is unreachable")))
    return kOfflineHint;
  if (!leave_fetch && (has(lower, "timeout") || has(lower, "timed out"))) {
    if (kind == JobKind::Install)
      return "Timed out while installing updates.";
    return "Timed out waiting for the update check. Check your network and try again.";
  }
  if (msg.empty())
    return "The update helper failed.";
  return msg;
}

std::string friendly_job_error(const std::string& msg, JobKind kind)
{
  const std::string lower = lower_copy(msg);
  const bool leave_fetch = has(lower, "some index files failed to download") ||
                           has(lower, "old ones used instead");
  if (msg.find('\n') == std::string::npos)
    return friendly_one_line(msg, kind, leave_fetch);
  std::istringstream in(msg);
  std::string line;
  std::string out;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty())
      continue;
    const std::string one = friendly_one_line(line, kind, leave_fetch);
    if (one.empty())
      continue;
    if (!out.empty())
      out += "\n";
    out += one;
  }
  if (out.empty())
    return friendly_one_line(msg, kind, leave_fetch);
  return out;
}

void apply_held_packages(SimulateResult& result, const std::string& status_text)
{
  if (result.kept_back.empty() || status_text.empty())
    return;
  std::vector<std::string> holds;
  std::string pkg;
  std::string status;
  std::string arch;
  auto flush = [&]() {
    if (!pkg.empty() && starts_with(status, "hold")) {
      holds.push_back(pkg);
      if (!arch.empty())
        holds.push_back(pkg + ":" + arch);
    }
    pkg.clear();
    status.clear();
    arch.clear();
  };
  std::istringstream in(status_text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty()) {
      flush();
      continue;
    }
    if (starts_with(line, "Package: "))
      pkg = line.substr(9);
    else if (starts_with(line, "Status: "))
      status = line.substr(8);
    else if (starts_with(line, "Architecture: "))
      arch = line.substr(14);
  }
  flush();
  if (holds.empty())
    return;
  std::vector<std::string> still;
  for (const auto& name : result.kept_back) {
    bool held = false;
    for (const auto& have : holds) {
      if (have == name)
        held = true;
    }
    if (held)
      result.held.push_back(name);
    else
      still.push_back(name);
  }
  result.kept_back.swap(still);
}

static void append_preserved(CaptureBuf& cap, const std::string& line)
{
  if (line.empty())
    return;
  cap.preserved += line;
  cap.preserved.push_back('\n');
}

static bool is_summary_line(const std::string& line)
{
  int upgraded = -1;
  int not_upgraded = -1;
  return parse_summary_counts(line, upgraded, not_upgraded);
}

static void note_error_line(CaptureBuf& cap, const std::string& line_in)
{
  std::string line = trim_cr(line_in);
  if (cap.first_error.empty() && line.compare(0, 2, "E:") == 0) {
    if (line.size() > 2048)
      line.resize(2048);
    cap.first_error = line;
  }
  if (cap.in_preserved_list) {
    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      append_preserved(cap, line);
      return;
    }
    cap.in_preserved_list = false;
  }
  if (is_kept_back_header(line) || is_phasing_header(line) || is_held_changed_header(line)) {
    append_preserved(cap, line);
    cap.in_preserved_list = true;
    return;
  }
  if (is_summary_line(line) || line.compare(0, 19, "Configuration file ") == 0 ||
      line.find("Keeping old config file") != std::string::npos ||
      line.find("kept back due to phasing") != std::string::npos)
    append_preserved(cap, line);
}

void capture_append(CaptureBuf& cap, const char* data, std::size_t n)
{
  if (data == nullptr || n == 0)
    return;
  std::size_t off = 0;
  while (off < n) {
    if (cap.pending.size() >= 8192) {
      note_error_line(cap, cap.pending);
      cap.pending.clear();
    }
    std::size_t take = n - off;
    const std::size_t room = 8192 - cap.pending.size();
    if (take > room)
      take = room;
    cap.pending.append(data + off, take);
    off += take;
    std::string::size_type pos = 0;
    while ((pos = cap.pending.find('\n')) != std::string::npos) {
      note_error_line(cap, cap.pending.substr(0, pos));
      cap.pending.erase(0, pos + 1);
    }
  }

  if (cap.data.size() + n <= kAptCaptureCap) {
    cap.data.append(data, n);
    return;
  }
  if (n >= kAptCaptureCap) {
    cap.data.assign(data + (n - kAptCaptureCap), kAptCaptureCap);
    return;
  }
  cap.data.append(data, n);
  cap.data.erase(0, cap.data.size() - kAptCaptureCap);
}

std::string capture_text(const CaptureBuf& cap)
{
  std::string prefix;
  auto add_line = [&](const std::string& line) {
    if (line.empty())
      return;
    if (cap.data.find(line) != std::string::npos)
      return;
    if (prefix.find(line) != std::string::npos)
      return;
    prefix += line;
    prefix.push_back('\n');
  };
  std::string first = cap.first_error;
  if (first.empty() && cap.pending.compare(0, 2, "E:") == 0) {
    first = trim_cr(cap.pending);
    if (first.size() > 2048)
      first.resize(2048);
  }
  add_line(first);
  std::istringstream preserved(cap.preserved);
  std::string line;
  while (std::getline(preserved, line))
    add_line(trim_cr(line));
  return prefix + cap.data;
}
