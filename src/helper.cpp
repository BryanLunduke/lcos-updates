/* lcos-updates-helper — pkexec-only apt-get update / simulate / upgrade.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv is "simulate", or "upgrade" plus the reviewed name=version pins.
 * Hard-coded /usr/bin/apt-get. No shell.
 */

#include "apt-parse.hpp"
#include "proc-group.hpp"
#include "timeouts.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

const char kAptGet[] = "/usr/bin/apt-get";
const char kSimNoteOpt[] = "-o";
const char kSimNoteVal[] = "APT::Get::Show-User-Simulation-Note=false";
const char kConfdef[] = "Dpkg::Options::=--force-confdef";
const char kConfold[] = "Dpkg::Options::=--force-confold";
const char kDpkgRecovery[] =
    "The package installer was interrupted or is still running. "
    "Run dpkg --configure -a to finish configuring packages.";
const char kSafePath[] = "/usr/sbin:/usr/bin:/sbin:/bin";

volatile sig_atomic_t g_stop = 0;
int g_cancel_fd = -1;

void on_stop_signal(int /*sig*/)
{
  g_stop = 1;
}

const char* apt_get_path()
{
  /* The override is ignored when this process is root, so pkexec cannot be
   * pointed at a caller-supplied apt. Direct non-root runs use it for tests. */
  if (geteuid() != 0) {
    const char* env = std::getenv("LCOS_UPDATES_APT_GET");
    if (env != nullptr && env[0] == '/')
      return env;
  }
  return kAptGet;
}

int effective_timeout(int fallback)
{
  if (geteuid() == 0)
    return fallback;
  const char* env = std::getenv("LCOS_UPDATES_TIMEOUT_SEC");
  if (env == nullptr || env[0] == '\0')
    return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(env, &end, 10);
  if (end == env || *end != '\0' || parsed < 1 || parsed > 30)
    return fallback;
  return static_cast<int>(parsed);
}

