/* BIOS grub-pc boot disk: decide whether install_devices must be set.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "grub-pc.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

std::string trim_copy(std::string text)
{
  while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
    text.pop_back();
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r'))
    ++i;
  return text.substr(i);
}

bool safe_name(const std::string& name)
{
  if (name.empty() || name.size() > 200 || name == "." || name == "..")
    return false;
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '+' || c == '-')
      continue;
    return false;
  }
  return true;
}

bool safe_dev_path(const std::string& path)
{
  if (path.size() < 6 || path.size() > 240 || path.compare(0, 5, "/dev/") != 0)
    return false;
  if (path.find("..") != std::string::npos)
    return false;
  for (unsigned char c : path) {
    if (std::isalnum(c) || c == '/' || c == '.' || c == '_' || c == '+' || c == '-')
      continue;
    return false;
  }
  return true;
}

struct Stanza {
  std::vector<std::pair<std::string, std::string>> fields;
  std::string get(const char* key) const
  {
    for (const auto& field : fields)
      if (field.first == key)
        return field.second;
    return {};
  }
};

std::vector<Stanza> parse_stanzas(const std::string& text)
{
  std::vector<Stanza> out;
  Stanza cur;
  std::string* cont = nullptr;
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t end = text.find('\n', i);
    if (end == std::string::npos)
      end = text.size();
    std::string line = text.substr(i, end - i);
    i = end < text.size() ? end + 1 : end;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty()) {
      if (!cur.fields.empty())
        out.push_back(std::move(cur));
      cur = Stanza{};
      cont = nullptr;
      continue;
    }
    if ((line[0] == ' ' || line[0] == '\t') && cont != nullptr) {
      *cont += trim_copy(line);
      continue;
    }
    cont = nullptr;
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string key = line.substr(0, colon);
    std::string value = colon + 1 < line.size() ? line.substr(colon + 1) : std::string();
    if (!value.empty() && value[0] == ' ')
      value.erase(0, 1);
    cur.fields.emplace_back(key, value);
    cont = &cur.fields.back().second;
  }
  if (!cur.fields.empty())
    out.push_back(std::move(cur));
  return out;
}

bool all_digits(const std::string& text, std::size_t from, std::size_t to)
{
  if (from >= to || to > text.size())
    return false;
  for (std::size_t i = from; i < to; ++i)
    if (!std::isdigit(static_cast<unsigned char>(text[i])))
      return false;
  return true;
}

bool trailing_letters(const std::string& text, std::size_t from)
{
  if (from >= text.size())
    return false;
  for (std::size_t i = from; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (!std::islower(c))
      return false;
  }
  return true;
}

bool disk_with_letter_tail(const std::string& name, const char* prefix)
{
  const std::size_t n = std::char_traits<char>::length(prefix);
  return name.size() > n && name.compare(0, n, prefix) == 0 && trailing_letters(name, n);
}

std::vector<std::string> field_words(const std::string& text)
{
  std::vector<std::string> words;
  std::string word;
  for (char ch : text) {
    if (ch == ' ' || ch == '\t') {
      if (!word.empty()) {
        words.push_back(word);
        word.clear();
      }
      continue;
    }
    word.push_back(ch);
  }
  if (!word.empty())
    words.push_back(word);
  return words;
}

std::string basename_of(const std::string& path)
{
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  return path.substr(slash + 1);
}

std::vector<std::string> probe_paths(const std::string& probe_output)
{
  std::vector<std::string> paths;
  std::string token;
  auto push = [&]() {
    if (token.empty())
      return;
    if (safe_dev_path(token) && paths.size() < 32)
      paths.push_back(token);
    token.clear();
  };
  for (char ch : probe_output) {
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
      push();
    else
      token.push_back(ch);
  }
  push();
  return paths;
}

std::vector<std::string> slaves_of(const DiskTopology& topo, const std::string& name)
{
  for (const auto& entry : topo.slaves)
    if (entry.first == name)
      return entry.second;
  return {};
}

std::string alias_of(const DiskTopology& topo, const std::string& name)
{
  for (const auto& entry : topo.alias)
    if (entry.first == name)
      return entry.second;
  return {};
}

std::string by_id_target(const DiskTopology& topo, const std::string& link)
{
  for (const auto& entry : topo.by_id)
    if (entry.first == link)
      return entry.second;
  return {};
}

int by_id_rank(const std::string& name)
{
  if (!safe_name(name))
    return 100;
  auto starts = [&](const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return name.size() >= n && name.compare(0, n, prefix) == 0;
  };
  if (starts("dm-name-") || starts("dm-uuid-") || starts("lvm-") || starts("md-name-") ||
      starts("md-uuid-") || starts("mpath-"))
    return 100;
  const std::size_t part = name.rfind("-part");
  if (part != std::string::npos && all_digits(name, part + 5, name.size()))
    return 100;
  if (starts("ata-"))
    return 0;
  if (starts("scsi-"))
    return 1;
  if (starts("nvme-eui."))
    return 6;
  if (starts("nvme-"))
    return 2;
  if (starts("virtio-"))
    return 3;
  if (starts("mmc-"))
    return 4;
  if (starts("usb-"))
    return 5;
  if (starts("wwn-"))
    return 7;
  return 8;
}

std::string device_path_for(const std::string& disk, const DiskTopology& topo)
{
  int best_rank = 100;
  std::string best;
  for (const auto& link : topo.by_id) {
    if (link.second != disk || !safe_name(link.first))
      continue;
    const int rank = by_id_rank(link.first);
    if (rank >= 100)
      continue;
    if (best.empty() || rank < best_rank || (rank == best_rank && link.first < best)) {
      best_rank = rank;
      best = link.first;
    }
  }
  if (!best.empty())
    return "/dev/disk/by-id/" + best;
  return "/dev/" + disk;
}

void collect_disks(const std::string& name, const DiskTopology& topo, std::vector<std::string>& seen,
                   std::vector<std::string>& out)
{
  if (!safe_name(name) || seen.size() > 64)
    return;
  if (std::find(seen.begin(), seen.end(), name) != seen.end())
    return;
  seen.push_back(name);

  std::string cur = name;
  const std::string via_id = by_id_target(topo, cur);
  if (safe_name(via_id) && via_id != cur) {
    if (std::find(seen.begin(), seen.end(), via_id) != seen.end())
      return;
    seen.push_back(via_id);
    cur = via_id;
  }
  for (int step = 0; step < 8; ++step) {
    const std::string next = alias_of(topo, cur);
    if (!safe_name(next) || next == cur)
      break;
    if (std::find(seen.begin(), seen.end(), next) != seen.end())
      return;
    seen.push_back(next);
    cur = next;
  }

  std::vector<std::string> slaves = slaves_of(topo, cur);
  std::sort(slaves.begin(), slaves.end());
  if (!slaves.empty()) {
    for (const auto& slave : slaves)
      collect_disks(slave, topo, seen, out);
    return;
  }
  const std::string parent = partition_parent_name(cur);
  if (!parent.empty()) {
    collect_disks(parent, topo, seen, out);
    return;
  }
  if (is_whole_disk_name(cur) && std::find(out.begin(), out.end(), cur) == out.end())
    out.push_back(cur);
}

std::string join_devices(const std::vector<std::string>& devices)
{
  std::string out;
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (i != 0)
      out += ", ";
    out += devices[i];
  }
  return out;
}

}  // namespace

bool grub_pc_package_present(const std::string& status_text)
{
  for (const auto& stanza : parse_stanzas(status_text)) {
    if (stanza.get("Package") != "grub-pc")
      continue;
    const std::vector<std::string> status = field_words(stanza.get("Status"));
    if (status.size() < 3 || status[0] != "install")
      return false;
    const std::string& state = status[2];
    return state == "installed" || state == "half-configured" || state == "unpacked" ||
           state == "half-installed" || state == "triggers-awaited" || state == "triggers-pending";
  }
  return false;
}

std::string grub_pc_install_devices_value(const std::string& config_dat)
{
  for (const auto& stanza : parse_stanzas(config_dat)) {
    if (stanza.get("Name") == "grub-pc/install_devices")
      return trim_copy(stanza.get("Value"));
  }
  return {};
}

std::string partition_parent_name(const std::string& name)
{
  if (!safe_name(name))
    return {};
  const std::size_t p = name.rfind('p');
  if (p != std::string::npos && p > 0) {
    auto prefixed = [&](const char* prefix) -> bool {
      const std::size_t n = std::char_traits<char>::length(prefix);
      if (name.compare(0, n, prefix) != 0 || p < n)
        return false;
      return all_digits(name, n, p) && all_digits(name, p + 1, name.size());
    };
    if (name.compare(0, 4, "nvme") == 0) {
      std::size_t i = 4;
      if (i < p && std::isdigit(static_cast<unsigned char>(name[i]))) {
        while (i < p && std::isdigit(static_cast<unsigned char>(name[i])))
          ++i;
        if (i < p && name[i] == 'n') {
          ++i;
          if (all_digits(name, i, p) && all_digits(name, p + 1, name.size()))
            return name.substr(0, p);
        }
      }
    } else if (prefixed("mmcblk") || prefixed("loop") || prefixed("md")) {
      return name.substr(0, p);
    }
  }
  std::size_t digit = name.size();
  while (digit > 0 && std::isdigit(static_cast<unsigned char>(name[digit - 1])))
    --digit;
  if (digit == name.size() || digit == 0)
    return {};
  const std::string base = name.substr(0, digit);
  if ((base.size() >= 3 && (base[0] == 's' || base[0] == 'v' || base[0] == 'h') && base[1] == 'd' &&
       trailing_letters(base, 2)) ||
      disk_with_letter_tail(base, "xvd"))
    return base;
  return {};
}

bool is_whole_disk_name(const std::string& name)
{
  if (!safe_name(name) || !partition_parent_name(name).empty())
    return false;
  if ((name.size() >= 3 && (name[0] == 's' || name[0] == 'v' || name[0] == 'h') && name[1] == 'd' &&
       trailing_letters(name, 2)) ||
      disk_with_letter_tail(name, "xvd"))
    return true;
  if (name.compare(0, 4, "nvme") == 0) {
    std::size_t i = 4;
    if (i >= name.size() || !std::isdigit(static_cast<unsigned char>(name[i])))
      return false;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])))
      ++i;
    if (i >= name.size() || name[i] != 'n')
      return false;
    ++i;
    return all_digits(name, i, name.size());
  }
  if (name.compare(0, 6, "mmcblk") == 0)
    return all_digits(name, 6, name.size());
  return false;
}

std::vector<std::string> grub_boot_disk_devices(const std::string& probe_output, const DiskTopology& topo)
{
  std::vector<std::string> disks;
  std::vector<std::string> seen;
  for (const auto& path : probe_paths(probe_output))
    collect_disks(basename_of(path), topo, seen, disks);
  std::vector<std::string> devices;
  for (const auto& disk : disks) {
    if (!is_whole_disk_name(disk))
      continue;
    const std::string path = device_path_for(disk, topo);
    if (!safe_dev_path(path))
      continue;
    if (std::find(devices.begin(), devices.end(), path) == devices.end() && devices.size() < 16)
      devices.push_back(path);
  }
  return devices;
}

GrubPcDecision decide_grub_pc(const GrubPcFacts& facts)
{
  GrubPcDecision decision;
  if (!facts.package_present || !facts.bios_core_img)
    return decision;
  if (!trim_copy(facts.install_devices).empty()) {
    decision.log = "grub-pc: install_devices is already set; left unchanged";
    return decision;
  }
  if (!facts.probe_ok) {
    decision.action = GrubPcAction::Unknown;
    decision.log = "grub-pc: could not determine the boot disk";
    return decision;
  }
  decision.devices = grub_boot_disk_devices(facts.probe_output, facts.topo);
  if (decision.devices.empty()) {
    decision.action = GrubPcAction::Unknown;
    decision.log = "grub-pc: could not determine the boot disk";
    return decision;
  }
  decision.action = GrubPcAction::Set;
  decision.log = "grub-pc: set grub-pc/install_devices to " + join_devices(decision.devices);
  return decision;
}

std::string grub_pc_debconf_line(const std::vector<std::string>& devices)
{
  if (devices.empty())
    return {};
  for (const auto& device : devices)
    if (!safe_dev_path(device))
      return {};
  const std::string line = "grub-pc grub-pc/install_devices multiselect " + join_devices(devices);
  if (line.find("install_devices_empty") != std::string::npos)
    return {};
  return line;
}

std::string grub_pc_manual_message()
{
  return "Could not tell which disk the bootloader should use.\n"
         "Run this command and choose the whole disk (for example /dev/sda, not a partition):\n"
         "sudo DEBIAN_FRONTEND=dialog dpkg --configure grub-pc";
}

bool grub_pc_configure_failed(const std::string& text)
{
  /* "Setting up grub-pc" is not a failure. dpkg names the package on the
   * "error processing package" line and again in the error list. */
  const std::string marker = "processing package grub-pc";
  std::size_t at = 0;
  while ((at = text.find(marker, at)) != std::string::npos) {
    const std::size_t end = at + marker.size();
    const bool boundary = end >= text.size() ||
                          (!std::isalnum(static_cast<unsigned char>(text[end])) && text[end] != '-' &&
                           text[end] != '+' && text[end] != '.');
    if (boundary)
      return true;
    at = end;
  }
  std::size_t i = 0;
  while (i < text.size()) {
    std::size_t end = text.find('\n', i);
    if (end == std::string::npos)
      end = text.size();
    if (trim_copy(text.substr(i, end - i)) == "grub-pc")
      return true;
    i = end < text.size() ? end + 1 : end;
  }
  return false;
}
