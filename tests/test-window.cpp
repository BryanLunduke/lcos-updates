/* GUI tests for pkexec spawn, timers, cancel, and install wording.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs under xvfb-run. A fake pkexec stands in for /usr/bin/pkexec via
 * LCOS_UPDATES_TEST_PKEXEC, which exists only in this -DLCOS_UPDATES_TEST
 * binary.
 */

#include "window.hpp"

#include <gtk/gtk.h>
#include <gtkmm/application.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

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

static bool contains(const Glib::ustring& text, const char* needle)
{
  return std::string(text).find(needle) != std::string::npos;
}

static void pump_for(int ms)
{
  const gint64 end = g_get_monotonic_time() + static_cast<gint64>(ms) * 1000;
  while (g_get_monotonic_time() < end) {
    g_main_context_iteration(nullptr, false);
    g_usleep(5000);
  }
}

static bool pump_until(int ms, const std::function<bool()>& pred)
{
  const gint64 end = g_get_monotonic_time() + static_cast<gint64>(ms) * 1000;
  while (g_get_monotonic_time() < end) {
    if (pred())
      return true;
    g_main_context_iteration(nullptr, false);
    g_usleep(5000);
  }
  return pred();
}

static void set_timeout_ms(int ms)
{
  const std::string text = std::to_string(ms);
  setenv("LCOS_UPDATES_GUI_TIMEOUT_MS", text.c_str(), 1);
}

static void set_mode(const char* mode)
{
  setenv("LCOS_PKEXEC_MODE", mode, 1);
}

static int count_lines(const std::string& path)
{
  std::ifstream in(path);
  int n = 0;
  std::string line;
  while (std::getline(in, line))
    ++n;
  return n;
}

static pid_t read_pid(const std::string& path)
{
  std::ifstream in(path);
  int pid = 0;
  if (!(in >> pid))
    return 0;
  return static_cast<pid_t>(pid);
}

static bool alive(pid_t pid)
{
  if (pid <= 1)
    return false;
  return kill(pid, 0) == 0;
}

static void kill_pidfile(const std::string& path)
{
  const pid_t pid = read_pid(path);
  if (pid <= 1)
    return;
  kill(pid, SIGKILL);
  kill(-pid, SIGKILL);
}

static std::string slurp(const std::string& path)
{
  std::ifstream in(path);
  std::string out;
  std::string line;
  while (std::getline(in, line)) {
    out += line;
    out += "\n";
  }
  return out;
}

static UpdatesWindow* new_window()
{
  auto* window = new UpdatesWindow(false);
  window->show();
  pump_for(30);
  return window;
}

static void destroy_window(UpdatesWindow* window, const std::string& pidfile)
{
  delete window;
  pump_for(40);
  kill_pidfile(pidfile);
}

static const char kPkexecScript[] =
    R"PK(#!/bin/bash
# Fake pkexec. argv: $0 script, $1 helper, $2 simulate|upgrade, then pins.
mode="${LCOS_PKEXEC_MODE:-ready-exit}"
log="${LCOS_PKEXEC_LOG:-/tmp/lcos-pkexec.log}"
pidfile="${LCOS_PKEXEC_PIDFILE:-}"
release="${LCOS_PKEXEC_RELEASE:-/tmp/lcos-no-release}"
cmd="${2:-}"
{
  printf '%s' "$$"
  printf ' %s' "$@"
  printf '\n'
} >> "$log"
if [ -n "$pidfile" ]; then
  printf '%s\n' "$$" > "$pidfile"
fi

wait_sec() {
  # bash: a failed if-test leaves $? as 0, so capture read's status directly.
  # Timeout is >128. EOF or a real line ends the fake pkexec.
  local rc=0
  read -r -t "$1" _ || rc=$?
  if [ "$rc" -le 128 ]; then
    exit 0
  fi
}