void write_all_fd(int fd, const std::string& s)
{
  const char* p = s.data();
  size_t left = s.size();
  while (left > 0) {
    const ssize_t n = write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
}

void write_all_stdout(const std::string& s)
{
  write_all_fd(STDOUT_FILENO, s);
}

void announce_ready()
{
  write_all_fd(STDERR_FILENO, "HELPER_READY\n");
}

void emit(const SimulateResult& result)
{
  write_all_stdout(format_protocol(result));
}

void emit_error(const std::string& msg)
{
  SimulateResult r;
  r.status = SimulateResult::Error;
  r.error_msg = msg;
  emit(r);
}

bool stop_requested()
{
  if (g_stop)
    return true;
  if (g_cancel_fd < 0)
    return false;
  pollfd pfd {};
  pfd.fd = g_cancel_fd;
  pfd.events = POLLIN | POLLHUP | POLLERR;
  const int pr = poll(&pfd, 1, 0);
  if (pr < 0)
    return false;
  if (pr == 0)
    return false;
  char buf[256];
  const ssize_t n = read(g_cancel_fd, buf, sizeof buf);
  if (n == 0 || (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
    return true;
  return n > 0;
}

std::string saved_env(const char* key)
{
  const char* value = std::getenv(key);
  return value == nullptr ? std::string() : std::string(value);
}

void restore_env(const char* key, const std::string& value)
{
  if (!value.empty())
    setenv(key, value.c_str(), 1);
}

/* watch_configure is false for apt-get update and for simulations. A
 * configuring dpkg latches only for the install invocation, and only while
 * that dpkg is still alive. */
int run_apt(const std::vector<const char*>& args, int timeout_sec, std::string& out,
            std::string& err, bool watch_configure)
{
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0)
    return -1;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return -1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    /* Own process group so a timeout can signal apt-get and its descendants
     * without signaling this helper. Descendants are signalled by pid. */
    if (setpgid(0, 0) != 0)
      _exit(127);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    std::vector<std::string> saved_args;
    saved_args.reserve(args.size());
    for (const char* arg : args) {
      if (arg == nullptr)
        break;
      saved_args.emplace_back(arg);
    }
    if (saved_args.empty())
      _exit(127);
    /* Test seams. Copied before clearenv and restored only when not root. */
    const std::string stub_script = saved_env("LCOS_STUB_SCRIPT");
    const std::string stub_pid = saved_env("LCOS_STUB_PIDFILE");
    const std::string stub_count = saved_env("LCOS_STUB_COUNTFILE");
    const std::string stub_argv = saved_env("LCOS_STUB_ARGVFILE");
    const std::string stub_env = saved_env("LCOS_STUB_ENVFILE");
    const int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    if (clearenv() != 0)
      _exit(127);
    if (setenv("PATH", kSafePath, 1) != 0 || setenv("LANG", "C.UTF-8", 1) != 0 ||
        setenv("LC_ALL", "C.UTF-8", 1) != 0 || setenv("DEBIAN_FRONTEND", "noninteractive", 1) != 0)
      _exit(127);
    if (geteuid() == 0) {
      setenv("HOME", "/root", 1);
    } else {
      restore_env("LCOS_STUB_SCRIPT", stub_script);
      restore_env("LCOS_STUB_PIDFILE", stub_pid);
      restore_env("LCOS_STUB_COUNTFILE", stub_count);
      restore_env("LCOS_STUB_ARGVFILE", stub_argv);
      restore_env("LCOS_STUB_ENVFILE", stub_env);
    }
    std::vector<char*> argv;
    argv.reserve(saved_args.size() + 1);
    for (std::string& arg : saved_args)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    execv(saved_args[0].c_str(), argv.data());
    _exit(127);
  }

  setpgid(pid, pid);
  close(out_pipe[1]);
  close(err_pipe[1]);
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  CaptureBuf out_cap;
  CaptureBuf err_cap;
  auto publish = [&]() {
    out = capture_text(out_cap);
    err = capture_text(err_cap);
  };

  bool out_open = true;
  bool err_open = true;
  bool timed_out = false;
  bool cancelled = false;
  bool configuring = false;
  bool interrupted = false;
  char buf[4096];
  struct timespec start {};
  clock_gettime(CLOCK_MONOTONIC, &start);
  struct timespec last_scan {};
  bool did_scan = false;

  auto remaining_ms = [&]() -> int {
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long elapsed = (now.tv_sec - start.tv_sec) * 1000L +
                         (now.tv_nsec - start.tv_nsec) / 1000000L;
    const long remain = static_cast<long>(timeout_sec) * 1000L - elapsed;
    if (remain <= 0)
      return 0;
    if (remain > 1000000000L)
      return 1000000000;
    return static_cast<int>(remain);
  };

  struct timespec cfg_began {};
  /* Pause the apt deadline while a configuring dpkg is alive, then apply
   * whatever time is left once that dpkg has left the tree. */
  auto publish_cfg = [&](bool now_cfg) {
    if (now_cfg == configuring)
      return;
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now_cfg) {
      cfg_began = now;
    } else {
      start.tv_sec += now.tv_sec - cfg_began.tv_sec;
      start.tv_nsec += now.tv_nsec - cfg_began.tv_nsec;
      if (start.tv_nsec >= 1000000000L) {
        start.tv_sec += start.tv_nsec / 1000000000L;
        start.tv_nsec %= 1000000000L;
      } else if (start.tv_nsec < 0) {
        const long borrow = (-start.tv_nsec + 999999999L) / 1000000000L;
        start.tv_sec -= borrow;
        start.tv_nsec += borrow * 1000000000L;
      }
    }
    configuring = now_cfg;
    write_all_fd(STDERR_FILENO, configuring ? "DPKG_STARTED\n" : "DPKG_IDLE\n");
  };

  /* /proc is walked at most once a second unless we are about to signal. */
  auto scan_configure = [&](bool force) {
    if (!watch_configure)
      return;
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!force && did_scan) {
      const long ms = (now.tv_sec - last_scan.tv_sec) * 1000L +
                      (now.tv_nsec - last_scan.tv_nsec) / 1000000L;
      if (ms < 1000)
        return;
    }
    did_scan = true;
    last_scan = now;
    publish_cfg(process_tree_contains_dpkg(pid));
  };

  auto reap_after_kill = [&](int code) -> int {
    if (out_open)
      close(out_pipe[0]);
    if (err_open)
      close(err_pipe[0]);
    out_open = false;
    err_open = false;
    for (;;) {
      int status = 0;
      const pid_t got = waitpid(pid, &status, 0);
      if (got == pid || (got < 0 && errno == ECHILD)) {
        publish();
        return code;
      }
      if (got < 0 && errno == EINTR)
        continue;
      publish();
      return code;
    }
  };

  for (;;) {
    while (out_open || err_open) {
      scan_configure(false);
      /* Latch cancel even while configuring, but do not act on it until that
       * dpkg has left. The deadline is paused for the same interval. */
      if (stop_requested())
        cancelled = true;
      if (!configuring) {
        if (cancelled)
          break;
        if (remaining_ms() <= 0) {
          timed_out = true;
          break;
        }
      }
      pollfd fds[3];
      nfds_t nfd = 0;
      int out_i = -1;
      int err_i = -1;
      int cancel_i = -1;
      if (out_open) {
        out_i = static_cast<int>(nfd);
        fds[nfd].fd = out_pipe[0];
        fds[nfd].events = POLLIN | POLLHUP | POLLERR;
        fds[nfd].revents = 0;
        nfd++;
      }
      if (err_open) {
        err_i = static_cast<int>(nfd);
        fds[nfd].fd = err_pipe[0];
        fds[nfd].events = POLLIN | POLLHUP | POLLERR;
        fds[nfd].revents = 0;
        nfd++;
      }
      if (!configuring && g_cancel_fd >= 0) {
        cancel_i = static_cast<int>(nfd);
        fds[nfd].fd = g_cancel_fd;
        fds[nfd].events = POLLIN | POLLHUP | POLLERR;
        fds[nfd].revents = 0;
        nfd++;
      }
      int slice = 1000;
      if (!configuring) {
        const int remain = remaining_ms();
        if (remain <= 0) {
          timed_out = true;
          break;
        }
        slice = remain > 200 ? 200 : remain;
      }
      const int pr = poll(fds, nfd, slice);
      if (pr < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (cancel_i >= 0 && (fds[cancel_i].revents & (POLLIN | POLLHUP | POLLERR))) {
        scan_configure(true);
        if (!configuring && stop_requested()) {
          cancelled = true;
          break;
        }
      }
      auto drain = [&](int idx, int fd, bool& open_flag, CaptureBuf& dest) {
        if (idx < 0)
          return;
        if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
          for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) {
              capture_append(dest, buf, static_cast<std::size_t>(n));
              continue;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
              open_flag = false;
            break;
          }
        }
      };
      drain(out_i, out_pipe[0], out_open, out_cap);
      drain(err_i, err_pipe[0], err_open, err_cap);
    }

    if (stop_requested())
      cancelled = true;
    if (!(timed_out || cancelled))
      break;
    scan_configure(true);
    if (configuring) {
      /* Do not close apt's pipes and do not return. Wait until apt exits.
       * A cancel stays latched so the result is still a cancel, not success. */
      timed_out = false;
      if (!out_open && !err_open)
        break;
      continue;
    }
    bool signaled = false;
    const bool gone = terminate_process_tree(pid, kTermGraceMs, &signaled);
    if (!gone) {
      /* Configuring dpkg is in the tree. Keep the pipes open so apt does not
       * take SIGPIPE, and leave this process alive until apt itself exits. */
      if (signaled)
        interrupted = true;
      scan_configure(true);
      timed_out = false;
      if (!configuring)
        poll(nullptr, 0, 200);
      if (!out_open && !err_open)
        break;
      continue;
    }
    if (interrupted)
      return reap_after_kill(-5);
    return reap_after_kill(timed_out && !cancelled ? -2 : -3);
  }

  if (out_open)
    close(out_pipe[0]);
  if (err_open)
    close(err_pipe[0]);
  out_open = false;
  err_open = false;
  int status = 0;
  for (;;) {
    const pid_t got = waitpid(pid, &status, 0);
    if (got == pid)
      break;
    if (got < 0 && errno == EINTR) {
      if (!configuring && stop_requested()) {
        cancelled = true;
        scan_configure(true);
        if (!configuring) {
          bool signaled = false;
          const bool gone = terminate_process_tree(pid, kTermGraceMs, &signaled);
          if (!gone) {
            if (signaled)
              interrupted = true;
            continue;
          }
          publish();
          return interrupted ? -5 : -3;
        }
      }
      continue;
    }
    if (got < 0 && errno == ECHILD)
      break;
    publish();
    return -1;
  }
  publish();
  if (interrupted)
    return -5;
  if (cancelled)
    return -3;
  if (timed_out)
    return -2;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

