/* lcos-updates-helper — pkexec-only apt-get update / simulate / upgrade.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv is only "simulate" or "upgrade". Hard-coded /usr/bin/apt-get. No shell.
 */

#include "apt-parse.hpp"
#include "proc-group.hpp"
#include "timeouts.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
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

volatile sig_atomic_t g_stop = 0;
int g_cancel_fd = -1;

void on_stop_signal(int /*sig*/)
{
  g_stop = 1;
}

const char* apt_get_path()
{
  /* pkexec clears the environment, so this override cannot be injected into
   * the root helper. Direct non-root runs use it to test process-group kill. */
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

int run_apt(const std::vector<const char*>& args, int timeout_sec, std::string& out,
            std::string& err)
{
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
    return -1;

  const pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    /* Own process group so a timeout can signal apt-get and dpkg together
     * without signaling this helper. */
    if (setpgid(0, 0) != 0)
      _exit(127);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
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
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    setenv("DEBIAN_FRONTEND", "noninteractive", 1);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const char* a : args)
      argv.push_back(const_cast<char*>(a));
    argv.push_back(nullptr);
    execv(args[0], argv.data());
    _exit(127);
  }

  setpgid(pid, pid);
  close(out_pipe[1]);
  close(err_pipe[1]);
  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  bool out_open = true;
  bool err_open = true;
  bool timed_out = false;
  bool cancelled = false;
  char buf[4096];
  struct timespec start {};
  clock_gettime(CLOCK_MONOTONIC, &start);

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

  while (out_open || err_open) {
    if (stop_requested()) {
      cancelled = true;
      break;
    }
    const int remain = remaining_ms();
    if (remain <= 0) {
      timed_out = true;
      break;
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
    if (g_cancel_fd >= 0) {
      cancel_i = static_cast<int>(nfd);
      fds[nfd].fd = g_cancel_fd;
      fds[nfd].events = POLLIN | POLLHUP | POLLERR;
      fds[nfd].revents = 0;
      nfd++;
    }
    const int slice = remain > 200 ? 200 : remain;
    const int pr = poll(fds, nfd, slice);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (cancel_i >= 0 && (fds[cancel_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      if (stop_requested()) {
        cancelled = true;
        break;
      }
    }
    auto drain = [&](int idx, int fd, bool& open_flag, std::string& dest) {
      if (idx < 0)
        return;
      if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
        for (;;) {
          const ssize_t n = read(fd, buf, sizeof buf);
          if (n > 0) {
            dest.append(buf, static_cast<size_t>(n));
            continue;
          }
          if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            open_flag = false;
          break;
        }
      }
    };
    drain(out_i, out_pipe[0], out_open, out);
    drain(err_i, err_pipe[0], err_open, err);
  }

  if (timed_out || cancelled || stop_requested()) {
    if (stop_requested())
      cancelled = true;
    /* Apt-get and dpkg are gone before the caller reports failure. */
    terminate_process_tree(pid, kTermGraceMs);
    close(out_pipe[0]);
    close(err_pipe[0]);
    if (timed_out && !cancelled)
      return -2;
    return -3;
  }

  close(out_pipe[0]);
  close(err_pipe[0]);
  int status = 0;
  for (;;) {
    const pid_t got = waitpid(pid, &status, 0);
    if (got == pid)
      break;
    if (got < 0 && errno == EINTR) {
      if (stop_requested()) {
        terminate_process_tree(pid, kTermGraceMs);
        return -3;
      }
      continue;
    }
    return -1;
  }
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

int do_simulate()
{
  if (stop_requested()) {
    emit_error("Update check was cancelled");
    return 1;
  }
  announce_ready();

  const char* apt = apt_get_path();
  const std::vector<const char*> update_argv = {apt, kSimNoteOpt, kSimNoteVal, "update"};
  std::string out;
  std::string err;
  int rc = run_apt(update_argv, effective_timeout(kUpdateTimeoutSec), out, err);
  if (rc == -3 || stop_requested()) {
    emit_error("Update check was cancelled");
    return 1;
  }
  if (rc == -2) {
    emit_error("Timed out while running apt-get update");
    return 1;
  }
  if (rc != 0) {
    emit_error(first_error_line(out, err));
    return 1;
  }

  out.clear();
  err.clear();
  const std::vector<const char*> sim_argv = {apt, kSimNoteOpt, kSimNoteVal, "-s", "-q", "upgrade"};
  rc = run_apt(sim_argv, effective_timeout(kSimulateTimeoutSec), out, err);
  if (rc == -3 || stop_requested()) {
    emit_error("Update check was cancelled");
    return 1;
  }
  if (rc == -2) {
    emit_error("Timed out while simulating apt-get upgrade");
    return 1;
  }
  if (rc != 0) {
    emit_error(first_error_line(out, err));
    return 1;
  }

  SimulateResult result = parse_apt_simulate(out + "\n" + err);
  emit(result);
  return result.status == SimulateResult::Error ? 1 : 0;
}

int do_upgrade()
{
  if (stop_requested()) {
    emit_error("Install was cancelled");
    return 1;
  }
  announce_ready();

  /* Keep existing conffiles; never block on TTY conffile prompts (Plymouth policy). */
  const char* apt = apt_get_path();
  const std::vector<const char*> up_argv = {
      apt, kSimNoteOpt, kSimNoteVal, "-y",
      "-o", "Dpkg::Options::=--force-confdef",
      "-o", "Dpkg::Options::=--force-confold",
      "upgrade"};
  std::string out;
  std::string err;
  const int rc = run_apt(up_argv, effective_timeout(kUpgradeTimeoutSec), out, err);
  if (rc == -3 || stop_requested()) {
    emit_error("Install was cancelled");
    return 1;
  }
  if (rc == -2) {
    emit_error("Timed out while installing updates");
    return 1;
  }
  if (rc != 0) {
    emit_error(first_error_line(out, err));
    return 1;
  }
  SimulateResult result;
  result.status = SimulateResult::Success;
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

  if (argc != 2 || argv[1] == nullptr) {
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
  if (cmd == "simulate")
    return do_simulate();
  if (cmd == "upgrade")
    return do_upgrade();
  emit_error("refused: helper accepts only simulate or upgrade");
  return 2;
}