case "$mode" in
  delay-ready)
    wait_sec 0.30
    echo HELPER_READY >&2
    wait_sec 0.40
    printf 'STATUS up-to-date\n'
    ;;
  split-ready)
    wait_sec 0.40
    printf 'HELP' >&2
    wait_sec 0.10
    printf 'ER_READY\n' >&2
    wait_sec 30
    printf 'STATUS up-to-date\n'
    ;;
  dpkg-then-idle)
    echo HELPER_READY >&2
    echo DPKG_STARTED >&2
    wait_sec 2
    echo DPKG_IDLE >&2
    wait_sec 30
    printf 'STATUS up-to-date\n'
    ;;
  term-exit)
    trap 'exit 0' TERM
    wait_sec 60
    ;;
  dismiss)
    echo 'Request dismissed' >&2
    exit 126
    ;;
  exit127)
    exit 127
    ;;
  list-or-install)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\nCONFKEPT /etc/ssh/sshd_config\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  list-or-changed)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 9\nSKIPPED\n'
      printf 'WARN The package list changed after refreshing indexes. Nothing was installed.\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  dpkg-hold)
    echo HELPER_READY >&2
    echo DPKG_STARTED >&2
    while [ ! -e "$release" ]; do
      wait_sec 0.05
    done
    echo DPKG_IDLE >&2
    printf 'STATUS up-to-date\n'
    ;;
  huge)
    echo HELPER_READY >&2
    python3 -c 'import sys; sys.stdout.buffer.write(b"x" * (2 * 1024 * 1024))'
    printf '\nSTATUS error\nMSG capped-ok\n'
    ;;
  agent)
    echo 'Error executing command as another user: No authentication agent found.' >&2
    exit 127
    ;;
  notauth)
    echo 'Not authorized' >&2
    exit 126
    ;;
  never-ready)
    wait_sec 60
    ;;
  hang-check)
    echo HELPER_READY >&2
    echo 'PHASE refresh' >&2
    wait_sec 60
    ;;
  hang-install)
    echo HELPER_READY >&2
    echo 'PHASE download' >&2
    wait_sec 60
    ;;
  many-lines)
    echo HELPER_READY >&2
    printf 'STATUS error\n'
    i=1
    while [ "$i" -le 26 ]; do
      printf 'MSG E: Failed to fetch http://mirror%s.example/debian/dists/stable/main/binary-amd64/Packages.xz 404 Not Found\n' "$i"
      i=$((i + 1))
    done
    ;;
  phased)
    echo HELPER_READY >&2
    printf 'STATUS kept-back\nPHASED shim-signed\nPHASED grub-efi-amd64-signed\n'
    ;;
  held)
    echo HELPER_READY >&2
    printf 'STATUS kept-back\nHELD vim\n'
    ;;
  mixed-kept)
    echo HELPER_READY >&2
    printf 'STATUS kept-back\nKEPT linux-image-amd64\nPHASED firefox\nHELD vim\n'
    ;;
  warn-uptodate)
    echo HELPER_READY >&2
    printf 'STATUS up-to-date\n'
    printf 'WARN E: Failed to fetch http://deb.example/InRelease  Connection timed out\n'
    printf 'WARN E: Some index files failed to download. They have been ignored, or old ones used instead.\n'
    ;;
  summary-missing)
    echo HELPER_READY >&2
    printf 'STATUS success\nSUMMARY_MISSING\n'
    ;;
  phases)
    echo HELPER_READY >&2
    echo 'PHASE refresh' >&2
    wait_sec 0.25
    echo 'PROGRESS Hit:1 http://deb.example stable InRelease' >&2
    wait_sec 0.25
    printf 'STATUS up-to-date\n'
    ;;
  list-or-hang)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      wait_sec 60
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  list-then-reboot)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\nREBOOT linux-image-amd64\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  list-then-commit)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      echo DPKG_STARTED >&2
      while [ ! -e "$release" ]; do
        wait_sec 0.05
      done
      echo DPKG_IDLE >&2
      printf 'STATUS success\nCONFKEPT /etc/ssh/sshd_config\nREBOOT linux-image-amd64\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  *)
    echo HELPER_READY >&2
    printf 'STATUS up-to-date\n'
    ;;