std::string first_error_line(const std::string& out, const std::string& err)
{
  const std::string text = err.empty() ? out : err + "\n" + out;
  std::istringstream in(text);
  std::string line;
  std::string first_e;
  std::string first_err;
  std::string last;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    last = line;
    if (first_e.empty() && line.compare(0, 2, "E:") == 0)
      first_e = line;
    if (first_err.empty() && line.compare(0, 4, "Err:") == 0)
      first_err = line;
  }
  if (!first_e.empty())
    return first_e;
  if (!first_err.empty())
    return first_err;
  if (!last.empty())
    return last;
  return "apt-get failed";
}

/* Returns false when the caller should already have emitted an error and
 * should return 1. A partial index failure sets warning and returns true
 * so the caller still simulates or upgrades. Update never latches dpkg. */
bool refresh_package_lists(const char* cancel_msg, const char* timeout_msg, std::string& warning)
{
  const std::string apt = apt_get_path();
  const std::vector<const char*> update_argv = {apt.c_str(), kSimNoteOpt, kSimNoteVal, "update"};
  std::string out;
  std::string err;
  const int rc = run_apt(update_argv, effective_timeout(kUpdateTimeoutSec), out, err, false);
  if (rc == -5) {
    emit_error(kDpkgRecovery);
    return false;
  }
  if (rc == -3) {
    emit_error(cancel_msg);
    return false;
  }
  if (rc == -2) {
    emit_error(timeout_msg);
    return false;
  }
  if (rc != 0) {
    const std::string blob = err + "\n" + out;
    if (apt_index_failure_is_partial(blob)) {
      warning = apt_index_warning(blob);
      if (warning.empty())
        warning = first_error_line(out, err);
      return true;
    }
    emit_error(first_error_line(out, err));
    return false;
  }
  return true;
}

