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
#include <vector>
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

static std::string self_exe()
{
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0)
    return {};
  buf[n] = '\0';
  return buf;
}

static int hold_mode(int argc, char** argv)
{
  prctl(PR_SET_NAME, "dpkg", 0, 0, 0);
  bool exit_now = false;
  int ready_fd = -1;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--exit-now") == 0)
      exit_now = true;
    if (std::strcmp(argv[i], "--ready-fd") == 0 && i + 1 < argc) {
      ready_fd = 0;
      for (const char* p = argv[++i]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
          ready_fd = -1;
          break;
        }
        ready_fd = ready_fd * 10 + (*p - '0');
      }
    }
  }
  if (exit_now)
    _exit(0);
  if (ready_fd >= 0) {
    char ok = 1;
    if (write(ready_fd, &ok, 1) < 0)
      _exit(1);
    close(ready_fd);
  }
  /* Do not hold the apt pipes open, or the helper never sees EOF. */
  detach_stdio();
  signal(SIGTERM, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  for (;;)
    pause();
  return 0;
}

/* Exec this binary so cmdline contains the dpkg arguments under test. */
static pid_t spawn_hold(const std::vector<const char*>& args, bool wait_ready)
{
  const std::string self = self_exe();
  int ready[2] = {-1, -1};
  if (wait_ready && pipe(ready) != 0)
    return -1;
  const pid_t child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    char fdnum[16];
    fdnum[0] = '\0';
    if (wait_ready) {
      fcntl(ready[1], F_SETFD, 0);
      std::snprintf(fdnum, sizeof fdnum, "%d", ready[1]);
      close(ready[0]);
    }
    std::vector<char*> av;
    av.push_back(const_cast<char*>("dpkg"));
    av.push_back(const_cast<char*>("--hold"));
    if (wait_ready) {
      av.push_back(const_cast<char*>("--ready-fd"));
      av.push_back(fdnum);
    }
    for (const char* arg : args)
      av.push_back(const_cast<char*>(arg));
    av.push_back(nullptr);
    execv(self.c_str(), av.data());
    _exit(127);
  }
  if (wait_ready) {
    close(ready[1]);
    char ok = 0;
    if (read(ready[0], &ok, 1) != 1) {
      close(ready[0]);
      return -1;
    }
    close(ready[0]);
  }
  return child;
}

