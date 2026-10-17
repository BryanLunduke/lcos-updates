/* GUI tests for pkexec spawn, timers, cancel, and install wording.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs under xvfb-run. A fake pkexec stands in for /usr/bin/pkexec via
 * LCOS_UPDATES_TEST_PKEXEC, which exists only in this -DLCOS_UPDATES_TEST
 * binary.
 */

#include "tmpdir-cleanup.hpp"
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

static int count_in(const std::string& hay, const char* needle)
{
  int n = 0;
  const std::string nd(needle);
  for (std::string::size_type pos = 0; (pos = hay.find(nd, pos)) != std::string::npos; pos += nd.size())
    ++n;
  return n;
}

static void ensure_app(const Glib::RefPtr<Gtk::Application>& app)
{
  static bool started = false;
  if (started || !app)
    return;
  GError* error = nullptr;
  g_application_register(G_APPLICATION(app->gobj()), nullptr, &error);
  if (error != nullptr)
    g_error_free(error);
  g_signal_emit_by_name(app->gobj(), "startup");
  started = true;
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

/* 0.9.5-3: one action-area order in every state. */
static const std::vector<std::string> kButtonOrder = {"_Cancel", "_Install updates", "_Check for updates",
                                                      "_Repair", "Remove _Unused Packages", "_Restart"};

static void expect_button_order(UpdatesWindow* window, const std::string& state,
                                const std::vector<std::string>& visible)
{
  pump_for(60);
  const std::vector<std::string> all = window->button_order_for_test(false);
  const std::string what_all = "0.9.5-3 order (" + state + "): Cancel, Install, Check, Repair, Remove Unused, Restart";
  expect(all == kButtonOrder, what_all.c_str());
  const std::vector<std::string> shown = window->button_order_for_test(true);
  std::string got;
  for (const auto& label : shown)
    got += (got.empty() ? "" : ", ") + label;
  const std::string what_vis = "0.9.5-3 visible buttons (" + state + "): " + got;
  expect(shown == visible, what_vis.c_str());
  /* The labels are also laid out left to right in that order. */
  int last_x = -1;
  bool rising = true;
  for (Gtk::Widget* child : window->button_widgets_for_test()) {
    if (!child->get_visible())
      continue;
    int x = 0;
    int y = 0;
    gtk_widget_translate_coordinates(GTK_WIDGET(child->gobj()), GTK_WIDGET(window->gobj()), 0, 0, &x, &y);
    if (x <= last_x)
      rising = false;
    last_x = x;
  }
  const std::string what_x = "0.9.5-3 layout (" + state + "): visible buttons run left to right in that order";
  expect(rising, what_x.c_str());
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
  list-with-new)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\n'
    else
      printf 'STATUS upgrades\nCOUNT 2\n'
      printf 'PKG bash 1 2\n'
      printf 'PKG lcos-base 1.0 2.0\n'
      printf 'NEW eject 2.38.2-5\n'
      printf 'NEW cifs-utils 2:7.0-2\n'
      printf 'NEW keyutils 1.6.3-3\n'
      printf 'NEW gvfs-backends 1.54.2-1\n'
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
  finish-during-dialog)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      echo DPKG_STARTED >&2
      while [ ! -e "$release" ]; do
        wait_sec 0.05
      done
      echo DPKG_IDLE >&2
      printf 'STATUS success\nREBOOT linux-image-amd64\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  stall-configure)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      echo DPKG_STARTED >&2
      while [ ! -e "$release" ]; do
        wait_sec 0.05
      done
      echo DPKG_IDLE >&2
      printf 'STATUS success\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  list-then-phased)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\nPHASED shim-signed\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  install-then-unused)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\nREBOOT linux-image-7.2.6-lunduke\nUNUSED libold1\nUNUSED oldtool\n'
    elif [ "$cmd" = "cleanup" ]; then
      echo 'PHASE remove' >&2
      echo DPKG_STARTED >&2
      printf 'STATUS success\nREMOVED_UNUSED 2\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG linux-image-7.2.6-lunduke 7.2.6-lcos24 7.2.6-lcos25\n'
    fi
    ;;
  list-then-reboot-already)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      printf 'STATUS success\nREBOOT_ALREADY linux-image-amd64\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  removal-only)
    echo HELPER_READY >&2
    printf 'STATUS kept-back\nREMOVE foo oldplug\n'
    ;;
  cancel-then-result)
    echo HELPER_READY >&2
    echo 'PHASE simulate' >&2
    sleep 0.8
    printf 'STATUS upgrades\nCOUNT 1\nPKG foo 1 2\nKEPT bar\n'
    ;;
  configure-named)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      echo DPKG_STARTED >&2
      echo 'PROGRESS Setting up linux-image-amd64 (6.1.0) ...' >&2
      wait_sec 0.6
      echo DPKG_IDLE >&2
      echo 'PHASE download' >&2
      echo 'PROGRESS Get:1 http://deb.example/linux-image-amd64.deb' >&2
      sleep 1.2
      printf 'STATUS success\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\n'
    fi
    ;;
  percent)
    echo HELPER_READY >&2
    echo 'PHASE refresh' >&2
    echo 'PROGRESS 12% [1 linux-image-amd64 4.2 MB/80 MB 5%]' >&2
    sleep 0.8
    printf 'STATUS up-to-date\n'
    ;;
  sized)
    echo HELPER_READY >&2
    printf 'STATUS upgrades\nCOUNT 1\nPKG libc6 1 2\nNEED Need to get 0 B/478 B of archives.\n'
    ;;
  blank-progress)
    echo HELPER_READY >&2
    echo 'PHASE download' >&2
    echo 'PROGRESS 28% [1 xz-utils 80.0 kB/267 kB 30%]' >&2
    while [ ! -e "$release.1" ]; do wait_sec 0.05; done
    printf 'PROGRESS %s\n' '                                        '
    while [ ! -e "$release.2" ]; do wait_sec 0.05; done
    echo 'PROGRESS Inst gzip [1.12-1ubuntu3.1] (1.12-1ubuntu3.2 Debian:12 [amd64])' >&2
    while [ ! -e "$release.3" ]; do wait_sec 0.05; done
    echo 'PROGRESS Reading package lists... 59%' >&2
    while [ ! -e "$release.4" ]; do wait_sec 0.05; done
    printf 'STATUS up-to-date\n'
    ;;
  kept-timer)
    echo HELPER_READY >&2
    echo 'PHASE simulate' >&2
    echo 'PROGRESS Checking kept-back packages… (3 of 12)' >&2
    wait_sec 2.5
    printf 'STATUS kept-back\nUNCLASSIFIED foo\n'
    ;;
  wide-list)
    echo HELPER_READY >&2
    printf 'STATUS upgrades\nCOUNT 9\n'
    printf 'PKG verylongpackagename-that-should-ellipsize 1.0 2.0\n'
    printf 'PKG openjdk-17-jre-headless 21.0.11+9-1 21.0.12.1+1-1~24.04.4\n'
    printf 'PKG bash 5.2.15-2 5.2.21-2\n'
    printf 'SEC openjdk-17-jre-headless\n'
    printf 'SIZE openjdk-17-jre-headless 12.4 MB\n'
    printf 'SIZE bash 1.1 kB\n'
    printf 'NEED Need to get 387 MB of archives.\n'
    printf 'DISK After this operation, 37.5 MB of additional disk space will be used.\n'
    ;;
  download-hold)
    echo HELPER_READY >&2
    if [ "$cmd" = "upgrade" ]; then
      echo 'PHASE download' >&2
      echo 'PROGRESS 28% [1 xz-utils 80.0 kB/267 kB 30%]' >&2
      while [ ! -e "$release" ]; do
        wait_sec 0.05
      done
      printf 'STATUS success\n'
    else
      printf 'STATUS upgrades\nCOUNT 1\nPKG xz-utils 1 2\n'
    fi
    ;;
  working-download)
    echo HELPER_READY >&2
    echo 'PHASE download' >&2
    echo 'PROGRESS 0% [Connecting to deb.debian.org (151.101.2.132)]' >&2
    while [ ! -e "$release.1" ]; do wait_sec 0.05; done
    echo 'PROGRESS 0% [Waiting for headers]' >&2
    while [ ! -e "$release.2" ]; do wait_sec 0.05; done
    echo 'PROGRESS 17% [Working]' >&2
    while [ ! -e "$release.3" ]; do wait_sec 0.05; done
    echo 'PROGRESS 40% [1 bash 20.0 kB/200 kB 10%]' >&2
    while [ ! -e "$release.4" ]; do wait_sec 0.05; done
    echo 'PROGRESS 55% [Working]' >&2
    while [ ! -e "$release.5" ]; do wait_sec 0.05; done
    echo 'PROGRESS dlstatus:2:70.0000:Retrieving file 2 of 2' >&2
    while [ ! -e "$release.6" ]; do wait_sec 0.05; done
    echo 'PROGRESS Hit:1 http://deb.example stable InRelease' >&2
    while [ ! -e "$release.7" ]; do wait_sec 0.05; done
    echo 'PROGRESS Err:1 http://deb.example/bash 404 Not Found' >&2
    while [ ! -e "$release.8" ]; do wait_sec 0.05; done
    printf 'STATUS up-to-date\n'
    ;;
  broken)
    # Round 10: the check fails on an interrupted dpkg; repair fixes it.
    echo HELPER_READY >&2
    if [ "$cmd" = "repair" ]; then
      echo 'PHASE configure' >&2
      echo DPKG_STARTED >&2
      echo 'PROGRESS Setting up foo (2.0-1) ...' >&2
      while [ ! -e "$release.repair" ]; do wait_sec 0.05; done
      echo 'PHASE repair' >&2
      printf 'STATUS success\n'
    else
      printf 'STATUS error\nMSG The package installer was interrupted. Use Repair to finish configuring packages.\n'
    fi
    ;;
  broken-refuse)
    echo HELPER_READY >&2
    if [ "$cmd" = "repair" ]; then
      echo 'PHASE configure' >&2
      echo DPKG_STARTED >&2
      printf 'STATUS error\nMSG Repair stopped: fixing the dependencies would remove lcos-desktop. Nothing was removed.\nMSG Ask for help before removing packages.\n'
    else
      printf 'STATUS error\nMSG E: Unmet dependencies. Try %s with no packages (or specify a solution).\n' "'apt --fix-broken install'"
    fi
    ;;
  broken-fail)
    echo HELPER_READY >&2
    if [ "$cmd" = "repair" ]; then
      echo 'PHASE configure' >&2
      echo DPKG_STARTED >&2
      printf 'STATUS error\nMSG Repair could not finish configuring packages.\nMSG installed foo package post-installation script subprocess returned error exit status 1\nMSG A package is still left unfinished.\n'
    else
      printf 'STATUS error\nMSG The package installer was interrupted. Use Repair to finish configuring packages.\n'
    fi
    ;;
  *)
    echo HELPER_READY >&2
    printf 'STATUS up-to-date\n'
    ;;
