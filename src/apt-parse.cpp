/* lcos-updates — parse apt-get -s Inst lines and the helper protocol.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "apt-parse.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

const char kAnotherInstaller[] =
    "Another program is installing software. Wait for it to finish and try again.";
const char kServerUnreachable[] =
    "Can't reach the update server. The results below may be out of date.";

/* Decimal count in [begin, end). False for a non-digit or a value above
 * kMaxCount, so a hostile digit run cannot overflow int (round 10, U-3). */
static constexpr int kMaxCount = 1000000000;
static bool checked_count(const std::string& text, std::size_t begin, std::size_t end, int& out)
{
  if (begin >= end || end > text.size())
    return false;
  long long value = 0;
  for (std::size_t i = begin; i < end; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (!std::isdigit(ch))
      return false;
    value = value * 10 + (ch - '0');
    if (value > kMaxCount)
      return false;
  }
  out = static_cast<int>(value);
  return true;
}

static std::string trim_cr(std::string line)
{
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  return line;
}

/* Apt percent lines end with a carriage return. Treat that as a line break
 * so a later summary, kept-back name, or E: line is not glued to the percent.
 * A CRLF pair is one break, not a blank line. */
static std::string normalize_breaks(std::string text)
{
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < text.size() && text[i + 1] == '\n')
        ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

static bool archive_is_security(const std::string& archive);

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
  rest = rest.substr(ver_end);
  while (!rest.empty() && rest[0] == ' ')
    rest.erase(0, 1);
  if (!rest.empty() && rest[0] != ')') {
    const std::string::size_type arch = rest.find_first_of(" )");
    out.archive = arch == std::string::npos ? rest : rest.substr(0, arch);
  }
  out.security = archive_is_security(out.archive);
  return !out.name.empty() && !out.new_version.empty();
}

static bool ends_ci(const std::string& text, const char* suffix)
{
  const std::size_t n = std::strlen(suffix);
  if (text.size() < n)
    return false;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char a = static_cast<unsigned char>(text[text.size() - n + i]);
    const unsigned char b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b))
      return false;
  }
  return true;
}

static bool eq_ci(const std::string& text, const char* other)
{
  const std::size_t n = std::strlen(other);
  if (text.size() != n)
    return false;
  return ends_ci(text, other);
}

/* Origin "Debian-Security", or a suite / label / codename ending in -security.
 * The Inst token is "Origin:version/suite". */
static bool archive_is_security(const std::string& archive)
{
  if (archive.empty())
    return false;
  const std::string::size_type slash = archive.find('/');
  const std::string left = slash == std::string::npos ? archive : archive.substr(0, slash);
  const std::string suite = slash == std::string::npos ? std::string() : archive.substr(slash + 1);
  const std::string::size_type colon = left.find(':');
  const std::string origin = colon == std::string::npos ? left : left.substr(0, colon);
  if (eq_ci(origin, "Debian-Security") || ends_ci(origin, "-security"))
    return true;
  if (ends_ci(suite, "-security") || ends_ci(left, "-security"))
    return true;
  return false;
}

bool progress_line_visible(const std::string& line)
{
  for (unsigned char ch : line) {
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v')
      return true;
  }
  return false;
}

static int percent_at(const std::string& line, std::size_t begin, std::size_t end)
{
  if (begin >= end || end > line.size())
    return -1;
  int value = 0;
  for (std::size_t i = begin; i < end; ++i) {
    const unsigned char ch = static_cast<unsigned char>(line[i]);
    if (!std::isdigit(ch))
      return -1;
    value = value * 10 + (line[i] - '0');
    if (value > 100)
      return -1;
  }
  return value;
}

static int hex_digit(char ch)
{
  const unsigned char c = static_cast<unsigned char>(ch);
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Apt writes an epoch colon as %3a in deb filenames. Other %XX sequences
 * are decoded the same way so a version can be compared with an Inst line. */
static std::string percent_decode(const std::string& text)
{
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = hex_digit(text[i + 1]);
      const int lo = hex_digit(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string canonical_deb_version(const std::string& version)
{
  return percent_decode(version);
}

bool deb_versions_match(const std::string& a, const std::string& b)
{
  return canonical_deb_version(a) == canonical_deb_version(b);
}

static bool status_fd_line(const std::string& line, std::string& tag, std::string& id,
                           std::string& percent, std::string& message)
{
  const std::string::size_type c1 = line.find(':');
  if (c1 == std::string::npos || c1 == 0)
    return false;
  tag = line.substr(0, c1);
  if (tag != "dlstatus" && tag != "pmstatus" && tag != "pmerror" && tag != "pmconffile")
    return false;
  const std::string::size_type c2 = line.find(':', c1 + 1);
  if (c2 == std::string::npos)
    return false;
  const std::string::size_type c3 = line.find(':', c2 + 1);
  if (c3 == std::string::npos)
    return false;
  id = line.substr(c1 + 1, c2 - c1 - 1);
  percent = line.substr(c2 + 1, c3 - c2 - 1);
  message = line.substr(c3 + 1);
  return true;
}

static int percent_float(const std::string& text)
{
  if (text.empty())
    return -1;
  double value = 0;
  std::size_t i = 0;
  bool any = false;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
    any = true;
    value = value * 10.0 + static_cast<double>(text[i] - '0');
    ++i;
  }
  if (i < text.size() && text[i] == '.') {
    ++i;
    double place = 0.1;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
      any = true;
      value += static_cast<double>(text[i] - '0') * place;
      place *= 0.1;
      ++i;
    }
  }
  if (!any || i != text.size() || value < 0.0 || value > 100.0001)
    return -1;
  int rounded = static_cast<int>(value + 0.5);
  if (rounded > 100)
    rounded = 100;
  if (rounded < 0)
    rounded = 0;
  return rounded;
}

int progress_percent(const std::string& line)
{
  std::string tag;
  std::string id;
  std::string percent;
  std::string message;
  if (status_fd_line(line, tag, id, percent, message))
    return percent_float(percent);
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
    ++i;
  std::size_t end = i;
  while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
    ++end;
  if (end > i && end < line.size() && line[end] == '%') {
    const int lead = percent_at(line, i, end);
    if (lead >= 0)
      return lead;
  }
  if (line.empty() || line.back() != '%')
    return -1;
  std::size_t digit = line.size() - 1;
  if (digit == 0)
    return -1;
  --digit;
  if (!std::isdigit(static_cast<unsigned char>(line[digit])))
    return -1;
  const std::size_t stop = digit + 1;
  while (digit > 0 && std::isdigit(static_cast<unsigned char>(line[digit - 1])))
    --digit;
  return percent_at(line, digit, stop);
}

static bool debian_package_name(const std::string& name)
{
  if (name.size() < 2 || name.size() > 200)
    return false;
  const unsigned char first = static_cast<unsigned char>(name[0]);
  if (!std::islower(first) && !std::isdigit(first))
    return false;
  for (char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::islower(c) || std::isdigit(c) || c == '+' || c == '-' || c == '.')
      continue;
    return false;
  }
  /* Tokens apt prints when it has no package yet, including the status-fd
   * stand-in for "about to run dpkg". */
  if (name == "dpkg-exec" || name == "working" || name == "connecting" || name == "waiting" ||
      name == "retrieving" || name == "hit" || name == "get" || name == "ign" || name == "err" ||
      name == "fetched")
    return false;
  return true;
}

static std::string word_package(const std::string& text, std::size_t begin)
{
  if (begin >= text.size())
    return {};
  const std::string::size_type end = text.find_first_of(" ](", begin);
  const std::string::size_type stop = end == std::string::npos ? text.size() : end;
  if (stop <= begin)
    return {};
  const std::string name = text.substr(begin, stop - begin);
  if (!debian_package_name(name))
    return {};
  return name;
}

/* ".deb" at the end of a filename, not the ".deb" inside "deb.debian.org". */
static std::string::size_type find_deb_suffix(const std::string& line)
{
  std::string::size_type pos = 0;
  while ((pos = line.find(".deb", pos)) != std::string::npos) {
    const std::size_t after = pos + 4;
    if (after >= line.size() || line[after] == ' ' || line[after] == '\'' || line[after] == '"' ||
        line[after] == '\t' || line[after] == '\r')
      return pos;
    pos = after;
  }
  return std::string::npos;
}