bool valid_pin(const std::string& pin)
{
  const std::string::size_type eq = pin.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= pin.size())
    return false;
  if (pin.find('=', eq + 1) != std::string::npos)
    return false;
  const std::string name = pin.substr(0, eq);
  const std::string ver = pin.substr(eq + 1);
  if (name.empty() || ver.empty() || name.size() > 200 || ver.size() > 200)
    return false;
  auto name_char = [](unsigned char c) {
    return std::islower(c) || std::isdigit(c);
  };
  if (!name_char(static_cast<unsigned char>(name[0])))
    return false;
  for (char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (name_char(c) || c == '+' || c == '-' || c == '.')
      continue;
    return false;
  }
  if (!std::isdigit(static_cast<unsigned char>(ver[0])))
    return false;
  int colons = 0;
  for (char ch : ver) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == ':')
      ++colons;
    if (std::isalnum(c) || c == '.' || c == '+' || c == '~' || c == '-' || c == ':')
      continue;
    return false;
  }
  return colons <= 1;
}

bool same_package_set(const std::vector<PackageUpgrade>& packages, const std::vector<std::string>& pins)
{
  if (packages.size() != pins.size())
    return false;
  std::vector<std::string> got;
  got.reserve(packages.size());
  for (const auto& pkg : packages)
    got.push_back(pkg.name + "=" + pkg.new_version);
  std::vector<std::string> want = pins;
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  return got == want;
}

int fail_apt(int rc, const std::string& out, const std::string& err, const char* cancel_msg,
             const char* timeout_msg, const std::string& update_warning)
{
  if (rc == -5) {
    emit_error(kDpkgRecovery);
    return 1;
  }
  if (rc == -3) {
    emit_error(cancel_msg);
    return 1;
  }
  if (rc == -2) {
    emit_error(timeout_msg);
    return 1;
  }
  if (rc != 0) {
    std::string msg = first_error_line(out, err);
    if (!update_warning.empty())
      msg = update_warning + "\n" + msg;
    emit_error(msg);
    return 1;
  }
  return 0;
}

int do_simulate()
{
  if (stop_requested()) {
    emit_error("Update check was cancelled");
    return 1;
  }
  announce_ready();

  std::string update_warning;
  if (!refresh_package_lists("Update check was cancelled",
                             "Timed out while running apt-get update", update_warning))
    return 1;
  if (stop_requested()) {
    emit_error("Update check was cancelled");
    return 1;
  }

  const std::string apt = apt_get_path();
  std::string out;
  std::string err;
  const std::vector<const char*> sim_argv = {apt.c_str(), kSimNoteOpt, kSimNoteVal, "-s", "-q",
                                             "upgrade"};
  const int rc = run_apt(sim_argv, effective_timeout(kSimulateTimeoutSec), out, err, false);
  if (fail_apt(rc, out, err, "Update check was cancelled",
               "Timed out while simulating apt-get upgrade", update_warning) != 0)
    return 1;

  SimulateResult result = parse_apt_simulate(out + "\n" + err);
  if (!update_warning.empty())
    result.warning = update_warning;
  emit(result);
  return result.status == SimulateResult::Error ? 1 : 0;
}