static void record_argv(int argc, char** argv)
{
  const char* path = std::getenv("LCOS_STUB_ARGVFILE");
  if (path == nullptr || path[0] == '\0')
    return;
  FILE* file = std::fopen(path, "a");
  if (file == nullptr)
    return;
  for (int i = 0; i < argc; ++i) {
    if (argv[i] == nullptr)
      break;
    for (const char* p = argv[i]; *p != '\0'; ++p) {
      if (*p == '\n' || *p == '\r')
        std::fputc(' ', file);
      else
        std::fputc(*p, file);
    }
    std::fputc('\n', file);
  }
  std::fputs("---\n", file);
  std::fclose(file);
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
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
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
  if (std::strcmp(script, "keep-dpkg") == 0) {
    signal(SIGTERM, SIG_IGN);
    setpgid(0, 0);
    const pid_t child = spawn_hold({"--configure", "pkg"}, true);
    if (child <= 1)
      return 1;
    setpgid(child, child);
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
    const pid_t child = spawn_hold({"--configure", "pkg"}, true);
    if (child <= 1)
      _exit(1);
    if (path != nullptr) {
      FILE* file = std::fopen(path, "w");
      if (file != nullptr) {
        std::fprintf(file, "%d %d\n", static_cast<int>(getpid()), static_cast<int>(child));
        std::fclose(file);
      }
    }
    signal(SIGTERM, SIG_IGN);
    /* Stay alive long enough that an early helper return is visible, then
     * exit so the helper can waitpid and report recovery. */
    poll(nullptr, 0, 1500);
    _exit(0);
  }
  if (std::strncmp(script, "print-dpkg:", 11) == 0) {
    signal(SIGTERM, SIG_IGN);
    setpgid(0, 0);
    const char* flag = script + 11;
    const pid_t child = spawn_hold({flag}, true);
    if (child <= 1)
      return 1;
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
  if (std::strcmp(script, "zombie-dpkg") == 0) {
    signal(SIGTERM, SIG_IGN);
    setpgid(0, 0);
    const pid_t child = spawn_hold({"--configure", "--exit-now"}, false);
    if (child <= 1)
      return 1;
    poll(nullptr, 0, 150);
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
  if (std::strcmp(script, "env-check") == 0) {
    const char* path = std::getenv("LCOS_STUB_ENVFILE");
    FILE* file = path != nullptr ? std::fopen(path, "a") : nullptr;
    if (file == nullptr)
      return 1;
    auto dump = [&](const char* key) {
      const char* value = std::getenv(key);
      std::fprintf(file, "%s=[%s]\n", key, value != nullptr ? value : "");
    };
    dump("APT_CONFIG");
    dump("LCOS_CANARY");
    dump("PATH");
    dump("LANG");
    dump("LC_ALL");
    dump("DEBIAN_FRONTEND");
    dump("HOME");
    dump("http_proxy");
    dump("DPKG_FORCE");
    std::fputs("---\n", file);
    std::fclose(file);
    return 0;
  }
  if (std::strcmp(script, "partial-then-lock") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      std::fputs(
          "E: Failed to fetch http://deb.example/InRelease 404 Not Found\n"
          "E: Some index files failed to download. They have been ignored, or old ones used instead.\n",
          stderr);
      return 100;
    }
    std::fputs("E: Could not get lock /var/lib/apt/lists/lock\n", stderr);
    return 100;
  }
  if (std::strcmp(script, "pin-upgrade") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "changed-set") == 0) {
    if (n == 1)
      return 0;
    std::fputs("Inst libc6 [1] (9 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "conffile-upgrade") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("Configuration file '/etc/ssh/sshd_config'\n"
               " ==> Modified (by you or by a script) since installation.\n"
               " ==> Package distributor has shipped an updated version.\n"
               " ==> Keeping old config file as default.\n"
               "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "dead-mirror") == 0) {
    std::fputs("W: Failed to fetch http://deb.example/InRelease  Connection timed out\n"
               "E: Some index files failed to download. They have been ignored, or old ones used instead.\n",
               stderr);
    return 0;
  }
  if (std::strcmp(script, "resolve-fail") == 0) {
    std::fputs("Err:1 http://deb.example stable InRelease\n"
               "  Temporary failure resolving 'deb.example'\n",
               stderr);
    return 0;
  }
  if (std::strcmp(script, "phasing") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    std::fputs("The following upgrades have been deferred due to phasing:\n"
               "  shim-signed grub-efi-amd64-signed\n"
               "Some packages may have been kept back due to phasing.\n"
               "0 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "held-packages") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    std::fputs("The following packages have been kept back:\n"
               "  linux-image-amd64 libc6\n"
               "0 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "arch-pin") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libfoo:i386 [1.0] (1.2-3 Debian:12 [i386])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "long-kept") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("The following packages have been kept back:\n"
               "  linux-image-amd64\n"
               "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
               stdout);
    std::fflush(stdout);
    std::string junk(1300 * 1024, 'x');
    for (std::size_t i = 80; i < junk.size(); i += 80)
      junk[i] = '\n';
    std::fwrite(junk.data(), 1, junk.size(), stdout);
    return 0;
  }
  if (std::strcmp(script, "no-summary") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("Reading package lists...\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "trickle") == 0) {
    if (n == 1) {
      for (int i = 0; i < 6; ++i) {
        std::fprintf(stdout, "Hit:%d http://deb.example stable InRelease\n", i + 1);
        std::fflush(stdout);
        poll(nullptr, 0, 400);
      }
      return 0;
    }
    std::fputs("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "chatter") == 0) {
    for (int i = 0; i < 400; ++i) {
      std::fprintf(stdout, "Get:%d http://deb.example/Packages\n", i + 1);
      std::fflush(stdout);
      poll(nullptr, 0, 50);
    }
    return 0;
  }
  if (std::strcmp(script, "hang-before") == 0) {
    const char* path = std::getenv("LCOS_STUB_STATEFILE");
    if (path != nullptr) {
      FILE* file = std::fopen(path, "a");
      if (file != nullptr) {
        std::fputs("started\n", file);
        std::fclose(file);
      }
    }
    for (;;)
      pause();
  }
  if (std::strcmp(script, "finish-after-dpkg") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    setpgid(0, 0);
    const pid_t child = spawn_hold({"--configure", "pkg"}, true);
    if (child <= 1)
      return 1;
    setpgid(child, child);
    const char* path = std::getenv("LCOS_STUB_STATEFILE");
    auto note = [&](const char* text) {
      if (path == nullptr)
        return;
      FILE* file = std::fopen(path, "a");
      if (file == nullptr)
        return;
      std::fputs(text, file);
      std::fclose(file);
    };
    note("after-dpkg\n");
    poll(nullptr, 0, 4000);
    note("finished\n");
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    if (child > 1)
      kill(child, SIGKILL);
    return 0;
  }
  if (std::strcmp(script, "kept-back-upgrade") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  linux-image-amd64\n"
                 "Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("The following packages have been kept back:\n"
               "  linux-image-amd64\n"
               "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
               stdout);
    return 0;
  }
  return 2;
}

/* Exec'd as fake-apt-get. Ignores SIGTERM and leaves a grandchild that does too. */
static int apt_stub(int argc, char** argv)
{
  record_argv(argc, argv);
  const char* script = std::getenv("LCOS_STUB_SCRIPT");
  if (script != nullptr && script[0] != '\0')
    return apt_script(script);
  signal(SIGTERM, SIG_IGN);
  setpgid(0, 0);
  const pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0);
    signal(SIGTERM, SIG_IGN);
    detach_stdio();
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
                      int* stdout_read = nullptr, const char* command = "simulate",
                      const std::vector<std::string>* extra = nullptr, int* stderr_read = nullptr,
                      const char* argvfile = nullptr, const char* envfile = nullptr,
                      const char* hard_cap = nullptr, const char* statefile = nullptr,
                      const char* reboot_file = nullptr, const char* dpkg_status = nullptr)
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
  int err_pipe[2] = {-1, -1};
  if (stderr_read != nullptr) {
    if (pipe(err_pipe) != 0)
      return -1;
  }
  const pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    setenv("LCOS_UPDATES_APT_GET", apt.c_str(), 1);
    setenv("LCOS_STUB_PIDFILE", pidfile.c_str(), 1);
    setenv("APT_CONFIG", "/no/such/apt.conf", 1);
    setenv("LCOS_CANARY", "should-not-leak", 1);
    setenv("http_proxy", "http://127.0.0.1:9", 1);
    setenv("DPKG_FORCE", "confnew", 1);
    if (script != nullptr)
      setenv("LCOS_STUB_SCRIPT", script, 1);
    else
      unsetenv("LCOS_STUB_SCRIPT");
    if (countfile != nullptr)
      setenv("LCOS_STUB_COUNTFILE", countfile, 1);
    else
      unsetenv("LCOS_STUB_COUNTFILE");
    if (argvfile != nullptr)
      setenv("LCOS_STUB_ARGVFILE", argvfile, 1);
    else
      unsetenv("LCOS_STUB_ARGVFILE");
    if (envfile != nullptr)
      setenv("LCOS_STUB_ENVFILE", envfile, 1);
    else
      unsetenv("LCOS_STUB_ENVFILE");
    if (timeout_sec != nullptr)
      setenv("LCOS_UPDATES_TIMEOUT_SEC", timeout_sec, 1);
    else
      unsetenv("LCOS_UPDATES_TIMEOUT_SEC");
    if (hard_cap != nullptr)
      setenv("LCOS_UPDATES_HARD_CAP_SEC", hard_cap, 1);
    else
      unsetenv("LCOS_UPDATES_HARD_CAP_SEC");
    if (statefile != nullptr)
      setenv("LCOS_STUB_STATEFILE", statefile, 1);
    else
      unsetenv("LCOS_STUB_STATEFILE");
    if (reboot_file != nullptr)
      setenv("LCOS_UPDATES_REBOOT_FILE", reboot_file, 1);
    else
      unsetenv("LCOS_UPDATES_REBOOT_FILE");
    if (dpkg_status != nullptr)
      setenv("LCOS_UPDATES_DPKG_STATUS", dpkg_status, 1);
    else
      unsetenv("LCOS_UPDATES_DPKG_STATUS");
    if (stdout_read != nullptr) {
      dup2(out_pipe[1], STDOUT_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
    }
    if (stderr_read != nullptr) {
      dup2(err_pipe[1], STDERR_FILENO);
      close(err_pipe[0]);
      close(err_pipe[1]);
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
    std::vector<char*> av;
    av.push_back(const_cast<char*>(helper));
    av.push_back(const_cast<char*>(command));
    if (extra != nullptr) {
      for (const std::string& arg : *extra)
        av.push_back(const_cast<char*>(arg.c_str()));
    }
    av.push_back(nullptr);
    execv(helper, av.data());
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
  if (stderr_read != nullptr) {
    close(err_pipe[1]);
    *stderr_read = err_pipe[0];
  }
  helper_pid = pid;
  return 0;
}

static std::string read_file_all(const std::string& path)
{
  FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr)
    return {};
  std::string out;
  char buf[1024];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, file)) > 0)
    out.append(buf, n);
  std::fclose(file);
  return out;
}

