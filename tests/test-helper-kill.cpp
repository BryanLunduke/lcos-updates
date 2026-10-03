/* Helper kills apt-get and a descendant when its deadline hits or cancel closes stdin.
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
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fails = 0;

static void expect(bool ok, const char* what)
{
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_fails;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static bool dead(pid_t pid)
{
  if (pid <= 0)
    return false;
  if (kill(pid, 0) == 0)
    return false;
  return errno == ESRCH;
}

static std::string basename_of(const char* path)
{
  const char* slash = std::strrchr(path, '/');
  return slash == nullptr ? path : slash + 1;
}

/* Exec'd as fake-apt-get. Ignores SIGTERM and leaves a grandchild that does too. */
static int apt_stub()
{
  signal(SIGTERM, SIG_IGN);
  setpgid(0, 0);
  const pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    signal(SIGTERM, SIG_IGN);
    for (;;)
      pause();
  }
  setpgid(child, child);
  const char* path = std::getenv("LCOS_STUB_PIDFILE");
  if (path != nullptr) {
    FILE* file = std::fopen(path, "w");
    if (file != nullptr) {
      std::fprintf(file, "%d %d\n", static_cast<int>(getpid()), static_cast<int>(child));
      std::fclose(file);
    }
  }
  for (;;)
    pause();
}

static bool read_pids(const std::string& path, pid_t& leader, pid_t& child)
{
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr)
    return false;
  int a = 0;
  int b = 0;
  const int n = std::fscanf(file, "%d %d", &a, &b);
  std::fclose(file);
  if (n != 2)
    return false;
  leader = static_cast<pid_t>(a);
  child = static_cast<pid_t>(b);
  return leader > 1 && child > 1;
}

static bool wait_for_pids(const std::string& path, pid_t& leader, pid_t& child, int ms)
{
  for (int waited = 0; waited < ms; waited += 20) {
    if (read_pids(path, leader, child))
      return true;
    poll(nullptr, 0, 20);
  }
  return read_pids(path, leader, child);
}

static int run_helper(const char* helper, const std::string& apt, const std::string& pidfile,
                      const char* timeout_sec, int* cancel_write, pid_t& helper_pid)
{
  int fds[2] = {-1, -1};
  if (cancel_write != nullptr) {
    if (pipe(fds) != 0)
      return -1;
  }
  const pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    setenv("LCOS_UPDATES_APT_GET", apt.c_str(), 1);
    setenv("LCOS_STUB_PIDFILE", pidfile.c_str(), 1);
    if (timeout_sec != nullptr)
      setenv("LCOS_UPDATES_TIMEOUT_SEC", timeout_sec, 1);
    else
      unsetenv("LCOS_UPDATES_TIMEOUT_SEC");
    if (cancel_write != nullptr) {
      dup2(fds[0], STDIN_FILENO);
      close(fds[0]);
      close(fds[1]);
    } else {
      const int devnull = open("/dev/null", O_RDONLY);
      if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
      }
    }
    execl(helper, helper, "simulate", static_cast<char*>(nullptr));
    _exit(127);
  }
  if (cancel_write != nullptr) {
    close(fds[0]);
    *cancel_write = fds[1];
  }
  helper_pid = pid;
  return 0;
}

static bool wait_pid(pid_t pid, int ms, int& status)
{
  for (int waited = 0; waited < ms; waited += 50) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid)
      return true;
    if (got < 0 && errno == ECHILD)
      return true;
    poll(nullptr, 0, 50);
  }
  return false;
}

int main(int argc, char** argv)
{
  if (argc >= 1 && basename_of(argv[0]) == "fake-apt-get")
    return apt_stub();
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s /path/to/lcos-updates-helper\n", argv[0]);
    return 2;
  }
  const char* helper = argv[1];

  char dir_template[] = "/tmp/lcos-updates-kill-XXXXXX";
  char* dir = mkdtemp(dir_template);
  if (dir == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  char self_path[4096];
  const ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
  if (self_len <= 0) {
    std::perror("readlink");
    return 1;
  }
  self_path[self_len] = '\0';
  const std::string apt = std::string(dir) + "/fake-apt-get";
  if (symlink(self_path, apt.c_str()) != 0) {
    std::perror("symlink");
    return 1;
  }

  {
    const std::string pidfile = std::string(dir) + "/timeout.pids";
    pid_t helper_pid = 0;
    if (run_helper(helper, apt, pidfile, "1", nullptr, helper_pid) != 0) {
      std::fprintf(stderr, "failed to spawn helper for timeout\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 15000, status);
    pid_t leader = 0;
    pid_t child = 0;
    const bool have = read_pids(pidfile, leader, child);
    expect(exited, "helper exits after its own deadline");
    expect(have, "timeout stub recorded apt and descendant");
    expect(dead(leader), "apt-get is dead after the helper reports timeout");
    expect(dead(child), "apt descendant is dead after the helper reports timeout");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string pidfile = std::string(dir) + "/cancel.pids";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    if (run_helper(helper, apt, pidfile, nullptr, &cancel_write, helper_pid) != 0) {
      std::fprintf(stderr, "failed to spawn helper for cancel\n");
      return 1;
    }
    pid_t leader = 0;
    pid_t child = 0;
    expect(wait_for_pids(pidfile, leader, child, 2000), "cancel stub started apt");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    expect(exited, "helper exits after stdin cancel");
    expect(dead(leader), "apt-get is dead after cancel");
    expect(dead(child), "apt descendant is dead after cancel");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all helper kill tests passed\n");
  return 0;
}
