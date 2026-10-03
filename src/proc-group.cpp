/* Stop a process and every descendant, including separate process groups.
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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

struct ProcInfo {
  pid_t pid = 0;
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
};

void sleep_ms(int ms)
{
  if (ms > 0)
    poll(nullptr, 0, ms);
}

bool read_proc_ids(pid_t pid, pid_t& ppid, pid_t& pgrp, char& state)
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
  int pp = 0;
  int pg = 0;
  if (std::sscanf(rparen + 1, " %c %d %d", &state, &pp, &pg) != 3)
    return false;
  ppid = static_cast<pid_t>(pp);
  pgrp = static_cast<pid_t>(pg);
  return pgrp > 1;
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
    if (!read_proc_ids(info.pid, info.ppid, info.pgrp, info.state))
      continue;
    procs.push_back(info);
  }
  closedir(dir);
  return procs;
}

bool contains_pid(const std::vector<pid_t>& ids, pid_t pid)
{
  for (pid_t id : ids) {
    if (id == pid)
      return true;
  }
  return false;
}

void collect_descendants(pid_t root, std::vector<pid_t>& members, std::vector<pid_t>& groups)
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
      if (contains_pid(seen, info.pid))
        continue;
      seen.push_back(info.pid);
      stack.push_back(info.pid);
    }
  }
  for (const ProcInfo& info : procs) {
    if (!contains_pid(seen, info.pid))
      continue;
    if (!contains_pid(members, info.pid))
      members.push_back(info.pid);
    if (info.pgrp > 1 && !contains_pid(groups, info.pgrp))
      groups.push_back(info.pgrp);
  }
  if (!contains_pid(members, root))
    members.push_back(root);
  if (!contains_pid(groups, root))
    groups.push_back(root);
}

bool safe_group(pid_t pgid);

bool process_alive(pid_t pid)
{
  if (pid <= 1 || pid == getpid())
    return false;
  pid_t ppid = 0;
  pid_t pgrp = 0;
  char state = '?';
  if (!read_proc_ids(pid, ppid, pgrp, state))
    return false;
  /* A zombie is not running apt or dpkg. Init (or waitpid) will reap it. */
  if (state == 'Z')
    return false;
  if (kill(pid, 0) == 0)
    return true;
  return errno == EPERM;
}

bool group_has_runner(pid_t pgid)
{
  if (!safe_group(pgid))
    return false;
  const std::vector<ProcInfo> procs = list_procs();
  for (const ProcInfo& info : procs) {
    if (info.pgrp == pgid && info.state != 'Z' && info.pid != getpid())
      return true;
  }
  return false;
}

bool safe_group(pid_t pgid)
{
  return pgid > 1 && pgid != getpid() && pgid != getpgrp();
}

void signal_targets(int sig, const std::vector<pid_t>& members, const std::vector<pid_t>& groups)
{
  for (pid_t pgid : groups) {
    if (safe_group(pgid))
      kill(-pgid, sig);
  }
  for (pid_t pid : members) {
    if (pid > 1 && pid != getpid())
      kill(pid, sig);
  }
}

bool any_alive(const std::vector<pid_t>& members, const std::vector<pid_t>& groups)
{
  for (pid_t pid : members) {
    if (process_alive(pid))
      return true;
  }
  for (pid_t pgid : groups) {
    if (group_has_runner(pgid))
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

bool terminate_process_tree(pid_t leader, int term_grace_ms)
{
  if (leader <= 1 || leader == getpid())
    return true;

  std::vector<pid_t> members;
  std::vector<pid_t> groups;
  collect_descendants(leader, members, groups);
  signal_targets(SIGTERM, members, groups);

  int waited = 0;
  bool reaped = false;
  while (waited <= term_grace_ms) {
    if (!reaped)
      reaped = reap_leader(leader, false);
    if (leader > 1 && process_alive(leader))
      collect_descendants(leader, members, groups);
    if (reaped && !any_alive(members, groups))
      return true;
    if (waited == term_grace_ms)
      break;
    const int step = term_grace_ms - waited > 50 ? 50 : term_grace_ms - waited;
    if (step <= 0)
      break;
    sleep_ms(step);
    waited += step;
  }

  if (process_alive(leader))
    collect_descendants(leader, members, groups);
  signal_targets(SIGKILL, members, groups);

  /* WNOHANG only: a root child we are not allowed to signal must not block
   * forever in waitpid. The GUI keeps the job busy until this returns true. */
  for (int i = 0; i < 20; ++i) {
    reap_leader(leader, false);
    if (!any_alive(members, groups))
      return true;
    signal_targets(SIGKILL, members, groups);
    sleep_ms(50);
  }
  reap_leader(leader, false);
  return !any_alive(members, groups);
}