static std::string deb_filename_package(const std::string& file)
{
  const std::string decoded = percent_decode(file);
  std::string stem = decoded;
  if (stem.size() >= 4 && stem.compare(stem.size() - 4, 4, ".deb") == 0)
    stem.resize(stem.size() - 4);
  const std::string::size_type us = stem.find('_');
  const std::string name = us == std::string::npos ? stem : stem.substr(0, us);
  if (!debian_package_name(name))
    return {};
  return name;
}

std::string progress_package_name(const std::string& line)
{
  std::string tag;
  std::string id;
  std::string percent;
  std::string message;
  if (status_fd_line(line, tag, id, percent, message)) {
    /* dlstatus names a file index, not a package. pmerror is a failure. */
    if (tag != "pmstatus")
      return {};
    if (!debian_package_name(id))
      return {};
    return id;
  }
  const char* configure[] = {"Setting up ", "Unpacking "};
  for (const char* prefix : configure) {
    const std::size_t n = std::strlen(prefix);
    if (line.compare(0, n, prefix) == 0)
      return word_package(line, n);
  }
  const std::string::size_type bracket = line.find('[');
  if (bracket != std::string::npos) {
    std::size_t i = bracket + 1;
    while (i < line.size() && line[i] == ' ')
      ++i;
    /* Apt's per-item form is "[<digits> <name> …]". [Working], [Connecting to …]
     * and [Waiting for headers] have no item number. */
    if (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
      while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
        ++i;
      if (i < line.size() && line[i] == ' ') {
        while (i < line.size() && line[i] == ' ')
          ++i;
        const std::string name = word_package(line, i);
        if (!name.empty())
          return name;
      }
    }
  }
  if (line.compare(0, 4, "Get:") == 0) {
    const std::string::size_type deb = find_deb_suffix(line);
    if (deb == std::string::npos)
      return {};
    const std::string::size_type slash = line.rfind('/', deb);
    const std::string::size_type start = slash == std::string::npos ? 0 : slash + 1;
    if (deb < start)
      return {};
    return deb_filename_package(line.substr(start, deb + 4 - start));
  }
  return {};
}

bool progress_is_configure_line(const std::string& line)
{
  if (line.compare(0, 11, "Setting up ") == 0 || line.compare(0, 10, "Unpacking ") == 0)
    return !progress_package_name(line).empty();
  std::string tag;
  std::string id;
  std::string percent;
  std::string message;
  if (!status_fd_line(line, tag, id, percent, message))
    return false;
  return tag == "pmstatus" && !progress_package_name(line).empty();
}

bool progress_is_download_noise(const std::string& line)
{
  if (!progress_package_name(line).empty())
    return false;
  std::string tag;
  std::string id;
  std::string percent;
  std::string message;
  if (status_fd_line(line, tag, id, percent, message))
    return true;
  if (line.compare(0, 4, "Hit:") == 0 || line.compare(0, 4, "Get:") == 0 ||
      line.compare(0, 4, "Ign:") == 0 || line.compare(0, 4, "Err:") == 0)
    return true;
  if (line.find('[') != std::string::npos)
    return true;
  return false;
}

static std::string downloading_file_sentence(const std::string& message)
{
  const std::string prefix = "Retrieving file ";
  if (message.compare(0, prefix.size(), prefix) != 0)
    return {};
  std::string rest = message.substr(prefix.size());
  const std::string::size_type paren = rest.find(" (");
  if (paren != std::string::npos)
    rest.resize(paren);
  while (!rest.empty() && rest.back() == ' ')
    rest.pop_back();
  if (rest.find(" of ") == std::string::npos)
    return {};
  return "Downloading file " + rest + "…";
}

std::string friendly_progress_text(const std::string& line, const std::string& phase)
{
  if (line.compare(0, 21, "Reading package lists") == 0)
    return "Reading package lists…";
  if (line.compare(0, 19, "Calculating upgrade") == 0)
    return "Calculating upgrade…";
  if (line.compare(0, 24, "Building dependency tree") == 0)
    return "Building dependency tree…";
  if (progress_is_configure_line(line)) {
    const std::string pkg = progress_package_name(line);
    if (!pkg.empty())
      return "Configuring " + pkg + "…";
  }
  const std::string pkg = progress_package_name(line);
  if (!pkg.empty())
    return "Downloading " + pkg + "…";
  std::string tag;
  std::string id;
  std::string percent;
  std::string message;
  if (status_fd_line(line, tag, id, percent, message) && tag == "dlstatus") {
    const std::string files = downloading_file_sentence(message);
    if (!files.empty())
      return files;
    return "Downloading updates…";
  }
  if (progress_is_download_noise(line)) {
    if (phase == "download" || progress_percent(line) >= 0)
      return "Downloading updates…";
    return line;
  }
  if (phase == "download")
    return "Downloading updates…";
  return line;
}

AptProgressNote apt_progress_note(const std::string& line, const std::string& current_shown)
{
  AptProgressNote out;
  out.shown = current_shown;
  if (!progress_line_visible(line))
    return out;
  if (line.compare(0, 5, "Inst ") == 0 || line.compare(0, 5, "Conf ") == 0)
    return out;
  const int pct = progress_percent(line);
  if (pct >= 0) {
    out.have_percent = true;
    out.percent = pct;
  }
  const std::string named = progress_package_name(line);
  if (!named.empty()) {
    out.replace_shown = true;
    out.shown = line;
    return out;
  }
  if (progress_is_download_noise(line)) {
    if (current_shown.empty() || progress_package_name(current_shown).empty()) {
      out.replace_shown = true;
      out.shown = line;
    }
    return out;
  }
  out.replace_shown = true;
  out.shown = line;
  return out;
}

static std::string field_value(const std::string& text, const char* key)
{
  const std::string prefix = std::string(key) + ":";
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.compare(0, prefix.size(), prefix) != 0)
      continue;
    std::string value = line.substr(prefix.size());
    while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
      value.erase(0, 1);
    const std::string::size_type cut = value.find_first_of(" \t");
    if (cut != std::string::npos)
      value.resize(cut);
    return value;
  }
  return {};
}

bool release_text_is_security(const std::string& text)
{
  const std::string origin = field_value(text, "Origin");
  const std::string label = field_value(text, "Label");
  const std::string suite = field_value(text, "Suite");
  const std::string codename = field_value(text, "Codename");
  if (eq_ci(origin, "Debian-Security"))
    return true;
  if (ends_ci(label, "-security") || ends_ci(suite, "-security") || ends_ci(codename, "-security"))
    return true;
  return false;
}

void collect_security_ids(const std::string& text, std::vector<std::string>& ids)
{
  if (!release_text_is_security(text))
    return;
  const char* keys[] = {"Origin", "Label", "Suite", "Codename"};
  for (const char* key : keys) {
    const std::string value = field_value(text, key);
    if (value.size() < 8)
      continue;
    if (!eq_ci(value, "Debian-Security") && !ends_ci(value, "-security"))
      continue;
    bool seen = false;
    for (const auto& have : ids) {
      if (have == value)
        seen = true;
    }
    if (!seen)
      ids.push_back(value);
  }
}

void apply_security_ids(std::vector<PackageUpgrade>& packages, const std::vector<std::string>& ids)
{
  for (auto& pkg : packages) {
    if (pkg.security || archive_is_security(pkg.archive)) {
      pkg.security = true;
      continue;
    }
    for (const auto& id : ids) {
      if (!pkg.archive.empty() && pkg.archive.find(id) != std::string::npos)
        pkg.security = true;
    }
  }
}

