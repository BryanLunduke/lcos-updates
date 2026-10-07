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

SimulateResult parse_apt_simulate(const std::string& text)
{
  SimulateResult result;
  result.status = SimulateResult::UpToDate;

  std::istringstream in(text);
  std::string line;
  bool in_kept = false;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (in_kept) {
      if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
        append_tokens(line, result.kept_back);
        continue;
      }
      in_kept = false;
    }
    if (line.compare(0, 2, "E:") == 0) {
      result.status = SimulateResult::Error;
      if (result.error_msg.empty())
        result.error_msg = line;
      continue;
    }
    if (is_kept_back_header(line)) {
      in_kept = true;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), result.kept_back);
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
  const bool kept = !result.kept_back.empty() || result.not_upgraded_count > 0;
  if (!result.packages.empty())
    result.status = SimulateResult::Upgrades;
  else if (kept)
    result.status = SimulateResult::KeptBack;
  else
    result.status = SimulateResult::UpToDate;
  return result;
}

bool apt_index_failure_is_partial(const std::string& text)
{
  const std::string lower = lower_copy(text);
  return has(lower, "some index files failed to download") ||
         has(lower, "old ones used instead");
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

static void emit_extra(std::ostringstream& out, const SimulateResult& result)
{
  if (result.not_upgraded_count > 0)
    out << "NOT_UPGRADED " << result.not_upgraded_count << "\n";
  for (const auto& name : result.kept_back)
    out << "KEPT " << name << "\n";
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
    if (!result.error_msg.empty())
      out << "MSG " << result.error_msg << "\n";
    else
      out << "MSG unknown error\n";
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

std::string friendly_job_error(const std::string& msg, JobKind kind)
{
  const std::string lower = lower_copy(msg);
  /* Offline wording is reserved for failures that mean the network itself
   * is unusable. "Failed to fetch" alone is not enough: a 404, a hash
   * mismatch, or a missing key must stay the apt line. "Connection timed
   * out" still matches below when apt includes it on a fetch line. */
  if (has(lower, "temporary failure resolving") || has(lower, "could not resolve") ||
      has(lower, "name or service not known") || has(lower, "network is unreachable") ||
      has(lower, "connection timed out") || has(lower, "unable to connect"))
    return "No network connection. Connect to the Internet and try again.";
  if (has(lower, "timeout") || has(lower, "timed out")) {
    if (kind == JobKind::Install)
      return "Timed out while installing updates.";
    return "Timed out waiting for the update check. Check your network and try again.";
  }
  if (msg.empty())
    return "The update helper failed.";
  return msg;
}
