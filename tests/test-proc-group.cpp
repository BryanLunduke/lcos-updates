/* Process-tree stop: SIGTERM, short wait, SIGKILL of every descendant group.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "proc-group.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

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
  if (kill(pid, 0) == 0)
    return false;
  return errno == ESRCH;
}

/* A SIGKILL'd child is often a zombie until init reaps it. That is not running. */
static bool not_running(pid_t pid)
{
  if (pid <= 1)
    return false;
  if (kill(pid, 0) < 0 && errno == ESRCH)
    return true;
  char path[64];
  std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
  FILE* file = std::fopen(path, "r");
  if (file == nullptr)
    return true;
  char buf[1024];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, file);
  std::fclose(file);
  if (n == 0)
    return true;
  buf[n] = '\0';
  char* rparen = std::strrchr(buf, ')');
  if (rparen == nullptr)
    return false;
  char state = '?';
  if (std::sscanf(rparen + 1, " %c", &state) != 1)
    return false;
  return state == 'Z' || state == 'X';
}

static long mono_ms()
{
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int ready_fd_from_args(int argc, char** argv)
{
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--ready-fd") == 0 && i + 1 < argc) {
      int fd = 0;
      for (const char* p = argv[i + 1]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
          return -1;
        fd = fd * 10 + (*p - '0');
      }
      return fd;
    }
  }
  return -1;
}

static int hold_main(int argc, char** argv)
{
  prctl(PR_SET_NAME, "dpkg", 0, 0, 0);
  bool exit_now = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--exit-now") == 0)
      exit_now = true;
  }
  if (exit_now)
    _exit(0);
  /* Do not use fd 3. Under meson that descriptor is already open, and a
   * dup2 onto it does not survive as a writable pipe. */
  const int ready_fd = ready_fd_from_args(argc, argv);
  if (ready_fd >= 0) {
    char ok = 1;
    if (write(ready_fd, &ok, 1) < 0)
      _exit(1);
    close(ready_fd);
  }
  signal(SIGTERM, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  for (;;)
    pause();
  return 0;
}

static void cleanup(pid_t leader, pid_t child)
{
  if (child > 1 && !dead(child))
    kill(child, SIGKILL);
  if (leader > 1 && !dead(leader))
    kill(leader, SIGKILL);
  if (leader > 1) {
    int status = 0;
    waitpid(leader, &status, 0);
  }
}

/* Leader ignores SIGTERM. Child execs this binary as argv0 with --hold and
 * the given dpkg arguments so /proc/pid/cmdline is what the checker reads. */
static bool spawn_dpkg_tree(const char* self, const char* argv0, const std::vector<const char*>& args,
                            pid_t& leader, pid_t& child)
{
  int fds[2];
  if (pipe(fds) != 0)
    return false;
  leader = fork();
  if (leader < 0)
    return false;
  if (leader == 0) {
    setpgid(0, 0);
    signal(SIGTERM, SIG_IGN);
    int ready[2];
    if (pipe(ready) != 0)
      _exit(1);
    const pid_t dpkg = fork();
    if (dpkg == 0) {
      setpgid(0, 0);
      fcntl(ready[1], F_SETFD, 0);
      char fdnum[16];
      std::snprintf(fdnum, sizeof fdnum, "%d", ready[1]);
      close(ready[0]);
      close(fds[0]);
      close(fds[1]);
      std::vector<char*> av;
      av.push_back(const_cast<char*>(argv0));
      av.push_back(const_cast<char*>("--hold"));
      av.push_back(const_cast<char*>("--ready-fd"));
      av.push_back(fdnum);
      for (const char* arg : args)
        av.push_back(const_cast<char*>(arg));
      av.push_back(nullptr);
      execv(self, av.data());
      _exit(127);
    }
    setpgid(dpkg, dpkg);
    close(ready[1]);
    char ok = 0;
    if (read(ready[0], &ok, 1) != 1)
      _exit(1);
    close(ready[0]);
    if (write(fds[1], &dpkg, sizeof dpkg) != static_cast<ssize_t>(sizeof dpkg))
      _exit(1);
    for (;;)
      pause();
  }
  setpgid(leader, leader);
  close(fds[1]);
  pid_t dpkg = 0;
  const ssize_t n = read(fds[0], &dpkg, sizeof dpkg);
  close(fds[0]);
  if (n != static_cast<ssize_t>(sizeof dpkg) || dpkg <= 1)
    return false;
  child = dpkg;
  return true;
}