esac
)PK";

int main(int main_argc, char** main_argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  /* 0.9.5-4: the real helper and the fixtures dir, for the replay of a real
   * Check failure. */
  const std::string helper_path = main_argc > 1 ? main_argv[1] : "";
  const std::string fixtures = main_argc > 2 ? main_argv[2] : "";
  const char* display = std::getenv("DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    std::fprintf(stderr, "DISPLAY is not set; the window test cannot run\n");
    return 1;
  }
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
  lcos_test::cleanup_at_exit(tmpl);
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
  /* 0.9.5-2: the box's own /var/run/reboot-required must not leak into the
   * other tests. Only the marker section below creates this file. */
  const std::string reboot_marker = dir + "/reboot-required";
  const std::string reboot_pkgs = reboot_marker + ".pkgs";
  setenv("LCOS_UPDATES_TEST_REBOOT_FILE", reboot_marker.c_str(), 1);
  set_timeout_ms(5000);

  {
    UpdatesWindow* window = new_window();
    expect(!window->cancel_sensitive_for_test(), "cancel starts insensitive");
    expect(!window->progress_visible_for_test(), "progress starts hidden");
    expect(window->status_text_for_test() == "Check for updates.",
           "the window opens with Check for updates.");
    expect(window->status_selectable_for_test(), "the status text can still be selected");
    expect(!window->status_can_focus_for_test(), "the status text is not focused on startup");
    expect(!window->status_has_focus_for_test(), "startup focus is not the status sentence");
    expect(!window->status_has_selection_for_test(), "the status sentence is not selected");
    expect(window->check_x_for_test() > window->install_x_for_test(),
           "Check is the trailing button before updates are listed");
    expect(!window->restart_visible_for_test(), "Restart is hidden until a reboot is required");
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
      return contains(window->status_text_for_test(), "1 update.");
    });
    expect(listed, "a simulate with packages offers updates");
    const bool install_left = pump_until(1000, [&]() {
      return window->install_x_for_test() < window->check_x_for_test();
    });
    expect(install_left, "0.9.5-3: Install stays left of Check when it becomes the next step");
    expect(window->install_is_default_for_test(), "Install is the default once updates are listed");
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
    unlink(log.c_str());
    set_mode("list-with-new");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() {
      return window->install_sensitive_for_test() && window->new_packages_visible_for_test() &&
             window->new_package_rows_for_test() == 4;
    });
    expect(listed, "new packages are listed before install");
    expect(window->package_rows_for_test() == 2, "upgrades stay in their own list");
    bool saw_base = false;
    bool saw_new_in_upgrades = false;
    for (int row = 0; row < window->package_rows_for_test(); ++row) {
      const std::string name = window->package_at_row_for_test(row);
      if (name == "lcos-base" || name == "bash")
        saw_base = true;
      if (name == "eject" || name == "gvfs-backends" || name == "cifs-utils" || name == "keyutils")
        saw_new_in_upgrades = true;
    }
    expect(saw_base, "the kept-back upgrade is in the upgrade list");
    expect(!saw_new_in_upgrades, "new packages are not upgrade rows");
    bool saw_eject = false;
    bool saw_gvfs = false;
    for (int row = 0; row < window->new_package_rows_for_test(); ++row) {
      const std::string name = window->new_package_at_row_for_test(row);
      if (name == "eject")
        saw_eject = true;
      if (name == "gvfs-backends")
        saw_gvfs = true;
    }
    expect(saw_eject && saw_gvfs, "the new-package list names eject and gvfs-backends");
    const std::string status = window->status_text_for_test();
    expect(status.find("New packages will also be installed: ") != std::string::npos,
           "the status names the new packages before install");
    expect(status.find("eject") != std::string::npos && status.find("gvfs-backends") != std::string::npos,
           "the status lists each new package");
    expect(count_lines(log) == 1, "showing the new packages has not started the install");
    window->test_click_install();
    const bool asking_first = pump_until(1000, [&]() {
      return window->new_dialog_up_for_test() && window->new_default_is_cancel_for_test();
    });
    expect(asking_first, "install asks before adding new packages");
    expect(window->new_default_is_cancel_for_test(), "the new-package question defaults to cancel");
    expect(window->new_secondary_for_test().find("eject") != std::string::npos &&
               window->new_secondary_for_test().find("gvfs-backends") != std::string::npos,
           "the confirmation names the new packages");
    expect(count_lines(log) == 1, "declining is still possible before the install starts");
    window->test_cancel_new_packages();
    pump_for(80);
    expect(!window->new_dialog_up_for_test(), "cancel closes the new-package question");
    expect(count_lines(log) == 1, "cancel does not install the new packages");
    expect(window->install_sensitive_for_test(), "install stays available after cancel");
    window->test_click_install();
    const bool asking = pump_until(1000, [&]() { return window->new_dialog_up_for_test(); });
    expect(asking, "install asks again");
    window->test_confirm_new_packages();
    const bool installed = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Updates installed.");
    });
    expect(installed, "confirming installs the combined set");
    const std::string text = slurp(log);
    expect(text.find(" upgrade ") != std::string::npos || text.find("\nupgrade ") != std::string::npos ||
               text.find(" upgrade\n") != std::string::npos || text.find(" upgrade") != std::string::npos,
           "confirm runs the install");
    expect(text.find("bash=2") != std::string::npos, "confirm passes the upgrade pin");
    expect(text.find("lcos-base=2.0") != std::string::npos, "confirm passes the kept-back pin");
    expect(text.find("eject=2.38.2-5") != std::string::npos, "confirm passes eject");
    expect(text.find("cifs-utils=2:7.0-2") != std::string::npos, "confirm passes the epoch pin");
    expect(text.find("keyutils=1.6.3-3") != std::string::npos, "confirm passes keyutils");
    expect(text.find("gvfs-backends=1.54.2-1") != std::string::npos, "confirm passes gvfs-backends");
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
    expect(status.find("Can't reach the update server") == std::string::npos,
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
      return contains(window->status_text_for_test(), "Restart this computer to finish installing updates");
    });
    expect(done, "a reboot flag says to restart");
    const std::string status = window->status_text_for_test();
    expect(status.find("Updates installed.") == 0, "a restart starts with Updates installed");
    expect(count_in(status, "Restart this computer to finish installing updates") == 1,
           "the restart sentence is printed once");
    expect(status.find("linux-image-amd64") != std::string::npos, "a reboot flag names the package");
    expect(status.find("You're up to date.") == std::string::npos, "a reboot flag is not up to date");
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
    ensure_app(app);
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
      return contains(window->notification_text_for_test(), "Restart this computer to finish installing updates");
    });
    expect(noted, "the notification says a restart is needed");
    const std::string note = window->notification_text_for_test();
    expect(note.find("Existing configuration was kept") != std::string::npos,
           "the notification names the kept conffile");
    expect(note.find("linux-image-amd64") != std::string::npos, "the notification names the reboot package");
    expect(note.find("You're up to date.") == std::string::npos,
           "the notification does not say the system is up to date");
    expect(note.find("Updates installed.") != std::string::npos,
           "the notification starts from Updates installed");
    expect(count_in(note, "Restart this computer to finish installing updates") == 1,
           "the notification prints the restart sentence once");
    pump_until(1000, [&]() { return !alive(pid); });
    app->remove_window(*window);
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("finish-during-dialog");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "close-after-finish listed a package");
    window->test_click_install();
    const bool configuring = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "close-after-finish reaches configuration");
    window->test_quit();
    pump_for(40);
    expect(window->get_visible(), "the close dialog is up before the helper exits");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool finished = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Updates installed.");
    });
    expect(finished, "the result is written while the close dialog is open");
    expect(window->notification_text_for_test().empty(), "no notification until Close is confirmed");
    expect(window->closing_for_test(), "Check stays blocked while the dialog is the reason");
    window->test_keep_open();
    pump_for(40);
    expect(window->notification_text_for_test().empty(), "Keep open does not notify");
    expect(!window->closing_for_test(), "Keep open clears the close latch");
    expect(window->check_sensitive_for_test(), "Keep open leaves Check enabled");
    unlink(pidfile.c_str());
    window->test_click_check();
    const bool checking = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Waiting for authentication");
    });
    expect(checking, "Check starts a job after Keep open");
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("finish-during-dialog");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    ensure_app(app);
    app->add_window(*window);
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "finished Close listed a package");
    window->test_click_install();
    const bool configuring = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "finished Close reaches configuration");
    window->test_quit();
    pump_for(40);
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool finished = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Updates installed.");
    });
    expect(finished, "the install finishes behind the close dialog");
    window->test_confirm_close();
    const bool noted = pump_until(2000, [&]() {
      return contains(window->notification_text_for_test(), "Updates installed.");
    });
    expect(noted, "Close after the helper exited sends the result");
    const std::string note = window->notification_text_for_test();
    expect(count_in(note, "Restart this computer to finish installing updates") == 1,
           "the late Close notification restarts once");
    expect(!window->closing_for_test(), "Close after finish clears the close latch");
    pump_until(1000, [&]() { return !window->background_for_test(); });
    expect(!window->background_for_test(), "a finished Close does not keep the process held");
    app->remove_window(*window);
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("stall-configure");
    set_timeout_ms(8000);
    setenv("LCOS_UPDATES_FAKE_INHIBIT", "1", 1);
    setenv("LCOS_UPDATES_STALL_NOTICE_SEC", "2", 1);
    setenv("LCOS_UPDATES_STALL_INHIBIT_SEC", "3", 1);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "stall test listed a package");
    window->test_click_install();
    const bool configuring = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "stall test reaches configuration");
    expect(window->inhibit_mode_for_test() == "block", "the install starts with a block inhibitor");
    const bool aged = pump_until(2500, [&]() {
      return contains(window->status_text_for_test(), "second") ||
             contains(window->commit_note_for_test(), "second");
    });
    expect(aged, "the window shows how long configuration has been running");
    const bool stalled = pump_until(4000, [&]() {
      return contains(window->status_text_for_test(), "Still installing") &&
             contains(window->status_text_for_test(), "no progress for") &&
             contains(window->status_text_for_test(), "don't turn off the computer");
    });
    expect(stalled, "a stall says the install is still running");
    expect(contains(window->status_text_for_test(), "0 minutes"),
           "the stall sentence counts minutes without progress");
    const bool relaxed = pump_until(4000, [&]() {
      return window->inhibit_mode_for_test() == "delay";
    });
    expect(relaxed, "a long stall downgrades the inhibitor to delay");
    expect(contains(window->status_text_for_test(), "Shutdown will wait a short time"),
           "the window says shutdown will wait instead of being blocked");
    expect(window->stall_notifications_for_test() == 0, "a visible window does not notify about the stall");
    expect(window->cancel_sensitive_for_test() == false, "Cancel stays off during a stalled configure");
    const pid_t pid = read_pid(pidfile);
    expect(alive(pid), "a stall does not kill the helper");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool done = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Updates installed.");
    });
    expect(done, "the install still finishes after the stall notice");
    expect(window->check_sensitive_for_test(), "Check is enabled after the stalled install finishes");
    destroy_window(window, pidfile);
    unsetenv("LCOS_UPDATES_FAKE_INHIBIT");
    unsetenv("LCOS_UPDATES_STALL_NOTICE_SEC");
    unsetenv("LCOS_UPDATES_STALL_INHIBIT_SEC");
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("stall-configure");
    set_timeout_ms(8000);
    setenv("LCOS_UPDATES_FAKE_INHIBIT", "1", 1);
    setenv("LCOS_UPDATES_STALL_NOTICE_SEC", "1", 1);
    setenv("LCOS_UPDATES_STALL_INHIBIT_SEC", "4", 1);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "hidden stall listed a package");
    window->test_click_install();
    const bool configuring = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(configuring, "hidden stall reaches configuration");
    const pid_t pid = read_pid(pidfile);
    window->test_quit();
    pump_for(40);
    window->test_confirm_close();
    pump_for(40);
    expect(window->background_for_test(), "close during configure keeps the install");
    expect(alive(pid), "the hidden install is still running");
    bool saw_notice = false;
    bool saw_inhibit = false;
    const bool noted = pump_until(8000, [&]() {
      const int n = window->stall_notifications_for_test();
      const std::string text(window->notification_text_for_test());
      if (n == 1 && text.find("don't turn off the computer") != std::string::npos &&
          text.find("Shutdown") == std::string::npos &&
          window->inhibit_mode_for_test() == "block")
        saw_notice = true;
      if (n >= 2 && text.find("Shutdown") != std::string::npos)
        saw_inhibit = true;
      return saw_notice && saw_inhibit;
    });
    expect(noted && saw_notice, "a hidden install notifies at 10 minutes without mentioning shutdown");
    expect(saw_inhibit, "a hidden install notifies again when shutdown is no longer blocked");
    expect(window->stall_notifications_for_test() == 2, "the stall notices are the 10-minute and the 30-minute");
    pump_for(1500);
    expect(window->stall_notifications_for_test() == 2, "the stall notices are not repeated");
    expect(alive(pid), "the stall notification does not stop the helper");
    {
      std::ofstream out(release);
      out << "go\n";
    }
    const bool finished = pump_until(3000, [&]() {
      return contains(window->notification_text_for_test(), "Updates installed.");
    });
    expect(finished, "the hidden install then reports the real outcome");
    destroy_window(window, pidfile);
    unsetenv("LCOS_UPDATES_FAKE_INHIBIT");
    unsetenv("LCOS_UPDATES_STALL_NOTICE_SEC");
    unsetenv("LCOS_UPDATES_STALL_INHIBIT_SEC");
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-then-phased");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "phased install listed a package");
    window->test_click_install();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Updates installed.");
    });
    expect(done, "an install with phased leftovers says updates were installed");
    const std::string status = window->status_text_for_test();
    expect(status.find("will be offered later: shim-signed.") != std::string::npos,
           "phased leftovers use the same sentence as Check");
    expect(status.find("You're up to date.") == std::string::npos,
           "phased leftovers are not up to date");
    expect(window->check_sensitive_for_test(), "Check is enabled after a phased install");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-then-reboot-already");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "already-pending reboot listed a package");
    window->test_click_install();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "A restart is still needed for an earlier update");
    });
    expect(done, "an old reboot flag says a restart was already pending");
    const std::string status = window->status_text_for_test();
    expect(status.find("Updates installed.") == 0, "an old flag still says updates were installed");
    expect(status.find("Restart this computer to finish installing updates") == std::string::npos,
           "an old flag does not use the restart sentence");
    expect(status.find("linux-image-amd64") != std::string::npos, "an old flag names the package");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("removal-only");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool done = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "would remove");
    });
    expect(done, "a removal keep-back says a package would be removed");
    const std::string status = window->status_text_for_test();
    expect(status.find("oldplug") != std::string::npos, "the removal names the package that would go");
    expect(status.find("need extra packages") == std::string::npos,
           "a removal is not described as extra packages");
    expect(window->check_sensitive_for_test(), "Check is enabled for a removal keep-back");
    expect(!window->install_sensitive_for_test(), "a removal-only result does not offer Install");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("cancel-then-result");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool ready = pump_until(2000, [&]() { return window->progress_visible_for_test(); });
    expect(ready, "cancel-during-check is past authentication");
    window->test_click_cancel();
    const bool cancelled = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Update check was cancelled");
    });
    expect(cancelled, "cancelling a check overrides a later success protocol");
    expect(!contains(window->status_text_for_test(), "Updates are available"),
           "a cancelled check is not reported as finished");
    expect(!window->install_sensitive_for_test(), "a cancelled check does not offer Install");
    expect(window->check_sensitive_for_test(), "Check is enabled after a cancelled check");
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("configure-named");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "named configure listed a package");
    window->test_click_install();
    const bool named = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Configuring linux-image-amd64");
    });
    expect(named, "configuring names the package apt is setting up");
    const bool downloading = pump_until(3000, [&]() {
      return contains(window->commit_note_for_test(), "Downloading") &&
             !contains(window->commit_note_for_test(), "Configuring");
    });
    expect(downloading, "after dpkg goes idle the note says downloading");
    expect(contains(window->commit_note_for_test(), "This install will finish on its own."),
           "the idle note still says the install finishes on its own");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("percent");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool shown = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Downloading linux-image-amd64") &&
             window->progress_fraction_for_test() > 0.11 &&
             window->progress_fraction_for_test() < 0.13;
    });
    expect(shown, "a download percent names the package and fills the bar");
    expect(window->progress_text_for_test() == "12%", "the bar shows the leading percent");
    expect(window->status_text_for_test().find("12% [") == std::string::npos,
           "the raw apt percent line is not the status");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("sized");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool sized = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "1 update.") &&
             contains(window->status_text_for_test(), "The packages are already downloaded (478 B).");
    });
    expect(sized, "a cached download names the package size instead of 0 B");
    expect(!contains(window->status_text_for_test(), "Need to get 0 B"),
           "the raw 0 B line is not shown");
    expect(window->install_sensitive_for_test(), "a sized check still offers Install");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("blank-progress");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool percent = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Downloading xz-utils") &&
             window->progress_fraction_for_test() > 0.27 &&
             window->progress_fraction_for_test() < 0.29;
    });
    expect(percent, "the download sentence keeps the package and the percent");
    {
      std::ofstream out(release + ".1");
      out << "go\n";
    }
    pump_for(200);
    expect(contains(window->status_text_for_test(), "Downloading xz-utils"),
           "a spaces-only progress line does not blank the status");
    expect(window->progress_fraction_for_test() > 0.27 && window->progress_fraction_for_test() < 0.29,
           "a spaces-only progress line does not clear the bar");
    expect(window->status_text_for_test().find_first_not_of(" \t") != std::string::npos,
           "the status is not whitespace");
    {
      std::ofstream out(release + ".2");
      out << "go\n";
    }
    pump_for(200);
    expect(window->status_text_for_test().find("Inst gzip") == std::string::npos,
           "an Inst line is not the status");
    expect(contains(window->status_text_for_test(), "Downloading xz-utils"),
           "an Inst line leaves the download sentence in place");
    {
      std::ofstream out(release + ".3");
      out << "go\n";
    }
    const bool reading = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Reading package lists") &&
             window->progress_fraction_for_test() > 0.58 &&
             window->progress_fraction_for_test() < 0.60;
    });
    expect(reading, "a trailing percent moves the bar");
    {
      std::ofstream out(release + ".4");
      out << "go\n";
    }
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("kept-timer");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool counting = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Checking kept-back packages") &&
             contains(window->status_text_for_test(), "(3 of 12)");
    });
    expect(counting, "the kept-back counter is the status");
    pump_for(1100);
    expect(contains(window->status_text_for_test(), "(3 of 12)"),
           "the kept-back counter stays while the phase runs");
    expect(window->status_text_for_test().find("second") == std::string::npos,
           "the kept-back counter does not grow a second timer");
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("wide-list");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() {
      return window->package_rows_for_test() == 3 && window->columns_fit_for_test();
    });
    expect(listed, "name, version, and size columns fit in the window");
    /* 0.9.3-2: the window sizes to its rows instead of opening ~760px tall
     * around three packages; it must still show the list and the buttons. */
    pump_for(200);
    expect(window->allocated_height_for_test() < 520, "a three-row list does not open a tall empty window");
    expect(window->buttons_inside_window_for_test(), "the list window keeps the buttons inside");
    const std::string status = window->status_text_for_test();
    expect(status.find("3 updates.") != std::string::npos, "the count matches the rows, not COUNT");
    expect(status.find("9 updates.") == std::string::npos, "a stale COUNT is not the row count");
    expect(status.find("Need to get 387 MB of archives.") != std::string::npos,
           "the download size stays in the summary");
    expect(status.find("After this operation, 37.5 MB of additional disk space will be used.") !=
               std::string::npos,
           "the disk-space sentence is in the summary");
    expect(status.find("1 of these is a security update.") != std::string::npos,
           "security updates are counted in the summary");
    expect(window->package_at_row_for_test(0) == "openjdk-17-jre-headless",
           "security updates sort first");
    expect(window->security_at_row_for_test(0) == "Yes", "a security row is marked");
    expect(window->security_at_row_for_test(1).empty(), "a normal row is not marked security");
    expect(window->size_at_row_for_test(0) == "12.4 MB", "the size column shows the archive size");
    expect(window->package_at_row_for_test(0).find("21.0.12.1+1-1~24.04.4") == std::string::npos,
           "the version stays in its own column");
    expect(window->version_columns_ready_for_test(),
           "version columns ellipsize in the middle and can be resized");
    expect(window->old_version_at_row_for_test(0) == "21.0.11+9-1", "the old version is kept in full");
    expect(window->new_version_at_row_for_test(0) == "21.0.12.1+1-1~24.04.4",
           "the new version is kept in full");
    expect(window->version_tip_for_test(0) == "21.0.11+9-1 → 21.0.12.1+1-1~24.04.4",
           "the version tooltip shows the full old and new versions");
    expect(window->install_x_for_test() < window->check_x_for_test(),
           "0.9.5-3: Install keeps its place left of Check with a long list");
    expect(window->column_count_for_test() == 5, "package, versions, size, and security are columns");
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("download-hold");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "download cancel listed a package");
    window->test_click_install();
    const bool downloading = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Downloading xz-utils");
    });
    expect(downloading, "install reaches the download");
    const pid_t pid = read_pid(pidfile);
    expect(alive(pid), "the helper is running during the download");
    window->test_click_cancel();
    pump_for(80);
    expect(window->get_visible(), "Cancel during download asks first");
    expect(contains(window->close_primary_for_test(), "Stop downloading?"),
           "Cancel during download asks to stop downloading");
    expect(window->ask_default_is_cancel_for_test(), "Keep installing is the default");
    expect(alive(pid), "the question does not stop the download");
    window->test_keep_open();
    pump_for(80);
    expect(alive(pid), "Keep installing leaves the download running");
    expect(contains(window->status_text_for_test(), "Downloading xz-utils"),
           "Keep installing leaves the download sentence");
    window->test_quit();
    pump_for(80);
    expect(contains(window->close_primary_for_test(), "Stop downloading?"),
           "closing during download asks the same question");
    expect(window->ask_default_is_cancel_for_test(), "closing during download defaults to Keep installing");
    window->test_keep_open();
    pump_for(40);
    expect(window->get_visible(), "Keep installing does not hide the window");
    expect(alive(pid), "declining the close leaves the helper running");
    window->test_click_cancel();
    pump_for(40);
    window->test_confirm_close();
    const bool cancelled = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Install was cancelled");
    });
    expect(cancelled, "confirming Stop downloading says the install was cancelled");
    expect(!alive(pid), "confirming Stop downloading stops the helper");
    expect(window->check_sensitive_for_test(), "Check is enabled after a cancelled download");
    destroy_window(window, pidfile);
  }

  {
    unlink(release.c_str());
    unlink(pidfile.c_str());
    set_mode("download-hold");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    ensure_app(app);
    app->add_window(*window);
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "close-during-download listed a package");
    window->test_click_install();
    const bool downloading = pump_until(2000, [&]() {
      return contains(window->status_text_for_test(), "Downloading");
    });
    expect(downloading, "close-during-download reaches the download");
    const pid_t pid = read_pid(pidfile);
    window->test_quit();
    pump_for(40);
    window->test_confirm_close();
    const bool noted = pump_until(2000, [&]() {
      return contains(window->notification_text_for_test(), "Install was cancelled");
    });
    expect(noted, "closing during download notifies that the install was cancelled");
    expect(!alive(pid), "closing during download stops the helper");
    pump_until(1000, [&]() { return !window->background_for_test(); });
    app->remove_window(*window);
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-then-reboot");
    set_timeout_ms(5000);
    const std::string loginctl = dir + "/loginctl";
    const std::string loginctl_log = dir + "/loginctl.log";
    {
      std::ofstream out(loginctl);
      out << "#!/bin/sh\n"
             "printf '%s\\n' \"$@\" >> \"$LCOS_LOGINCTL_LOG\"\n"
             "if [ -n \"$LCOS_LOGINCTL_FAIL\" ]; then\n"
             "  echo 'Interactive authentication required.' >&2\n"
             "  exit 1\n"
             "fi\n"
             "exit 0\n";
    }
    chmod(loginctl.c_str(), 0755);
    setenv("LCOS_UPDATES_TEST_LOGINCTL", loginctl.c_str(), 1);
    setenv("LCOS_LOGINCTL_LOG", loginctl_log.c_str(), 1);
    unsetenv("LCOS_LOGINCTL_FAIL");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool listed = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed, "restart test listed a package");
    window->test_click_install();
    const bool done = pump_until(2000, [&]() {
      return window->restart_visible_for_test() &&
             contains(window->status_text_for_test(), "Restart this computer to finish installing updates");
    });
    expect(done, "a reboot flag shows a Restart button");
    expect(contains(window->status_text_for_test(), "linux-image-amd64"),
           "the restart sentence still names the package");
    expect(window->restart_sensitive_for_test(), "Restart can be clicked");
    const bool trailing = pump_until(1000, [&]() {
      return window->restart_x_for_test() > window->check_x_for_test() &&
             window->restart_x_for_test() > window->install_x_for_test();
    });
    expect(trailing, "Restart is the trailing button when a reboot is required");
    expect(window->restart_is_default_for_test(), "Restart is the default after an install that needs it");
    window->test_click_restart();
    pump_for(40);
    expect(contains(window->status_text_for_test(), "Restart this computer to finish installing updates"),
           "opening the question does not restart yet");
    expect(window->restart_default_is_cancel_for_test(), "the restart question defaults to Don't restart");
    expect(window->restart_primary_for_test() == "Restart this computer now?",
           "the restart question says the computer restarts");
    expect(window->restart_secondary_for_test().find("Open applications will be closed") != std::string::npos &&
               window->restart_secondary_for_test().find("save your work") != std::string::npos,
           "the restart question says to save your work");
    window->test_cancel_restart();
    pump_for(80);
    expect(slurp(loginctl_log).empty(), "Cancel does not run loginctl");
    expect(contains(window->status_text_for_test(), "Restart this computer to finish installing updates"),
           "Cancel leaves the restart sentence");
    window->test_click_restart();
    pump_for(40);
    window->test_confirm_restart();
    const bool ran = pump_until(1500, [&]() { return slurp(loginctl_log).find("reboot") != std::string::npos; });
    expect(ran, "Restart runs loginctl reboot");
    expect(slurp(loginctl_log).find("reboot\n") != std::string::npos, "the only loginctl verb is reboot");
    expect(window->status_text_for_test().find("Could not restart") == std::string::npos,
           "a successful loginctl is not an error");
    destroy_window(window, pidfile);
    unlink(loginctl_log.c_str());
    setenv("LCOS_LOGINCTL_FAIL", "1", 1);
    window = new_window();
    window->test_click_check();
    const bool listed_again = pump_until(2000, [&]() { return window->install_sensitive_for_test(); });
    expect(listed_again, "a failed restart still listed a package");
    window->test_click_install();
    const bool again = pump_until(2000, [&]() { return window->restart_visible_for_test(); });
    expect(again, "Restart is offered again");
    window->test_click_restart();
    pump_for(40);
    window->test_confirm_restart();
    const bool failed = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Could not restart this computer.");
    });
    expect(failed, "a failed loginctl shows an error");
    expect(contains(window->status_text_for_test(), "Interactive authentication required."),
           "a failed restart shows loginctl's reason");
    expect(window->restart_sensitive_for_test(), "Restart can be tried again after a failure");
    destroy_window(window, pidfile);
    unsetenv("LCOS_LOGINCTL_FAIL");
    unsetenv("LCOS_UPDATES_TEST_LOGINCTL");
    unsetenv("LCOS_LOGINCTL_LOG");
  }

  {
    unlink(pidfile.c_str());
    for (int i = 1; i <= 8; ++i)
      unlink((release + "." + std::to_string(i)).c_str());
    set_mode("working-download");
    set_timeout_ms(8000);
    UpdatesWindow* window = new_window();
    window->test_click_check();
    const bool connecting = pump_until(2000, [&]() {
      return window->progress_visible_for_test() &&
             window->status_text_for_test().find("Connecting") == std::string::npos &&
             window->status_text_for_test().find("Working") == std::string::npos;
    });
    expect(connecting, "connecting is not shown as a package name");
    expect(contains(window->status_text_for_test(), "Downloading"),
           "a download without a package name still says downloading");
    {
      std::ofstream out(release + ".1");
      out << "go\n";
    }
    pump_for(200);
    expect(window->status_text_for_test().find("Waiting") == std::string::npos,
           "waiting for headers is not a package name");
    {
      std::ofstream out(release + ".2");
      out << "go\n";
    }
    const bool working = pump_until(1500, [&]() {
      return window->progress_fraction_for_test() > 0.16 && window->progress_fraction_for_test() < 0.18;
    });
    expect(working, "a Working line still moves the bar");
    expect(window->status_text_for_test().find("Working") == std::string::npos,
           "Working is not shown as a package name");
    expect(contains(window->status_text_for_test(), "Downloading updates"),
           "a Working line falls back to downloading updates");
    {
      std::ofstream out(release + ".3");
      out << "go\n";
    }
    const bool named = pump_until(1500, [&]() {
      return contains(window->status_text_for_test(), "Downloading bash");
    });
    expect(named, "a numbered progress line names the package");
    {
      std::ofstream out(release + ".4");
      out << "go\n";
    }
    const bool kept = pump_until(1500, [&]() {
      return window->progress_fraction_for_test() > 0.54 && window->progress_fraction_for_test() < 0.56 &&
             contains(window->status_text_for_test(), "Downloading bash");
    });
    expect(kept, "a later Working line keeps the package name and the new percent");
    expect(window->status_text_for_test().find("Working") == std::string::npos,
           "the later Working line is not a package name");
    {
      std::ofstream out(release + ".5");
      out << "go\n";
    }
    const bool files = pump_until(1500, [&]() {
      return window->progress_fraction_for_test() > 0.69 && window->progress_fraction_for_test() < 0.71 &&
             contains(window->status_text_for_test(), "Downloading bash");
    });
    expect(files, "a dlstatus line updates the percent and keeps the package");
    expect(window->status_text_for_test().find("Retrieving") == std::string::npos,
           "dlstatus does not show Retrieving as a package");
    {
      std::ofstream out(release + ".6");
      out << "go\n";
    }
    pump_for(200);
    expect(contains(window->status_text_for_test(), "Downloading bash"),
           "a Hit line does not replace the package being downloaded");
    expect(window->status_text_for_test().find("Hit:") == std::string::npos,
           "a Hit prefix is not the download status");
    {
      std::ofstream out(release + ".7");
      out << "go\n";
    }
    pump_for(200);
    expect(contains(window->status_text_for_test(), "Downloading bash"),
           "an Err line does not replace the package being downloaded");
    expect(window->status_text_for_test().find("Err:") == std::string::npos,
           "an Err prefix is not the download status");
    {
      std::ofstream out(release + ".8");
      out << "go\n";
    }
    pump_until(2000, [&]() { return window->check_sensitive_for_test(); });
    destroy_window(window, pidfile);
  }

  /* Round 10 (U-1): Repair for an interrupted or broken install. */
  {
    unlink(pidfile.c_str());
    unlink((release + ".repair").c_str());
    set_mode("broken");
    set_timeout_ms(5000);
    UpdatesWindow* window = new_window();
    expect(!window->repair_visible_for_test(), "U-1: Repair is hidden before any error");
    window->test_click_check();
    const bool failed = pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(failed, "U-1: the check on a broken system finishes");
    expect(window->repair_visible_for_test() && window->repair_sensitive_for_test(),
           "U-1: an interrupted install shows a sensitive Repair button");
    expect(window->repair_is_default_for_test(), "U-1: Repair is the default button");
    expect(!window->install_sensitive_for_test(), "U-1: Install stays off until repaired");
    expect(!contains(window->status_text_for_test(), "terminal") &&
               !contains(window->status_text_for_test(), "dpkg --configure"),
           "U-1: the error does not tell the user to use a terminal");
    window->test_click_repair();
    const bool running = pump_until(3000, [&]() {
      return contains(window->status_text_for_test(), "Configuring foo") ||
             contains(window->status_text_for_test(), "Configuring packages");
    });
    expect(running, "U-1: the repair shows configure progress");
    expect(!window->repair_visible_for_test(), "U-1: Repair hides while repairing");
    expect(!window->cancel_sensitive_for_test(), "U-1: a committed repair cannot be cancelled");
    expect(!window->check_sensitive_for_test(), "U-1: Check is off while repairing");
    {
      std::ofstream out(release + ".repair");
      out << "go\n";
    }
    const bool done = pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(done && contains(window->status_text_for_test(), "Repair finished"),
           "U-1: a successful repair says so and offers Check");
    expect(!window->repair_visible_for_test(), "U-1: Repair hides after a successful repair");
    expect(window->check_is_default_for_test(), "U-1: Check is the default after repair");
    const std::string calls = slurp(log);
    expect(calls.find(" repair\n") != std::string::npos, "U-1: the button runs the helper's repair verb");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("broken-refuse");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(window->repair_visible_for_test(), "U-1: unmet dependencies show Repair");
    window->test_click_repair();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(contains(window->status_text_for_test(), "would remove lcos-desktop") &&
               contains(window->status_text_for_test(), "Nothing was removed"),
           "U-1: a refused repair names the package it would have removed");
    expect(!window->repair_visible_for_test(), "U-1: a refused repair does not offer Repair again");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("broken-fail");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    window->test_click_repair();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(contains(window->status_text_for_test(), "post-installation script"),
           "U-1: a failed repair shows dpkg's reason");
    expect(window->repair_visible_for_test(), "U-1: a failed repair can be tried again");
    destroy_window(window, pidfile);
  }

  /* Round 15 (U-1, U-2): Restart after a kernel install; Remove Unused
   * Packages asks first and passes exactly the listed names. */
  {
    unlink(pidfile.c_str());
    set_mode("install-then-unused");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
    window->test_click_install();
    pump_until(3000, [&]() { return window->cleanup_visible_for_test(); });
    expect(window->restart_visible_for_test(), "U-1 r15: a kernel install offers Restart");
    expect(contains(window->status_text_for_test(), "Restart this computer to finish installing updates"),
           "U-1 r15: the restart sentence is shown");
    expect(window->cleanup_visible_for_test() &&
               contains(window->status_text_for_test(), "2 packages are no longer needed"),
           "U-2 r15: unused packages are offered");
    window->test_click_cleanup();
    expect(window->cleanup_dialog_for_test() != nullptr, "U-2 r15: Remove Unused Packages asks first");
    window->test_respond_cleanup(Gtk::RESPONSE_CANCEL);
    pump_until(300, [&]() { return false; });
    expect(window->cleanup_visible_for_test() && window->cleanup_dialog_for_test() == nullptr,
           "U-2 r15: Cancel removes nothing and keeps the button");
    window->test_click_cleanup();
    window->test_respond_cleanup(Gtk::RESPONSE_ACCEPT);
    pump_until(3000, [&]() { return contains(window->status_text_for_test(), "Removed 2 unused packages."); });
    expect(contains(window->status_text_for_test(), "Removed 2 unused packages."), "U-2 r15: cleanup result shown");
    expect(!window->cleanup_visible_for_test(), "U-2 r15: the button goes away when nothing is left");
    expect(window->restart_visible_for_test(), "U-1 r15: Restart is still offered after the cleanup");
    const std::string pk = slurp(log);
    expect(contains(pk, " cleanup libold1 oldtool\n"), "U-2 r15: the helper gets exactly the listed names");
    destroy_window(window, pidfile);
  }

  /* 0.9.5-2 (095-02 QA U-1): /var/run/reboot-required shows the restart
   * notice on launch, after Check and after Install, not only after an
   * Install that wrote it. */
  {
    const std::vector<std::string> pkgs = parse_reboot_marker_pkgs(
        "linux-image-7.2.6-lunduke\n  linux-image-7.2.6-lunduke \n\nfirmware-misc-nonfree\r\nlinux-image-7.2.6-lunduke\n");
    expect(pkgs.size() == 2 && pkgs[0] == "linux-image-7.2.6-lunduke" && pkgs[1] == "firmware-misc-nonfree",
           "095-2: reboot-required.pkgs is trimmed and deduplicated in order");
    expect(parse_reboot_marker_pkgs("bad\x1b[31mname\nok\n") == std::vector<std::string>{"ok"},
           "095-2: names with control characters are skipped");
    RebootMarker none;
    expect(reboot_marker_sentence(none).empty(), "095-2: no marker, no notice");
    RebootMarker bare;
    bare.present = true;
    expect(reboot_marker_sentence(bare) ==
               "Restart this computer to finish a system update (such as a kernel or firmware update).",
           "095-2: a marker without .pkgs names a system update");
    RebootMarker many;
    many.present = true;
    for (int i = 1; i <= 8; ++i)
      many.pkgs.push_back("pkg" + std::to_string(i));
    expect(reboot_marker_sentence(many) ==
               "Restart this computer to finish installing updates (needed by pkg1, pkg2, pkg3, pkg4, pkg5, pkg6 and 2 more).",
           "095-2: a long .pkgs list is capped");
  }

  auto write_file = [](const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
  };
  const char* kNeeded = "Restart this computer to finish installing updates (needed by linux-image-7.2.6-lunduke).";

  /* Marker present, with a .pkgs list that repeats the kernel. */
  {
    write_file(reboot_marker, "*** System restart required ***\n");
    write_file(reboot_pkgs, "linux-image-7.2.6-lunduke\nlinux-image-7.2.6-lunduke\n");
    unlink(pidfile.c_str());
    set_mode("ready-exit");
    UpdatesWindow* window = new_window();
    expect(window->restart_visible_for_test() && window->restart_sensitive_for_test(),
           "095-2: launch with the marker shows Restart");
    expect(window->restart_note_for_test() == kNeeded && window->restart_note_visible_for_test() &&
               window->status_text_for_test() == "Check for updates.",
           "095-4: launch names the package once, in its own notice line");
    expect(window->check_is_default_for_test() && !window->restart_is_default_for_test(),
           "095-2: on launch Enter still means Check, not Restart");
    window->test_click_restart();
    expect(window->restart_default_is_cancel_for_test(), "095-2: the marker's Restart still asks first");
    window->test_cancel_restart();
    pump_for(50);
    expect(window->restart_visible_for_test() && window->restart_note_visible_for_test(),
           "095-2: Don't restart keeps the notice");

    window->test_click_check();
    expect(!window->restart_visible_for_test() && !window->restart_note_visible_for_test(),
           "095-2: Restart and its notice are hidden while Check runs");
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(window->status_text_for_test() == "You're up to date." && window->restart_note_for_test() == kNeeded &&
               window->restart_note_visible_for_test(),
           "095-2: an up-to-date Check keeps the restart notice");
    expect(count_in(window->status_text_for_test(), "Restart this computer") == 0,
           "095-4: the notice is not repeated in the status");
    expect(window->restart_visible_for_test() && window->restart_sensitive_for_test(),
           "095-2: Restart is offered after an up-to-date Check");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("many-lines");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    const std::string status = window->status_text_for_test();
    expect(contains(status, "Failed to fetch") && window->restart_note_for_test() == kNeeded &&
               window->restart_note_visible_for_test(),
           "095-2: a failed Check keeps the restart notice beside the error");
    expect(window->restart_visible_for_test(), "095-2: Restart is offered after a failed Check");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-or-install");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
    expect(window->restart_visible_for_test() && window->restart_note_for_test() == kNeeded &&
               window->restart_note_visible_for_test(),
           "095-2: a Check that lists updates keeps the restart notice");
    expect(window->install_is_default_for_test(), "095-2: with updates listed Enter still means Install");
    window->test_click_install();
    pump_until(3000, [&]() { return window->check_sensitive_for_test() && !window->install_sensitive_for_test(); });
    expect(contains(window->status_text_for_test(), "Updates installed.") &&
               window->restart_note_for_test() == kNeeded && window->restart_note_visible_for_test(),
           "095-2: an Install the helper did not flag still shows the marker's notice");
    expect(window->restart_visible_for_test() && window->restart_sensitive_for_test(),
           "095-2: Restart is offered after Install");
    destroy_window(window, pidfile);
  }

  {
    unlink(pidfile.c_str());
    set_mode("list-then-reboot-already");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
    window->test_click_install();
    pump_until(3000, [&]() { return window->restart_visible_for_test(); });
    const std::string status = window->status_text_for_test();
    expect(contains(status, "A restart is still needed for an earlier update (linux-image-amd64).") &&
               count_in(status, "Restart this computer") == 0 && !window->restart_note_visible_for_test(),
           "095-2: the helper's post-install wording is unchanged and not doubled");
    expect(window->restart_is_default_for_test(), "095-2: after Install Restart is still the default");
    destroy_window(window, pidfile);
  }

  /* Marker without .pkgs. */
  {
    unlink(reboot_pkgs.c_str());
    unlink(pidfile.c_str());
    set_mode("ready-exit");
    UpdatesWindow* window = new_window();
    expect(window->restart_visible_for_test() &&
               contains(window->restart_note_for_test(), "finish a system update (such as a kernel or firmware update)") &&
               window->restart_note_visible_for_test(),
           "095-2: a marker without .pkgs still offers Restart on launch");
    destroy_window(window, pidfile);
  }

  /* No marker: no notice anywhere. */
  {
    unlink(reboot_marker.c_str());
    unlink(pidfile.c_str());
    set_mode("ready-exit");
    UpdatesWindow* window = new_window();
    expect(!window->restart_visible_for_test() && !window->restart_note_visible_for_test() &&
               window->status_text_for_test() == "Check for updates.",
           "095-2: no marker, no notice on launch");
    window->test_click_check();
    pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
    expect(!window->restart_visible_for_test() && !window->restart_note_visible_for_test() &&
               window->status_text_for_test() == "You're up to date.",
           "095-2: no marker, no notice after Check");
    destroy_window(window, pidfile);
  }
  {
    unlink(pidfile.c_str());
    set_mode("list-or-install");
    UpdatesWindow* window = new_window();
    window->test_click_check();
    pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
    window->test_click_install();
    pump_until(3000, [&]() { return window->check_sensitive_for_test() && !window->install_sensitive_for_test(); });
    expect(!window->restart_visible_for_test() &&
               !contains(window->status_text_for_test(), "Restart this computer"),
           "095-2: no marker, no notice after Install");
    destroy_window(window, pidfile);
  }

  /* 0.9.5-3: the action-area order is the same in every state, marker or
   * not, so no button jumps when another appears or the default moves. */
  {
    const std::string C = "_Cancel", I = "_Install updates", K = "_Check for updates", R = "_Repair",
                      U = "Remove _Unused Packages", S = "_Restart";
    for (int with_marker = 0; with_marker <= 1; ++with_marker) {
      const std::string tag = with_marker ? "marker, " : "no marker, ";
      if (with_marker) {
        write_file(reboot_marker, "*** System restart required ***\n");
        write_file(reboot_pkgs, "linux-image-7.2.6-lunduke\n");
      } else {
        unlink(reboot_marker.c_str());
        unlink(reboot_pkgs.c_str());
      }
      auto with_restart = [&](std::vector<std::string> v) {
        if (with_marker && (v.empty() || v.back() != S))
          v.push_back(S);
        return v;
      };
      {
        unlink(pidfile.c_str());
        set_mode("ready-exit");
        UpdatesWindow* window = new_window();
        expect_button_order(window, tag + "launch", with_restart({C, I, K}));
        expect(window->check_is_default_for_test(), "0.9.5-3: Check is still the default on launch");
        window->test_click_check();
        pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
        expect_button_order(window, tag + "Check up to date", with_restart({C, I, K}));
        destroy_window(window, pidfile);
      }
      {
        unlink(pidfile.c_str());
        set_mode("many-lines");
        UpdatesWindow* window = new_window();
        window->test_click_check();
        pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
        expect_button_order(window, tag + "Check failed", with_restart({C, I, K}));
        destroy_window(window, pidfile);
      }
      {
        unlink(pidfile.c_str());
        set_mode("list-or-install");
        UpdatesWindow* window = new_window();
        window->test_click_check();
        pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
        expect_button_order(window, tag + "Check listed updates", with_restart({C, I, K}));
        expect(window->install_is_default_for_test(), "0.9.5-3: Install is still the default with updates listed");
        window->test_click_install();
        pump_until(3000, [&]() { return window->check_sensitive_for_test() && !window->install_sensitive_for_test(); });
        expect_button_order(window, tag + "after Install", with_restart({C, I, K}));
        destroy_window(window, pidfile);
      }
      {
        unlink(pidfile.c_str());
        set_mode("install-then-unused");
        UpdatesWindow* window = new_window();
        window->test_click_check();
        pump_until(3000, [&]() { return window->install_sensitive_for_test(); });
        window->test_click_install();
        pump_until(3000, [&]() { return window->cleanup_visible_for_test(); });
        expect_button_order(window, tag + "after a kernel Install", {C, I, K, U, S});
        expect(window->restart_is_default_for_test(), "0.9.5-3: Restart is still the default after a kernel Install");
        window->test_click_cleanup();
        window->test_respond_cleanup(Gtk::RESPONSE_ACCEPT);
        pump_until(3000, [&]() { return contains(window->status_text_for_test(), "Removed 2 unused packages."); });
        expect_button_order(window, tag + "after Remove Unused Packages", {C, I, K, S});
        destroy_window(window, pidfile);
      }
      {
        unlink(pidfile.c_str());
        set_mode("broken-fail");
        UpdatesWindow* window = new_window();
        window->test_click_check();
        pump_until(3000, [&]() { return window->repair_visible_for_test(); });
        expect_button_order(window, tag + "Check offers Repair", with_restart({C, I, K, R}));
        expect(window->repair_is_default_for_test(), "0.9.5-3: Repair is still the default when offered");
        window->test_click_repair();
        pump_until(3000, [&]() { return window->check_sensitive_for_test(); });
        expect_button_order(window, tag + "after a failed Repair", with_restart({C, I, K, R}));
        destroy_window(window, pidfile);
      }
      {
        unlink(pidfile.c_str());
        unlink((release + ".repair").c_str());
        set_mode("broken");
        UpdatesWindow* window = new_window();
        window->test_click_check();
        pump_until(3000, [&]() { return window->repair_visible_for_test(); });
        write_file(release + ".repair", "go\n");
        window->test_click_repair();
        pump_until(3000, [&]() { return contains(window->status_text_for_test(), "Repair finished."); });
        expect_button_order(window, tag + "after Repair", with_restart({C, I, K}));
        unlink((release + ".repair").c_str());
        destroy_window(window, pidfile);
      }
    }
    unlink(reboot_marker.c_str());
    unlink(reboot_pkgs.c_str());
  }

  /* 0.9.5-4 (095-03 real-ISO QA): with the Devuan mirror unreachable the
   * status showed only apt's W:/Err: lines; the restart notice was appended
   * below them and the 160px status scroller hid it. Replay Chloe's captured
   * output through the real helper (its apt-get update parsing and protocol)
   * and the real window code. */
  {
    const std::string C = "_Cancel", I = "_Install updates", K = "_Check for updates", S = "_Restart";
    write_file(reboot_marker, "*** System restart required ***\n");
    write_file(reboot_pkgs, "linux-image-7.2.6-lunduke\n");
    if (helper_path.empty() || fixtures.empty() || access(helper_path.c_str(), X_OK) != 0) {
      expect(false, "095-4: test-window needs the helper path and the fixtures dir");
    } else {
      const std::string replay = dir + "/replay";
      mkdir(replay.c_str(), 0755);
      for (const char* sub : {"/lists", "/state", "/updates"})
        mkdir((replay + sub).c_str(), 0755);
      write_file(replay + "/status",
                 "Package: lcos-updates\nStatus: install ok installed\nVersion: 0.9.5-3\nArchitecture: amd64\n\n");
      /* pkexec's argv is the helper path and its arguments: run the real
       * helper as this (non-root) user instead. */
      write_file(replay + "/pkexec", "#!/bin/sh\nshift\nexec \"$LCOS_REPLAY_HELPER\" \"$@\"\n");
      chmod((replay + "/pkexec").c_str(), 0755);
      setenv("LCOS_REPLAY_HELPER", helper_path.c_str(), 1);
      setenv("LCOS_UPDATES_APT_GET", (replay + "/apt-get").c_str(), 1);
      setenv("LCOS_UPDATES_DPKG", "/bin/true", 1);
      setenv("LCOS_UPDATES_DPKG_STATUS", (replay + "/status").c_str(), 1);
      setenv("LCOS_UPDATES_DPKG_UPDATES_DIR", (replay + "/updates").c_str(), 1);
      setenv("LCOS_UPDATES_LISTS_DIR", (replay + "/lists").c_str(), 1);
      setenv("LCOS_UPDATES_STATE_DIR", (replay + "/state").c_str(), 1);
      setenv("LCOS_UPDATES_TEST_PKEXEC", (replay + "/pkexec").c_str(), 1);
      set_timeout_ms(15000);
      struct ReplayCase {
        const char* stdout_file;
        const char* rc;
        bool total;
      };
      const ReplayCase cases[] = {{"update-partial.stdout", "0", false},
                                  {"update-partial.stdout", "100", false},
                                  {"update-total.stdout", "0", true},
                                  {"update-total.stdout", "100", true}};
      for (const ReplayCase& c : cases) {
        const std::string tag = std::string(c.total ? "every source down" : "Devuan mirror down") + ", apt rc " + c.rc;
        /* The helper runs apt with a cleaned environment, so the replay
         * apt-get has the fixture paths and exit code written into it. */
        write_file(replay + "/apt-get",
                   "#!/bin/sh\n"
                   "case \" $* \" in\n"
                   "  *\" update \"*) cat '" + fixtures + "/chloe-095-03/" + c.stdout_file + "'; cat '" + fixtures +
                       "/chloe-095-03/update.stderr' >&2; exit " + c.rc + ";;\n"
                   "esac\n"
                   "echo '0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.'\n"
                   "exit 0\n");
        chmod((replay + "/apt-get").c_str(), 0755);
        UpdatesWindow* window = new_window();
        window->test_click_check();
        const bool done = pump_until(10000, [&]() { return window->check_sensitive_for_test(); });
        pump_for(200);
        const std::string status = window->status_text_for_test();
        expect(done && contains(status, "W: Failed to fetch http://ftp.fau.de/devuan/merged/dists/excalibur/InRelease  "
                                        "Unable to connect to ftp.fau.de:http:"),
               ("095-4 replay (" + tag + "): the real helper's apt lines reach the status").c_str());
        if (c.total)
          expect(contains(status, "Could not connect to the update server"),
                 ("095-4 replay (" + tag + "): the check failed with the plain sentence").c_str());
        else
          expect(status.rfind("W: Failed to fetch", 0) == 0 && contains(status, "You're up to date."),
                 ("095-4 replay (" + tag + "): the status starts with W: as on Chloe's screen").c_str());
        expect(window->status_overflows_for_test(),
               ("095-4 replay (" + tag + "): the status is taller than its scroller (the case that hid the notice)").c_str());
        expect(window->restart_note_for_test() == kNeeded && window->restart_note_visible_for_test(),
               ("095-4 replay (" + tag + "): the restart notice is shown, laid out and outside the scroller").c_str());
        expect(window->restart_visible_for_test() && window->restart_sensitive_for_test(),
               ("095-4 replay (" + tag + "): Restart is offered").c_str());
        expect(window->check_is_default_for_test(),
               ("095-4 replay (" + tag + "): Check stays the default").c_str());
        expect_button_order(window, "095-4 replay, " + tag, {C, I, K, S});
        destroy_window(window, pidfile);
      }
      for (const char* key : {"LCOS_UPDATES_APT_GET", "LCOS_UPDATES_DPKG", "LCOS_UPDATES_DPKG_STATUS",
                              "LCOS_UPDATES_DPKG_UPDATES_DIR", "LCOS_UPDATES_LISTS_DIR", "LCOS_UPDATES_STATE_DIR",
                              "LCOS_REPLAY_HELPER"})
        unsetenv(key);
      setenv("LCOS_UPDATES_TEST_PKEXEC", script.c_str(), 1);
      set_timeout_ms(5000);
    }

    /* Every other way a Check can fail keeps the notice too. */
    struct FailCase {
      const char* mode;
      const char* says;
      int timeout_ms;
      bool cancel;
    };
    const FailCase fails[] = {{"exit127", "Could not run the update helper", 5000, false},
                              {"notauth", "", 5000, false},
                              {"dismiss", "", 5000, false},
                              {"never-ready", "Timed out waiting for authentication", 600, false},
                              {"never-ready", "cancelled", 5000, true},
                              {"many-lines", "Failed to fetch", 5000, false}};
    for (const FailCase& f : fails) {
      const std::string tag = std::string(f.mode) + (f.cancel ? " + Cancel" : "");
      unlink(pidfile.c_str());
      set_mode(f.mode);
      set_timeout_ms(f.timeout_ms);
      UpdatesWindow* window = new_window();
      window->test_click_check();
      if (f.cancel) {
        pump_for(200);
        window->test_click_cancel();
      }
      pump_until(4000, [&]() { return window->check_sensitive_for_test(); });
      pump_for(100);
      const std::string status = window->status_text_for_test();
      expect(window->check_sensitive_for_test() && (f.says[0] == '\0' || contains(status, f.says)) &&
                 status != "Checking for updates…",
             ("095-4 (" + tag + "): the Check ended with its own message: " + status.substr(0, 60)).c_str());
      expect(window->restart_note_for_test() == kNeeded && window->restart_note_visible_for_test() &&
                 window->restart_visible_for_test(),
             ("095-4 (" + tag + "): the restart notice and Restart stay").c_str());
      destroy_window(window, pidfile);
    }
    set_timeout_ms(5000);
    unlink(reboot_marker.c_str());
    unlink(reboot_pkgs.c_str());
  }

  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all window tests passed\n");
  return 0;
}