esac
)PK";

int main()
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  int argc = 1;
  char arg0[] = "test-window";
  char* argv[] = {arg0, nullptr};
  char** argv_ptr = argv;
  /* gtkmm's wrap table is not set up by gtk_init alone. */
  const Glib::RefPtr<Gtk::Application> app =
      Gtk::Application::create(argc, argv_ptr, "org.lunduke.LcosUpdates.Test");
  if (!app) {
    std::fprintf(stderr, "Gtk::Application::create failed (no DISPLAY?)\n");
    return 1;
  }

  char tmpl[] = "/tmp/lcos-win-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  const std::string dir = tmpl;
  const std::string script = dir + "/pkexec";
  const std::string log = dir + "/pkexec.log";
  const std::string pidfile = dir + "/pkexec.pid";
  const std::string pidfile2 = dir + "/pkexec2.pid";
  const std::string release = dir + "/release";

  {
    std::ofstream out(script);
    out << kPkexecScript;
  }
  if (chmod(script.c_str(), 0755) != 0) {
    std::perror("chmod");
    return 1;
  }
  setenv("LCOS_UPDATES_TEST_PKEXEC", script.c_str(), 1);
  setenv("LCOS_PKEXEC_LOG", log.c_str(), 1);
  setenv("LCOS_PKEXEC_PIDFILE", pidfile.c_str(), 1);
  setenv("LCOS_PKEXEC_RELEASE", release.c_str(), 1);
  set_timeout_ms(5000);

  {
    UpdatesWindow* window = new_window();
    expect(!window->cancel_sensitive_for_test(), "cancel starts insensitive");
    expect(!window->progress_visible_for_test(), "progress starts hidden");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("delay-ready");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_for(80);
    expect(contains(window->status_text_for_test(), "Waiting for authentication"),
           "the password wait says it is waiting for authentication");
    expect(window->cancel_sensitive_for_test(), "cancel is sensitive while the check is running");
    expect(!window->check_sensitive_for_test(), "check is insensitive while the check is running");
    expect(!window->progress_visible_for_test(), "progress stays hidden until HELPER_READY");
    const bool shown = pump_until(1500, [&]() { return window->progress_visible_for_test(); });
    expect(shown, "progress appears after HELPER_READY");
    expect(contains(window->status_text_for_test(), "Checking for updates"),
           "after authentication the status names the check");
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "You're up to date.");
    });
    expect(done, "delay-ready check finishes up to date");
    expect(!window->cancel_sensitive_for_test(), "cancel is insensitive after the check");
    expect(!window->progress_visible_for_test(), "progress hides when the check finishes");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("split-ready");
    set_timeout_ms(600);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool started = pump_until(1000, [&]() { return read_pid(pidfile) > 1; });
    expect(started, "split-ready pkexec started");
    const pid_t pid = read_pid(pidfile);
    pump_for(800);
    expect(alive(pid), "split HELPER_READY restarts the timer, so the helper is still alive");
    expect(!contains(window->status_text_for_test(), "Timed out"),
           "split HELPER_READY has not already timed out");
    const bool timed = pump_until(2500, [&]() {
      return contains(window->status_text_for_test(), "Timed out");
    });
    expect(timed, "the re-armed timer still fires after HELPER_READY");
    expect(!alive(pid), "split-ready pkexec exits once the re-armed timer fires");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("dpkg-then-idle");
    set_timeout_ms(500);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool configuring = pump_until(1000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "DPKG_STARTED says packages are being configured");
    const pid_t pid = read_pid(pidfile);
    pump_for(700);
    expect(alive(pid), "a configuring dpkg keeps the helper past the original deadline");
    expect(contains(window->status_text_for_test(), "Configuring packages"),
           "the configuring status stays while dpkg is still running");
    const bool timed = pump_until(4000, [&]() {
      return contains(window->status_text_for_test(), "Timed out");
    });
    expect(timed, "DPKG_IDLE re-arms the deadline");
    expect(!alive(pid), "the helper exits after DPKG_IDLE and the new deadline");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    unlink(pidfile2.c_str());
    set_mode("term-exit");
    set_timeout_ms(8000);
    setenv("LCOS_PKEXEC_PIDFILE", pidfile.c_str(), 1);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool started = pump_until(1000, [&]() { return read_pid(pidfile) > 1; });
    expect(started, "auth pkexec started");
    const pid_t first = read_pid(pidfile);
    const gint64 t0 = g_get_monotonic_time();
    window->test_click_cancel();
    const bool stopped = pump_until(200, [&]() { return window->check_sensitive_for_test(); });
    const gint64 elapsed = g_get_monotonic_time() - t0;
    expect(stopped, "cancel returns the window to idle");
    expect(elapsed < 250000, "the next check starts inside the 250ms SIGKILL window");
    expect(!alive(first), "cancel stops the pkexec it was aimed at");
    setenv("LCOS_PKEXEC_PIDFILE", pidfile2.c_str(), 1);
    window->test_click_check();
    const bool second_up = pump_until(1000, [&]() { return read_pid(pidfile2) > 1; });
    expect(second_up, "a second check starts after cancel");
    const pid_t second = read_pid(pidfile2);
    pump_for(500);
    expect(alive(second), "a stale SIGKILL does not hit the next pkexec");
    expect(second != first, "the second check is a different pkexec");
    window->test_click_cancel();
    pump_until(1000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile2);
    kill_pidfile(pidfile);
    setenv("LCOS_PKEXEC_PIDFILE", pidfile.c_str(), 1);
  }

  {
    unlink(pidfile.c_str());
    set_mode("dismiss");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    expect(done, "dismissed auth returns to idle");
    expect(contains(window->status_text_for_test(), "Authentication was cancelled."),
           "a dismissed dialog says authentication was cancelled");
    expect(!contains(window->status_text_for_test(), "or failed"),
           "a dismissed dialog is not an authentication failure");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("exit127");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    expect(done, "pkexec exit 127 returns to idle");
    const std::string status = window->status_text_for_test();
    expect(status.find("Could not run the update helper (pkexec)") != std::string::npos,
           "exit 127 names the helper");
    expect(status.find("could not create a pipe") == std::string::npos,
           "exit 127 is not a pipe failure");
    expect(status.find("cancelled") == std::string::npos,
           "an empty exit 127 is not an authentication cancel");
    destroy_window(window, pidfile);
  }

  {
    unlink(log.c_str());
    set_mode("list-or-install");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Updates are available.");
    });
    expect(listed, "a simulate with packages offers updates");
    expect(window->install_sensitive_for_test(), "install is available for the reviewed list");
    window->test_click_install();
    const bool installed = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Existing configuration was kept");
    });
    expect(installed, "a kept conffile is named after install");
    expect(!contains(window->status_text_for_test(), "You're up to date."),
           "a kept conffile is not reported as up to date");
    const std::string text = slurp(log);
    expect(text.find("libc6=2") != std::string::npos, "install passes the reviewed name=version pin");
    destroy_window(window, pidfile);
  }

  {
    set_mode("list-or-changed");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() {
      return window->install_sensitive_for_test();
    });
    expect(listed, "changed-set check still offers install");
    window->test_click_install();
    const bool skipped = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Nothing was installed");
    });
    expect(skipped, "a changed set installs nothing");
    expect(contains(window->status_text_for_test(), "The update list changed"),
           "a changed set asks the user to look again");
    expect(!contains(window->status_text_for_test(), "Updates installed successfully"),
           "a skipped install is not a success");
    expect(window->install_sensitive_for_test(), "the new list can be installed");
    destroy_window(window, pidfile);
  }

  {
    unlink(log.c_str());
    unlink(release.c_str());
    set_mode("dpkg-hold");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool configuring = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "held dpkg reports configuring");
    expect(count_lines(log) == 1, "one pkexec is running during configuration");
    window->request_check();
    pump_for(300);
    expect(count_lines(log) == 1, "a second check waits until configuration finishes");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool followed = pump_until(3000, [&]() { return count_lines(log) >= 2; });
    expect(followed, "the queued check runs after configuration finishes");
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    set_mode("dpkg-hold");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->show();
    window->test_click_check();
    const bool configuring = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "quit-during-configure sees configuring");
    window->test_quit();
    pump_for(100);
    expect(window->get_visible(), "quit during configuration does not hide the window");
    bool dialog = false;
    GList* tops = gtk_window_list_toplevels();
    for (GList* item = tops; item != nullptr; item = item->next) {
      if (GTK_IS_MESSAGE_DIALOG(item->data) && gtk_widget_get_visible(GTK_WIDGET(item->data)))
        dialog = true;
    }
    g_list_free(tops);
    expect(dialog, "quit during configuration shows a non-blocking close dialog");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    destroy_window(window, pidfile);
  }

  {
    set_mode("ready-exit");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->show();
    window->test_quit();
    pump_for(50);
    expect(!window->get_visible(), "File Quit hides an idle window");
    destroy_window(window, pidfile);
  }

  {
    const int before = count_lines(log);
    set_mode("delay-ready");
    UpdatesWindow* window = new UpdatesWindow(false);
    window->request_check();
    delete window;
    pump_for(200);
    expect(count_lines(log) == before, "destroying the window before idle does not start pkexec");
  }

  {
    set_mode("ready-exit");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    std::vector<int> extra;
    for (;;) {
      const int fd = open("/dev/null", O_RDONLY);
      if (fd < 0)
        break;
      extra.push_back(fd);
    }
    window->test_click_check();
    const std::string pipe_status = window->status_text_for_test();
    for (int fd : extra)
      close(fd);
    expect(pipe_status.find("could not create a pipe") != std::string::npos,
           "a pipe failure names the pipe");
    expect(pipe_status.find("Could not run /usr/bin/pkexec") == std::string::npos,
           "a pipe failure is not a missing pkexec");

    setenv("LCOS_UPDATES_TEST_PKEXEC", "/no/such/pkexec", 1);
    window->test_click_check();
    const std::string spawn_status = window->status_text_for_test();
    expect(spawn_status.find("Could not run /usr/bin/pkexec") != std::string::npos,
           "a missing pkexec names pkexec");
    expect(spawn_status.find("could not create a pipe") == std::string::npos,
           "a missing pkexec is not a pipe failure");
    expect(pipe_status != spawn_status, "pipe and pkexec failures use different sentences");
    setenv("LCOS_UPDATES_TEST_PKEXEC", script.c_str(), 1);
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("huge");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(8000, [&]() {
      return contains(window->status_text_for_test(), "capped-ok");
    });
    expect(done, "a multi-megabyte helper stream still delivers the tail");
    expect(window->check_sensitive_for_test(), "the window stays usable after a huge stream");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("agent");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    expect(done, "a missing agent returns to idle");
    expect(contains(window->status_text_for_test(), "No authentication agent is running"),
           "a missing agent names the agent");
    expect(contains(window->status_text_for_test(), "xfce-polkit"),
           "a missing agent says which agent to start");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("notauth");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    expect(done, "a rejected password returns to idle");
    expect(contains(window->status_text_for_test(), "password was not accepted"),
           "a rejected password says the password or the account");
    expect(!contains(window->status_text_for_test(), "Authentication was cancelled"),
           "a rejected password is not a dismissed dialog");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("never-ready");
    set_timeout_ms(400);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Timed out waiting for authentication");
    });
    expect(done, "authentication has its own short timeout");
    expect(!contains(window->status_text_for_test(), "Timed out waiting for the update check"),
           "an unanswered password is not a check timeout");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("phases");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool refreshing = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Refreshing package lists");
    });
    expect(refreshing, "a check says it is refreshing package lists");
    const bool line = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Hit:1");
    });
    expect(line, "the window shows the last apt line");
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("many-lines");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "mirror26.example");
    });
    expect(done, "a long apt error is still shown");
    pump_for(100);
    expect(window->status_selectable_for_test(), "the status text can be selected");
    expect(window->allocated_height_for_test() < 700, "a long error does not grow past the screen");
    expect(window->allocated_width_for_test() < 900, "a long URL does not widen the window");
    expect(window->buttons_inside_window_for_test(), "the buttons stay inside the window");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("phased");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    expect(window->button_mnemonics_for_test(), "the action buttons use mnemonics");
    expect(contains(window->check_label_for_test(), "_Check for updates"), "Check has an underline");
    expect(contains(window->install_label_for_test(), "_Install updates"), "Install has an underline");
    expect(contains(window->cancel_label_for_test(), "_Cancel"), "Cancel has an underline");
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "will be offered later");
    });
    expect(done, "phased updates say they will be offered later");
    expect(contains(window->status_text_for_test(), "shim-signed"), "phased updates are named");
    expect(!contains(window->status_text_for_test(), "need extra packages"),
           "phased updates are not described as extra packages");
    expect(!window->install_sensitive_for_test(), "phased-only updates cannot be installed");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("held");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "These packages are held");
    });
    expect(done, "a held package says it is held");
    expect(contains(window->status_text_for_test(), "vim"), "a held package is named");
    expect(!contains(window->status_text_for_test(), "need extra packages"),
           "a held package is not described as an extra package");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("mixed-kept");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Some updates were kept back");
    });
    expect(done, "a mix of skipped updates uses the kept-back headline");
    const std::string status = window->status_text_for_test();
    expect(status.find("will be offered later") != std::string::npos, "the mix names the phased update");
    expect(status.find("These packages are held") != std::string::npos, "the mix names the hold");
    expect(status.find("need extra packages") != std::string::npos, "the mix names the classic kept-back");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("warn-uptodate");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "You're up to date.");
    });
    expect(done, "a partial refresh still reports the lists");
    const std::string status = window->status_text_for_test();
    const auto warn = status.find("Connection timed out");
    const auto head = status.find("You're up to date.");
    expect(warn != std::string::npos && head != std::string::npos && warn < head,
           "a partial refresh warning is above the headline");
    expect(status.find("No network connection") == std::string::npos,
           "one skipped mirror is not called no network");
    expect(!window->warning_visible_for_test(), "a check does not show the logout warning");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("summary-missing");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    /* Install needs a reviewed row. Seed one, then replace the mode. */
    destroy_window(window, pidfile);
    set_mode("list-then-reboot");
    window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "reboot test listed a package");
    set_mode("summary-missing");
    window->test_click_install();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "did not report whether any packages were kept back");
    });
    expect(done, "a missing summary does not say the system is up to date");
    expect(!contains(window->status_text_for_test(), "You're up to date."),
           "a missing summary is not the up-to-date sentence");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-then-reboot");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "a package is offered before the reboot install");
    window->test_click_install();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Restart to finish installing updates.");
    });
    expect(done, "a reboot flag says to restart");
    expect(contains(window->status_text_for_test(), "linux-image-amd64"), "a reboot flag names the package");
    expect(!contains(window->status_text_for_test(), "You're up to date."),
           "a reboot flag is not up to date");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("hang-check");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool ready = pump_until(2000, [&]() { return window->progress_visible_for_test(); });
    expect(ready, "hang-check reaches the helper");
    expect(window->cancel_is_default_for_test(), "Cancel is the default while a check is running");
    const pid_t pid = read_pid(pidfile);
    GdkEventKey key {};
    key.type = GDK_KEY_PRESS;
    key.window = gtk_widget_get_window(GTK_WIDGET(window->gobj()));
    key.keyval = GDK_KEY_Escape;
    key.send_event = 1;
    gtk_widget_event(GTK_WIDGET(window->gobj()), reinterpret_cast<GdkEvent*>(&key));
    expect(contains(window->status_text_for_test(), "Stopping the update check"),
           "Escape cancels a running check");
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    expect(!alive(pid), "Escape stops the check it was aimed at");
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("list-or-hang");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "finding 13 listed a package");
    set_timeout_ms(600);
    window->test_click_install();
    const bool timed = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Timed out while installing updates");
    });
    expect(timed, "the install times out");
    set_mode("hang-check");
    set_timeout_ms(8000);
    unlink(pidfile.c_str());
    window->test_click_check();
    const bool ready = pump_until(2000, [&]() { return window->progress_visible_for_test(); });
    expect(ready, "the later check is running");
    window->test_click_cancel();
    expect(contains(window->status_text_for_test(), "Stopping the update check"),
           "cancelling a check says it is stopping the check");
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("list-then-commit");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    /* The close path holds the application so the process outlives the
     * window. Register and start the test application once so add_window
     * is legal, then this window can take that hold. */
    static bool app_started = false;
    if (!app_started) {
      GError* error = nullptr;
      g_application_register(G_APPLICATION(app->gobj()), nullptr, &error);
      if (error != nullptr)
        g_error_free(error);
      g_signal_emit_by_name(app->gobj(), "startup");
      app_started = true;
    }
    app->add_window(*window);
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "commit test listed a package");
    expect(!window->warning_visible_for_test(), "the logout warning is hidden during a check");
    window->test_click_install();
    const bool configuring = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "install reports that packages are being configured");
    expect(!window->cancel_sensitive_for_test(), "Cancel is insensitive once dpkg has started");
    expect(window->commit_note_visible_for_test(), "the window says the install will finish");
    expect(contains(window->commit_note_for_test(), "This install will finish on its own."),
           "the commit note matches the behavior");
    expect(window->warning_visible_for_test(), "install shows the logout warning when nothing inhibits");
    expect(contains(window->warning_text_for_test(),
                    "Don't log out or shut down until the install finishes."),
           "the logout warning is the fallback sentence");
    const pid_t pid = read_pid(pidfile);
    window->test_click_cancel();
    pump_for(100);
    expect(alive(pid), "Cancel after dpkg does not stop the helper");
    expect(contains(window->status_text_for_test(), "Configuring packages"),
           "Cancel after dpkg leaves the configuring status");
    window->test_quit();
    pump_for(80);
    expect(window->get_visible(), "close during install asks before hiding");
    expect(contains(window->close_primary_for_test(), "The install will keep running."),
           "the close dialog says the install keeps running");
    expect(contains(window->close_secondary_for_test(), "It will finish in the background."),
           "the close dialog says the install finishes in the background");
    window->test_confirm_close();
    pump_for(80);
    expect(!window->get_visible(), "confirming close hides the window");
    expect(window->background_for_test(), "the hidden window keeps the install");
    expect(alive(pid), "hiding the window leaves the helper running");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool noted = pump_until(3000, [&]() {
      return contains(window->notification_text_for_test(), "Restart to finish installing updates.");
    });
    expect(noted, "the notification says a restart is needed");
    const std::string note = window->notification_text_for_test();
    expect(note.find("Existing configuration was kept") != std::string::npos,
           "the notification names the kept conffile");
    expect(note.find("linux-image-amd64") != std::string::npos, "the notification names the reboot package");
    expect(note.find("You're up to date.") == std::string::npos,
           "the notification does not say the system is up to date");
    pump_until(1000, [&]() { return !alive(pid); });
    app->remove_window(*window);
    destroy_window(window, pidfile);
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all window tests passed\n");
  return 0;
}