static std::string format_byte_size(unsigned long long bytes)
{
  if (bytes < 1000)
    return std::to_string(bytes) + " B";
  const char* unit = "kB";
  double value = static_cast<double>(bytes) / 1000.0;
  if (bytes >= 1000ull * 1000ull * 1000ull) {
    value = static_cast<double>(bytes) / 1000000000.0;
    unit = "GB";
  } else if (bytes >= 1000ull * 1000ull) {
    value = static_cast<double>(bytes) / 1000000.0;
    unit = "MB";
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(1);
  out << value << " " << unit;
  return out.str();
}

static bool split_deb_filename(const std::string& file, std::string& name, std::string& version)
{
  const std::string decoded = percent_decode(file);
  if (decoded.size() < 5 || decoded.compare(decoded.size() - 4, 4, ".deb") != 0)
    return false;
  const std::string stem = decoded.substr(0, decoded.size() - 4);
  const std::string::size_type us2 = stem.rfind('_');
  if (us2 == std::string::npos || us2 == 0)
    return false;
  const std::string::size_type us1 = stem.rfind('_', us2 - 1);
  if (us1 == std::string::npos || us1 == 0)
    return false;
  name = stem.substr(0, us1);
  version = stem.substr(us1 + 1, us2 - us1 - 1);
  return !name.empty() && !version.empty();
}

void apply_download_details(SimulateResult& result, const std::string& text)
{
  const std::string need = parse_download_need(text);
  if (!need.empty())
    result.download_need = need;
  std::istringstream in(normalize_breaks(text));
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (result.disk_use.empty() && line.compare(0, 20, "After this operation") == 0)
      result.disk_use = line;
    if (line.size() < 6 || line.find(".deb") == std::string::npos)
      continue;
    std::istringstream fields(line);
    std::string quoted;
    std::string file;
    std::string bytes_text;
    if (!(fields >> quoted >> file >> bytes_text))
      continue;
    unsigned long long bytes = 0;
    bool digits = !bytes_text.empty();
    for (char ch : bytes_text) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        digits = false;
        break;
      }
      /* 10^15 bytes is far beyond any real package; a longer run is garbage. */
      if (bytes > 100000000000000ull) {
        digits = false;
        break;
      }
      bytes = bytes * 10ull + static_cast<unsigned long long>(ch - '0');
    }
    if (!digits)
      continue;
    std::string name;
    std::string version;
    if (!split_deb_filename(file, name, version))
      continue;
    for (auto& pkg : result.packages) {
      if (pkg.name == name && deb_versions_match(pkg.new_version, version))
        pkg.size = format_byte_size(bytes);
    }
    for (auto& pkg : result.new_packages) {
      if (pkg.name == name && deb_versions_match(pkg.new_version, version))
        pkg.size = format_byte_size(bytes);
    }
  }
}

std::string present_download_need(const std::string& text)
{
  std::istringstream in(text);
  std::string line;
  std::string out;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    std::string shown = line;
    const std::string prefix = "Need to get ";
    if (line.compare(0, prefix.size(), prefix) == 0) {
      const std::string rest = line.substr(prefix.size());
      if (rest.compare(0, 4, "0 B/") == 0) {
        std::string size = rest.substr(4);
        const std::string::size_type of = size.find(" of archives");
        if (of != std::string::npos)
          size.resize(of);
        while (!size.empty() && size.back() == '.')
          size.pop_back();
        shown = size.empty() ? "The packages are already downloaded."
                             : "The packages are already downloaded (" + size + ").";
      } else if (rest.compare(0, 4, "0 B ") == 0 || rest == "0 B of archives." ||
                 rest == "0 B of archives") {
        shown = "The packages are already downloaded.";
      }
    }
    if (!out.empty())
      out += " ";
    out += shown;
  }
  return out;
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
  if (!checked_count(line, i, end, value))
    return -1;
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

  std::istringstream in(normalize_breaks(text));
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
    "Can't reach the update server. Check your network connection and try again.";

static bool is_hit_or_get(const std::string& line)
{
  return starts_with(line, "Hit:") || starts_with(line, "Get:");
}

static bool is_connect_failure(const std::string& lower)
{
  return has(lower, "connection timed out") || has(lower, "unable to connect") ||
         has(lower, "could not connect") || has(lower, "connection refused");
}

static bool is_resolve_failure(const std::string& lower)
{
  return has(lower, "temporary failure resolving") || has(lower, "could not resolve") ||
         has(lower, "name or service not known") || has(lower, "network is unreachable");
}

static bool is_apt_diagnostic_line(const std::string& line)
{
  return starts_with(line, "E:") || starts_with(line, "W:") || starts_with(line, "Err:") ||
         starts_with(line, "N:");
}

/* Keep apt's own lines. Add at most one hint, and only for a total failure
 * that is a connect problem or a resolve / unreachable network. */
static std::string append_fetch_hint(std::string detail, bool connect, bool resolve)
{
  if (resolve) {
    if (detail.find(kOfflineHint) == std::string::npos) {
      if (!detail.empty())
        detail += "\n";
      detail += kOfflineHint;
    }
    return detail;
  }
  if (connect && detail.find(kProxyHint) == std::string::npos) {
    if (!detail.empty())
      detail += "\n";
    detail += kProxyHint;
  }
  return detail;
}

UpdateFetch classify_apt_update(const std::string& text)
{
  UpdateFetch out;
  bool hit = false;
  bool remote_hit = false;
  bool fetch_fail = false;
  bool connect = false;
  bool resolve = false;
  std::string apt_lines;
  bool keep_next = false;

  std::istringstream in(normalize_breaks(text));
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty())
      continue;
    if (is_hit_or_get(line)) {
      hit = true;
      if (line.find("http://") != std::string::npos || line.find("https://") != std::string::npos ||
          line.find("ftp://") != std::string::npos || line.find("tor+") != std::string::npos)
        remote_hit = true;
    }
    const std::string lower = lower_copy(line);
    const bool index_note = has(lower, "some index files failed to download") ||
                            has(lower, "old ones used instead");
    const bool connect_line = is_connect_failure(lower);
    const bool resolve_line = is_resolve_failure(lower);
    const bool fetch_line = has(lower, "failed to fetch") || has(lower, "hash sum mismatch") ||
                            has(lower, "no_pubkey") || has(lower, "release file") ||
                            starts_with(line, "Err:") || index_note || connect_line || resolve_line;
    if (fetch_line)
      fetch_fail = true;
    if (connect_line)
      connect = true;
    if (resolve_line)
      resolve = true;
    const bool diagnostic = is_apt_diagnostic_line(line);
    const bool continuation = keep_next && !line.empty() && (line[0] == ' ' || line[0] == '\t');
    if (diagnostic || continuation || connect_line || resolve_line) {
      if (!apt_lines.empty())
        apt_lines += "\n";
      apt_lines += line;
      keep_next = diagnostic || continuation;
      continue;
    }
    keep_next = false;
  }

  if (!fetch_fail)
    return out;
  /* No Hit: or Get: means nothing was fetched. That includes a dead mirror
   * whose apt still exits 0, and a resolve failure of every source. The apt
   * lines stay; a hint is only added after them. */
  if (!hit) {
    out.kind = UpdateFetchKind::Total;
    if (apt_lines.empty())
      out.detail = "The package lists could not be refreshed.";
    else if (connect || resolve)
      /* Round 11 (U-4): the plain sentence first, apt's lines below. */
      out.detail = append_fetch_hint(std::string(), connect, resolve) + "\n" + apt_lines;
    else
      out.detail = apt_lines;
    return out;
  }
  out.kind = UpdateFetchKind::Partial;
  out.detail = apt_lines;
  /* Round 11 (U-4): no network server answered and one could not be resolved
   * or connected (network down; only local file: repos were read). Lead with one plain sentence;
   * apt's own lines stay below it. */
  if ((connect || resolve) && !remote_hit)
    out.detail = std::string(kServerUnreachable) + (apt_lines.empty() ? "" : "\n" + apt_lines);
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
  std::istringstream in(normalize_breaks(text));
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

/* Round 10 (U-2): every field value is escaped, so no value can start a
 * new protocol line. Backslash, control bytes and DEL become \\, \n, \r,
 * \t or \xHH. Token fields (package names, versions) also escape a space,
 * so a name cannot shift the next token. parse_protocol undoes this. */
std::string protocol_escape(const std::string& value, bool token)
{
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (c < 0x20 || c == 0x7f || (token && c == ' ')) {
      out += "\\x";
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    } else
      out.push_back(ch);
  }
  return out;
}

static int hex_value(char ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  if (ch >= 'a' && ch <= 'f')
    return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F')
    return ch - 'A' + 10;
  return -1;
}

