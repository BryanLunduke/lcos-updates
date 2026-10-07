/* Stop a process and its descendants. Signals pids, not process groups.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proc-group.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ProcInfo {
  pid_t pid = 0;
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
  unsigned long long starttime = 0;
};

struct Tracked {
  pid_t pid = 0;
  unsigned long long starttime = 0;
};

void sleep_ms(int ms)
{
  if (ms > 0)
    poll(nullptr, 0, ms);
}

bool read_proc_ids(pid_t pid, pid_t& ppid, pid_t& pgrp, char& state, unsigned long long& starttime)
{
  char path[64];
  std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
  FILE* file = std::fopen(path, "r");
  if (file == nullptr)
    return false;
  char buf[1024];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, file);
  std::fclose(file);
  if (n == 0)
    return false;
  buf[n] = '\0';
  char* rparen = std::strrchr(buf, ')');
  if (rparen == nullptr)
    return false;
  /* Fields after comm: state ppid pgrp session tty_nr tpgid flags minflt
   * cminflt majflt cmajflt utime stime cutime cstime priority nice
   * num_threads itrealvalue starttime. */
  int pp = 0;
  int pg = 0;
  int session = 0;
  int tty = 0;
  int tpgid = 0;
  unsigned flags = 0;
  unsigned long minflt = 0;
  unsigned long cminflt = 0;
  unsigned long majflt = 0;
  unsigned long cmajflt = 0;
  unsigned long utime = 0;
  unsigned long stime = 0;
  long cutime = 0;
  long cstime = 0;
  long priority = 0;
  long nice = 0;
  long threads = 0;
  long itrealvalue = 0;
  unsigned long long start = 0;
  if (std::sscanf(rparen + 1,
                  " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu",
                  &state, &pp, &pg, &session, &tty, &tpgid, &flags, &minflt, &cminflt, &majflt,
                  &cmajflt, &utime, &stime, &cutime, &cstime, &priority, &nice, &threads,
                  &itrealvalue, &start) != 20)
    return false;
  ppid = static_cast<pid_t>(pp);
  pgrp = static_cast<pid_t>(pg);
  starttime = start;
  return pgrp > 1 && starttime != 0;
}

std::vector<ProcInfo> list_procs()
{
  std::vector<ProcInfo> procs;
  DIR* dir = opendir("/proc");
  if (dir == nullptr)
    return procs;
  while (const dirent* ent = readdir(dir)) {
    if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
      continue;
    const int id = std::atoi(ent->d_name);
    if (id <= 1)
      continue;
    ProcInfo info;
    info.pid = static_cast<pid_t>(id);
    if (!read_proc_ids(info.pid, info.ppid, info.pgrp, info.state, info.starttime))
      continue;
    procs.push_back(info);
  }
  closedir(dir);
  return procs;
}

bool contains_pid(const std::vector<Tracked>& ids, pid_t pid)
{
  for (const Tracked& id : ids) {
    if (id.pid == pid)
      return true;
  }
  return false;
}

bool contains_seen(const std::vector<pid_t>& ids, pid_t pid)
{
  for (pid_t id : ids) {
    if (id == pid)
      return true;
  }
  return false;
}

void collect_descendants(pid_t root, std::vector<Tracked>& members)
{
  const std::vector<ProcInfo> procs = list_procs();
  std::vector<pid_t> stack;
  stack.push_back(root);
  std::vector<pid_t> seen;
  seen.push_back(root);
  while (!stack.empty()) {
    const pid_t cur = stack.back();
    stack.pop_back();
    for (const ProcInfo& info : procs) {
      if (info.ppid != cur || info.pid == cur)
        continue;
      if (contains_seen(seen, info.pid))
        continue;
      seen.push_back(info.pid);
      stack.push_back(info.pid);
    }
  }
  for (const ProcInfo& info : procs) {
    if (!contains_seen(seen, info.pid))
      continue;
    if (!contains_pid(members, info.pid))
      members.push_back(Tracked{info.pid, info.starttime});
  }
  if (!contains_pid(members, root)) {
    unsigned long long start = 0;
    pid_t ppid = 0;
    pid_t pgrp = 0;
    char state = '?';
    if (!read_proc_ids(root, ppid, pgrp, state, start))
      start = 0;
    members.push_back(Tracked{root, start});
  }
}

/* The pid is still the process we snapshotted. A reused pid has a different
 * start time and is not signalled. Zombies are not running. */
bool same_process(const Tracked& tracked)
{
  if (tracked.pid <= 1 || tracked.pid == getpid() || tracked.starttime == 0)
    return false;
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
  unsigned long long start = 0;
  if (!read_proc_ids(tracked.pid, ppid, pgrp, state, start))
    return false;
  if (state == 'Z' || state == 'X')
    return false;
  return start == tracked.starttime;
}

bool process_alive(const Tracked& tracked)
{
  return same_process(tracked);
}

void signal_targets(int sig, const std::vector<Tracked>& members)
{
  for (const Tracked& tracked : members) {
    /* Re-read start time immediately before the signal. */
    if (!same_process(tracked))
      continue;
    kill(tracked.pid, sig);
  }
}

bool any_alive(const std::vector<Tracked>& members)
{
  for (const Tracked& tracked : members) {
    if (process_alive(tracked))
      return true;
  }
  return false;
}

