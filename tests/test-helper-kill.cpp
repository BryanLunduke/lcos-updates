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
#include <fstream>
#include <fcntl.h>
#include <sstream>
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

static bool argv_has(int argc, char** argv, const char* flag)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0)
      return true;
  }
  return false;
}

static bool is_print_uris(int argc, char** argv)
{
  return argv_has(argc, argv, "--print-uris");
}

/* A read-only per-name probe. Not the real install, which passes -y. */
static bool is_kept_probe(int argc, char** argv)
{
  return argv_has(argc, argv, "-s") && argv_has(argc, argv, "--only-upgrade") &&
         !argv_has(argc, argv, "-y");
}

static int count_lines_equal(const std::string& text, const char* token)
{
  int n = 0;
  const std::string needle = std::string("\n") + token + "\n";
  for (std::string::size_type pos = 0; (pos = text.find(needle, pos)) != std::string::npos;
       pos += needle.size())
    ++n;
  if (text.compare(0, std::strlen(token), token) == 0 &&
      (text.size() == std::strlen(token) || text[std::strlen(token)] == '\n'))
    ++n;
  return n;
}

static std::string reboot_beside_state()
{
  const char* state = std::getenv("LCOS_STUB_STATEFILE");
  if (state == nullptr || state[0] == '\0')
    return {};
  const std::string path(state);
  return path + ".reboot";
}

static void write_reboot_pkgs(const std::string& path, const char* pkgs)
{
  if (path.empty())
    return;
  FILE* file = std::fopen(path.c_str(), "w");
  if (file != nullptr) {
    std::fputs("*** System restart required ***\n", file);
    std::fclose(file);
  }
  file = std::fopen((path + ".pkgs").c_str(), "w");
  if (file != nullptr) {
    std::fputs(pkgs, file);
    std::fclose(file);
  }
}

static void note_state(const char* text)
{
  const char* path = std::getenv("LCOS_STUB_STATEFILE");
  if (path == nullptr)
    return;
  FILE* file = std::fopen(path, "a");
  if (file == nullptr)
    return;
  std::fputs(text, file);
  std::fclose(file);
}

/* Keep a configuring dpkg visible long enough for the 1 Hz scan, then reap it. */
static void configure_then_exit(int alive_ms)
{
  signal(SIGTERM, SIG_IGN);
  setpgid(0, 0);
  const pid_t child = spawn_hold({"--configure", "pkg"}, true);
  if (child > 1) {
    setpgid(child, child);
    poll(nullptr, 0, alive_ms);
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
  }
  note_state("dpkg-exited\n");
}