std::string protocol_unescape(const std::string& value)
{
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch != '\\' || i + 1 >= value.size()) {
      out.push_back(ch);
      continue;
    }
    const char next = value[i + 1];
    if (next == '\\') {
      out.push_back('\\');
      ++i;
    } else if (next == 'n') {
      out.push_back('\n');
      ++i;
    } else if (next == 'r') {
      out.push_back('\r');
      ++i;
    } else if (next == 't') {
      out.push_back('\t');
      ++i;
    } else if (next == 'x' && i + 3 < value.size() && hex_value(value[i + 2]) >= 0 &&
               hex_value(value[i + 3]) >= 0) {
      out.push_back(static_cast<char>(hex_value(value[i + 2]) * 16 + hex_value(value[i + 3])));
      i += 3;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

static std::string esc(const std::string& value)
{
  return protocol_escape(value, false);
}

static std::string tok(const std::string& value)
{
  return protocol_escape(value, true);
}

/* Multi-line text keeps one tagged line per source line. */
static void emit_lines(std::ostringstream& out, const char* tag, const std::string& text,
                       bool fallback = true)
{
  std::istringstream lines(text);
  std::string line;
  bool any = false;
  while (std::getline(lines, line)) {
    line = trim_cr(line);
    if (line.empty())
      continue;
    any = true;
    out << tag << " " << esc(line) << "\n";
  }
  if (!any && fallback)
    out << tag << " unknown error\n";
}

static void emit_extra(std::ostringstream& out, const SimulateResult& result)
{
  if (result.not_upgraded_count > 0)
    out << "NOT_UPGRADED " << result.not_upgraded_count << "\n";
  for (const auto& name : result.kept_back)
    out << "KEPT " << esc(name) << "\n";
  for (const auto& pkg : result.new_packages) {
    if (!pkg.name.empty() && !pkg.new_version.empty())
      out << "NEW " << tok(pkg.name) << " " << tok(pkg.new_version) << "\n";
  }
  for (const auto& name : result.unclassified)
    out << "UNCLASSIFIED " << esc(name) << "\n";
  if (!result.download_need.empty())
    emit_lines(out, "NEED", result.download_need, false);
  if (!result.disk_use.empty())
    out << "DISK " << esc(result.disk_use) << "\n";
  for (const auto& pkg : result.packages) {
    if (pkg.security)
      out << "SEC " << esc(pkg.name) << "\n";
    if (!pkg.size.empty())
      out << "SIZE " << tok(pkg.name) << " " << esc(pkg.size) << "\n";
  }
  /* Round 12 (U-2): new dependencies carry a size too. */
  for (const auto& pkg : result.new_packages) {
    if (!pkg.size.empty())
      out << "SIZE " << tok(pkg.name) << " " << esc(pkg.size) << "\n";
  }
  for (const auto& name : result.phased)
    out << "PHASED " << esc(name) << "\n";
  for (const auto& name : result.held)
    out << "HELD " << esc(name) << "\n";
  if (result.reboot_required) {
    if (result.reboot_pkgs.empty())
      out << "REBOOT\n";
    for (const auto& name : result.reboot_pkgs)
      out << "REBOOT " << esc(name) << "\n";
  }
  if (result.reboot_already) {
    if (result.reboot_pending_pkgs.empty())
      out << "REBOOT_ALREADY\n";
    for (const auto& name : result.reboot_pending_pkgs)
      out << "REBOOT_ALREADY " << esc(name) << "\n";
  }
  for (const auto& removal : result.kept_removals) {
    if (removal.removes.empty())
      out << "REMOVE " << tok(removal.package) << "\n";
    for (const auto& victim : removal.removes)
      out << "REMOVE " << tok(removal.package) << " " << tok(victim) << "\n";
  }
  if (result.summary_missing)
    out << "SUMMARY_MISSING\n";
  for (const auto& path : result.conffiles_kept)
    out << "CONFKEPT " << esc(path) << "\n";
  for (const auto& name : result.debconf_pkgs)
    out << "ASKS " << tok(name) << "\n";
  for (const auto& ans : result.debconf_answers)
  {
    out << "ANSWER " << tok(ans.package) << " " << tok(ans.question) << " " << esc(ans.value) << "\n";
    if (!ans.text.empty())
      out << "QTEXT " << tok(ans.question) << " " << esc(ans.text) << "\n";
  }
  if (result.install_skipped)
    out << "SKIPPED\n";
  if (!result.warning.empty())
    emit_lines(out, "WARN", result.warning, false);
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
      out << "PKG " << tok(pkg.name) << " " << tok(pkg.old_version) << " " << tok(pkg.new_version)
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
  bool duplicate_status = false;
  std::vector<std::string> security_names;
  std::vector<std::pair<std::string, std::string>> sizes;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.compare(0, 7, "STATUS ") == 0) {
      const std::string st = line.substr(7);
      if (saw_status) {
        /* One STATUS per result. A second one is never trusted. */
        duplicate_status = true;
        continue;
      }
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
      const std::string msg = protocol_unescape(line.substr(4));
      if (result.error_msg.empty())
        result.error_msg = msg;
      else
        result.error_msg += "\n" + msg;
    } else if (line.compare(0, 9, "CONFKEPT ") == 0) {
      const std::string path = protocol_unescape(line.substr(9));
      if (!path.empty())
        result.conffiles_kept.push_back(path);
    } else if (line.compare(0, 5, "ASKS ") == 0) {
      const std::string name = protocol_unescape(line.substr(5));
      if (!name.empty() && name.find(' ') == std::string::npos)
        result.debconf_pkgs.push_back(name);
    } else if (line.compare(0, 7, "ANSWER ") == 0) {
      const std::string rest = line.substr(7);
      const std::size_t a = rest.find(' ');
      const std::size_t b = a == std::string::npos ? a : rest.find(' ', a + 1);
      if (a != std::string::npos && a > 0) {
        SimulateResult::DebconfAnswer ans;
        ans.package = protocol_unescape(rest.substr(0, a));
        if (b == std::string::npos) {
          ans.question = protocol_unescape(rest.substr(a + 1));
        } else {
          ans.question = protocol_unescape(rest.substr(a + 1, b - a - 1));
          ans.value = protocol_unescape(rest.substr(b + 1));
        }
        if (!ans.question.empty())
          result.debconf_answers.push_back(ans);
      }
    } else if (line.compare(0, 6, "QTEXT ") == 0) {
      /* Round 13 (U-3): question text for the ANSWER just before it. */
      const std::string rest = line.substr(6);
      const std::size_t a = rest.find(' ');
      if (a != std::string::npos && !result.debconf_answers.empty() &&
          result.debconf_answers.back().question == protocol_unescape(rest.substr(0, a)))
        result.debconf_answers.back().text = protocol_unescape(rest.substr(a + 1));
    } else if (line == "SKIPPED") {
      result.install_skipped = true;
    } else if (line.compare(0, 5, "WARN ") == 0) {
      const std::string msg = protocol_unescape(line.substr(5));
      if (result.warning.empty())
        result.warning = msg;
      else
        result.warning += "\n" + msg;
    } else if (line.compare(0, 5, "KEPT ") == 0) {
      const std::string name = protocol_unescape(line.substr(5));
      if (!name.empty())
        result.kept_back.push_back(name);
    } else if (line.compare(0, 4, "NEW ") == 0) {
      std::istringstream ps(line.substr(4));
      PackageUpgrade pkg;
      pkg.old_version = "-";
      if (ps >> pkg.name >> pkg.new_version && !pkg.name.empty() && !pkg.new_version.empty()) {
        pkg.name = protocol_unescape(pkg.name);
        pkg.new_version = protocol_unescape(pkg.new_version);
        result.new_packages.push_back(pkg);
      }
    } else if (line.compare(0, 13, "UNCLASSIFIED ") == 0) {
      const std::string name = protocol_unescape(line.substr(13));
      if (!name.empty())
        result.unclassified.push_back(name);
    } else if (line.compare(0, 5, "NEED ") == 0) {
      const std::string need = protocol_unescape(line.substr(5));
      if (!need.empty()) {
        if (result.download_need.empty())
          result.download_need = need;
        else
          result.download_need += "\n" + need;
      }
    } else if (line.compare(0, 5, "DISK ") == 0) {
      const std::string disk = protocol_unescape(line.substr(5));
      if (!disk.empty())
        result.disk_use = disk;
    } else if (line.compare(0, 4, "SEC ") == 0) {
      const std::string name = protocol_unescape(line.substr(4));
      if (!name.empty())
        security_names.push_back(name);
    } else if (line.compare(0, 5, "SIZE ") == 0) {
      std::istringstream ps(line.substr(5));
      std::string name;
      std::string size;
      if (ps >> name) {
        std::getline(ps, size);
        while (!size.empty() && size[0] == ' ')
          size.erase(0, 1);
        if (!name.empty() && !size.empty())
          sizes.emplace_back(protocol_unescape(name), protocol_unescape(size));
      }
    } else if (line.compare(0, 7, "PHASED ") == 0) {
      const std::string name = protocol_unescape(line.substr(7));
      if (!name.empty())
        result.phased.push_back(name);
    } else if (line.compare(0, 5, "HELD ") == 0) {
      const std::string name = protocol_unescape(line.substr(5));
      if (!name.empty())
        result.held.push_back(name);
    } else if (line == "REBOOT_ALREADY") {
      result.reboot_already = true;
    } else if (line.compare(0, 15, "REBOOT_ALREADY ") == 0) {
      result.reboot_already = true;
      const std::string name = protocol_unescape(line.substr(15));
      if (!name.empty())
        result.reboot_pending_pkgs.push_back(name);
    } else if (line == "REBOOT") {
      result.reboot_required = true;
    } else if (line.compare(0, 7, "REBOOT ") == 0) {
      result.reboot_required = true;
      const std::string name = protocol_unescape(line.substr(7));
      if (!name.empty())
        result.reboot_pkgs.push_back(name);
    } else if (line.compare(0, 7, "REMOVE ") == 0) {
      std::istringstream ps(line.substr(7));
      std::string package;
      std::string victim;
      if (!(ps >> package))
        continue;
      package = protocol_unescape(package);
      SimulateResult::KeptRemoval* slot = nullptr;
      for (auto& have : result.kept_removals) {
        if (have.package == package)
          slot = &have;
      }
      if (slot == nullptr) {
        result.kept_removals.push_back(SimulateResult::KeptRemoval{});
        slot = &result.kept_removals.back();
        slot->package = package;
      }
      while (ps >> victim) {
        if (!victim.empty())
          slot->removes.push_back(protocol_unescape(victim));
      }
    } else if (line == "SUMMARY_MISSING") {
      result.summary_missing = true;
    } else if (line.compare(0, 13, "NOT_UPGRADED ") == 0) {
      const std::string digits = line.substr(13);
      std::size_t end = 0;
      while (end < digits.size() && std::isdigit(static_cast<unsigned char>(digits[end])))
        ++end;
      int value = 0;
      if (end > 0 && checked_count(digits, 0, end, value))
        result.not_upgraded_count = value;
    } else if (line.compare(0, 4, "PKG ") == 0) {
      std::istringstream ps(line.substr(4));
      PackageUpgrade pkg;
      if (ps >> pkg.name >> pkg.old_version >> pkg.new_version) {
        pkg.name = protocol_unescape(pkg.name);
        pkg.old_version = protocol_unescape(pkg.old_version);
        pkg.new_version = protocol_unescape(pkg.new_version);
        result.packages.push_back(pkg);
      }
    }
  }

  for (auto& pkg : result.packages) {
    for (const auto& name : security_names) {
      if (name == pkg.name)
        pkg.security = true;
    }
    for (const auto& size : sizes) {
      if (size.first == pkg.name)
        pkg.size = size.second;
    }
  }
  for (auto& pkg : result.new_packages) {
    for (const auto& size : sizes) {
      if (size.first == pkg.name)
        pkg.size = size.second;
    }
  }

  if (duplicate_status) {
    result = SimulateResult{};
    result.status = SimulateResult::Error;
    result.error_msg = "Malformed helper output (more than one STATUS line)";
    return result;
  }
  if (!saw_status) {
    result.status = SimulateResult::Error;
    if (result.error_msg.empty())
      result.error_msg = "No STATUS from helper";
  }
  return result;
}

