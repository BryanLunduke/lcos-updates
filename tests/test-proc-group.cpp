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
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

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

static long mono_ms()
{
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main()
{
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

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all proc-group tests passed\n");
  return 0;
}