static std::string read_until(int fd, const char* needle, int ms)
{
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  std::string out;
  for (int waited = 0; waited < ms; waited += 20) {
    char buf[1024];
    for (;;) {
      const ssize_t n = read(fd, buf, sizeof buf);
      if (n > 0) {
        out.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      break;
    }
    if (needle != nullptr && out.find(needle) != std::string::npos)
      return out;
    poll(nullptr, 0, 20);
  }
  return out;
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
  if (argc >= 2 && std::strcmp(argv[1], "--hold") == 0)
    return hold_mode(argc, argv);
  if (argc >= 1 && basename_of(argv[0]) == "fake-apt-get")
    return apt_stub(argc, argv);
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
    pid_t dpkg = 0;
    expect(wait_for_pids(pidfile, leader, dpkg, 4000), "dpkg pid was recorded after SIGTERM");
    poll(nullptr, 0, 200);
    expect(!dead(helper_pid), "helper stays up while apt is still running");
    expect(!dead(leader), "apt was not SIGKILLed after dpkg appeared");
    expect(!dead(dpkg), "dpkg survived the kill");
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits only after apt has exited");
    expect(dead(leader), "apt has exited before the helper returns");
    expect(!dead(dpkg), "dpkg is still alive after the helper exits");
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
    const std::string argvfile = std::string(dir) + "/upgrade.argv";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/upgrade.pids", nullptr, nullptr, helper_pid,
                   "pin-upgrade", countfile.c_str(), &stdout_read, "upgrade", &pins, nullptr,
                   argvfile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for upgrade update\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    const std::string argv_text = read_file_all(argvfile);
    expect(exited, "helper exits after upgrade");
    expect(read_count(countfile) == 3, "upgrade refreshes, re-simulates, then installs the pins");
    expect(out.find("STATUS success\n") != std::string::npos, "clean upgrade reports success");
    expect(argv_text.find("\nupdate\n") != std::string::npos, "upgrade argv includes update");
    expect(argv_text.find("\n-s\n") != std::string::npos && argv_text.find("\nupgrade\n") != std::string::npos,
           "upgrade re-simulates with upgrade");
    expect(argv_text.find("\n-y\n") != std::string::npos, "install argv includes -y");
    expect(argv_text.find("\ninstall\n") != std::string::npos, "install uses install");
    expect(argv_text.find("\n--only-upgrade\n") != std::string::npos, "install is only-upgrade");
    expect(argv_text.find("\nlibc6=2\n") != std::string::npos, "install argv includes the reviewed pin");
    expect(argv_text.find("Dpkg::Options::=--force-confdef") != std::string::npos,
           "install keeps --force-confdef");
    expect(argv_text.find("Dpkg::Options::=--force-confold") != std::string::npos,
           "install keeps --force-confold");
    expect(argv_text.find("dist-upgrade") == std::string::npos, "install argv has no dist-upgrade");
    expect(argv_text.find("full-upgrade") == std::string::npos, "install argv has no full-upgrade");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/changed.count";
    const std::string argvfile = std::string(dir) + "/changed.argv";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/changed.pids", nullptr, nullptr, helper_pid,
                   "changed-set", countfile.c_str(), &stdout_read, "upgrade", &pins, nullptr,
                   argvfile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for changed set\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    const std::string argv_text = read_file_all(argvfile);
    expect(exited, "helper exits when the reviewed set changed");
    expect(read_count(countfile) == 2, "a changed set does not run install");
    expect(out.find("SKIPPED\n") != std::string::npos, "changed set sets SKIPPED");
    expect(out.find("PKG libc6 1 9\n") != std::string::npos, "changed set returns the new package");
    expect(out.find("Nothing was installed") != std::string::npos, "changed set says nothing was installed");
    expect(argv_text.find("\ninstall\n") == std::string::npos, "changed set does not exec install");
    expect(argv_text.find("dist-upgrade") == std::string::npos, "changed set has no dist-upgrade");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/kept.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/kept.pids", nullptr, nullptr, helper_pid,
                   "kept-back-upgrade", countfile.c_str(), &stdout_read, "upgrade", &pins) != 0) {
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

  {
    const std::string countfile = std::string(dir) + "/conf.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/conf.pids", nullptr, nullptr, helper_pid,
                   "conffile-upgrade", countfile.c_str(), &stdout_read, "upgrade", &pins) != 0) {
      std::fprintf(stderr, "failed to spawn helper for conffile upgrade\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a kept conffile");
    expect(out.find("STATUS success\n") != std::string::npos, "kept conffile is still a finished install");
    expect(out.find("CONFKEPT /etc/ssh/sshd_config\n") != std::string::npos, "kept conffile is named");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/lockmsg.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/lockmsg.pids", nullptr, nullptr, helper_pid,
                   "partial-then-lock", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for partial-then-lock\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after partial update plus a later failure");
    expect(out.find("MSG E: Failed to fetch http://deb.example/InRelease 404 Not Found\n") !=
               std::string::npos,
           "partial warning is an MSG line");
    expect(out.find("MSG E: Could not get lock /var/lib/apt/lists/lock\n") != std::string::npos,
           "later lock failure is its own MSG line");
    expect(out.find("\nE: Could not get lock") == std::string::npos,
           "lock failure is not dropped as a raw continuation");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  for (const char* flag : {"--print-foreign-architectures", "--print-architecture"}) {
    const std::string script = std::string("print-dpkg:") + flag;
    const std::string pidfile = std::string(dir) + "/print-" + flag + ".pids";
    pid_t helper_pid = 0;
    int stderr_read = -1;
    if (run_helper(helper, apt, pidfile, "2", nullptr, helper_pid, script.c_str(), nullptr, nullptr,
                   "simulate", nullptr, &stderr_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for %s\n", flag);
      return 1;
    }
    pid_t leader = 0;
    pid_t dpkg = 0;
    expect(wait_for_pids(pidfile, leader, dpkg, 2000), "print-architecture stub started dpkg");
    int status = 0;
    const bool exited = wait_pid(helper_pid, 12000, status);
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits on the deadline when dpkg is only a query");
    expect(err.find("DPKG_STARTED") == std::string::npos, "query dpkg does not emit DPKG_STARTED");
    expect(dead(leader), "query apt parent is dead after the deadline");
    expect(dead(dpkg), "query dpkg is dead after the deadline");
    if (!dead(dpkg))
      kill(dpkg, SIGKILL);
    if (!dead(leader))
      kill(leader, SIGKILL);
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string pidfile = std::string(dir) + "/zombie.pids";
    pid_t helper_pid = 0;
    int stderr_read = -1;
    if (run_helper(helper, apt, pidfile, "2", nullptr, helper_pid, "zombie-dpkg", nullptr, nullptr,
                   "simulate", nullptr, &stderr_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for zombie dpkg\n");
      return 1;
    }
    pid_t leader = 0;
    pid_t zombie = 0;
    expect(wait_for_pids(pidfile, leader, zombie, 2000), "zombie dpkg stub started");
    int status = 0;
    const bool exited = wait_pid(helper_pid, 12000, status);
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits on the deadline when dpkg is a zombie");
    expect(err.find("DPKG_STARTED") == std::string::npos, "zombie dpkg does not emit DPKG_STARTED");
    expect(dead(leader), "parent of a dpkg zombie is dead after the deadline");
    if (!dead(leader))
      kill(leader, SIGKILL);
    if (!dead(zombie))
      kill(zombie, SIGKILL);
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string envfile = std::string(dir) + "/apt.env";
    pid_t helper_pid = 0;
    if (run_helper(helper, apt, std::string(dir) + "/env.pids", nullptr, nullptr, helper_pid,
                   "env-check", nullptr, nullptr, "simulate", nullptr, nullptr, nullptr,
                   envfile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for env check\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string text = read_file_all(envfile);
    expect(exited, "helper exits after the env check");
    expect(text.find("should-not-leak") == std::string::npos, "apt child does not inherit LCOS_CANARY");
    expect(text.find("APT_CONFIG=[]") != std::string::npos, "apt child does not inherit APT_CONFIG");
    expect(text.find("/no/such/apt.conf") == std::string::npos, "APT_CONFIG path is not visible to apt");
    expect(text.find("http_proxy=[]") != std::string::npos, "apt child does not inherit http_proxy");
    expect(text.find("DPKG_FORCE=[]") != std::string::npos, "apt child does not inherit DPKG_FORCE");
    expect(text.find("PATH=[/usr/sbin:/usr/bin:/sbin:/bin]") != std::string::npos,
           "apt child PATH is the safe path");
    expect(text.find("LANG=[C.UTF-8]") != std::string::npos, "apt child LANG is C.UTF-8");
    expect(text.find("LC_ALL=[C.UTF-8]") != std::string::npos, "apt child LC_ALL is C.UTF-8");
    expect(text.find("DEBIAN_FRONTEND=[noninteractive]") != std::string::npos,
           "apt child stays noninteractive");
    expect(text.find("HOME=[]") != std::string::npos, "non-root apt child does not keep HOME");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    auto refuse = [&](const std::vector<std::string>& args, const char* what) {
      int out_pipe[2];
      if (pipe(out_pipe) != 0)
        return;
      const pid_t pid = fork();
      if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
          dup2(devnull, STDIN_FILENO);
          close(devnull);
        }
        std::vector<char*> av;
        av.push_back(const_cast<char*>(helper));
        for (const auto& arg : args)
          av.push_back(const_cast<char*>(arg.c_str()));
        av.push_back(nullptr);
        execv(helper, av.data());
        _exit(127);
      }
      close(out_pipe[1]);
      int status = 0;
      const bool exited = wait_pid(pid, 5000, status);
      const std::string out = read_all_fd(out_pipe[0]);
      close(out_pipe[0]);
      expect(exited && WIFEXITED(status) && WEXITSTATUS(status) != 0, what);
      expect(out.find("STATUS error\n") != std::string::npos, what);
      expect(out.find("refused:") != std::string::npos, what);
    };
    refuse({}, "missing command is refused");
    refuse({"upgrade"}, "upgrade without pins is refused");
    refuse({"upgrade", "not a pin"}, "invalid pin is refused");
    refuse({"upgrade", "-y"}, "option pin is refused");
    refuse({"simulate", "extra"}, "simulate with an extra argument is refused");
    int out_pipe[2];
    if (pipe(out_pipe) == 0) {
      const pid_t pid = fork();
      if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
          dup2(devnull, STDIN_FILENO);
          close(devnull);
        }
        execl(helper, helper, "upgrade", "not a pin", static_cast<char*>(nullptr));
        _exit(127);
      }
      close(out_pipe[1]);
      int status = 0;
      wait_pid(pid, 5000, status);
      const std::string out = read_all_fd(out_pipe[0]);
      close(out_pipe[0]);
      expect(out.find("not a pin") != std::string::npos, "a refused pin is named in the message");
    }
    if (pipe(out_pipe) == 0) {
      const pid_t pid = fork();
      if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
          dup2(devnull, STDIN_FILENO);
          close(devnull);
        }
        execl(helper, helper, "upgrade", "libc6=2", "libc6=2", static_cast<char*>(nullptr));
        _exit(127);
      }
      close(out_pipe[1]);
      int status = 0;
      wait_pid(pid, 5000, status);
      const std::string out = read_all_fd(out_pipe[0]);
      close(out_pipe[0]);
      expect(out.find("libc6=2") != std::string::npos, "a duplicate pin is named in the message");
    }
  }

  {
    const std::string countfile = std::string(dir) + "/dead.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/dead.pids", nullptr, nullptr, helper_pid,
                   "dead-mirror", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for dead mirror\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a dead mirror");
    expect(read_count(countfile) == 1, "a dead mirror does not simulate");
    expect(out.find("STATUS error\n") != std::string::npos, "a dead mirror is an error");
    expect(out.find("STATUS up-to-date\n") == std::string::npos, "a dead mirror is not up to date");
    expect(out.find("/etc/apt/apt.conf.d") != std::string::npos, "a dead mirror mentions the apt proxy");
    expect(out.find("No network connection") == std::string::npos,
           "a connection timeout is not reported as no network");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/resolve.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/resolve.pids", nullptr, nullptr, helper_pid,
                   "resolve-fail", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for resolve failure\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a resolve failure");
    expect(read_count(countfile) == 1, "a resolve failure does not simulate");
    expect(out.find("No network connection") != std::string::npos, "a resolve failure is offline");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/phase.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/phase.pids", nullptr, nullptr, helper_pid,
                   "phasing", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for phasing\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a phased simulate");
    expect(out.find("STATUS kept-back\n") != std::string::npos, "phasing is kept-back, not up to date");
    expect(out.find("PHASED shim-signed\n") != std::string::npos, "phasing names the first package");
    expect(out.find("PHASED grub-efi-amd64-signed\n") != std::string::npos,
           "phasing names the second package");
    expect(out.find("KEPT ") == std::string::npos, "phasing is not described as a classic kept-back");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string status_path = std::string(dir) + "/dpkg-status";
    {
      FILE* file = std::fopen(status_path.c_str(), "w");
      if (file != nullptr) {
        std::fputs("Package: linux-image-amd64\n"
                   "Status: hold ok installed\n"
                   "Architecture: amd64\n"
                   "\n"
                   "Package: libc6\n"
                   "Status: install ok installed\n"
                   "Architecture: amd64\n",
                   file);
        std::fclose(file);
      }
    }
    const std::string countfile = std::string(dir) + "/held.count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/held.pids", nullptr, nullptr, helper_pid,
                   "held-packages", countfile.c_str(), &stdout_read, "simulate", nullptr, nullptr,
                   nullptr, nullptr, nullptr, nullptr, nullptr, status_path.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for held packages\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a held package");
    expect(out.find("HELD linux-image-amd64\n") != std::string::npos, "a dpkg hold is named as held");
    expect(out.find("KEPT libc6\n") != std::string::npos, "a package that is not held stays kept back");
    expect(out.find("HELD libc6\n") == std::string::npos, "an installed package is not called held");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string argvfile = std::string(dir) + "/arch.argv";
    const std::string countfile = std::string(dir) + "/arch.count";
    const std::vector<std::string> pins = {"libfoo:i386=1.2-3"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/arch.pids", nullptr, nullptr, helper_pid,
                   "arch-pin", countfile.c_str(), &stdout_read, "upgrade", &pins, nullptr,
                   argvfile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for arch pin\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string argv_text = read_file_all(argvfile);
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper accepts a name:arch pin");
    expect(out.find("STATUS success\n") != std::string::npos, "a name:arch pin is installed");
    expect(out.find("invalid package pin") == std::string::npos, "a name:arch pin is not refused");
    expect(argv_text.find("\nlibfoo:i386=1.2-3\n") != std::string::npos,
           "install argv keeps the name:arch pin");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/long.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/long.pids", nullptr, nullptr, helper_pid,
                   "long-kept", countfile.c_str(), &stdout_read, "upgrade", &pins) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a long kept-back log\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a long install log");
    expect(out.find("KEPT linux-image-amd64\n") != std::string::npos,
           "a log past 1 MiB still names the kept-back package");
    expect(out.find("NOT_UPGRADED 1\n") != std::string::npos, "a log past 1 MiB still has the summary");
    expect(out.find("STATUS success\n") == std::string::npos,
           "a kept-back package past 1 MiB is not a clean success");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string countfile = std::string(dir) + "/nosum.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/nosum.pids", nullptr, nullptr, helper_pid,
                   "no-summary", countfile.c_str(), &stdout_read, "upgrade", &pins) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a missing summary\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits when apt prints no summary");
    expect(out.find("SUMMARY_MISSING\n") != std::string::npos, "a missing summary is reported");
    expect(out.find("STATUS up-to-date\n") == std::string::npos, "a missing summary is not up to date");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string reboot = std::string(dir) + "/reboot-required";
    {
      FILE* file = std::fopen(reboot.c_str(), "w");
      if (file != nullptr) {
        std::fputs("*** System restart required ***\n", file);
        std::fclose(file);
      }
      file = std::fopen((reboot + ".pkgs").c_str(), "w");
      if (file != nullptr) {
        std::fputs("linux-image-amd64\n", file);
        std::fclose(file);
      }
    }
    const std::string countfile = std::string(dir) + "/reboot.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/reboot.pids", nullptr, nullptr, helper_pid,
                   "pin-upgrade", countfile.c_str(), &stdout_read, "upgrade", &pins, nullptr, nullptr,
                   nullptr, nullptr, nullptr, reboot.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for reboot-required\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after an install that needs a restart");
    expect(out.find("STATUS success\n") != std::string::npos, "a reboot flag is still a success");
    expect(out.find("REBOOT linux-image-amd64\n") != std::string::npos,
           "a reboot flag names the package");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stderr_read = -1;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/trickle.pids", "1", nullptr, helper_pid, "trickle",
                   nullptr, &stdout_read, "simulate", nullptr, &stderr_read, nullptr, nullptr, "8") !=
        0) {
      std::fprintf(stderr, "failed to spawn helper for trickle\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 15000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "slow apt output does not hit the idle clock");
    expect(out.find("STATUS up-to-date\n") != std::string::npos, "a trickle still finishes the check");
    expect(out.find("Timed out") == std::string::npos, "a trickle is not a timeout");
    expect(err.find("PHASE refresh\n") != std::string::npos, "the helper announces the refresh phase");
    expect(err.find("PROGRESS Hit:") != std::string::npos, "the helper forwards an apt progress line");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/chatter.pids", "1", nullptr, helper_pid, "chatter",
                   nullptr, &stdout_read, "simulate", nullptr, nullptr, nullptr, nullptr, "3") != 0) {
      std::fprintf(stderr, "failed to spawn helper for chatter\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 12000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "the hard cap still ends apt that never goes idle");
    expect(out.find("Timed out") != std::string::npos, "the hard cap is reported as a timeout");
    expect(out.find("STATUS up-to-date\n") == std::string::npos, "the hard cap does not look finished");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/before.state";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/before.pids", nullptr, &cancel_write, helper_pid,
                   "hang-before", nullptr, &stdout_read, "simulate", nullptr, nullptr, nullptr, nullptr,
                   nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for cancel before dpkg\n");
      return 1;
    }
    for (int waited = 0; waited < 2000; waited += 20) {
      if (read_file_all(statefile).find("started") != std::string::npos)
        break;
      poll(nullptr, 0, 20);
    }
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    kill(helper_pid, SIGTERM);
    kill(helper_pid, SIGHUP);
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string state = read_file_all(statefile);
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits when cancelled before dpkg");
    expect(out.find("cancelled") != std::string::npos, "EOF and SIGTERM before dpkg still cancel");
    expect(state.find("finished") == std::string::npos, "a cancel before dpkg does not finish apt");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/finish.state";
    const std::string countfile = std::string(dir) + "/finish.count";
    const std::vector<std::string> pins = {"libc6=2"};
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    int stderr_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/finish.pids", nullptr, &cancel_write, helper_pid,
                   "finish-after-dpkg", countfile.c_str(), &stdout_read, "upgrade", &pins, &stderr_read,
                   nullptr, nullptr, nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for finish after dpkg\n");
      return 1;
    }
    const std::string err_so_far = stderr_read >= 0 ? read_until(stderr_read, "DPKG_STARTED", 5000)
                                                    : std::string();
    expect(err_so_far.find("DPKG_STARTED") != std::string::npos, "install announces DPKG_STARTED");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    kill(helper_pid, SIGTERM);
    kill(helper_pid, SIGHUP);
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string state = read_file_all(statefile);
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits after apt finishes past DPKG_STARTED");
    expect(state.find("after-dpkg") != std::string::npos, "apt reached the dpkg phase");
    expect(state.find("finished") != std::string::npos,
           "EOF and SIGTERM after DPKG_STARTED still reach finished");
    expect(out.find("STATUS success\n") != std::string::npos, "the install is reported as finished");
    expect(out.find("cancelled") == std::string::npos, "a committed install is not cancelled");
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
