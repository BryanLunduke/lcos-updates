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
#include <sys/prctl.h>
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

static void detach_stdio();

static sig_atomic_t g_got_term = 0;

static void on_stub_term(int /*sig*/)
{
  g_got_term = 1;
}

static int count_and_mark(const char* count_path)
{
  int n = 0;
  if (count_path == nullptr)
    return 1;
  FILE* file = std::fopen(count_path, "r");
  if (file != nullptr) {
    if (std::fscanf(file, "%d", &n) != 1)
      n = 0;
    std::fclose(file);
  }
  ++n;
  file = std::fopen(count_path, "w");
  if (file != nullptr) {
    std::fprintf(file, "%d\n", n);
    std::fclose(file);
  }
  return n;
}

static int apt_script(const char* script)
{
  const char* count_path = std::getenv("LCOS_STUB_COUNTFILE");
  const int n = count_and_mark(count_path);
  if (std::strcmp(script, "partial-update") == 0) {
    if (n == 1) {
      std::fputs(
          "E: Failed to fetch http://deb.example/dists/stable/InRelease 404 Not Found\n"
          "E: Some index files failed to download. They have been ignored, or old ones used instead.\n",
          stderr);
      return 100;
    }
    std::fputs("Inst foo [1] (2 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "hard-update-fail") == 0) {
    std::fputs("E: Could not get lock /var/lib/apt/lists/lock\n", stderr);
    return 100;
  }
  if (std::strcmp(script, "count-only") == 0)
    return 0;
  if (std::strcmp(script, "kept-back-upgrade") == 0) {
    if (n == 1)
      return 0;
    std::fputs("The following packages have been kept back:\n"
               "  linux-image-amd64\n"
               "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "keep-dpkg") == 0) {
    signal(SIGTERM, SIG_IGN);
    setpgid(0, 0);
    int ready[2];
    if (pipe(ready) != 0)
      return 1;
    const pid_t child = fork();
    if (child == 0) {
      prctl(PR_SET_NAME, "dpkg", 0, 0, 0);
      setpgid(0, 0);
      signal(SIGTERM, SIG_IGN);
      char ok = 1;
      if (write(ready[1], &ok, 1) < 0)
        _exit(1);
      detach_stdio();
      for (;;)
        pause();
    }
    setpgid(child, child);
    char ok = 0;
    if (read(ready[0], &ok, 1) != 1)
      _exit(1);
    close(ready[0]);
    close(ready[1]);
    const char* path = std::getenv("LCOS_STUB_PIDFILE");
    if (path != nullptr) {
      FILE* file = std::fopen(path, "w");
      if (file != nullptr) {
        std::fprintf(file, "%d %d\n", static_cast<int>(getpid()), static_cast<int>(child));
        std::fclose(file);
      }
    }
    /* Exit on our own so the helper can finish. A kill would get here first. */
    poll(nullptr, 0, 1500);
    _exit(0);
  }
  if (std::strcmp(script, "dpkg-on-term") == 0) {
    signal(SIGTERM, on_stub_term);
    setpgid(0, 0);
    const char* path = std::getenv("LCOS_STUB_PIDFILE");
    if (path != nullptr) {
      FILE* file = std::fopen(path, "w");
      if (file != nullptr) {
        std::fprintf(file, "%d 0\n", static_cast<int>(getpid()));
        std::fclose(file);
      }
    }
    while (!g_got_term)
      pause();
    int ready[2];
    if (pipe(ready) != 0)
      _exit(1);
    const pid_t child = fork();
    if (child == 0) {
      prctl(PR_SET_NAME, "dpkg", 0, 0, 0);
      signal(SIGTERM, SIG_IGN);
      char ok = 1;
      if (write(ready[1], &ok, 1) < 0)
        _exit(1);
      detach_stdio();
      for (;;)
        pause();
    }
    char ok = 0;
    if (read(ready[0], &ok, 1) != 1)
      _exit(1);
    close(ready[0]);
    close(ready[1]);
    if (path != nullptr) {
      FILE* file = std::fopen(path, "w");
      if (file != nullptr) {
        std::fprintf(file, "%d %d\n", static_cast<int>(getpid()), static_cast<int>(child));
        std::fclose(file);
      }
    }
    signal(SIGTERM, SIG_IGN);
    for (;;)
      pause();
  }
  return 2;
}

/* Exec'd as fake-apt-get. Ignores SIGTERM and leaves a grandchild that does too. */
static int apt_stub()
{
  const char* script = std::getenv("LCOS_STUB_SCRIPT");
  if (script != nullptr && script[0] != '\0')
    return apt_script(script);
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

/* Leader has started; the second pid may still be 0. */
static bool wait_for_leader(const std::string& path, pid_t& leader, int ms)
{
  for (int waited = 0; waited < ms; waited += 20) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (file != nullptr) {
      int a = 0;
      int b = 0;
      const int n = std::fscanf(file, "%d %d", &a, &b);
      std::fclose(file);
      if (n == 2 && a > 1) {
        leader = static_cast<pid_t>(a);
        (void)b;
        return true;
      }
    }
    poll(nullptr, 0, 20);
  }
  return false;
}

static int run_helper(const char* helper, const std::string& apt, const std::string& pidfile,
                      const char* timeout_sec, int* cancel_write, pid_t& helper_pid,
                      const char* script = nullptr, const char* countfile = nullptr,
                      int* stdout_read = nullptr, const char* command = "simulate")
{
  int fds[2] = {-1, -1};
  if (cancel_write != nullptr) {
    if (pipe(fds) != 0)
      return -1;
  }
  int out_pipe[2] = {-1, -1};
  if (stdout_read != nullptr) {
    if (pipe(out_pipe) != 0)
      return -1;
  }
  const pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    setenv("LCOS_UPDATES_APT_GET", apt.c_str(), 1);
    setenv("LCOS_STUB_PIDFILE", pidfile.c_str(), 1);
    if (script != nullptr)
      setenv("LCOS_STUB_SCRIPT", script, 1);
    else
      unsetenv("LCOS_STUB_SCRIPT");
    if (countfile != nullptr)
      setenv("LCOS_STUB_COUNTFILE", countfile, 1);
    else
      unsetenv("LCOS_STUB_COUNTFILE");
    if (timeout_sec != nullptr)
      setenv("LCOS_UPDATES_TIMEOUT_SEC", timeout_sec, 1);
    else
      unsetenv("LCOS_UPDATES_TIMEOUT_SEC");
    if (stdout_read != nullptr) {
      dup2(out_pipe[1], STDOUT_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
    }
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
    execl(helper, helper, command, static_cast<char*>(nullptr));
    _exit(127);
  }
  if (cancel_write != nullptr) {
    close(fds[0]);
    *cancel_write = fds[1];
  }
  if (stdout_read != nullptr) {
    close(out_pipe[1]);
    *stdout_read = out_pipe[0];
  }
  helper_pid = pid;
  return 0;
}

static std::string read_all_fd(int fd)
{
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  std::string out;
  char buf[1024];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  return out;
}

/* dpkg must not keep apt's stdout open, or the helper never sees EOF. */
static void detach_stdio()
{
  const int devnull = open("/dev/null", O_RDWR);
  if (devnull < 0)
    return;
  dup2(devnull, STDIN_FILENO);
  dup2(devnull, STDOUT_FILENO);
  dup2(devnull, STDERR_FILENO);
  if (devnull > 2)
    close(devnull);
}

static int read_count(const std::string& path)
{
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr)
    return -1;
  int n = -1;
  if (std::fscanf(file, "%d", &n) != 1)
    n = -1;
  std::fclose(file);
  return n;
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

  {
    const std::string pidfile = std::string(dir) + "/keep-dpkg.pids";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    if (run_helper(helper, apt, pidfile, nullptr, &cancel_write, helper_pid, "keep-dpkg", nullptr,
                   &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for keep-dpkg\n");
      return 1;
    }
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(wait_for_pids(pidfile, leader, dpkg, 2000), "keep-dpkg stub started dpkg");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after cancel once dpkg is already running");
    expect(!dead(dpkg), "dpkg is still alive after cancel");
    expect(out.find("cancelled") != std::string::npos, "cancel while configuring reports cancel");
    expect(out.find("dpkg --configure -a") == std::string::npos,
           "a clean skip of dpkg is not the recovery message");
    if (!dead(dpkg))
      kill(dpkg, SIGKILL);
    if (!dead(leader))
      kill(leader, SIGKILL);
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string pidfile = std::string(dir) + "/dpkg-on-term.pids";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    if (run_helper(helper, apt, pidfile, nullptr, &cancel_write, helper_pid, "dpkg-on-term", nullptr,
                   &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for dpkg-on-term\n");
      return 1;
    }
    pid_t leader = 0;
    expect(wait_for_leader(pidfile, leader, 2000), "dpkg-on-term stub is running");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    pid_t dpkg = 0;
    const bool have = read_pids(pidfile, leader, dpkg);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits when a kill already hit a tree that grew dpkg");
    expect(have && dpkg > 1, "dpkg pid was recorded after SIGTERM");
    expect(!dead(dpkg), "dpkg survived the kill");
    expect(!dead(leader), "apt was not SIGKILLed after dpkg appeared");
    expect(out.find("dpkg --configure -a") != std::string::npos,
           "interrupted dpkg tells the user to run dpkg --configure -a");
    if (!dead(dpkg))
      kill(dpkg, SIGKILL);
    if (!dead(leader))
      kill(leader, SIGKILL);
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/partial.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/partial.pids", nullptr, nullptr, helper_pid,
                   "partial-update", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for partial update\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a partial index update");
    expect(read_count(countfile) == 2, "partial index failure still runs the simulation");
    expect(out.find("STATUS upgrades\n") != std::string::npos, "partial update still reports upgrades");
    expect(out.find("PKG foo 1 2\n") != std::string::npos, "partial update keeps the package");
    expect(out.find("404 Not Found") != std::string::npos, "partial update shows the apt warning");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/hard.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/hard.pids", nullptr, nullptr, helper_pid,
                   "hard-update-fail", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for hard update failure\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a hard update failure");
    expect(read_count(countfile) == 1, "a lock failure does not continue to simulate");
    expect(out.find("STATUS error\n") != std::string::npos, "hard update failure is an error");
    expect(out.find("Could not get lock") != std::string::npos, "hard update failure keeps the apt line");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/upgrade.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/upgrade.pids", nullptr, nullptr, helper_pid,
                   "count-only", countfile.c_str(), &stdout_read, "upgrade") != 0) {
      std::fprintf(stderr, "failed to spawn helper for upgrade update\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after upgrade");
    expect(read_count(countfile) == 2, "upgrade refreshes package lists before installing");
    expect(out.find("STATUS success\n") != std::string::npos, "clean upgrade reports success");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/kept.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/kept.pids", nullptr, nullptr, helper_pid,
                   "kept-back-upgrade", countfile.c_str(), &stdout_read, "upgrade") != 0) {
      std::fprintf(stderr, "failed to spawn helper for kept-back upgrade\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a kept-back upgrade");
    expect(out.find("STATUS kept-back\n") != std::string::npos, "kept-back upgrade is not success");
    expect(out.find("STATUS success\n") == std::string::npos, "kept-back upgrade is not up to date");
    expect(out.find("KEPT linux-image-amd64\n") != std::string::npos, "kept-back upgrade names the package");
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