static bool is_helper_timeout_line(const std::string& lower)
{
  return has(lower, "timed out while") || has(lower, "timed out waiting");
}

static std::string map_helper_timeout(JobKind kind)
{
  if (kind == JobKind::Install)
    return "Timed out while installing updates.";
  return "Timed out waiting for the update check. Check your network and try again.";
}

static std::string friendly_one_line(const std::string& msg, JobKind kind)
{
  const std::string lower = lower_copy(msg);
  /* Helper deadlines stay in the check or install wording. Apt's own
   * "connection timed out" line is not one of those sentences. */
  if (is_helper_timeout_line(lower))
    return map_helper_timeout(kind);
  if (msg.empty())
    return "The update helper failed.";
  return msg;
}

std::string friendly_job_error(const std::string& msg, JobKind kind)
{
  const std::string lower = lower_copy(msg);
  /* A partial refresh already says some indexes were skipped. Do not add a
   * proxy or offline hint on top of apt's own summary. */
  const bool leave_fetch = has(lower, "some index files failed to download") ||
                           has(lower, "old ones used instead");
  std::string out;
  if (msg.find('\n') == std::string::npos) {
    out = friendly_one_line(msg, kind);
  } else {
    std::istringstream in(msg);
    std::string line;
    while (std::getline(in, line)) {
      line = trim_cr(line);
      if (line.empty())
        continue;
      const std::string one = friendly_one_line(line, kind);
      if (one.empty())
        continue;
      if (!out.empty())
        out += "\n";
      out += one;
    }
    if (out.empty())
      out = friendly_one_line(msg, kind);
  }
  if (leave_fetch || is_helper_timeout_line(lower_copy(out)))
    return out;
  const std::string shown = lower_copy(out);
  return append_fetch_hint(out, is_connect_failure(shown), is_resolve_failure(shown));
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
  std::string normalized;
  normalized.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (data[i] == '\r') {
      normalized.push_back('\n');
      if (i + 1 < n && data[i + 1] == '\n')
        ++i;
      continue;
    }
    normalized.push_back(data[i]);
  }
  data = normalized.data();
  n = normalized.size();
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

static bool is_removed_header(const std::string& line)
{
  const std::string header = "The following packages will be REMOVED:";
  return line.compare(0, header.size(), header) == 0;
}

static bool is_new_header(const std::string& line)
{
  const std::string header = "The following NEW packages will be installed:";
  return line.compare(0, header.size(), header) == 0;
}

static bool is_downgrade_header(const std::string& line)
{
  const std::string header = "The following packages will be DOWNGRADED:";
  return line.compare(0, header.size(), header) == 0;
}

static void note_summary_count(const std::string& line, const char* phrase, int& slot)
{
  const std::string::size_type pos = line.find(phrase);
  if (pos == std::string::npos)
    return;
  const int value = integer_before(line, pos);
  if (value >= 0)
    slot = value;
}

InstallPlan parse_install_plan(const std::string& text)
{
  InstallPlan plan;
  std::istringstream in(normalize_breaks(text));
  std::string line;
  enum class Section { None, Removed, New, Down };
  Section section = Section::None;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (section != Section::None) {
      if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
        if (section == Section::Removed)
          append_tokens(line, plan.removed);
        else if (section == Section::New)
          append_tokens(line, plan.newly);
        else
          append_tokens(line, plan.downgraded);
        continue;
      }
      section = Section::None;
    }
    if (is_removed_header(line)) {
      section = Section::Removed;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), plan.removed);
      continue;
    }
    if (is_new_header(line)) {
      section = Section::New;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), plan.newly);
      continue;
    }
    if (is_downgrade_header(line)) {
      section = Section::Down;
      const std::string::size_type colon = line.find(':');
      if (colon != std::string::npos)
        append_tokens(line.substr(colon + 1), plan.downgraded);
      continue;
    }
    note_summary_count(line, " to remove", plan.remove_count);
    note_summary_count(line, " downgraded", plan.downgrade_count);
    PackageUpgrade pkg;
    if (parse_inst_line(line, pkg))
      plan.inst.push_back(pkg);
  }
  return plan;
}

void parse_removal_plan(const std::string& text, std::vector<std::string>& removed,
                        std::vector<std::string>& newly)
{
  const InstallPlan plan = parse_install_plan(text);
  removed = plan.removed;
  newly = plan.newly;
}

std::string parse_download_need(const std::string& text)
{
  std::istringstream in(normalize_breaks(text));
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.compare(0, 12, "Need to get ") == 0 && line.find("of archives") != std::string::npos)
      return line;
  }
  return {};
}

static std::string join_names(const std::vector<std::string>& names)
{
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0)
      out += ", ";
    out += names[i];
  }
  return out;
}

static std::string removal_sentence(const SimulateResult& result)
{
  if (result.kept_removals.empty())
    return {};
  std::string out = "This tool will not install updates that would remove packages: ";
  for (std::size_t i = 0; i < result.kept_removals.size(); ++i) {
    if (i != 0)
      out += ", ";
    const auto& removal = result.kept_removals[i];
    out += removal.package;
    if (!removal.removes.empty()) {
      out += " (would remove ";
      out += join_names(removal.removes);
      out += ")";
    }
  }
  out += ".";
  return out;
}