bool is_zombie(pid_t pid)
{
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
  unsigned long long start = 0;
  if (!read_proc_ids(pid, ppid, pgrp, state, start))
    return false;
  return state == 'Z' || state == 'X';
}

std::string read_cmdline(pid_t pid)
{
  char path[64];
  std::snprintf(path, sizeof path, "/proc/%d/cmdline", static_cast<int>(pid));
  FILE* file = std::fopen(path, "r");
  if (file == nullptr)
    return {};
  std::string out;
  char buf[512];
  while (out.size() < 65536) {
    const size_t n = std::fread(buf, 1, sizeof buf, file);
    if (n == 0)
      break;
    for (size_t i = 0; i < n; ++i)
      out.push_back(buf[i] == '\0' ? ' ' : buf[i]);
  }
  std::fclose(file);
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::string basename_of(const std::string& path)
{
  const std::string::size_type slash = path.rfind('/');
  if (slash == std::string::npos)
    return path;
  return path.substr(slash + 1);
}

bool token_is(const std::string& token, const char* literal)
{
  return token == literal;
}

/* Configuration has started only for a live dpkg (not dpkg-query) whose
 * argv contains a configure/unpack/install/remove/triggers action. */
bool pid_is_configuring_dpkg(pid_t pid)
{
  if (is_zombie(pid))
    return false;
  const std::string cmdline = read_cmdline(pid);
  if (cmdline.empty())
    return false;
  std::vector<std::string> args;
  std::string cur;
  for (char ch : cmdline) {
    if (ch == ' ') {
      if (!cur.empty()) {
        args.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty())
    args.push_back(cur);
  if (args.empty())
    return false;
  const std::string base = basename_of(args[0]);
  if (base == "dpkg-query" || base == "dpkg-divert" || base == "dpkg-split" ||
      base == "dpkg-statoverride" || base == "dpkg-trigger")
    return false;
  for (const std::string& arg : args) {
    if (token_is(arg, "--unpack") || token_is(arg, "--configure") || token_is(arg, "--install") ||
        token_is(arg, "--remove") || token_is(arg, "--triggers-only"))
      return true;
  }
  return false;
}

bool reap_leader(pid_t leader, bool block)
{
  if (leader <= 1 || leader == getpid())
    return true;
  for (;;) {
    int status = 0;
    const pid_t got = waitpid(leader, &status, block ? 0 : WNOHANG);
    if (got == leader)
      return true;
    if (got < 0 && errno == ECHILD)
      return true;
    if (got < 0 && errno == EINTR)
      continue;
    return false;
  }
}

} // namespace

unsigned long long proc_starttime(pid_t pid)
{
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
  unsigned long long start = 0;
  if (!read_proc_ids(pid, ppid, pgrp, state, start))
    return 0;
  return start;
}

bool process_tree_contains_dpkg(pid_t leader)
{
  if (leader <= 1)
    return false;
  std::vector<Tracked> members;
  collect_descendants(leader, members);
  for (const Tracked& tracked : members) {
    if (pid_is_configuring_dpkg(tracked.pid))
      return true;
  }
  return false;
}

bool terminate_process_tree(pid_t leader, int term_grace_ms, bool* signaled)
{
  if (signaled != nullptr)
    *signaled = false;
  if (leader <= 1 || leader == getpid())
    return true;

  /* Configuring packages. Do not signal apt-get, dpkg, or maintainer scripts. */
  if (process_tree_contains_dpkg(leader))
    return false;

  std::vector<Tracked> members;
  collect_descendants(leader, members);
  signal_targets(SIGTERM, members);
  if (signaled != nullptr)
    *signaled = true;

  int waited = 0;
  bool reaped = false;
  while (waited <= term_grace_ms) {
    if (!reaped)
      reaped = reap_leader(leader, false);
    if (proc_starttime(leader) != 0)
      collect_descendants(leader, members);
    /* dpkg appeared after SIGTERM. Do not follow it with SIGKILL. */
    if (process_tree_contains_dpkg(leader))
      return false;
    if (reaped && !any_alive(members))
      return true;
    if (waited == term_grace_ms)
      break;
    const int step = term_grace_ms - waited > 50 ? 50 : term_grace_ms - waited;
    if (step <= 0)
      break;
    sleep_ms(step);
    waited += step;
  }

  if (process_tree_contains_dpkg(leader))
    return false;

  if (proc_starttime(leader) != 0)
    collect_descendants(leader, members);
  signal_targets(SIGKILL, members);

  /* A zero grace is the non-blocking path: one SIGKILL, no sleep. */
  if (term_grace_ms == 0) {
    reap_leader(leader, false);
    return !any_alive(members);
  }

  /* WNOHANG only: a child we are not allowed to signal must not block
   * forever in waitpid. */
  for (int i = 0; i < 20; ++i) {
    reap_leader(leader, false);
    if (!any_alive(members))
      return true;
    if (process_tree_contains_dpkg(leader))
      return false;
    if (proc_starttime(leader) != 0)
      collect_descendants(leader, members);
    signal_targets(SIGKILL, members);
    sleep_ms(50);
  }
  reap_leader(leader, false);
  return !any_alive(members);
}