int main(int argc, char** argv)
{
  if (argc >= 2 && std::strcmp(argv[1], "--hold") == 0)
    return hold_main(argc, argv);

  char self_path[4096];
  const ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
  if (self_len <= 0) {
    std::perror("readlink");
    return 1;
  }
  self_path[self_len] = '\0';

  {
    int fds[2];
    if (pipe(fds) != 0) {
      std::perror("pipe");
      return 1;
    }
    const pid_t child = fork();
    if (child < 0) {
      std::perror("fork");
      return 1;
    }
    if (child == 0) {
      setpgid(0, 0);
      signal(SIGTERM, SIG_IGN);
      const pid_t grand = fork();
      if (grand == 0) {
        /* Separate group, like apt-get's dpkg after setpgid. Ignore SIGTERM. */
        setpgid(0, 0);
        signal(SIGTERM, SIG_IGN);
        for (;;)
          pause();
      }
      setpgid(grand, grand);
      if (write(fds[1], &grand, sizeof grand) < 0)
        _exit(1);
      for (;;)
        pause();
    }
    setpgid(child, child);
    close(fds[1]);
    pid_t grand = 0;
    if (read(fds[0], &grand, sizeof grand) != static_cast<ssize_t>(sizeof grand)) {
      std::fprintf(stderr, "did not receive grandchild pid\n");
      return 1;
    }
    close(fds[0]);
    const bool gone = terminate_process_tree(child, 200);
    expect(gone, "stubborn tree reports gone");
    expect(dead(child), "group leader is dead");
    expect(dead(grand), "grandchild in its own group is dead");
    int status = 0;
    waitpid(child, &status, WNOHANG);
  }

  {
    const pid_t child = fork();
    if (child == 0) {
      setpgid(0, 0);
      _exit(0); /* exits as soon as SIGTERM is delivered; also exits immediately anyway */
    }
    setpgid(child, child);
    /* Child has already exited or will on SIGTERM. A long grace must not be spent. */
    const long start = mono_ms();
    const bool gone = terminate_process_tree(child, 5000);
    const long elapsed = mono_ms() - start;
    expect(gone, "exited child reports gone");
    expect(elapsed < 1500, "do not wait out the grace after the process is gone");
    int status = 0;
    waitpid(child, &status, WNOHANG);
  }

  {
    /* term_grace_ms == 0 must not spend the 20x50ms SIGKILL sleeps. */
    const pid_t child = fork();
    if (child == 0) {
      setpgid(0, 0);
      signal(SIGTERM, SIG_IGN);
      for (;;)
        pause();
    }
    setpgid(child, child);
    const long start = mono_ms();
    const bool gone = terminate_process_tree(child, 0);
    const long elapsed = mono_ms() - start;
    expect(elapsed < 400, "zero grace does not sleep");
    for (int i = 0; i < 25 && !dead(child); ++i)
      poll(nullptr, 0, 20);
    int status = 0;
    /* A zombie still answers kill(pid, 0). Reaping it counts as dead. */
    const pid_t reaped = waitpid(child, &status, WNOHANG);
    expect(gone || reaped == child || dead(child), "zero grace still SIGKILLs");
    if (!dead(child))
      kill(child, SIGKILL);
  }

  {
    const unsigned long long self_start = proc_starttime(getpid());
    expect(self_start != 0, "starttime of a live process is non-zero");
    expect(proc_starttime(getpid()) == self_start, "starttime does not change");
    const pid_t exited = fork();
    if (exited == 0)
      _exit(0);
    int status = 0;
    waitpid(exited, &status, 0);
    expect(proc_starttime(exited) == 0, "a reaped process has no starttime");
  }

  {
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(spawn_dpkg_tree(self_path, "dpkg", {"--configure", "pkg"}, leader, dpkg),
           "configuring dpkg tree started");
    const long start = mono_ms();
    const bool gone = terminate_process_tree(leader, 5000);
    const long elapsed = mono_ms() - start;
    expect(!gone, "configuring dpkg tree is not reported gone");
    expect(elapsed < 1500, "configuring dpkg tree does not wait out the grace");
    expect(!dead(leader), "apt parent was not signaled");
    expect(!dead(dpkg), "dpkg --configure was not signaled");
    cleanup(leader, dpkg);
  }

  for (const char* flag : {"--unpack", "--install", "--remove", "--triggers-only"}) {
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(spawn_dpkg_tree(self_path, "dpkg", {flag, "pkg"}, leader, dpkg), "configure-family tree started");
    const bool gone = terminate_process_tree(leader, 5000);
    expect(!gone, "configure-family dpkg is not signaled");
    expect(!dead(dpkg), "configure-family child stayed alive");
    cleanup(leader, dpkg);
  }

  for (const char* flag : {"--print-foreign-architectures", "--print-architecture",
                           "--assert-multi-arch", "--compare-versions"}) {
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(spawn_dpkg_tree(self_path, "dpkg", {flag}, leader, dpkg), "query dpkg tree started");
    /* Grace 0 sends SIGKILL and returns without waiting for the pid to vanish. */
    terminate_process_tree(leader, 0);
    for (int i = 0; i < 25 && (!dead(leader) || !dead(dpkg)); ++i)
      poll(nullptr, 0, 20);
    int status = 0;
    const pid_t reaped = waitpid(leader, &status, WNOHANG);
    expect(not_running(dpkg), "print/query dpkg was killed");
    expect(not_running(leader) || reaped == leader, "apt parent of a query dpkg was killed");
    cleanup(leader, dpkg);
  }

  {
    /* comm is dpkg and argv contains --configure, but argv0 is dpkg-query. */
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(spawn_dpkg_tree(self_path, "dpkg-query", {"--configure"}, leader, dpkg),
           "dpkg-query tree started");
    terminate_process_tree(leader, 0);
    for (int i = 0; i < 25 && !dead(dpkg); ++i)
      poll(nullptr, 0, 20);
    expect(not_running(dpkg), "dpkg-query was killed");
    cleanup(leader, dpkg);
  }

  {
    /* Zombie whose cmdline contains --configure must not block the kill. */
    int fds[2];
    if (pipe(fds) != 0)
      return 1;
    const pid_t leader = fork();
    if (leader == 0) {
      setpgid(0, 0);
      signal(SIGTERM, SIG_IGN);
      const pid_t zombie = fork();
      if (zombie == 0) {
        std::vector<char*> av;
        av.push_back(const_cast<char*>("dpkg"));
        av.push_back(const_cast<char*>("--hold"));
        av.push_back(const_cast<char*>("--configure"));
        av.push_back(const_cast<char*>("--exit-now"));
        av.push_back(nullptr);
        execv(self_path, av.data());
        _exit(0);
      }
      poll(nullptr, 0, 100);
      if (write(fds[1], &zombie, sizeof zombie) < 0)
        _exit(1);
      for (;;)
        pause();
    }
    setpgid(leader, leader);
    close(fds[1]);
    pid_t zombie = 0;
    if (read(fds[0], &zombie, sizeof zombie) != static_cast<ssize_t>(sizeof zombie))
      return 1;
    close(fds[0]);
    terminate_process_tree(leader, 0);
    for (int i = 0; i < 25 && !dead(leader); ++i)
      poll(nullptr, 0, 20);
    int status = 0;
    const pid_t reaped = waitpid(leader, &status, WNOHANG);
    expect(not_running(leader) || reaped == leader, "parent of a dpkg zombie was killed");
    cleanup(leader, zombie);
  }

  {
    /* A process that shares apt's group but is not a descendant must survive.
     * kill(-pgid) would have taken it with the leader. */
    int fds[2];
    if (pipe(fds) != 0)
      return 1;
    const pid_t leader = fork();
    if (leader == 0) {
      setpgid(0, 0);
      signal(SIGTERM, SIG_IGN);
      const pid_t grand = fork();
      if (grand == 0) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_IGN);
        for (;;)
          pause();
      }
      setpgid(grand, grand);
      if (write(fds[1], &grand, sizeof grand) < 0)
        _exit(1);
      for (;;)
        pause();
    }
    setpgid(leader, leader);
    close(fds[1]);
    pid_t grand = 0;
    if (read(fds[0], &grand, sizeof grand) != static_cast<ssize_t>(sizeof grand))
      return 1;
    close(fds[0]);
    int okpipe[2];
    if (pipe(okpipe) != 0)
      return 1;
    const pid_t sibling = fork();
    if (sibling == 0) {
      close(okpipe[0]);
      const char status = setpgid(0, leader) == 0 ? 1 : 0;
      if (write(okpipe[1], &status, 1) < 0)
        _exit(1);
      if (status == 0)
        _exit(2);
      signal(SIGTERM, SIG_IGN);
      for (;;)
        pause();
    }
    close(okpipe[1]);
    char joined = 0;
    if (read(okpipe[0], &joined, 1) != 1)
      return 1;
    close(okpipe[0]);
    expect(joined == 1, "sibling joined the apt process group");
    const bool gone = terminate_process_tree(leader, 200);
    expect(gone, "descendant tree reports gone");
    expect(dead(leader) || waitpid(leader, nullptr, WNOHANG) == leader, "leader is dead");
    expect(dead(grand), "descendant is dead");
    expect(!dead(sibling), "unrelated process in the apt group was not killed");
    if (!dead(sibling))
      kill(sibling, SIGKILL);
    int status = 0;
    waitpid(sibling, &status, 0);
    waitpid(leader, &status, WNOHANG);
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all proc-group tests passed\n");
  return 0;
}