std::string describe_remaining(const SimulateResult& result)
{
  std::string out;
  auto add = [&](const std::string& sentence) {
    if (sentence.empty())
      return;
    if (!out.empty())
      out += " ";
    out += sentence;
  };
  if (!result.phased.empty())
    add("These updates are waiting and will be offered later: " + join_names(result.phased) + ".");
  if (!result.held.empty())
    add("These packages are held: " + join_names(result.held) + ".");
  add(removal_sentence(result));
  if (!result.unclassified.empty())
    add("These updates were not included and will be offered on a later check: " +
        join_names(result.unclassified) + ".");
  if (!result.kept_back.empty())
    add("This tool will not install updates that need extra packages: " + join_names(result.kept_back) +
        ".");
  else if (result.phased.empty() && result.held.empty() && result.kept_removals.empty() &&
           result.unclassified.empty() && result.not_upgraded_count > 0)
    add("This tool will not install updates that need extra packages.");
  return out;
}

static bool has_remaining(const SimulateResult& result)
{
  return !result.kept_back.empty() || !result.phased.empty() || !result.held.empty() ||
         !result.kept_removals.empty() || !result.unclassified.empty() || result.not_upgraded_count > 0;
}

std::string kept_headline(const SimulateResult& result)
{
  const bool only_phased = !result.phased.empty() && result.kept_back.empty() && result.held.empty() &&
                           result.kept_removals.empty() && result.unclassified.empty();
  const bool only_held = !result.held.empty() && result.kept_back.empty() && result.phased.empty() &&
                         result.kept_removals.empty() && result.unclassified.empty();
  const bool only_removal = !result.kept_removals.empty() && result.kept_back.empty() &&
                            result.phased.empty() && result.held.empty() && result.unclassified.empty();
  const bool only_unclassified = !result.unclassified.empty() && result.kept_back.empty() &&
                                 result.phased.empty() && result.held.empty() &&
                                 result.kept_removals.empty();
  const std::string detail = describe_remaining(result);
  if (only_phased || only_held || only_removal || only_unclassified)
    return detail;
  if (detail.empty())
    return "Some updates were kept back.";
  return "Some updates were kept back. " + detail;
}

static std::string conf_sentence(const SimulateResult& result)
{
  if (result.conffiles_kept.empty())
    return {};
  return "Existing configuration was kept for " + join_names(result.conffiles_kept) + ".";
}

static std::string reboot_sentence(const SimulateResult& result)
{
  if (!result.reboot_required)
    return {};
  std::string sentence = "Restart to finish installing updates.";
  if (!result.reboot_pkgs.empty())
    sentence += " (needed by " + join_names(result.reboot_pkgs) + ").";
  return sentence;
}

static std::string pending_sentence(const SimulateResult& result)
{
  if (!result.reboot_already)
    return {};
  std::string sentence = "A restart was already pending.";
  if (!result.reboot_pending_pkgs.empty())
    sentence += " (needed by " + join_names(result.reboot_pending_pkgs) + ").";
  return sentence;
}

static std::string with_warning_text(std::string status, const std::string& warning, JobKind kind)
{
  if (warning.empty())
    return status;
  const std::string shown = friendly_job_error(warning, kind);
  if (shown.empty())
    return status;
  if (status.empty())
    return shown;
  return shown + "\n" + status;
}

static void append_sentence(std::string& status, const std::string& sentence)
{
  if (sentence.empty())
    return;
  if (!status.empty() && status.back() != ' ' && status.back() != '\n')
    status += " ";
  status += sentence;
}

JobOutcome outcome_check(const SimulateResult& result, int exit_code, bool have_rows)
{
  JobOutcome out;
  out.check_enabled = true;
  if ((result.status == SimulateResult::UpToDate || result.status == SimulateResult::Success) &&
      !has_remaining(result)) {
    out.packages = PackageListAction::Hide;
    out.install_enabled = false;
    out.status = with_warning_text("You're up to date.", result.warning, JobKind::Check);
    return out;
  }
  if (result.status == SimulateResult::Upgrades && !result.packages.empty()) {
    out.packages = PackageListAction::Show;
    out.install_enabled = true;
    const std::size_t count = result.packages.size();
    std::string status = std::to_string(count);
    status += count == 1 ? " update." : " updates.";
    int security = 0;
    for (const auto& pkg : result.packages) {
      if (pkg.security)
        ++security;
    }
    if (security == 1)
      status += " 1 of these is a security update.";
    else if (security > 1)
      status += " " + std::to_string(security) + " of these are security updates.";
    const std::string need = present_download_need(result.download_need);
    if (!need.empty())
      status += " " + need;
    if (!result.disk_use.empty())
      status += " " + result.disk_use;
    const std::string kept = describe_remaining(result);
    if (!kept.empty())
      status += " " + kept;
    if (!result.new_packages.empty()) {
      std::vector<std::string> names;
      names.reserve(result.new_packages.size());
      for (const auto& pkg : result.new_packages) {
        if (!pkg.name.empty())
          names.push_back(pkg.name);
      }
      std::sort(names.begin(), names.end());
      status += " New packages will also be installed: " + join_names(names) + ".";
    }
    out.status = with_warning_text(status, result.warning, JobKind::Check);
    return out;
  }
  if (result.status == SimulateResult::KeptBack || (result.packages.empty() && has_remaining(result))) {
    out.packages = PackageListAction::Hide;
    out.install_enabled = false;
    out.status = with_warning_text(kept_headline(result), result.warning, JobKind::Check);
    return out;
  }
  std::string msg = result.error_msg;
  if (msg.empty() || msg == "No STATUS from helper") {
    if (exit_code == 127 || exit_code == 126)
      msg = "Could not run the update helper (pkexec).";
    else
      msg = "The update check failed.";
  }
  out.status = friendly_job_error(msg, JobKind::Check);
  out.packages = have_rows ? PackageListAction::Keep : PackageListAction::Hide;
  out.install_enabled = have_rows;
  return out;
}

JobOutcome outcome_install(const SimulateResult& result, int exit_code, bool have_rows)
{
  JobOutcome out;
  out.check_enabled = true;
  const std::string conf = conf_sentence(result);
  const std::string reboot = reboot_sentence(result);
  const std::string pending = pending_sentence(result);
  const std::string extras = describe_remaining(result);
  if (result.install_skipped) {
    std::string status = "The update list changed. Nothing was installed.";
    if (result.status == SimulateResult::Upgrades && !result.packages.empty()) {
      out.packages = PackageListAction::Show;
      out.install_enabled = true;
      append_sentence(status, extras);
    } else {
      out.packages = PackageListAction::Hide;
      out.install_enabled = false;
      if (has_remaining(result))
        append_sentence(status, extras);
      else if (result.status == SimulateResult::UpToDate || result.status == SimulateResult::Success)
        append_sentence(status, "You're up to date.");
    }
    append_sentence(status, conf);
    out.status = with_warning_text(status, result.warning, JobKind::Install);
    return out;
  }
  if (result.status == SimulateResult::Error) {
    std::string msg = result.error_msg;
    if (msg.empty() || msg == "No STATUS from helper") {
      if (exit_code == 127 || exit_code == 126)
        msg = "Could not run the update helper (pkexec).";
      else
        msg = "apt-get upgrade failed.";
    }
    out.status = friendly_job_error(msg, JobKind::Install);
    out.packages = PackageListAction::Keep;
    out.install_enabled = have_rows;
    return out;
  }

  const bool nothing_installed =
      result.status == SimulateResult::KeptBack && result.upgraded_count <= 0 && result.packages.empty();
  std::string status;
  if (nothing_installed) {
    status = kept_headline(result);
  } else {
    status = "Updates installed.";
    if (result.summary_missing)
      append_sentence(status, "Apt did not report whether any packages were kept back.");
    append_sentence(status, extras);
    const bool empty_set = !result.summary_missing && extras.empty() && conf.empty() && reboot.empty() &&
                           pending.empty() && !has_remaining(result);
    if (empty_set)
      append_sentence(status, "You're up to date.");
  }
  append_sentence(status, conf);
  append_sentence(status, debconf_answer_sentence(result));
  append_sentence(status, reboot);
  append_sentence(status, pending);
  out.packages = PackageListAction::Hide;
  out.install_enabled = false;
  out.offer_restart = result.reboot_required;
  out.status = with_warning_text(status, result.warning, JobKind::Install);
  return out;
}

JobOutcome outcome_timeout(bool installing, bool helper_ready, bool have_rows)
{
  JobOutcome out;
  out.check_enabled = true;
  out.install_enabled = have_rows;
  out.packages = (!installing && !have_rows) ? PackageListAction::Hide : PackageListAction::Keep;
  if (!helper_ready) {
    out.status = "Timed out waiting for authentication. Try again.";
    if (!have_rows)
      out.packages = PackageListAction::Hide;
  } else if (!installing) {
    out.status = "Timed out waiting for the update check. Check your network and try again.";
  } else {
    out.status = "Timed out while installing updates.";
    out.packages = PackageListAction::Keep;
  }
  return out;
}