static int apt_script(const char* script, int argc, char** argv)
{
  const char* count_path = std::getenv("LCOS_STUB_COUNTFILE");
  const int n = count_and_mark(count_path);
  /* A read-only probe of one kept-back name. Checked before the n-based
   * branches so it does not look like the install. */
  if (std::strcmp(script, "removal-kept") == 0 && argv_has(argc, argv, "-s") &&
      argv_has(argc, argv, "--only-upgrade") && !argv_has(argc, argv, "-y")) {
    std::fputs("The following packages will be REMOVED:\n"
               "  oldplug\n"
               "0 upgraded, 0 newly installed, 1 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
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
  if (std::strcmp(script, "dpkg-term-gone") == 0) {
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
    poll(nullptr, 0, 300);
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
    poll(nullptr, 0, 200);
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
  if (std::strcmp(script, "carry-phased") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following upgrades have been deferred due to phasing:\n"
                 "  shim-signed\n"
                 "Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "carry-held") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  vim\n"
                 "Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "carry-kept") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  foo\n"
                 "Inst bar [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "removal-kept") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  foo\n"
                 "Inst bar [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "stall-after-idle") == 0 || std::strcmp(script, "stall-hard") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    configure_then_exit(2200);
    if (std::strcmp(script, "stall-hard") == 0) {
      for (int i = 0; i < 20; ++i) {
        std::fputs("still working\n", stdout);
        std::fflush(stdout);
        poll(nullptr, 0, 250);
      }
    } else {
      poll(nullptr, 0, 4000);
    }
    note_state("finished\n");
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "disk-full") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst foo [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("dpkg: error processing archive foo.deb (--unpack):\n"
               " No space left on device\n"
               "E: Sub-process /usr/bin/dpkg returned an error code (1)\n",
               stdout);
    return 100;
  }
  if (std::strcmp(script, "connect-refused") == 0) {
    std::fputs("Err:1 http://192.0.2.1/debian stable InRelease\n"
               "  Could not connect to 192.0.2.1:80 (192.0.2.1). - connect (111: Connection refused)\n",
               stderr);
    return 0;
  }
  if (std::strcmp(script, "release-missing") == 0) {
    std::fputs("E: The repository 'http://deb.example stable' does not have a Release file.\n", stderr);
    return 0;
  }
  if (std::strcmp(script, "cr-progress") == 0) {
    if (n == 1) {
      const char* lines[] = {
          "Get:1 http://deb.example stable/main amd64 linux-image-amd64 amd64 6.1 [12%]",
          "Get:1 http://deb.example stable/main amd64 linux-image-amd64 amd64 6.1 [100%]",
      };
      for (const char* line : lines) {
        std::fputs(line, stdout);
        std::fputc('\r', stdout);
        std::fflush(stdout);
        poll(nullptr, 0, 300);
      }
      std::fputs("\nHit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    std::fputs("Inst libc6 [1] (2 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "reboot-during") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst bash [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    write_reboot_pkgs(reboot_beside_state(), "linux-image-amd64\n");
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "reboot-grow") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst bash [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    write_reboot_pkgs(reboot_beside_state(), "bash\nlinux-image-amd64\n");
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "reboot-touch") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst bash [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    write_reboot_pkgs(reboot_beside_state(), "linux-image-amd64\n");
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
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
  if (std::strcmp(script, "probe-sleep") == 0) {
    if (is_kept_probe(argc, argv)) {
      note_state("probe\n");
      for (;;)
        pause();
    }
    if (n == 1)
      return 0;
    std::fputs("The following packages have been kept back:\n"
               "  foo\n"
               "Inst bar [1] (2 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "probe-removed") == 0) {
    if (is_kept_probe(argc, argv)) {
      std::fputs("The following packages will be REMOVED:\n"
                 "  oldplug\n",
                 stdout);
      return 100;
    }
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  foo\n"
                 "Inst bar [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "probe-plain") == 0) {
    if (is_kept_probe(argc, argv)) {
      std::fputs("E: Failed to fetch http://deb.example/foo  Something went wrong\n", stderr);
      return 100;
    }
    if (n == 1)
      return 0;
    std::fputs("The following packages have been kept back:\n"
               "  foo\n"
               "Inst bar [1] (2 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "probe-once") == 0) {
    if (is_kept_probe(argc, argv)) {
      note_state("probe\n");
      return 0;
    }
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n"
                 "  foo\n"
                 "Inst bar [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    std::fputs("1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "many-kept") == 0) {
    if (is_kept_probe(argc, argv)) {
      note_state("probe\n");
      std::fputs("The following packages will be REMOVED:\n"
                 "  oldplug\n",
                 stdout);
      return 0;
    }
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("The following packages have been kept back:\n ", stdout);
      for (int i = 1; i <= 40; ++i)
        std::fprintf(stdout, " pkg%02d", i);
      std::fputs("\n0 upgraded, 0 newly installed, 0 to remove and 40 not upgraded.\n", stdout);
      return 0;
    }
    return 0;
  }
  if (std::strcmp(script, "quick-progress") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      std::fputs("12% [1 linux-image-amd64 4 MB/80 MB 5%]\r", stdout);
      std::fflush(stdout);
      /* Far enough apart that the first line is published, and the second is
       * still inside the 200ms hold when apt exits. */
      usleep(250000);
      std::fputs("47% [1 linux-image-amd64 40 MB/80 MB 50%]\r", stdout);
      std::fflush(stdout);
      return 0;
    }
    std::fputs("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "postinst-fail") == 0) {
    if (n == 1)
      return 0;
    if (n == 2) {
      std::fputs("Inst foo [1] (2 Debian:12 [amd64])\n"
                 "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
                 stdout);
      return 0;
    }
    /* Real apt 2.8.3 / dpkg wording from a postinst that exits 1. */
    std::fputs("Setting up foo (2) ...\n"
               "dpkg: error processing package foo (--configure):\n"
               " installed foo package post-installation script subprocess returned error exit status 1\n"
               "Errors were encountered while processing:\n"
               " foo\n"
               "E: Sub-process /usr/bin/dpkg returned an error code (1)\n",
               stdout);
    return 100;
  }
  if (std::strcmp(script, "with-size") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    std::fputs("Inst foo [1] (2 Debian:12 [amd64])\n"
               "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n",
               stdout);
    return 0;
  }
  if (std::strcmp(script, "space-clear") == 0) {
    if (n == 1) {
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      std::fputs("4% [1 xz-utils 13.0 kB/267 kB 5%]\r", stdout);
      std::fflush(stdout);
      usleep(250000);
      std::fputs("                                        \r", stdout);
      std::fflush(stdout);
      usleep(250000);
      std::fputs("28% [1 xz-utils 80.0 kB/267 kB 30%]\r", stdout);
      std::fflush(stdout);
      std::fputs("                                        \r", stdout);
      std::fflush(stdout);
      std::fputs("Inst gzip [1] (2 Debian:12 [amd64])\n", stdout);
      return 0;
    }
    std::fputs("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
    return 0;
  }
  if (std::strcmp(script, "reuse-snap") == 0) {
    if (is_kept_probe(argc, argv)) {
      note_state("probe\n");
      std::fputs("The following packages will be REMOVED:\n"
                 "  oldplug\n",
                 stdout);
      return 0;
    }
    if (argv_has(argc, argv, "update")) {
      note_state("update\n");
      std::fputs("Hit:1 http://deb.example stable InRelease\n", stdout);
      return 0;
    }
    const char* state = std::getenv("LCOS_STUB_STATEFILE");
    const bool mismatch =
        state != nullptr && access((std::string(state) + ".mismatch").c_str(), F_OK) == 0;
    if (argv_has(argc, argv, "-s") && argv_has(argc, argv, "upgrade") &&
        !argv_has(argc, argv, "--only-upgrade")) {
      std::fputs("The following packages have been kept back:\n", stdout);
      std::fputs(mismatch ? "  bar\n" : "  foo\n", stdout);
      std::fputs("Inst bash [1] (2 Debian:12/stable [amd64])\n"
                 "Inst openssl [1] (2 Debian-Security:12/stable-security [amd64])\n"
                 "2 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n",
                 stdout);
      return 0;
    }
    if (argv_has(argc, argv, "-y")) {
      note_state("install\n");
      std::fputs("2 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", stdout);
      return 0;
    }
    return 0;
  }
  return 2;
}

/* Exec'd as fake-apt-get. Ignores SIGTERM and leaves a grandchild that does too. */
static int apt_stub(int argc, char** argv)
{
  record_argv(argc, argv);
  /* Download size. Does not increment the script counter and does not install.
   * The sentence is apt 2.8.3's real --print-uris line. */
  if (is_print_uris(argc, argv)) {
    std::fputs("Need to get 0 B/478 B of archives.\n", stdout);
    std::fputs("After this operation, 0 B of additional disk space will be used.\n", stdout);
    std::fputs("'file:/tmp/localrepo/./lcos-fixture-pkg_2.0_all.deb' lcos-fixture-pkg_2.0_all.deb 478 "
               "MD5Sum:3502cb7bee13b76cb4e82fd4ae080673\n",
               stdout);
    return 0;
  }
  const char* script = std::getenv("LCOS_STUB_SCRIPT");
  if (script != nullptr && script[0] != '\0')
    return apt_script(script, argc, argv);
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
    expect(out.find("still running") != std::string::npos,
           "an interrupted dpkg that is still alive does not offer configure -a");
    expect(out.find("dpkg --configure -a") == std::string::npos,
           "configure -a is withheld while that dpkg process is still alive");
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
    expect(out.find("left unfinished") == std::string::npos,
           "a lock message stays as apt printed it");
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
    expect(argv_text.find("\nquiet=0\n") != std::string::npos, "update and install pass quiet=0");
    expect(argv_text.find("\nAPT::Status-Fd=3\n") != std::string::npos,
           "apt is given a machine-readable status fd");
    expect(count_lines_equal(argv_text, "quiet=0") == 2,
           "quiet=0 is on update and install, not the simulation");
    expect(argv_text.find("--print-uris") == std::string::npos,
           "an install does not use the download-size request");
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
    expect(out.find("http://deb.example/InRelease") != std::string::npos,
           "a dead mirror keeps apt's URL");
    expect(out.find("Connection timed out") != std::string::npos, "a dead mirror keeps the timeout words");
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
    expect(out.find("deb.example") != std::string::npos, "a resolve failure still shows the hostname");
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
    expect(out.find("STATUS success\n") != std::string::npos, "an old reboot flag is still a success");
    expect(out.find("REBOOT_ALREADY linux-image-amd64\n") != std::string::npos,
           "a reboot flag that was already there is not this install");
    expect(out.find("\nREBOOT ") == std::string::npos && out.find("\nREBOOT\n") == std::string::npos,
           "an old reboot flag is not reported as this install's restart");
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

  {
    const std::string pidfile = std::string(dir) + "/dpkg-gone.pids";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    if (run_helper(helper, apt, pidfile, nullptr, &cancel_write, helper_pid, "dpkg-term-gone", nullptr,
                   &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for dpkg-term-gone\n");
      return 1;
    }
    pid_t leader = 0;
    expect(wait_for_leader(pidfile, leader, 2000), "dpkg-term-gone stub is running");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    pid_t dpkg = 0;
    expect(wait_for_pids(pidfile, leader, dpkg, 4000), "dpkg pid was recorded after SIGTERM");
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after the interrupted dpkg is gone");
    expect(dead(dpkg), "the interrupted dpkg has exited");
    expect(out.find("dpkg --configure -a") != std::string::npos,
           "configure -a is mentioned only after the interrupted dpkg is gone");
    expect(out.find("still running") == std::string::npos,
           "a gone dpkg is not described as still running");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  auto run_upgrade = [&](const char* script, const char* tag, const std::vector<std::string>& pins,
                         const char* timeout_sec, const char* hard_cap, const char* statefile,
                         const char* reboot_file, const char* dpkg_status, int* stderr_read,
                         std::string& out, bool& exited) -> pid_t {
    const std::string countfile = std::string(dir) + "/" + tag + ".count";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    if (run_helper(helper, apt, std::string(dir) + "/" + tag + ".pids", timeout_sec, nullptr,
                   helper_pid, script, countfile.c_str(), &stdout_read, "upgrade", &pins, stderr_read,
                   nullptr, nullptr, hard_cap, statefile, reboot_file, dpkg_status) != 0)
      return -1;
    int status = 0;
    const int budget = hard_cap != nullptr ? 20000 : 10000;
    exited = wait_pid(helper_pid, budget, status);
    out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    if (!exited)
      terminate_process_tree(helper_pid, 200);
    return helper_pid;
  };

  {
    const std::vector<std::string> pins = {"libc6=2"};
    std::string out;
    bool exited = false;
    const std::string statefile = std::string(dir) + "/stall-idle.state";
    if (run_upgrade("stall-after-idle", "stall-idle", pins, "2", "30", statefile.c_str(), nullptr,
                    nullptr, nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for stall after idle\n");
      return 1;
    }
    const std::string state = read_file_all(statefile);
    expect(exited, "helper exits after DPKG_IDLE without killing apt");
    expect(state.find("dpkg-exited") != std::string::npos, "configure was allowed to finish");
    expect(state.find("finished") != std::string::npos,
           "idle budget after DPKG_STARTED does not kill the rest of apt");
    expect(out.find("STATUS success\n") != std::string::npos, "the stalled-idle install still succeeds");
    expect(out.find("Timed out") == std::string::npos, "no timeout after DPKG_STARTED");
  }

  {
    const std::vector<std::string> pins = {"libc6=2"};
    std::string out;
    bool exited = false;
    const std::string statefile = std::string(dir) + "/stall-hard.state";
    if (run_upgrade("stall-hard", "stall-hard", pins, "1", "3", statefile.c_str(), nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for stall hard cap\n");
      return 1;
    }
    const std::string state = read_file_all(statefile);
    expect(exited, "helper exits under a hard cap after DPKG_STARTED");
    expect(state.find("finished") != std::string::npos,
           "the hard cap after DPKG_STARTED does not kill apt");
    expect(out.find("Timed out") == std::string::npos, "hard cap after DPKG_STARTED is not a timeout");
    expect(out.find("STATUS success\n") != std::string::npos, "progress after DPKG_IDLE still finishes");
  }

  {
    const std::vector<std::string> pins = {"libc6=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("carry-phased", "carry-phased", pins, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for carried phasing\n");
      return 1;
    }
    expect(exited, "helper exits after an install that had phased updates");
    expect(out.find("STATUS success\n") != std::string::npos, "installed packages are a success");
    expect(out.find("PHASED shim-signed\n") != std::string::npos,
           "phasing from the pre-install simulation is kept");
    expect(out.find("STATUS up-to-date\n") == std::string::npos, "phased leftovers are not up to date");
  }

  {
    const std::vector<std::string> pins = {"libc6=2"};
    std::string out;
    bool exited = false;
    const std::string status_path = std::string(dir) + "/carry-held.status";
    {
      FILE* file = std::fopen(status_path.c_str(), "w");
      if (file != nullptr) {
        std::fputs("Package: vim\nStatus: hold ok installed\nArchitecture: amd64\n\n", file);
        std::fclose(file);
      }
    }
    if (run_upgrade("carry-held", "carry-held", pins, nullptr, nullptr, nullptr, nullptr,
                    status_path.c_str(), nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for carried holds\n");
      return 1;
    }
    expect(exited, "helper exits after an install that had a hold");
    expect(out.find("HELD vim\n") != std::string::npos, "a hold from the pre-install simulation is kept");
    expect(out.find("STATUS success\n") != std::string::npos, "the installed package is still a success");
  }

  {
    const std::vector<std::string> pins = {"bar=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("carry-kept", "carry-kept", pins, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for carried kept-back\n");
      return 1;
    }
    expect(exited, "helper exits after an install that left a package kept back");
    expect(out.find("KEPT foo\n") != std::string::npos,
           "a kept-back name from the pre-install simulation is kept");
    expect(out.find("STATUS success\n") != std::string::npos, "the installed package is a success");
  }

  {
    const std::vector<std::string> pins = {"bar=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("removal-kept", "removal", pins, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a removal keep-back\n");
      return 1;
    }
    expect(exited, "helper exits after classifying a removal keep-back");
    expect(out.find("REMOVE foo oldplug\n") != std::string::npos,
           "a Breaks keep-back names the package that would be removed");
    expect(out.find("KEPT foo\n") == std::string::npos, "a removal is not a classic kept-back");
    expect(out.find("STATUS success\n") != std::string::npos, "the other package is still installed");
  }

  {
    const std::vector<std::string> pins = {"foo=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("disk-full", "disk", pins, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                    out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a full disk\n");
      return 1;
    }
    expect(exited, "helper exits after a full disk");
    expect(out.find("No space left on device") != std::string::npos, "a full disk shows the cause");
    expect(out.find("foo.deb") != std::string::npos, "a full disk names the archive");
    expect(out.find("dpkg --configure -a") == std::string::npos,
           "a full disk does not say to run dpkg --configure -a");
    expect(out.find("The upgrade stopped with a package left unfinished.") != std::string::npos,
           "a full disk says the upgrade stopped unfinished");
    expect(out.find("Free disk space, then finish configuring packages.") != std::string::npos,
           "a full disk says to free space and finish configuring");
  }

  {
    pid_t helper_pid = 0;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/refused.count";
    if (run_helper(helper, apt, std::string(dir) + "/refused.pids", nullptr, nullptr, helper_pid,
                   "connect-refused", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for connection refused\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after connection refused");
    expect(out.find("Connection refused") != std::string::npos, "connection refused keeps apt's words");
    expect(out.find("192.0.2.1") != std::string::npos, "connection refused keeps the address");
    expect(out.find("/etc/apt/apt.conf.d") != std::string::npos, "connection refused adds the proxy hint");
    expect(out.find("could not be refreshed") == std::string::npos,
           "connection refused is not the generic sentence");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/release.count";
    if (run_helper(helper, apt, std::string(dir) + "/release.pids", nullptr, nullptr, helper_pid,
                   "release-missing", countfile.c_str(), &stdout_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a missing Release file\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a missing Release file");
    expect(out.find("does not have a Release file") != std::string::npos,
           "a missing Release file keeps apt's E: line");
    expect(out.find("could not be refreshed") == std::string::npos,
           "a missing Release file is not the generic sentence");
    expect(out.find("/etc/apt/apt.conf.d") == std::string::npos,
           "a missing Release file is not a proxy hint");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stderr_read = -1;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/cr.count";
    if (run_helper(helper, apt, std::string(dir) + "/cr.pids", nullptr, nullptr, helper_pid,
                   "cr-progress", countfile.c_str(), &stdout_read, "simulate", nullptr,
                   &stderr_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for carriage-return progress\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits after carriage-return progress");
    expect(err.find("PROGRESS ") != std::string::npos, "a carriage return emits PROGRESS");
    expect(err.find("linux-image-amd64") != std::string::npos, "progress names the package");
    expect(err.find("[12%]") != std::string::npos || err.find("[100%]") != std::string::npos,
           "progress includes a percent");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/reboot-new.state";
    const std::string reboot = statefile + ".reboot";
    const std::vector<std::string> pins = {"bash=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("reboot-during", "reboot-new", pins, nullptr, nullptr, statefile.c_str(),
                    reboot.c_str(), nullptr, nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a new reboot flag\n");
      return 1;
    }
    expect(exited, "helper exits after this install creates reboot-required");
    expect(out.find("REBOOT linux-image-amd64\n") != std::string::npos,
           "a flag created by this install says to restart");
    expect(out.find("REBOOT_ALREADY") == std::string::npos, "a new flag was not already pending");
  }

  {
    const std::string statefile = std::string(dir) + "/reboot-grew.state";
    const std::string reboot = statefile + ".reboot";
    write_reboot_pkgs(reboot, "bash\n");
    const std::vector<std::string> pins = {"bash=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("reboot-grow", "reboot-grew", pins, nullptr, nullptr, statefile.c_str(),
                    reboot.c_str(), nullptr, nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a grown reboot flag\n");
      return 1;
    }
    expect(exited, "helper exits after reboot-required grows");
    expect(out.find("REBOOT linux-image-amd64\n") != std::string::npos,
           "a new package on the flag is this install's restart");
    expect(out.find("REBOOT_ALREADY bash\n") != std::string::npos,
           "packages already listed stay on the old flag");
    expect(out.find("REBOOT bash\n") == std::string::npos, "an old package is not a new restart");
  }

  {
    const std::string statefile = std::string(dir) + "/reboot-same.state";
    const std::string reboot = statefile + ".reboot";
    write_reboot_pkgs(reboot, "linux-image-amd64\n");
    const std::vector<std::string> pins = {"bash=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("reboot-touch", "reboot-same", pins, nullptr, nullptr, statefile.c_str(),
                    reboot.c_str(), nullptr, nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for an unchanged reboot flag\n");
      return 1;
    }
    expect(exited, "helper exits after an unchanged reboot flag");
    expect(out.find("REBOOT_ALREADY linux-image-amd64\n") != std::string::npos,
           "touching the same package list does not attribute the restart");
    expect(out.find("\nREBOOT ") == std::string::npos, "an unchanged flag is not this install");
  }

  {
    const std::string statefile = std::string(dir) + "/probe-cancel.state";
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/probe-cancel.count";
    if (run_helper(helper, apt, std::string(dir) + "/probe-cancel.pids", nullptr, &cancel_write,
                   helper_pid, "probe-sleep", countfile.c_str(), &stdout_read, "simulate", nullptr,
                   nullptr, nullptr, nullptr, nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for cancel during kept-back probes\n");
      return 1;
    }
    for (int waited = 0; waited < 3000; waited += 20) {
      if (read_file_all(statefile).find("probe") != std::string::npos)
        break;
      poll(nullptr, 0, 20);
    }
    expect(read_file_all(statefile).find("probe") != std::string::npos,
           "the kept-back probe started");
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits when a kept-back probe is cancelled");
    expect(out.find("Update check was cancelled") != std::string::npos,
           "cancelling a check during kept-back probes says the check was cancelled");
    expect(out.find("STATUS upgrades") == std::string::npos,
           "a cancelled check is not a finished check");
    expect(out.find("KEPT ") == std::string::npos,
           "a cancelled probe is not described as needing extra packages");
    expect(out.find("Updates are available") == std::string::npos,
           "a cancelled check does not offer updates");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/probe-install-cancel.state";
    const std::string argvfile = std::string(dir) + "/probe-install-cancel.argv";
    const std::vector<std::string> pins = {"bar=2"};
    pid_t helper_pid = 0;
    int cancel_write = -1;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/probe-install-cancel.count";
    if (run_helper(helper, apt, std::string(dir) + "/probe-install-cancel.pids", nullptr, &cancel_write,
                   helper_pid, "probe-sleep", countfile.c_str(), &stdout_read, "upgrade", &pins,
                   nullptr, argvfile.c_str(), nullptr, nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for cancel during an install probe\n");
      return 1;
    }
    for (int waited = 0; waited < 3000; waited += 20) {
      if (read_file_all(statefile).find("probe") != std::string::npos)
        break;
      poll(nullptr, 0, 20);
    }
    if (cancel_write >= 0) {
      close(cancel_write);
      cancel_write = -1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string argv_text = read_file_all(argvfile);
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits when an install probe is cancelled");
    expect(out.find("Install was cancelled") != std::string::npos,
           "cancelling during the pre-install probe says the install was cancelled");
    expect(argv_text.find("\n-y\n") == std::string::npos,
           "a cancelled pre-install probe does not start the real install");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/probe-plain.count";
    if (run_helper(helper, apt, std::string(dir) + "/probe-plain.pids", nullptr, nullptr, helper_pid,
                   "probe-plain", countfile.c_str(), &stdout_read, "simulate") != 0) {
      std::fprintf(stderr, "failed to spawn helper for a failed kept-back probe\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after a kept-back probe that fails");
    expect(out.find("UNCLASSIFIED foo\n") != std::string::npos,
           "a probe that does not exit 0 leaves the package unclassified");
    expect(out.find("KEPT foo\n") == std::string::npos,
           "a failed probe is not described as needing extra packages");
    expect(out.find("PKG bar 1 2\n") != std::string::npos, "the other upgrade is still listed");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::vector<std::string> pins = {"bar=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("probe-removed", "probe-removed", pins, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a removal printed by a failed probe\n");
      return 1;
    }
    expect(exited, "helper exits after a probe that printed a removal and failed");
    expect(out.find("REMOVE foo oldplug\n") != std::string::npos,
           "a removal printed before a non-zero probe exit is kept");
    expect(out.find("KEPT foo\n") == std::string::npos,
           "that removal is not relabeled as extra packages");
    expect(out.find("STATUS success\n") != std::string::npos, "the reviewed package is still installed");
  }

  {
    const std::string statefile = std::string(dir) + "/many-kept.state";
    const std::string argvfile = std::string(dir) + "/many-kept.argv";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    int stderr_read = -1;
    const std::string countfile = std::string(dir) + "/many-kept.count";
    if (run_helper(helper, apt, std::string(dir) + "/many-kept.pids", nullptr, nullptr, helper_pid,
                   "many-kept", countfile.c_str(), &stdout_read, "simulate", nullptr, &stderr_read,
                   argvfile.c_str(), nullptr, nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for many kept-back packages\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 20000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    const std::string state = read_file_all(statefile);
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits after classifying many kept-back packages");
    const auto heading = err.find("The following packages have been kept back:");
    const auto checking = err.rfind("Checking kept-back packages");
    expect(checking != std::string::npos && (heading == std::string::npos || checking > heading),
           "kept-back classification replaces apt's heading");
    int removes = 0;
    for (std::string::size_type pos = 0; (pos = out.find("REMOVE pkg", pos)) != std::string::npos;
         pos += 7)
      ++removes;
    expect(removes == 40, "every kept-back name is classified when the budget allows it");
    expect(out.find("KEPT ") == std::string::npos, "a classified removal is not an extra package");
    expect(out.find("UNCLASSIFIED ") == std::string::npos, "a finished probe is not left unclassified");
    int probes = 0;
    for (std::string::size_type pos = 0; (pos = state.find("probe\n", pos)) != std::string::npos;
         pos += 6)
      ++probes;
    expect(probes == 40, "each name is probed once");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/many-capped.state";
    setenv("LCOS_UPDATES_KEPT_BUDGET_SEC", "0", 1);
    pid_t helper_pid = 0;
    int stdout_read = -1;
    int stderr_read = -1;
    const std::string countfile = std::string(dir) + "/many-capped.count";
    if (run_helper(helper, apt, std::string(dir) + "/many-capped.pids", nullptr, nullptr, helper_pid,
                   "many-kept", countfile.c_str(), &stdout_read, "simulate", nullptr, &stderr_read,
                   nullptr, nullptr, nullptr, statefile.c_str()) != 0) {
      unsetenv("LCOS_UPDATES_KEPT_BUDGET_SEC");
      std::fprintf(stderr, "failed to spawn helper for a capped kept-back pass\n");
      return 1;
    }
    unsetenv("LCOS_UPDATES_KEPT_BUDGET_SEC");
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    const std::string state = read_file_all(statefile);
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits when the kept-back budget is already spent");
    expect(state.find("probe") == std::string::npos, "a spent budget does not start probes");
    expect(out.find("KEPT ") == std::string::npos,
           "names past the budget are not called extra packages");
    expect(out.find("UNCLASSIFIED pkg01\n") != std::string::npos, "the first unprobed name is unclassified");
    expect(out.find("UNCLASSIFIED pkg40\n") != std::string::npos, "the last unprobed name is unclassified");
    expect(err.find("Checking kept-back packages") != std::string::npos,
           "the status says kept-back packages are being checked");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string statefile = std::string(dir) + "/probe-once.state";
    const std::vector<std::string> pins = {"bar=2"};
    std::string out;
    bool exited = false;
    pid_t helper_pid = 0;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/probe-once.count";
    if (run_helper(helper, apt, std::string(dir) + "/probe-once.pids", nullptr, nullptr, helper_pid,
                   "probe-once", countfile.c_str(), &stdout_read, "upgrade", &pins, nullptr, nullptr,
                   nullptr, nullptr, statefile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a second kept-back pass\n");
      return 1;
    }
    int status = 0;
    exited = wait_pid(helper_pid, 10000, status);
    out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    const std::string state = read_file_all(statefile);
    expect(exited, "helper exits after an install whose kept-back name was already classified");
    expect(count_lines_equal(state, "probe") == 1,
           "a name classified before install is not probed again");
    expect(out.find("KEPT foo\n") != std::string::npos, "the pre-install classification is kept");
    expect(out.find("STATUS success\n") != std::string::npos, "the reviewed package is installed");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    pid_t helper_pid = 0;
    int stderr_read = -1;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/quick-progress.count";
    if (run_helper(helper, apt, std::string(dir) + "/quick-progress.pids", nullptr, nullptr, helper_pid,
                   "quick-progress", countfile.c_str(), &stdout_read, "simulate", nullptr,
                   &stderr_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a short progress flush\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits after a short progress burst");
    expect(err.find("PROGRESS 12% [1 linux-image-amd64") != std::string::npos,
           "the first percent line is forwarded");
    expect(err.find("PROGRESS 47% [1 linux-image-amd64") != std::string::npos,
           "the last percent line is forwarded when apt exits inside the hold");
    expect(out.find("STATUS up-to-date\n") != std::string::npos,
           "percent lines do not hide an up-to-date summary");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::string argvfile = std::string(dir) + "/with-size.argv";
    pid_t helper_pid = 0;
    int stdout_read = -1;
    const std::string countfile = std::string(dir) + "/with-size.count";
    if (run_helper(helper, apt, std::string(dir) + "/with-size.pids", nullptr, nullptr, helper_pid,
                   "with-size", countfile.c_str(), &stdout_read, "simulate", nullptr, nullptr,
                   argvfile.c_str()) != 0) {
      std::fprintf(stderr, "failed to spawn helper for download size\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
    const std::string argv_text = read_file_all(argvfile);
    if (stdout_read >= 0)
      close(stdout_read);
    expect(exited, "helper exits after reporting a download size");
    expect(out.find("STATUS upgrades\n") != std::string::npos, "a sized check still lists upgrades");
    expect(out.find("NEED Need to get 0 B/478 B of archives.\n") != std::string::npos,
           "the check reports apt's download size");
    expect(out.find("DISK After this operation, 0 B of additional disk space will be used.\n") !=
               std::string::npos,
           "the check keeps the disk-space line");
    expect(argv_text.find("\n--print-uris\n") != std::string::npos,
           "the size comes from print-uris");
    expect(argv_text.find("\nupgrade\n") != std::string::npos, "print-uris asks apt to upgrade");
    expect(argv_text.find("dist-upgrade") == std::string::npos, "the size request is not dist-upgrade");
    expect(argv_text.find("full-upgrade") == std::string::npos, "the size request is not full-upgrade");
    expect(argv_text.find("\ninstall\n") == std::string::npos, "the size request does not install");
    expect(argv_text.find("\nquiet=0\n") != std::string::npos, "the check's apt update passes quiet=0");
    expect(argv_text.find("\nAPT::Status-Fd=3\n") != std::string::npos,
           "the check asks apt for machine-readable progress");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    const std::vector<std::string> pins = {"foo=2"};
    std::string out;
    bool exited = false;
    if (run_upgrade("postinst-fail", "postinst", pins, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, out, exited) < 0) {
      std::fprintf(stderr, "failed to spawn helper for a postinst failure\n");
      return 1;
    }
    expect(exited, "helper exits after a postinst failure");
    expect(out.find("dpkg: error processing package foo (--configure):") != std::string::npos,
           "a postinst failure keeps dpkg's header");
    expect(out.find("post-installation script subprocess returned error exit status 1") != std::string::npos,
           "a postinst failure keeps the detail line");
    expect(out.find("Errors were encountered while processing: foo") != std::string::npos,
           "a postinst failure names the package that failed");
    expect(out.find("The upgrade stopped with a package left unfinished.") != std::string::npos,
           "a postinst failure says the upgrade stopped unfinished");
    expect(out.find("dpkg --configure -a") == std::string::npos,
           "a script failure is not an interrupted dpkg");
    expect(out.find("Sub-process /usr/bin/dpkg returned an error code") == std::string::npos,
           "the generic dpkg subprocess line is not the message");
  }

  {
    /* A carriage return that holds only spaces must not become the progress
     * line, and an Inst line must not either. The real percentages stay. */
    pid_t helper_pid = 0;
    int stderr_read = -1;
    int stdout_read = -1;
    const std::vector<std::string> pins = {"bash=2"};
    const std::string countfile = std::string(dir) + "/space-clear.count";
    if (run_helper(helper, apt, std::string(dir) + "/space-clear.pids", nullptr, nullptr, helper_pid,
                   "space-clear", countfile.c_str(), &stdout_read, "upgrade", &pins,
                   &stderr_read) != 0) {
      std::fprintf(stderr, "failed to spawn helper for a cleared progress line\n");
      return 1;
    }
    int status = 0;
    const bool exited = wait_pid(helper_pid, 10000, status);
    const std::string err = stderr_read >= 0 ? read_all_fd(stderr_read) : std::string();
    if (stdout_read >= 0)
      close(stdout_read);
    if (stderr_read >= 0)
      close(stderr_read);
    expect(exited, "helper exits after a progress line that apt then clears");
    expect(err.find("PROGRESS 4% [1 xz-utils 13.0 kB/267 kB 5%]\n") != std::string::npos,
           "the first percent line is forwarded");
    expect(err.find("PROGRESS 28% [1 xz-utils 80.0 kB/267 kB 30%]\n") != std::string::npos,
           "a later percent line replaces the cleared one");
    bool blank_progress = false;
    bool inst_progress = false;
    std::istringstream lines(err);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.compare(0, 9, "PROGRESS ") != 0)
        continue;
      const std::string body = line.substr(9);
      if (body.find_first_not_of(" \t\r") == std::string::npos)
        blank_progress = true;
      if (body.compare(0, 5, "Inst ") == 0)
        inst_progress = true;
    }
    expect(!blank_progress, "whitespace progress lines are dropped");
    expect(!inst_progress, "Inst lines are not progress text");
    if (!exited)
      terminate_process_tree(helper_pid, 200);
  }

  {
    /* The install reuses a check whose lists have not changed: no second
     * apt-get update and no second kept-back probe. A changed Release file,
     * or a kept-back set that no longer matches, falls back and says so. */
    const std::string lists = std::string(dir) + "/lists";
    const std::string statedir = std::string(dir) + "/check-state";
    expect(mkdir(lists.c_str(), 0755) == 0, "lists dir");
    expect(mkdir(statedir.c_str(), 0755) == 0, "state dir");
    {
      std::ofstream rel(lists + "/noble_InRelease");
      rel << "Origin: Ubuntu\nLabel: Ubuntu\nSuite: noble\n";
    }
    setenv("LCOS_UPDATES_LISTS_DIR", lists.c_str(), 1);
    setenv("LCOS_UPDATES_STATE_DIR", statedir.c_str(), 1);

    auto run_snap = [&](const char* command, const std::vector<std::string>* pins, const char* tag,
                        std::string& out, std::string& argv_text, std::string& state,
                        bool& exited) -> bool {
      pid_t helper_pid = 0;
      int stdout_read = -1;
      const std::string argvfile = std::string(dir) + "/" + tag + ".argv";
      const std::string statefile = std::string(dir) + "/" + tag + ".state";
      if (run_helper(helper, apt, std::string(dir) + "/" + tag + ".pids", nullptr, nullptr,
                     helper_pid, "reuse-snap", nullptr, &stdout_read, command, pins, nullptr,
                     argvfile.c_str(), nullptr, nullptr, statefile.c_str()) != 0)
        return false;
      int status = 0;
      exited = wait_pid(helper_pid, 10000, status);
      out = stdout_read >= 0 ? read_all_fd(stdout_read) : std::string();
      if (stdout_read >= 0)
        close(stdout_read);
      argv_text = read_file_all(argvfile);
      state = read_file_all(statefile);
      if (!exited)
        terminate_process_tree(helper_pid, 200);
      return true;
    };

    std::string out;
    std::string argv_text;
    std::string state;
    bool exited = false;
    if (!run_snap("simulate", nullptr, "reuse-check", out, argv_text, state, exited)) {
      unsetenv("LCOS_UPDATES_LISTS_DIR");
      unsetenv("LCOS_UPDATES_STATE_DIR");
      std::fprintf(stderr, "failed to spawn helper for a reusable check\n");
      return 1;
    }
    expect(exited, "helper exits after a check that can be reused");
    expect(out.find("SEC openssl\n") != std::string::npos, "security marker from the archive token");
    expect(out.find("REMOVE foo oldplug\n") != std::string::npos,
           "check classified the kept-back row");
    expect(count_lines_equal(argv_text, "update") == 1, "the check still refreshes indexes");
    expect(count_lines_equal(state, "probe") == 1, "the check probed kept-back once");

    const std::vector<std::string> pins = {"bash=2", "openssl=2"};
    if (!run_snap("upgrade", &pins, "reuse-install", out, argv_text, state, exited)) {
      unsetenv("LCOS_UPDATES_LISTS_DIR");
      unsetenv("LCOS_UPDATES_STATE_DIR");
      std::fprintf(stderr, "failed to spawn helper for a reused install\n");
      return 1;
    }
    expect(exited, "helper exits after reusing a check");
    expect(count_lines_equal(argv_text, "update") == 0, "unchanged lists skip apt-get update");
    expect(state.find("probe") == std::string::npos, "a reused classification is not probed again");
    expect(count_lines_equal(state, "install") == 1, "the pinned install still runs");
    expect(out.find("REMOVE foo oldplug\n") != std::string::npos, "the reused removal is reported");
    expect(out.find("STATUS success\n") != std::string::npos, "reused install succeeds");
    expect(out.find("Checking again") == std::string::npos, "a match does not announce a fallback");

    /* Lists still match the snapshot. The kept-back name does not. */
    {
      std::ofstream flag(std::string(dir) + "/reuse-shift.state.mismatch");
      flag << "1\n";
    }
    if (!run_snap("upgrade", &pins, "reuse-shift", out, argv_text, state, exited)) {
      unsetenv("LCOS_UPDATES_LISTS_DIR");
      unsetenv("LCOS_UPDATES_STATE_DIR");
      std::fprintf(stderr, "failed to spawn helper for a changed kept-back set\n");
      return 1;
    }
    expect(exited, "helper exits after a kept-back set that no longer matches");
    expect(out.find("WARN Package lists changed since the check. Checking again.\n") !=
               std::string::npos,
           "a different kept-back set is not reused");
    expect(count_lines_equal(argv_text, "update") >= 1, "a different kept-back set refreshes indexes");
    expect(count_lines_equal(state, "probe") == 1, "the new kept-back name is probed");
    std::remove((std::string(dir) + "/reuse-shift.state.mismatch").c_str());

    {
      std::ofstream rel(lists + "/noble_InRelease", std::ios::app);
      rel << "Date: later\n";
    }
    if (!run_snap("upgrade", &pins, "reuse-changed", out, argv_text, state, exited)) {
      unsetenv("LCOS_UPDATES_LISTS_DIR");
      unsetenv("LCOS_UPDATES_STATE_DIR");
      std::fprintf(stderr, "failed to spawn helper for changed package lists\n");
      return 1;
    }
    expect(exited, "helper exits after package lists change");
    expect(count_lines_equal(argv_text, "update") >= 1, "changed lists refresh indexes again");
    expect(out.find("WARN Package lists changed since the check. Checking again.\n") !=
               std::string::npos,
           "the fallback is announced");
    expect(count_lines_equal(state, "probe") == 1, "a changed classification is probed");

    unsetenv("LCOS_UPDATES_LISTS_DIR");
    unsetenv("LCOS_UPDATES_STATE_DIR");
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all helper kill tests passed\n");
  return 0;
}