int do_upgrade(const std::vector<std::string>& pins)
{
  if (stop_requested()) {
    emit_error("Install was cancelled");
    return 1;
  }
  announce_ready();

  std::string update_warning;
  if (!refresh_package_lists("Install was cancelled",
                             "Timed out while refreshing package lists", update_warning))
    return 1;
  if (stop_requested()) {
    emit_error("Install was cancelled");
    return 1;
  }

  /* Confirm the reviewed set against the indexes just fetched. A different
   * set is returned to the window and nothing is installed. */
  const std::string apt = apt_get_path();
  std::string out;
  std::string err;
  const std::vector<const char*> sim_argv = {apt.c_str(), kSimNoteOpt, kSimNoteVal, "-s", "-q",
                                             "upgrade"};
  int rc = run_apt(sim_argv, effective_timeout(kSimulateTimeoutSec), out, err, false);
  if (fail_apt(rc, out, err, "Install was cancelled",
               "Timed out while simulating apt-get upgrade", update_warning) != 0)
    return 1;

  SimulateResult sim = parse_apt_simulate(out + "\n" + err);
  if (sim.status == SimulateResult::Error) {
    std::string msg = sim.error_msg.empty() ? first_error_line(out, err) : sim.error_msg;
    if (!update_warning.empty())
      msg = update_warning + "\n" + msg;
    emit_error(msg);
    return 1;
  }
  if (!same_package_set(sim.packages, pins)) {
    sim.install_skipped = true;
    const std::string note =
        "The package list changed after refreshing indexes. Nothing was installed.";
    if (!update_warning.empty())
      sim.warning = update_warning + "\n" + note;
    else
      sim.warning = note;
    emit(sim);
    return 0;
  }

  /* Keep existing conffiles; never block on TTY conffile prompts.
   * Install only the reviewed name=version pins. Not dist-upgrade. */
  std::vector<std::string> argv_store;
  argv_store.push_back(apt);
  argv_store.push_back(kSimNoteOpt);
  argv_store.push_back(kSimNoteVal);
  argv_store.push_back("-y");
  argv_store.push_back("-o");
  argv_store.push_back(kConfdef);
  argv_store.push_back("-o");
  argv_store.push_back(kConfold);
  argv_store.push_back("install");
  argv_store.push_back("--only-upgrade");
  for (const auto& pin : pins)
    argv_store.push_back(pin);
  std::vector<const char*> up_argv;
  up_argv.reserve(argv_store.size());
  for (const auto& arg : argv_store)
    up_argv.push_back(arg.c_str());

  out.clear();
  err.clear();
  rc = run_apt(up_argv, effective_timeout(kUpgradeTimeoutSec), out, err, true);
  if (fail_apt(rc, out, err, "Install was cancelled", "Timed out while installing updates",
               update_warning) != 0)
    return 1;

  const SimulateResult parsed = parse_apt_simulate(out + "\n" + err);
  SimulateResult result;
  const bool kept = !parsed.kept_back.empty() || parsed.not_upgraded_count > 0;
  if (kept && parsed.upgraded_count <= 0 && parsed.packages.empty())
    result.status = SimulateResult::KeptBack;
  else
    result.status = SimulateResult::Success;
  result.kept_back = parsed.kept_back;
  result.not_upgraded_count = parsed.not_upgraded_count;
  result.conffiles_kept = parsed.conffiles_kept;
  result.warning = update_warning;
  emit(result);
  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa {};
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  if (argc < 2 || argv[1] == nullptr) {
    emit_error("refused: helper accepts only simulate or upgrade");
    return 2;
  }

  struct stat st {};
  if (fstat(STDIN_FILENO, &st) == 0 && S_ISFIFO(st.st_mode)) {
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0)
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    g_cancel_fd = STDIN_FILENO;
  }

  const std::string cmd = argv[1];
  if (cmd == "simulate") {
    if (argc != 2) {
      emit_error("refused: helper accepts only simulate or upgrade");
      return 2;
    }
    return do_simulate();
  }
  if (cmd == "upgrade") {
    std::vector<std::string> pins;
    for (int i = 2; i < argc; ++i) {
      if (argv[i] == nullptr || !valid_pin(argv[i])) {
        emit_error("refused: invalid package pin");
        return 2;
      }
      pins.emplace_back(argv[i]);
    }
    if (pins.empty()) {
      emit_error("refused: upgrade requires the reviewed package list");
      return 2;
    }
    if (pins.size() > 4096) {
      emit_error("refused: too many packages");
      return 2;
    }
    std::vector<std::string> sorted = pins;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
      emit_error("refused: invalid package pin");
      return 2;
    }
    return do_upgrade(pins);
  }
  emit_error("refused: helper accepts only simulate or upgrade");
  return 2;
}