bool error_suggests_repair(const std::string& msg)
{
  std::string lower = msg;
  for (char& ch : lower)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  auto has_text = [&](const char* needle) { return lower.find(needle) != std::string::npos; };
  /* A dpkg that is still running is not repairable yet. */
  if (has_text("still running"))
    return false;
  return has_text("use repair") || has_text("dpkg was interrupted") ||
         has_text("dpkg --configure -a") || has_text("--fix-broken") ||
         has_text("unmet dependencies") || has_text("left unfinished") ||
         has_text("half-installed") || has_text("half-configured") ||
         has_text("package installer was interrupted");
}

/* ---------------- Round 11 ---------------- */


JobOutcome outcome_repair_timeout(bool helper_ready)
{
  JobOutcome out;
  out.check_enabled = true;
  out.install_enabled = false;
  out.packages = PackageListAction::Hide;
  if (!helper_ready)
    out.status = "Timed out waiting for the password. The repair did not start. "
                 "Click Repair to try again.";
  else
    out.status = "Timed out while repairing packages.";
  return out;
}

std::vector<DpkgPackageState> parse_dpkg_status(const std::string& status_text)
{
  std::vector<DpkgPackageState> out;
  DpkgPackageState cur;
  auto flush = [&]() {
    if (!cur.name.empty())
      out.push_back(cur);
    cur = DpkgPackageState{};
  };
  std::istringstream in(status_text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim_cr(line);
    if (line.empty()) {
      flush();
      continue;
    }
    if (starts_with(line, "Package: ")) {
      cur.name = line.substr(9);
    } else if (starts_with(line, "Version: ")) {
      cur.version = line.substr(9);
    } else if (starts_with(line, "Status: ")) {
      std::istringstream words(line.substr(8));
      std::string want;
      std::string flag;
      std::string state;
      words >> want >> flag >> state;
      cur.state = state;
    }
  }
  flush();
  return out;
}

std::vector<std::string> unfinished_packages(const std::string& status_text)
{
  std::vector<std::string> names;
  for (const auto& pkg : parse_dpkg_status(status_text)) {
    const std::string& st = pkg.state;
    if (st == "half-installed" || st == "unpacked" || st == "half-configured" ||
        st == "triggers-awaited" || st == "triggers-pending") {
      if (std::find(names.begin(), names.end(), pkg.name) == names.end())
        names.push_back(pkg.name);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string unfinished_message(const std::vector<std::string>& names, bool journal)
{
  std::string msg = journal ? "An earlier install was interrupted and did not finish."
                            : "An earlier install did not finish.";
  if (!names.empty()) {
    msg += names.size() == 1 ? " This package is not fully set up: "
                             : " These packages are not fully set up: ";
    std::string list;
    for (std::size_t i = 0; i < names.size() && i < 20; ++i) {
      if (!list.empty())
        list += ", ";
      list += protocol_escape(names[i], false);
    }
    if (names.size() > 20)
      list += ", …";
    msg += list + ".";
  }
  msg += "\nUse Repair to finish setting up packages before installing updates.";
  return msg;
}

bool error_says_locked(const std::string& text)
{
  const std::string lower = lower_copy(text);
  return has(lower, "could not get lock") || has(lower, "unable to lock") ||
         has(lower, "lock was locked by") || has(lower, "frontend lock") ||
         has(lower, "is locked by another process") || has(lower, "unable to acquire the dpkg");
}

/* ---- Round 12 (U-1): debconf questions ---- */

namespace {

struct Stanza {
  std::vector<std::pair<std::string, std::string>> fields;
  std::string get(const char* key) const
  {
    for (const auto& f : fields)
      if (f.first == key)
        return f.second;
    return {};
  }
};

std::vector<Stanza> parse_stanzas(const std::string& text)
{
  std::vector<Stanza> out;
  std::istringstream in(text);
  std::string line;
  Stanza cur;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty()) {
      if (!cur.fields.empty())
        out.push_back(std::move(cur));
      cur = Stanza{};
      continue;
    }
    if (line[0] == ' ' || line[0] == '\t')
      continue; /* continuation: not needed here */
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
      continue;
    std::string value = line.substr(colon + 1);
    std::size_t s = value.find_first_not_of(" \t");
    value = s == std::string::npos ? std::string() : value.substr(s);
    cur.fields.emplace_back(line.substr(0, colon), value);
  }
  if (!cur.fields.empty())
    out.push_back(std::move(cur));
  return out;
}

std::vector<std::string> split_list(const std::string& text)
{
  std::vector<std::string> out;
  std::string item;
  for (char ch : text + ",") {
    if (ch == ',') {
      std::size_t a = item.find_first_not_of(" \t");
      std::size_t b = item.find_last_not_of(" \t");
      if (a != std::string::npos)
        out.push_back(item.substr(a, b - a + 1));
      item.clear();
    } else {
      item += ch;
    }
  }
  return out;
}

bool pkg_char(char ch)
{
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-';
}

/* True when a relationship field names debconf or debconf-2.0 as a package. */
bool names_debconf(const std::string& field)
{
  std::size_t i = 0;
  while (i < field.size()) {
    if (!pkg_char(field[i])) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < field.size() && pkg_char(field[j]))
      ++j;
    const std::string word = field.substr(i, j - i);
    /* Skip version numbers inside (>= ...). */
    bool in_version = false;
    for (std::size_t k = i; k > 0; --k) {
      const char ch = field[k - 1];
      if (ch == '(') { in_version = true; break; }
      if (ch == ')' || ch == ',' || ch == '|') break;
    }
    if (!in_version && (word == "debconf" || word == "debconf-2.0"))
      return true;
    i = j;
  }
  return false;
}

void sort_unique(std::vector<std::string>& names)
{
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

}  // namespace

std::vector<std::string> packages_depending_on_debconf(const std::string& apt_cache_show)
{
  std::vector<std::string> out;
  for (const auto& st : parse_stanzas(apt_cache_show)) {
    const std::string name = st.get("Package");
    if (name.empty())
      continue;
    if (names_debconf(st.get("Depends")) || names_debconf(st.get("Pre-Depends")))
      out.push_back(name);
  }
  sort_unique(out);
  return out;
}

std::vector<std::string> debconf_question_owners(const std::string& config_dat)
{
  std::vector<std::string> out;
  for (const auto& st : parse_stanzas(config_dat))
    for (const auto& owner : split_list(st.get("Owners")))
      out.push_back(owner);
  sort_unique(out);
  return out;
}

std::vector<SimulateResult::DebconfAnswer> unseen_debconf_answers(const std::string& config_dat,
                                                                  const std::string& templates_dat,
                                                                  const std::vector<std::string>& pkgs)
{
  std::vector<std::pair<std::string, std::string>> types;
  for (const auto& st : parse_stanzas(templates_dat))
    types.emplace_back(st.get("Name"), st.get("Type"));
  auto type_of = [&](const std::string& tmpl) -> std::string {
    for (const auto& t : types)
      if (t.first == tmpl)
        return t.second;
    return {};
  };
  std::vector<SimulateResult::DebconfAnswer> out;
  for (const auto& st : parse_stanzas(config_dat)) {
    const std::string name = st.get("Name");
    if (name.empty())
      continue;
    bool seen = false;
    for (const auto& flag : split_list(st.get("Flags")))
      if (flag == "seen")
        seen = true;
    if (seen)
      continue;
    std::string tmpl = st.get("Template");
    if (tmpl.empty())
      tmpl = name;
    const std::string type = type_of(tmpl);
    if (type != "string" && type != "boolean" && type != "select" && type != "multiselect" &&
        type != "password")
      continue;
    std::string owner;
    for (const auto& o : split_list(st.get("Owners")))
      if (owner.empty() && std::find(pkgs.begin(), pkgs.end(), o) != pkgs.end())
        owner = o;
    if (owner.empty())
      continue;
    SimulateResult::DebconfAnswer ans;
    ans.package = owner;
    ans.question = name;
    ans.value = type == "password" ? std::string("(password not shown)") : st.get("Value");
    out.push_back(ans);
  }
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.package != b.package ? a.package < b.package : a.question < b.question;
  });
  return out;
}


namespace {

std::string debconf_change_hint(const std::vector<std::string>& pkgs)
{
  std::string names;
  for (const auto& p : pkgs)
    names += (names.empty() ? "" : " ") + p;
  return "To change an answer later, open Terminal from the applications menu and run: "
         "sudo dpkg-reconfigure " + names + ".";
}

int priority_rank(const std::string& p)
{
  if (p == "low")
    return 0;
  if (p == "medium")
    return 1;
  if (p == "high")
    return 2;
  if (p == "critical")
    return 3;
  return -1;
}

}  // namespace

bool is_debconf_trace_line(const std::string& line)
{
  return line.find("debconf (developer):") != std::string::npos;
}

std::string strip_debconf_trace(const std::string& text)
{
  if (text.find("debconf (developer):") == std::string::npos)
    return text;
  std::string out;
  std::size_t pos = 0;
  while (pos < text.size()) {
    std::size_t nl = text.find('\n', pos);
    const std::size_t end = nl == std::string::npos ? text.size() : nl + 1;
    const std::string line = text.substr(pos, end - pos);
    if (!is_debconf_trace_line(line))
      out += line;
    pos = end;
  }
  return out;
}

std::vector<DebconfInput> debconf_inputs(const std::string& apt_output)
{
  std::vector<DebconfInput> out;
  std::string package;
  std::size_t pos = 0;
  const std::string tag = "debconf (developer): ";
  while (pos < apt_output.size()) {
    std::size_t nl = apt_output.find_first_of("\r\n", pos);
    if (nl == std::string::npos)
      nl = apt_output.size();
    const std::string line = apt_output.substr(pos, nl - pos);
    pos = nl + 1;
    const std::size_t t = line.find(tag);
    if (t == std::string::npos)
      continue;
    const std::string msg = line.substr(t + tag.size());
    const std::string pkg_tag = "package name is ";
    const std::size_t pn = msg.find(pkg_tag);
    if (msg.compare(0, 9, "frontend ") == 0 && pn != std::string::npos) {
      std::istringstream ps(msg.substr(pn + pkg_tag.size()));
      package.clear();
      ps >> package;
      continue;
    }
    if (msg.compare(0, 10, "<-- INPUT ") != 0)
      continue;
    std::istringstream is(msg.substr(10));
    DebconfInput in;
    is >> in.priority >> in.question;
    if (in.question.empty() || priority_rank(in.priority) < 0)
      continue;
    in.package = package;
    if (in.package.empty())
      in.package = in.question.substr(0, in.question.find('/'));
    out.push_back(in);
  }
  return out;
}

std::vector<DebconfInput> debconf_newly_seen(const std::vector<DebconfQuestion>& before,
                                             const std::vector<DebconfQuestion>& after)
{
  std::vector<DebconfInput> out;
  for (const auto& q : after) {
    if (!q.seen)
      continue;
    bool was_seen = false;
    for (const auto& b : before)
      if (b.name == q.name && b.seen)
        was_seen = true;
    if (was_seen)
      continue;
    bool dup = false;
    for (const auto& o : out)
      if (o.question == q.name)
        dup = true;
    if (dup)
      continue;
    DebconfInput in;
    in.question = q.name;
    in.package = q.owner.empty() ? q.name.substr(0, q.name.find('/')) : q.owner;
    /* Debconf already filtered by priority; pass every threshold. */
    in.priority = "critical";
    out.push_back(in);
  }
  return out;
}

std::string debconf_priority(const std::string& config_dat)
{
  for (const auto& st : parse_stanzas(config_dat))
    if (st.get("Name") == "debconf/priority") {
      const std::string v = st.get("Value");
      return priority_rank(v) >= 0 ? v : std::string("high");
    }
  return "high";
}

std::vector<DebconfQuestion> debconf_snapshot(const std::string& config_dat, const std::string& passwords_dat)
{
  std::vector<DebconfQuestion> out;
  auto add = [&](const std::string& text, bool password) {
    for (const auto& st : parse_stanzas(text)) {
      DebconfQuestion q;
      q.name = st.get("Name");
      if (q.name.empty())
        continue;
      q.password = password;
      bool has = false;
      for (const auto& f : st.fields)
        if (f.first == "Value")
          has = true;
      const std::string v = st.get("Value");
      q.has_value = has && !v.empty();
      if (!password)
        q.value = v;
      const std::vector<std::string> owners = split_list(st.get("Owners"));
      if (!owners.empty())
        q.owner = owners.front();
      for (const auto& flag : split_list(st.get("Flags")))
        if (flag == "seen")
          q.seen = true;
      out.push_back(std::move(q));
    }
  };
  add(config_dat, false);
  add(passwords_dat, true);
  return out;
}

std::vector<SimulateResult::DebconfAnswer> debconf_auto_answers(const std::vector<DebconfInput>& inputs,
                                                                const std::vector<DebconfQuestion>& before,
                                                                const std::vector<DebconfQuestion>& after,
                                                                const std::string& templates_dat,
                                                                const std::string& priority)
{
  const int threshold = priority_rank(priority) < 0 ? 2 : priority_rank(priority);
  auto find = [](const std::vector<DebconfQuestion>& qs, const std::string& name,
                 bool password) -> const DebconfQuestion* {
    for (const auto& q : qs)
      if (q.name == name && q.password == password)
        return &q;
    return nullptr;
  };
  std::vector<Stanza> templates = parse_stanzas(templates_dat);
  auto template_of = [&](const std::string& name) -> const Stanza* {
    for (const auto& t : templates)
      if (t.get("Name") == name)
        return &t;
    return nullptr;
  };
  std::vector<SimulateResult::DebconfAnswer> out;
  std::vector<std::string> done;
  for (const auto& in : inputs) {
    if (priority_rank(in.priority) < threshold)
      continue; /* U-3: a normal install would not have asked this either */
    if (std::find(done.begin(), done.end(), in.question) != done.end())
      continue;
    const Stanza* tmpl = template_of(in.question);
    const std::string type = tmpl != nullptr ? tmpl->get("Type") : std::string();
    if (type != "string" && type != "boolean" && type != "select" && type != "multiselect" &&
        type != "password")
      continue;
    const bool password = type == "password";
    /* Debconf keeps passwords in passwords.dat; an older or custom setup
     * may keep them in config.dat. Either way only has_value is used. */
    const DebconfQuestion* was = find(before, in.question, password);
    if (was == nullptr && password)
      was = find(before, in.question, false);
    const DebconfQuestion* now = find(after, in.question, password);
    if (now == nullptr && password)
      now = find(after, in.question, false);
    if (was != nullptr && was->seen)
      continue; /* the user answered it before; debconf kept that answer */
    /* U-2: an unseen answer that was already there and did not change was
     * reported (or set) by an earlier install; do not list it again. */
    if (was != nullptr && now != nullptr && was->has_value == now->has_value && was->value == now->value)
      continue;
    if (was != nullptr && now == nullptr)
      continue;
    done.push_back(in.question);
    SimulateResult::DebconfAnswer ans;
    ans.package = in.package;
    ans.question = in.question;
    ans.text = tmpl->get("Description");
    if (password)
      ans.value = now != nullptr && now->has_value ? "(password set, not shown)" : "(password left blank)";
    else
      ans.value = now != nullptr ? now->value : std::string();
    out.push_back(ans);
  }
  return out;
}

std::string debconf_review_notice(const std::vector<std::string>& pkgs)
{
  if (pkgs.empty())
    return {};
  /* Round 13 (U-5): earlier answers are kept; only new questions get the
   * package's default. The hint names the Terminal app, not just a command. */
  return join_names(pkgs) + " may ask setup questions. " +
         "Updates installs without stopping to ask: answers you gave before are kept, and new "
         "questions get the package's suggested answer. When the install finishes, Updates lists "
         "the answers it chose. " + debconf_change_hint(pkgs);
}

std::string debconf_answer_sentence(const SimulateResult& result)
{
  if (result.debconf_answers.empty())
    return {};
  std::string list;
  std::vector<std::string> owners;
  for (const auto& ans : result.debconf_answers) {
    if (!list.empty())
      list += "; ";
    const std::string text = ans.text.empty() ? ans.question : ans.text;
    list += "\u201c" + text + "\u201d (" + ans.package + "): " +
            (ans.value.empty() ? std::string("left blank") : ans.value);
    owners.push_back(ans.package);
  }
  sort_unique(owners);
  return "Setup questions were answered automatically: " + list + ". " + debconf_change_hint(owners);
}
