/* lcos-updates — GTK3 window (never runs as root).
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.hpp"

#include "timeouts.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <gio/gio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
const char kHelperPath[] = "/usr/libexec/lcos-updates-helper";
const char kPkexecPath[] = "/usr/bin/pkexec";
/* 120s update + 120s simulate + slack. Install also re-simulates.
 * Restarted when the helper announces HELPER_READY, so polkit time does not
 * eat the budget. Restarted again when a configuring dpkg exits. */
const int kCheckTimeoutMs = kCheckTimeoutSec * 1000;
const int kInstallTimeoutMs = kInstallTimeoutSec * 1000;
const int kAuthTimeoutMs = kAuthTimeoutSec * 1000;
const int kPulseIntervalMs = 100;

const char* pkexec_path()
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_TEST_PKEXEC");
  if (env != nullptr && env[0] == '/')
    return env;
#else
  (void)0;
#endif
  return kPkexecPath;
}

/* elogind provides loginctl. The session's own polkit action authorizes it.
 * This is not a root helper, and a failure is reported here. */
const char* loginctl_path()
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_TEST_LOGINCTL");
  if (env != nullptr && env[0] == '/')
    return env;
#else
  (void)0;
#endif
  return "/usr/bin/loginctl";
}

int gui_timeout_ms(int fallback)
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_GUI_TIMEOUT_MS");
  if (env != nullptr && env[0] != '\0') {
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end != env && *end == '\0' && parsed >= 200 && parsed <= 600000)
      return static_cast<int>(parsed);
  }
#else
  (void)fallback;
#endif
  return fallback;
}

std::string strip_helper_marker(std::string err, const char* tag)
{
  const std::string needle = tag;
  for (;;) {
    const std::string::size_type pos = err.find(needle);
    if (pos == std::string::npos)
      break;
    err.erase(pos, needle.size());
  }
  return err;
}

std::string strip_helper_markers(std::string err)
{
  err = strip_helper_marker(std::move(err), "HELPER_READY\n");
  err = strip_helper_marker(std::move(err), "DPKG_STARTED\n");
  return strip_helper_marker(std::move(err), "DPKG_IDLE\n");
}

struct InhibitHold {
  int logind_fd = -1;
  guint cookie = 0;
};

int stall_notice_sec()
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_STALL_NOTICE_SEC");
  if (env != nullptr && env[0] != '\0') {
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end != env && *end == '\0' && parsed >= 1 && parsed <= kStallNoticeSec)
      return static_cast<int>(parsed);
  }
#else
  (void)0;
#endif
  return kStallNoticeSec;
}

int stall_inhibit_sec()
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_STALL_INHIBIT_SEC");
  if (env != nullptr && env[0] != '\0') {
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (end != env && *end == '\0' && parsed >= 1 && parsed <= kStallInhibitSec)
      return static_cast<int>(parsed);
  }
#else
  (void)0;
#endif
  const int notice = stall_notice_sec();
  return kStallInhibitSec < notice ? notice : kStallInhibitSec;
}

bool fake_inhibit()
{
#ifdef LCOS_UPDATES_TEST
  const char* env = std::getenv("LCOS_UPDATES_FAKE_INHIBIT");
  return env != nullptr && env[0] == '1' && env[1] == '\0';
#else
  return false;
#endif
}

/* elogind Inhibit. mode is "block" or "delay". Returns a held fd, or -1. */
int take_logind(const Glib::ustring& reason, const char* mode)
{
  if (fake_inhibit()) {
    int pipes[2] = {-1, -1};
    if (pipe(pipes) != 0)
      return -1;
    ::close(pipes[1]);
    return pipes[0];
  }
  int fd = -1;
  try {
    auto conn = Gio::DBus::Connection::get_sync(Gio::DBus::BUS_TYPE_SYSTEM);
    auto proxy = Gio::DBus::Proxy::create_sync(conn, "org.freedesktop.login1",
                                               "/org/freedesktop/login1",
                                               "org.freedesktop.login1.Manager");
    GError* error = nullptr;
    GUnixFDList* out_fds = nullptr;
    GVariant* result = g_dbus_proxy_call_with_unix_fd_list_sync(
        G_DBUS_PROXY(proxy->gobj()), "Inhibit",
        g_variant_new("(ssss)", "shutdown:sleep", "LCOS Updates", reason.c_str(), mode),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &out_fds, nullptr, &error);
    if (error != nullptr) {
      g_error_free(error);
    } else if (result != nullptr && out_fds != nullptr) {
      gint32 index = -1;
      g_variant_get(result, "(h)", &index);
      if (index >= 0)
        fd = g_unix_fd_list_get(out_fds, index, nullptr);
    }
    if (result != nullptr)
      g_variant_unref(result);
    if (out_fds != nullptr)
      g_object_unref(out_fds);
  } catch (const Glib::Error&) {
  }
  return fd;
}

/* elogind's login1 block inhibitor, plus Gtk's session inhibit. Either may
 * fail; the caller shows a logout warning when both do. */
InhibitHold take_inhibitors(Gtk::Application* app, Gtk::Window& window, const Glib::ustring& reason)
{
  InhibitHold hold;
  hold.logind_fd = take_logind(reason, "block");
  if (fake_inhibit())
    return hold;
  if (app != nullptr) {
    try {
      hold.cookie = app->inhibit(window, Gtk::APPLICATION_INHIBIT_LOGOUT | Gtk::APPLICATION_INHIBIT_SUSPEND,
                                 reason);
    } catch (const Glib::Error&) {
      hold.cookie = 0;
    }
  }
  return hold;
}

bool apt_plan_line(const Glib::ustring& line)
{
  return line.compare(0, 5, "Inst ") == 0 || line.compare(0, 5, "Conf ") == 0;
}

bool progress_has_count(const Glib::ustring& line)
{
  return line.find('(') != Glib::ustring::npos && line.find(" of ") != Glib::ustring::npos;
}

Glib::ustring package_being_configured(const Glib::ustring& line)
{
  if (!progress_is_configure_line(std::string(line)))
    return {};
  return progress_package_name(std::string(line));
}

std::string read_fd_text(int fd)
{
  if (fd < 0)
    return {};
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  std::string out;
  char buf[512];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n > 0) {
      out.append(buf, static_cast<std::size_t>(n));
      if (out.size() > 2000) {
        out.resize(2000);
        break;
      }
      continue;
    }
    break;
  }
  std::string reason;
  for (char ch : out) {
    if (ch == '\r' || ch == '\n' || ch == '\t')
      reason.push_back(' ');
    else
      reason.push_back(ch);
  }
  std::size_t begin = 0;
  while (begin < reason.size() && reason[begin] == ' ')
    ++begin;
  std::size_t end = reason.size();
  while (end > begin && reason[end - 1] == ' ')
    --end;
  reason = reason.substr(begin, end - begin);
  if (reason.size() > 180)
    reason.resize(180);
  return reason;
}

std::string restart_failure_text(const std::string& reason)
{
  if (reason.empty())
    return "Could not restart this computer.";
  return "Could not restart this computer. " + reason;
}

std::string authentication_message(const std::string& err, int exit_code)
{
  const Glib::ustring lower = Glib::ustring(err).lowercase();
  const std::string text(lower);
  auto has = [&](const char* needle) { return text.find(needle) != std::string::npos; };
  if (has("no authentication agent") || has("authentication agent found"))
    return "No authentication agent is running. Start xfce-polkit or lxpolkit, then try again.";
  if (has("not authorized") || has("not authorised"))
    return "The password was not accepted, or this account cannot administer this computer.";
  if (has("dismiss"))
    return "Authentication was cancelled.";
  if ((exit_code == 127 || exit_code == 126) && err.find_first_not_of(" \t\r\n") == std::string::npos)
    return "Could not run the update helper (pkexec).";
  return {};
}
}

UpdatesWindow::UpdatesWindow(bool check_on_start)
{
  set_title("Check for updates…");
  /* Width only: height follows natural request so the pre-check window hugs
   * status + buttons (no blank strip under a taller default). */
  set_default_size(560, -1);
  set_border_width(0);
  /* Window-manager chrome only: do not call set_titlebar / HeaderBar. */
  /* Reinforce default icon for WMs that ignore gtk_window_set_default_icon_name. */
  set_icon_name(lcos_updates::kIconName);

  auto* file_menu = Gtk::manage(new Gtk::Menu());
  auto* quit_item = Gtk::manage(new Gtk::MenuItem("_Quit", true));
  quit_item->signal_activate().connect(sigc::mem_fun(*this, &UpdatesWindow::on_quit));
  file_menu->append(*quit_item);
  auto* file_item = Gtk::manage(new Gtk::MenuItem("_File", true));
  file_item->set_submenu(*file_menu);
  m_menubar.append(*file_item);

  auto* help_menu = Gtk::manage(new Gtk::Menu());
  auto* about_item = Gtk::manage(new Gtk::MenuItem("_About…", true));
  about_item->signal_activate().connect(sigc::mem_fun(*this, &UpdatesWindow::on_about));
  help_menu->append(*about_item);
  auto* help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));
  help_item->set_submenu(*help_menu);
  m_menubar.append(*help_item);
  m_menubar.show_all();

  m_status.set_line_wrap(true);
  m_status.set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
  m_status.set_max_width_chars(64);
  /* Selectable so a long error can be copied, but not focusable: a focused
   * selectable label opens with its whole sentence highlighted. */
  m_status.set_selectable(true);
  m_status.set_can_focus(false);
  m_status.set_xalign(0.0f);
  m_status.set_yalign(0.0f);
  set_status_text("Check for updates.");
  m_status.select_region(0, 0);

  /* A long apt error wraps inside the window and scrolls instead of pushing
   * the buttons off the screen. */
  m_status_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
  m_status_scroll.set_shadow_type(Gtk::SHADOW_NONE);
  m_status_scroll.set_propagate_natural_height(true);
  m_status_scroll.set_max_content_height(160);
  m_status_scroll.add(m_status);
  m_status_scroll.signal_size_allocate().connect([this](Gtk::Allocation& allocation) {
    const int width = allocation.get_width() > 80 ? allocation.get_width() : 80;
    if (m_status.get_allocated_width() != width)
      m_status.set_size_request(width, -1);
  });

  m_commit_note.set_line_wrap(true);
  m_commit_note.set_xalign(0.0f);
  m_commit_note.set_max_width_chars(64);
  m_commit_note.set_no_show_all(true);
  m_commit_note.set_text("This install will finish on its own.");
  m_commit_note.hide();

  m_warning.set_line_wrap(true);
  m_warning.set_xalign(0.0f);
  m_warning.set_max_width_chars(64);
  m_warning.set_no_show_all(true);
  m_warning.hide();

  /* Spinner lives beside status (not in ButtonBox — GtkButtonBox is for buttons). */
  m_spinner.set_no_show_all(true);
  m_spinner.set_size_request(18, 18);
  m_status_row.pack_start(m_spinner, Gtk::PACK_SHRINK, 0);
  m_status_row.pack_start(m_status_scroll, Gtk::PACK_EXPAND_WIDGET, 0);

  m_progress.set_no_show_all(true);
  m_progress.set_show_text(false);
  m_progress.set_pulse_step(0.05);
  m_progress.hide();

  m_store = Gtk::ListStore::create(m_cols);
  m_view.set_model(m_store);
  m_view.append_column("Package", m_cols.package);
  m_view.append_column("Old version", m_cols.old_version);
  m_view.append_column("New version", m_cols.new_version);
  m_view.append_column("Size", m_cols.size);
  m_view.append_column("Security", m_cols.security);
  m_view.set_headers_visible(true);
  m_view.get_selection()->set_mode(Gtk::SELECTION_NONE);
  /* Fixed shares keep the versions and the size inside the window. Long
   * package names ellipsize instead of pushing the new version off screen. */
  tune_column(0, 160, true, Pango::ELLIPSIZE_END, false);
  tune_column(1, 168, false, Pango::ELLIPSIZE_MIDDLE, true);
  tune_column(2, 210, false, Pango::ELLIPSIZE_MIDDLE, true);
  tune_column(3, 96, false, Pango::ELLIPSIZE_NONE, false);
  tune_column(4, 84, false, Pango::ELLIPSIZE_NONE, false);
  m_view.set_has_tooltip(true);
  m_view.signal_query_tooltip().connect(sigc::mem_fun(*this, &UpdatesWindow::on_view_tooltip));

  m_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  m_scroller.set_shadow_type(Gtk::SHADOW_IN);
  m_scroller.set_min_content_height(180);
  m_scroller.set_no_show_all(true);
  m_scroller.add(m_view);
  m_scroller.hide();

  m_new_heading.set_text("New packages");
  m_new_heading.set_halign(Gtk::ALIGN_START);
  m_new_heading.set_margin_top(6);
  m_new_heading.set_no_show_all(true);
  m_new_heading.hide();
  m_new_store = Gtk::ListStore::create(m_new_cols);
  m_new_view.set_model(m_new_store);
  m_new_view.append_column("Package", m_new_cols.package);
  m_new_view.append_column("Version", m_new_cols.version);
  m_new_view.set_headers_visible(true);
  m_new_view.get_selection()->set_mode(Gtk::SELECTION_NONE);
  m_new_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  m_new_scroller.set_shadow_type(Gtk::SHADOW_IN);
  m_new_scroller.set_min_content_height(72);
  m_new_scroller.set_no_show_all(true);
  m_new_scroller.add(m_new_view);
  m_new_scroller.hide();

  m_buttons.set_layout(Gtk::BUTTONBOX_END);
  m_buttons.set_spacing(8);
  m_install.set_sensitive(false);
  m_install.set_can_default(true);
  m_check.set_can_default(true);
  m_cancel.set_sensitive(false);
  m_cancel.set_can_default(true);
  m_restart.set_no_show_all(true);
  m_restart.set_sensitive(false);
  m_restart.hide();
  m_restart.set_can_default(true);
  m_buttons.pack_start(m_cancel, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_install, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_check, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_restart, Gtk::PACK_SHRINK);
  m_check.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_check_clicked));
  m_install.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_install_clicked));
  m_cancel.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_cancel_clicked));
  m_restart.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_restart_clicked));

  m_content.set_border_width(12);
  m_content.pack_start(m_status_row, Gtk::PACK_SHRINK);
  m_content.pack_start(m_commit_note, Gtk::PACK_SHRINK);
  m_content.pack_start(m_warning, Gtk::PACK_SHRINK);
  m_content.pack_start(m_progress, Gtk::PACK_SHRINK);
  m_content.pack_start(m_scroller, Gtk::PACK_EXPAND_WIDGET);
  m_content.pack_start(m_new_heading, Gtk::PACK_SHRINK);
  m_content.pack_start(m_new_scroller, Gtk::PACK_SHRINK);
  m_content.pack_start(m_buttons, Gtk::PACK_SHRINK);

  m_vbox.pack_start(m_menubar, Gtk::PACK_SHRINK);
  /* SHRINK while the list is hidden so expand space does not open under the
   * buttons. show_package_list() switches this to EXPAND when the list appears. */
  m_vbox.pack_start(m_content, Gtk::PACK_SHRINK);
  add(m_vbox);
  show_all_children();
  m_check.grab_default();
  set_default(m_check);
  signal_map().connect([this]() {
    m_status.select_region(0, 0);
    if (m_job == Job::None && m_check.get_sensitive())
      m_check.grab_focus();
  });

  if (check_on_start)
    request_check();
}

UpdatesWindow::~UpdatesWindow()
{
  m_leave_idle.disconnect();
  m_phase_timer.disconnect();
  m_check_idle.disconnect();
  m_check_queued = false;
  m_retry.disconnect();
  ++m_spawn_gen;
  m_timeout.disconnect();
  m_pulse.disconnect();
  release_inhibitors();
  if (m_app_held) {
    if (auto app = get_application())
      app->release();
    m_app_held = false;
  }
  if (m_close_dialog != nullptr) {
    Gtk::MessageDialog* dialog = m_close_dialog;
    m_close_dialog = nullptr;
    delete dialog;
  }
  if (m_restart_dialog != nullptr) {
    Gtk::MessageDialog* dialog = m_restart_dialog;
    m_restart_dialog = nullptr;
    delete dialog;
  }
  if (m_new_dialog != nullptr) {
    Gtk::MessageDialog* dialog = m_new_dialog;
    m_new_dialog = nullptr;
    delete dialog;
  }
  m_reboot_watch.disconnect();
  if (m_reboot_err_fd >= 0) {
    ::close(m_reboot_err_fd);
    m_reboot_err_fd = -1;
  }
  /* Once dpkg has started, closing the window must not cancel the install.
   * The helper ignores EOF and SIGTERM for the rest of that apt run. */
  if (m_leave_helper_running || m_install_committed || m_dpkg_started) {
    release_job_io();
    if (m_cancel_fd >= 0) {
      ::close(m_cancel_fd);
      m_cancel_fd = -1;
    }
    m_have_pid = false;
    m_pid = 0;
    m_job = Job::None;
    return;
  }
  if (m_cancel_fd >= 0) {
    ::close(m_cancel_fd);
    m_cancel_fd = -1;
  }
  /* pkexec is still this user during the password dialog. Signal that one
   * pid. After HELPER_READY the child is root; the closed pipe is the cancel. */
  if (m_have_pid && !m_helper_ready) {
    const pid_t child = static_cast<pid_t>(m_pid);
    if (child > 1)
      ::kill(child, SIGTERM);
  }
  release_job_io();
  m_have_pid = false;
  m_pid = 0;
  m_job = Job::None;
}

void UpdatesWindow::request_check()
{
  if (m_closing)
    return;
  if (m_job != Job::None || m_have_pid) {
    m_check_after_job = true;
    return;
  }
  if (m_check_queued)
    return;
  m_check_queued = true;
  m_check_idle.disconnect();
  m_check_idle = Glib::signal_idle().connect(sigc::mem_fun(*this, &UpdatesWindow::on_check_idle));
}

bool UpdatesWindow::on_check_idle()
{
  m_check_queued = false;
  if (m_closing)
    return false;
  on_check_clicked();
  return false;
}

void UpdatesWindow::set_status_text(const Glib::ustring& status)
{
  m_status.set_text(status);
  m_status.select_region(0, 0);
}

void UpdatesWindow::set_busy(bool busy, const Glib::ustring& status)
{
  /* Always replace prior error/status text — never leave a stale string. */
  set_status_text(status);
  m_check.set_sensitive(!busy);
  m_cancel.set_sensitive(busy && !m_install_committed);
  if (busy)
    m_install.set_sensitive(false);
  if (busy && !m_install_committed)
    use_cancel_as_default();
  m_pulse.disconnect();
  if (busy) {
    m_restart.hide();
    m_restart.set_sensitive(false);
    m_spinner.show();
    m_spinner.start();
    /* The bar stays hidden until HELPER_READY. During the password dialog
     * it would otherwise look like packages were already moving. */
    m_progress.hide();
  } else {
    m_spinner.stop();
    m_spinner.hide();
    m_progress.set_fraction(0.0);
    m_progress.hide();
  }
}

void UpdatesWindow::set_idle_status(const Glib::ustring& status)
{
  /* Explicit success/idle path: clear any prior error and stop busy chrome. */
  set_busy(false, status);
}

bool UpdatesWindow::on_pulse_tick()
{
  if (m_job == Job::None)
    return false;
  if (m_progress_percent >= 0) {
    m_progress.set_show_text(true);
    m_progress.set_text(std::to_string(m_progress_percent) + "%");
    m_progress.set_fraction(static_cast<double>(m_progress_percent) / 100.0);
  } else {
    m_progress.set_show_text(false);
    m_progress.pulse();
  }
  return true;
}

void UpdatesWindow::show_job_progress()
{
  if (m_job == Job::None)
    return;
  m_progress.set_fraction(0.0);
  m_progress.pulse();
  m_progress.show();
  m_pulse.disconnect();
  m_pulse = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_pulse_tick),
                                           kPulseIntervalMs);
}

void UpdatesWindow::show_packages(const std::vector<PackageUpgrade>& packages)
{
  std::vector<PackageUpgrade> rows = packages;
  std::stable_sort(rows.begin(), rows.end(), [](const PackageUpgrade& a, const PackageUpgrade& b) {
    if (a.security != b.security)
      return a.security;
    return a.name < b.name;
  });
  m_store->clear();
  for (const auto& pkg : rows) {
    Gtk::TreeModel::Row row = *(m_store->append());
    row[m_cols.package] = pkg.name;
    row[m_cols.old_version] = pkg.old_version;
    row[m_cols.new_version] = pkg.new_version;
    row[m_cols.version_tip] = pkg.old_version + " → " + pkg.new_version;
    row[m_cols.size] = pkg.size;
    row[m_cols.security] = pkg.security ? "Yes" : "";
  }
  show_package_list();
}

void UpdatesWindow::show_new_packages(const std::vector<PackageUpgrade>& packages)
{
  std::vector<PackageUpgrade> rows = packages;
  std::stable_sort(rows.begin(), rows.end(), [](const PackageUpgrade& a, const PackageUpgrade& b) {
    return a.name < b.name;
  });
  m_new_store->clear();
  for (const auto& pkg : rows) {
    if (pkg.name.empty() || pkg.new_version.empty())
      continue;
    Gtk::TreeModel::Row row = *(m_new_store->append());
    row[m_new_cols.package] = pkg.name;
    row[m_new_cols.version] = pkg.new_version;
  }
  if (m_new_store->children().empty()) {
    m_new_heading.hide();
    m_new_scroller.hide();
    return;
  }
  m_new_view.show();
  m_new_heading.show();
  m_new_scroller.show();
}

void UpdatesWindow::show_package_list()
{
  /* Explicit show(): show_all() is a no-op while no_show_all is set on the scroller. */
  m_view.show();
  m_scroller.show();
  /* gtkmm Box no longer wraps this; expand content so the scroller absorbs growth. */
  gtk_box_set_child_packing(m_vbox.gobj(), GTK_WIDGET(m_content.gobj()), TRUE, TRUE, 0,
                            GTK_PACK_START);
  int list_height = 640;
  if (const auto screen = get_screen()) {
    const int sh = screen->get_height();
    if (sh > 200) {
      list_height = sh - 80;
      if (list_height > 760)
        list_height = 760;
      if (list_height < 520)
        list_height = 520;
    }
  }
  resize(780, list_height);
  use_install_as_default();
}

void UpdatesWindow::hide_package_list()
{
  m_store->clear();
  m_scroller.hide();
  if (m_new_store)
    m_new_store->clear();
  m_new_heading.hide();
  m_new_scroller.hide();
  gtk_box_set_child_packing(m_vbox.gobj(), GTK_WIDGET(m_content.gobj()), FALSE, FALSE, 0,
                            GTK_PACK_START);
  /* Collapse to the natural height of menubar + status + buttons. */
  int width = 0;
  int height = 0;
  get_size(width, height);
  if (width < 560)
    width = 560;
  queue_resize();
  int min_h = 0;
  int nat_h = 0;
  get_preferred_height(min_h, nat_h);
  resize(width, nat_h > 0 ? nat_h : 1);
  use_check_as_default();
}

void UpdatesWindow::tune_column(int index, int width, bool expand, Pango::EllipsizeMode ellipsize,
                                bool resizable)
{
  Gtk::TreeViewColumn* column = m_view.get_column(index);
  if (column == nullptr)
    return;
  column->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
  column->set_fixed_width(width);
  column->set_min_width(resizable ? 96 : width);
  column->set_resizable(resizable);
  column->set_expand(expand);
  if (auto* cell = dynamic_cast<Gtk::CellRendererText*>(column->get_first_cell()))
    cell->property_ellipsize() = ellipsize;
}

bool UpdatesWindow::on_view_tooltip(int x, int y, bool /*keyboard_tooltip*/,
                                    const Glib::RefPtr<Gtk::Tooltip>& tooltip)
{
  Gtk::TreeModel::Path path;
  Gtk::TreeViewColumn* column = nullptr;
  int cell_x = 0;
  int cell_y = 0;
  if (!m_view.get_path_at_pos(x, y, path, column, cell_x, cell_y) || column == nullptr)
    return false;
  const std::vector<Gtk::TreeViewColumn*> columns = m_view.get_columns();
  const auto found = std::find(columns.begin(), columns.end(), column);
  if (found == columns.end())
    return false;
  const int index = static_cast<int>(found - columns.begin());
  if (index != 1 && index != 2)
    return false;
  auto iter = m_store->get_iter(path);
  if (!iter)
    return false;
  const Glib::ustring tip = (*iter)[m_cols.version_tip];
  if (tip.empty())
    return false;
  tooltip->set_text(tip);
  m_view.set_tooltip_cell(tooltip, &path, column, nullptr);
  return true;
}

void UpdatesWindow::order_trailing(Gtk::Button& trailing)
{
  Gtk::Button* buttons[] = {&m_cancel, &m_install, &m_check, &m_restart};
  int pos = 0;
  for (Gtk::Button* button : buttons) {
    if (button == &trailing)
      continue;
    gtk_box_reorder_child(GTK_BOX(m_buttons.gobj()), GTK_WIDGET(button->gobj()), pos++);
  }
  gtk_box_reorder_child(GTK_BOX(m_buttons.gobj()), GTK_WIDGET(trailing.gobj()), pos);
}

void UpdatesWindow::use_check_as_default()
{
  order_trailing(m_check);
  m_check.set_can_default(true);
  set_default(m_check);
  m_check.grab_default();
}

void UpdatesWindow::use_install_as_default()
{
  order_trailing(m_install);
  m_install.set_can_default(true);
  set_default(m_install);
  m_install.grab_default();
}

void UpdatesWindow::use_cancel_as_default()
{
  if (!m_cancel.get_sensitive())
    return;
  m_cancel.set_can_default(true);
  set_default(m_cancel);
  m_cancel.grab_default();
}

void UpdatesWindow::release_job_io()
{
  m_timeout.disconnect();
  m_pulse.disconnect();
  m_out_watch.disconnect();
  m_err_watch.disconnect();
  m_child_watch.disconnect();
  m_out_ch.reset();
  m_err_ch.reset();
  if (m_out_fd >= 0) {
    ::close(m_out_fd);
    m_out_fd = -1;
  }
  if (m_err_fd >= 0) {
    ::close(m_err_fd);
    m_err_fd = -1;
  }
}

void UpdatesWindow::request_stop()
{
  m_timeout.disconnect();
  if (m_install_committed || m_dpkg_started) {
    /* dpkg has started. Do not close the pipe: that used to cancel the rest
     * of the install after the current package finished configuring. */
    m_cancel.set_sensitive(false);
    m_check.set_sensitive(false);
    m_install.set_sensitive(false);
    m_commit_note.show();
    if (m_dpkg_started)
      set_status_text("Configuring packages…");
    return;
  }
  m_stopping = true;
  /* Closing the pipe is the cancel. The helper stops apt-get during
   * download. It will not signal a configuring dpkg. Do not walk /proc. */
  if (m_cancel_fd >= 0) {
    ::close(m_cancel_fd);
    m_cancel_fd = -1;
  }
  const Job job = m_job;
  m_check.set_sensitive(false);
  m_install.set_sensitive(false);
  set_status_text(job == Job::Install ? "Stopping the update…" : "Stopping the update check…");
  if (m_helper_ready || !m_have_pid)
    return;
  const pid_t child = static_cast<pid_t>(m_pid);
  if (child > 1)
    ::kill(child, SIGTERM);
  m_retry.disconnect();
  m_retry_pid = m_pid;
  m_retry_gen = m_spawn_gen;
  m_retry = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_stop_retry), 250);
}

void UpdatesWindow::finish_job()
{
  m_retry.disconnect();
  m_phase_timer.disconnect();
  m_check_idle.disconnect();
  m_check_queued = false;
  ++m_spawn_gen;
  release_job_io();
  if (m_cancel_fd >= 0) {
    ::close(m_cancel_fd);
    m_cancel_fd = -1;
  }
  if (m_have_pid) {
    Glib::spawn_close_pid(m_pid);
    m_have_pid = false;
    m_pid = 0;
  }
  m_job = Job::None;
  m_helper_ready = false;
  m_dpkg_started = false;
  m_install_committed = false;
  m_stop_was_timeout = false;
  m_stopping = false;
  m_stopped_job = Job::None;
  m_markers.clear();
  m_err_partial.clear();
  m_phase.clear();
  m_progress_line.clear();
  m_phase_mark_us = 0;
  m_progress_mark_us = 0;
  m_inhibit_relaxed = false;
  m_stall_notice_sent = false;
  m_stall_inhibit_sent = false;
  m_delay_inhibit = false;
  m_progress_percent = -1;
  m_commit_note.hide();
  release_inhibitors();
  update_logout_warning();
}

void UpdatesWindow::arm_job_timeout()
{
  /* Once dpkg has started, the install is not on a kill clock. */
  if (m_dpkg_started || m_install_committed || (m_job != Job::Check && m_job != Job::Install))
    return;
  const int fallback = m_job == Job::Install ? kInstallTimeoutMs : kCheckTimeoutMs;
  m_timeout.disconnect();
  m_timeout = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_timeout),
                                             gui_timeout_ms(fallback));
}

void UpdatesWindow::start_helper(const std::vector<std::string>& helper_args, Job job, int timeout_ms)
{
  m_retry.disconnect();
  if (m_closing || m_job != Job::None || m_have_pid)
    return;

  ++m_spawn_gen;
  m_stdout.clear();
  m_stderr.clear();
  m_out_cap = CaptureBuf{};
  m_markers.clear();
  m_err_partial.clear();
  m_stderr_first_error.clear();
  m_helper_ready = false;
  m_dpkg_started = false;
  m_install_committed = false;
  m_stop_was_timeout = false;
  m_stopping = false;
  m_phase.clear();
  m_progress_line.clear();
  m_phase_mark_us = 0;
  m_progress_mark_us = 0;
  m_inhibit_relaxed = false;
  m_stall_notice_sent = false;
  m_stall_inhibit_sent = false;
  m_delay_inhibit = false;
  m_stall_notifications = 0;
  m_progress_percent = -1;
  m_commit_note.hide();
  m_job = job;

  int cancel_pipe[2] = {-1, -1};
  if (pipe(cancel_pipe) != 0) {
    m_job = Job::None;
    set_idle_status("Could not start the update helper (could not create a pipe).");
    m_install.set_sensitive(!m_store->children().empty());
    return;
  }
  const int cancel_read = cancel_pipe[0];
  const int cancel_write = cancel_pipe[1];
  /* Child must not keep the write end across pkexec's exec, or the helper
   * never observes EOF. FD_CLOEXEC drops it even if child setup is skipped. */
  fcntl(cancel_write, F_SETFD, FD_CLOEXEC);

  std::vector<std::string> argv;
  argv.push_back(pkexec_path());
  argv.push_back(kHelperPath);
  for (const auto& arg : helper_args)
    argv.push_back(arg);

  /* pkexec keeps stdin and closes every other fd on exec. The write end is
   * closed in the child so the helper sees EOF when we cancel. */
  auto child_setup = [cancel_read, cancel_write]() {
    setpgid(0, 0);
    if (dup2(cancel_read, STDIN_FILENO) < 0)
      _exit(127);
    if (cancel_read != STDIN_FILENO)
      ::close(cancel_read);
    if (cancel_write != STDIN_FILENO)
      ::close(cancel_write);
  };

  int out_fd = -1;
  int err_fd = -1;
  Glib::Pid pid = 0;
  try {
    /* Absolute /usr/bin/pkexec. Do not search PATH. */
    Glib::spawn_async_with_pipes(std::string(), argv, Glib::SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                 &pid, nullptr, &out_fd, &err_fd);
  } catch (const Glib::SpawnError&) {
    ::close(cancel_read);
    ::close(cancel_write);
    m_job = Job::None;
    set_idle_status("Could not run /usr/bin/pkexec. Install policykit-1 and try again.");
    m_install.set_sensitive(!m_store->children().empty());
    return;
  }

  ::close(cancel_read);
  setpgid(static_cast<pid_t>(pid), static_cast<pid_t>(pid));
  m_cancel_fd = cancel_write;
  m_pid = pid;
  m_have_pid = true;
  m_out_fd = out_fd;
  m_err_fd = err_fd;

  /* A blocking read_chars() fills its buffer. The helper writes little until
   * apt exits, so a blocking read freezes the main loop and a close is ignored. */
  fcntl(out_fd, F_SETFL, O_NONBLOCK);
  fcntl(err_fd, F_SETFL, O_NONBLOCK);
  m_out_ch = Glib::IOChannel::create_from_fd(out_fd);
  m_err_ch = Glib::IOChannel::create_from_fd(err_fd);
  try {
    m_out_ch->set_encoding("");
    m_err_ch->set_encoding("");
    m_out_ch->set_flags(Glib::IO_FLAG_NONBLOCK);
    m_err_ch->set_flags(Glib::IO_FLAG_NONBLOCK);
  } catch (const Glib::Exception&) {
  }
  m_out_ch->set_close_on_unref(false);
  m_err_ch->set_close_on_unref(false);

  m_out_watch = Glib::signal_io().connect(sigc::mem_fun(*this, &UpdatesWindow::on_stdout), out_fd,
                                          Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
  m_err_watch = Glib::signal_io().connect(sigc::mem_fun(*this, &UpdatesWindow::on_stderr), err_fd,
                                          Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR);
  m_child_watch =
      Glib::signal_child_watch().connect(sigc::mem_fun(*this, &UpdatesWindow::on_child_exited), pid);
  m_timeout = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_timeout),
                                             timeout_ms);
}

void UpdatesWindow::append_stdout(const char* data, std::size_t n)
{
  capture_append(m_out_cap, data, n);
}

void UpdatesWindow::append_stderr(const char* data, std::size_t n)
{
  if (data == nullptr || n == 0)
    return;
  m_err_partial.append(data, n);
  std::string::size_type pos = 0;
  while ((pos = m_err_partial.find('\n')) != std::string::npos) {
    std::string line = m_err_partial.substr(0, pos);
    m_err_partial.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line == "HELPER_READY" || line == "DPKG_STARTED" || line == "DPKG_IDLE")
      m_markers += line + "\n";
    else if (line.compare(0, 6, "PHASE ") == 0) {
      const Glib::ustring next(line.substr(6));
      const auto started = m_markers.rfind("DPKG_STARTED\n");
      const auto idle = m_markers.rfind("DPKG_IDLE\n");
      const bool dpkg_on =
          started != std::string::npos && (idle == std::string::npos || started > idle);
      if (next != m_phase && !dpkg_on) {
        m_phase_mark_us = g_get_monotonic_time();
        m_progress_percent = -1;
      }
      m_phase = next;
    }
    else if (line.compare(0, 9, "PROGRESS ") == 0) {
      std::string raw = line.substr(9);
      if (raw.size() > 240)
        raw.resize(240);
      /* A spaces-only clear-line, or an Inst/Conf plan line, must not blank
       * the status or drop the percent the bar is already showing. A
       * placeholder such as [Working] updates the percent and leaves the
       * package name that is already on screen. */
      const AptProgressNote note = apt_progress_note(raw, std::string(m_progress_line));
      if (note.replace_shown) {
        m_progress_line = note.shown;
        if (m_progress_line.size() > 240)
          m_progress_line.resize(240);
      }
      if (note.have_percent)
        m_progress_percent = note.percent;
      if (m_progress_percent >= 0 && m_progress.get_visible()) {
        m_progress.set_show_text(true);
        m_progress.set_text(std::to_string(m_progress_percent) + "%");
        m_progress.set_fraction(static_cast<double>(m_progress_percent) / 100.0);
      }
    }
    if (m_stderr_first_error.empty() && line.compare(0, 2, "E:") == 0) {
      if (line.size() > 2048)
        line.resize(2048);
      m_stderr_first_error = line;
    }
  }
  if (m_err_partial.size() > 8192)
    m_err_partial.clear();

  if (m_stderr.size() + n <= kAptCaptureCap) {
    m_stderr.append(data, n);
    return;
  }
  if (n >= kAptCaptureCap) {
    m_stderr.assign(data + (n - kAptCaptureCap), kAptCaptureCap);
    return;
  }
  m_stderr.append(data, n);
  m_stderr.erase(0, m_stderr.size() - kAptCaptureCap);
}

std::string UpdatesWindow::stderr_text() const
{
  std::string err = strip_helper_markers(m_stderr);
  if (!m_stderr_first_error.empty() && err.find(m_stderr_first_error) == std::string::npos)
    err = m_stderr_first_error + "\n" + err;
  return err;
}

bool UpdatesWindow::on_stdout(Glib::IOCondition cond)
{
  if (!m_out_ch)
    return false;
  if (cond & (Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR)) {
    gchar buf[4096];
    gsize n = 0;
    const Glib::IOStatus st = m_out_ch->read(buf, sizeof buf, n);
    if (n > 0)
      append_stdout(buf, n);
    if (st == Glib::IO_STATUS_AGAIN)
      return true;
    if (st == Glib::IO_STATUS_EOF || (cond & (Glib::IO_HUP | Glib::IO_ERR)))
      return false;
  }
  return true;
}

bool UpdatesWindow::on_stderr(Glib::IOCondition cond)
{
  if (!m_err_ch)
    return false;
  if (cond & (Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR)) {
    gchar buf[4096];
    gsize n = 0;
    const Glib::IOStatus st = m_err_ch->read(buf, sizeof buf, n);
    if (n > 0)
      append_stderr(buf, n);
    if (n > 0) {
      note_helper_activity();
      maybe_rearm_job_timeout();
      maybe_note_dpkg();
      apply_phase_status();
    }
    if (st == Glib::IO_STATUS_AGAIN)
      return true;
    if (st == Glib::IO_STATUS_EOF || (cond & (Glib::IO_HUP | Glib::IO_ERR)))
      return false;
  }
  return true;
}

void UpdatesWindow::maybe_rearm_job_timeout()
{
  if (m_helper_ready || (m_job != Job::Check && m_job != Job::Install))
    return;
  if (m_markers.find("HELPER_READY\n") == std::string::npos)
    return;
  m_helper_ready = true;
  arm_phase_timer();
  show_job_progress();
  if (!m_dpkg_started && m_phase.empty() && m_progress_line.empty())
    set_status_text(m_job == Job::Install ? "Installing updates…" : "Checking for updates…");
  apply_phase_status();
  if (m_dpkg_started || m_install_committed)
    return;
  arm_job_timeout();
}

void UpdatesWindow::maybe_note_dpkg()
{
  const auto started = m_markers.rfind("DPKG_STARTED\n");
  const auto idle = m_markers.rfind("DPKG_IDLE\n");
  const bool on = started != std::string::npos && (idle == std::string::npos || started > idle);
  if (on == m_dpkg_started)
    return;
  m_dpkg_started = on;
  if (on) {
    m_progress_percent = -1;
    if (m_job == Job::Install)
      m_install_committed = true;
    m_timeout.disconnect();
    m_retry.disconnect();
    m_stop_was_timeout = false;
    m_stopping = false;
    m_phase_mark_us = g_get_monotonic_time();
    m_progress_mark_us = m_phase_mark_us;
    arm_phase_timer();
    set_status_text("Configuring packages…");
    if (m_install_committed) {
      m_cancel.set_sensitive(false);
      m_commit_note.show();
    }
    return;
  }
  if (m_job != Job::Check && m_job != Job::Install)
    return;
  if (m_install_committed)
    m_cancel.set_sensitive(false);
  /* The configure clock stops when dpkg goes idle. The next stretch is the
   * download of the following package, timed from now. */
  m_phase_mark_us = g_get_monotonic_time();
  arm_job_timeout();
  apply_phase_status();
  if (m_progress_line.empty() && m_phase.empty())
    set_status_text(m_job == Job::Install ? "Installing updates…" : "Checking for updates…");
}

void UpdatesWindow::present_outcome(const JobOutcome& outcome, const std::vector<PackageUpgrade>& packages,
                                     const std::vector<PackageUpgrade>& new_packages)
{
  if (outcome.packages == PackageListAction::Show) {
    show_packages(packages);
    show_new_packages(new_packages);
  } else if (outcome.packages == PackageListAction::Hide)
    hide_package_list();
  set_idle_status(outcome.status);
  m_check.set_sensitive(outcome.check_enabled);
  m_install.set_sensitive(outcome.install_enabled);
  if (outcome.offer_restart) {
    m_restart.show();
    m_restart.set_sensitive(true);
    order_trailing(m_restart);
    m_restart.set_can_default(true);
    set_default(m_restart);
    m_restart.grab_default();
    queue_resize();
    int min_h = 0;
    int nat_h = 0;
    get_preferred_height(min_h, nat_h);
    int width = 0;
    int height = 0;
    get_size(width, height);
    if (width < 560)
      width = 560;
    if (nat_h > height)
      resize(width, nat_h);
  } else {
    m_restart.set_sensitive(false);
    m_restart.hide();
  }
}

void UpdatesWindow::show_timeout_message(Job job, bool helper_ready)
{
  const bool keep = !m_store->children().empty();
  const JobOutcome outcome = outcome_timeout(job == Job::Install, helper_ready, keep);
  present_outcome(outcome, {}, {});
}

bool UpdatesWindow::on_timeout()
{
  if (m_job == Job::None && !m_have_pid)
    return false;
  if (m_dpkg_started || m_install_committed) {
    m_cancel.set_sensitive(false);
    m_commit_note.show();
    if (m_dpkg_started)
      set_status_text("Configuring packages…");
    return false;
  }
  m_stopped_job = m_job == Job::None ? m_stopped_job : m_job;
  m_stop_was_timeout = true;
  request_stop();
  return false;
}

bool UpdatesWindow::on_stop_retry()
{
  /* One signal to the pkexec this timer was armed for. A later pkexec has a
   * different pid and generation and is left alone. */
  if (m_retry_gen != m_spawn_gen)
    return false;
  if (!m_have_pid || m_pid != m_retry_pid)
    return false;
  if (m_helper_ready || m_dpkg_started)
    return false;
  const pid_t child = static_cast<pid_t>(m_pid);
  if (child > 1)
    ::kill(child, SIGKILL);
  return false;
}

void UpdatesWindow::on_child_exited(Glib::Pid /*pid*/, int wait_status)
{
  /* Drain remaining output. */
  if (m_out_ch) {
    for (;;) {
      gchar buf[4096];
      gsize n = 0;
      const Glib::IOStatus st = m_out_ch->read(buf, sizeof buf, n);
      if (n > 0)
        append_stdout(buf, n);
      if (n == 0 || st == Glib::IO_STATUS_EOF || st == Glib::IO_STATUS_ERROR ||
          st == Glib::IO_STATUS_AGAIN)
        break;
    }
  }
  if (m_err_ch) {
    for (;;) {
      gchar buf[4096];
      gsize n = 0;
      const Glib::IOStatus st = m_err_ch->read(buf, sizeof buf, n);
      if (n > 0)
        append_stderr(buf, n);
      if (n == 0 || st == Glib::IO_STATUS_EOF || st == Glib::IO_STATUS_ERROR ||
          st == Glib::IO_STATUS_AGAIN)
        break;
    }
  }

  /* The last chunk can carry HELPER_READY or DPKG_STARTED after the watches
   * have stopped. Honor it before deciding this was an auth or apt timeout. */
  maybe_rearm_job_timeout();
  maybe_note_dpkg();

  const Job job = m_job;
  const bool timed_out = m_stop_was_timeout;
  const bool helper_ready = m_helper_ready;
  const bool check_again = m_check_after_job;
  const bool user_stopped = m_stopping;
  m_check_after_job = false;
  m_stdout = capture_text(m_out_cap);
  const std::string out = m_stdout;
  const std::string err = stderr_text();
  finish_job();

  if (timed_out) {
    show_timeout_message(job, helper_ready);
    if (check_again) {
      if (m_closing)
        m_check_after_job = true;
      else
        request_check();
    }
    return;
  }

  SimulateResult parsed = parse_protocol(out);
  if (parsed.status == SimulateResult::Error && parsed.error_msg == "No STATUS from helper") {
    const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
    const std::string auth = authentication_message(err, exit_code);
    if (!auth.empty())
      parsed.error_msg = auth;
    else if (!err.empty())
      parsed.error_msg = err;
  }

  /* Cancel wins over a success protocol that arrived after the pipe closed.
   * That includes a check that finished classifying kept-back packages. */
  if (user_stopped && !timed_out && (job == Job::Check || job == Job::Install)) {
    const std::string want =
        job == Job::Install ? "Install was cancelled" : "Update check was cancelled";
    const bool said = parsed.status == SimulateResult::Error &&
                      parsed.error_msg.find("cancelled") != std::string::npos;
    if (!said) {
      parsed.status = SimulateResult::Error;
      parsed.error_msg = want;
    }
  }

  if (job == Job::Check)
    apply_check_result(parsed, wait_status);
  else if (job == Job::Install)
    apply_install_result(parsed, wait_status);
  else
    set_idle_status(m_status.get_text());

  /* The close dialog is the only reason Check stays blocked after the
   * helper has exited. Keep open and a finished Close both clear it. */
  if (m_close_dialog == nullptr)
    m_closing = false;

  if (m_background) {
    notify_and_leave(m_status.get_text());
    return;
  }

  if (check_again) {
    if (m_closing)
      m_check_after_job = true;
    else
      request_check();
  }
}

void UpdatesWindow::apply_check_result(const SimulateResult& result, int wait_status)
{
  SimulateResult shown = result;
  const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  if (shown.status == SimulateResult::Error &&
      (shown.error_msg.empty() || shown.error_msg == "No STATUS from helper")) {
    const std::string auth = authentication_message({}, exit_code);
    if (!auth.empty())
      shown.error_msg = auth;
  }
  const bool have_rows = !m_store->children().empty();
  present_outcome(outcome_check(shown, exit_code, have_rows), shown.packages, shown.new_packages);
}

void UpdatesWindow::apply_install_result(const SimulateResult& result, int wait_status)
{
  SimulateResult shown = result;
  const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  if (shown.status == SimulateResult::Error &&
      (shown.error_msg.empty() || shown.error_msg == "No STATUS from helper")) {
    const std::string auth = authentication_message({}, exit_code);
    if (!auth.empty())
      shown.error_msg = auth;
  }
  const bool have_rows = !m_store->children().empty();
  present_outcome(outcome_install(shown, exit_code, have_rows), shown.packages, shown.new_packages);
}

void UpdatesWindow::on_about()
{
  Gtk::AboutDialog dialog;
  dialog.set_transient_for(*this);
  dialog.set_program_name(lcos_updates::kProductName);
  dialog.set_version(lcos_updates::kVersion);
  dialog.set_comments("Updater for the Lunduke Computer Operating System");
  dialog.set_copyright("Copyright © 2026 The Lunduke Journal");
  dialog.set_license_type(Gtk::LICENSE_GPL_3_0);
  dialog.set_wrap_license(true);
  dialog.set_website("https://lunduke.com");
  dialog.set_website_label("lunduke.com");
  dialog.set_logo_icon_name(lcos_updates::kIconName);
  dialog.run();
}

bool UpdatesWindow::prepare_close()
{
  m_check_idle.disconnect();
  m_check_queued = false;
  if (m_close_dialog != nullptr)
    return false;
  if ((m_install_committed || m_dpkg_started) && m_have_pid && !m_background) {
    m_closing = true;
    show_close_dialog();
    return false;
  }
  if (install_downloading() && !m_background) {
    m_closing = true;
    m_download_stop_hides = true;
    show_download_dialog();
    return false;
  }
  m_closing = true;
  return true;
}

bool UpdatesWindow::install_downloading() const
{
  return m_job == Job::Install && m_have_pid && !m_dpkg_started && !m_install_committed &&
         m_phase == "download";
}

void UpdatesWindow::show_close_dialog()
{
  if (m_close_dialog != nullptr)
    return;
  m_close_primary = "The install will keep running.";
  m_close_secondary = "It will finish in the background.";
  auto* dialog = new Gtk::MessageDialog(*this, m_close_primary, false, Gtk::MESSAGE_WARNING,
                                        Gtk::BUTTONS_NONE, true);
  dialog->set_secondary_text(m_close_secondary);
  dialog->add_button("_Keep open", Gtk::RESPONSE_CANCEL);
  dialog->add_button("_Close", Gtk::RESPONSE_CLOSE);
  dialog->set_default_response(Gtk::RESPONSE_CANCEL);
  m_close_dialog = dialog;
  dialog->signal_response().connect([this](int response) {
    Gtk::MessageDialog* dying = m_close_dialog;
    m_close_dialog = nullptr;
    if (dying != nullptr)
      dying->hide();
    Glib::signal_idle().connect_once([dying]() { delete dying; });
    if (response == Gtk::RESPONSE_CLOSE) {
      /* The helper already exited behind the dialog. Notify with the result
       * that is on screen and quit. Do not hold a process that is gone. */
      if (m_job == Job::None && !m_have_pid) {
        m_closing = false;
        m_leave_helper_running = false;
        m_background = true;
        hide();
        notify_and_leave(m_status.get_text());
        return;
      }
      m_leave_helper_running = true;
      m_background = true;
      if (auto app = get_application()) {
        app->hold();
        m_app_held = true;
      }
      hide();
      return;
    }
    m_closing = false;
    if (m_check_after_job && m_job == Job::None && !m_have_pid)
      request_check();
  });
  dialog->show_all();
}

void UpdatesWindow::show_download_dialog()
{
  if (m_close_dialog != nullptr)
    return;
  m_ask_download = true;
  m_close_primary = "Stop downloading?";
  m_close_secondary = "Nothing will be installed.";
  auto* dialog = new Gtk::MessageDialog(*this, m_close_primary, false, Gtk::MESSAGE_QUESTION,
                                        Gtk::BUTTONS_NONE, true);
  dialog->set_secondary_text(m_close_secondary);
  dialog->add_button("_Keep installing", Gtk::RESPONSE_CANCEL);
  dialog->add_button("_Stop downloading", Gtk::RESPONSE_CLOSE);
  dialog->set_default_response(Gtk::RESPONSE_CANCEL);
  m_close_dialog = dialog;
  dialog->signal_response().connect([this](int response) {
    Gtk::MessageDialog* dying = m_close_dialog;
    m_close_dialog = nullptr;
    const bool hide_window = m_download_stop_hides;
    m_download_stop_hides = false;
    m_ask_download = false;
    if (dying != nullptr)
      dying->hide();
    Glib::signal_idle().connect_once([dying]() { delete dying; });
    if (response != Gtk::RESPONSE_CLOSE) {
      m_closing = false;
      if (m_check_after_job && m_job == Job::None && !m_have_pid)
        request_check();
      return;
    }
    request_stop();
    if (!hide_window) {
      m_closing = false;
      return;
    }
    m_leave_helper_running = false;
    m_background = true;
    if (auto app = get_application()) {
      app->hold();
      m_app_held = true;
    }
    hide();
  });
  dialog->show_all();
}

void UpdatesWindow::on_restart_clicked()
{
  if (!m_restart.get_sensitive() || m_restart_dialog != nullptr || m_job != Job::None)
    return;
  m_restart_primary = "Restart this computer now?";
  m_restart_secondary =
      "The computer will restart now. Open applications will be closed, so save your work first.";
  auto* dialog = new Gtk::MessageDialog(*this, m_restart_primary, false, Gtk::MESSAGE_QUESTION,
                                        Gtk::BUTTONS_NONE, true);
  dialog->set_secondary_text(m_restart_secondary);
  dialog->add_button("_Don't restart", Gtk::RESPONSE_CANCEL);
  dialog->add_button("_Restart now", Gtk::RESPONSE_OK);
  dialog->set_default_response(Gtk::RESPONSE_CANCEL);
  m_restart_dialog = dialog;
  dialog->signal_response().connect([this](int response) {
    Gtk::MessageDialog* dying = m_restart_dialog;
    m_restart_dialog = nullptr;
    if (dying != nullptr)
      dying->hide();
    Glib::signal_idle().connect_once([dying]() { delete dying; });
    if (response == Gtk::RESPONSE_OK)
      spawn_reboot();
  });
  dialog->show_all();
}

void UpdatesWindow::spawn_reboot()
{
  m_restart.set_sensitive(false);
  const char* bin = loginctl_path();
  std::vector<std::string> argv;
  argv.emplace_back(bin);
  argv.emplace_back("reboot");
  int err_fd = -1;
  Glib::Pid pid = 0;
  try {
    Glib::spawn_async_with_pipes(std::string(), argv, Glib::SPAWN_DO_NOT_REAP_CHILD,
                                 sigc::slot<void>(), &pid, nullptr, nullptr, &err_fd);
  } catch (const Glib::Error&) {
    set_status_text("Could not restart this computer.");
    m_restart.set_sensitive(true);
    return;
  }
  if (m_reboot_err_fd >= 0)
    ::close(m_reboot_err_fd);
  m_reboot_err_fd = err_fd;
  if (m_reboot_err_fd >= 0) {
    const int flags = fcntl(m_reboot_err_fd, F_GETFL, 0);
    if (flags >= 0)
      fcntl(m_reboot_err_fd, F_SETFL, flags | O_NONBLOCK);
  }
  set_status_text("Restarting…");
  m_reboot_watch.disconnect();
  m_reboot_watch = Glib::signal_child_watch().connect(
      [this](Glib::Pid child, int wait_status) {
        Glib::spawn_close_pid(child);
        m_reboot_watch.disconnect();
        const std::string reason = read_fd_text(m_reboot_err_fd);
        if (m_reboot_err_fd >= 0) {
          ::close(m_reboot_err_fd);
          m_reboot_err_fd = -1;
        }
        const bool ok = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
        if (!ok) {
          set_status_text(restart_failure_text(reason));
          m_restart.set_sensitive(true);
        }
      },
      pid);
}

void UpdatesWindow::on_quit()
{
  if (!prepare_close())
    return;
  hide();
}

bool UpdatesWindow::on_delete_event(GdkEventAny* event)
{
  if (!prepare_close())
    return true;
  return Gtk::ApplicationWindow::on_delete_event(event);
}

void UpdatesWindow::on_cancel_clicked()
{
  if (m_job == Job::None && !m_have_pid)
    return;
  if (m_close_dialog != nullptr)
    return;
  if (install_downloading()) {
    m_download_stop_hides = false;
    show_download_dialog();
    return;
  }
  request_stop();
}

void UpdatesWindow::on_check_clicked()
{
  if (m_closing || m_job != Job::None || m_have_pid)
    return;
  /* Keep the current rows until this check returns a definitive result.
   * The password dialog is not the check yet. */
  set_busy(true, "Waiting for authentication…");
  start_helper({"simulate"}, Job::Check, gui_timeout_ms(kAuthTimeoutMs));
  if (m_job != Job::None)
    acquire_inhibitors("Checking for updates");
  update_logout_warning();
}

void UpdatesWindow::start_reviewed_install()
{
  if (m_closing || m_job != Job::None || m_have_pid)
    return;
  std::vector<std::string> args;
  args.push_back("upgrade");
  const auto children = m_store->children();
  for (const auto& row : children) {
    const std::string name = static_cast<Glib::ustring>((*row)[m_cols.package]);
    const std::string ver = static_cast<Glib::ustring>((*row)[m_cols.new_version]);
    if (name.empty() || ver.empty())
      continue;
    args.push_back(name + "=" + ver);
  }
  if (m_new_store) {
    const auto fresh = m_new_store->children();
    for (const auto& row : fresh) {
      const std::string name = static_cast<Glib::ustring>((*row)[m_new_cols.package]);
      const std::string ver = static_cast<Glib::ustring>((*row)[m_new_cols.version]);
      if (name.empty() || ver.empty())
        continue;
      args.push_back(name + "=" + ver);
    }
  }
  if (args.size() < 2)
    return;
  set_busy(true, "Waiting for authentication…");
  start_helper(args, Job::Install, gui_timeout_ms(kAuthTimeoutMs));
  if (m_job != Job::None)
    acquire_inhibitors("Installing updates");
  update_logout_warning();
}

void UpdatesWindow::confirm_new_packages()
{
  if (m_new_dialog != nullptr || !m_new_store || m_new_store->children().empty())
    return;
  std::string names;
  const auto fresh = m_new_store->children();
  for (const auto& row : fresh) {
    const std::string name = static_cast<Glib::ustring>((*row)[m_new_cols.package]);
    if (name.empty())
      continue;
    if (!names.empty())
      names += ", ";
    names += name;
  }
  m_new_primary = "Install new packages with these updates?";
  m_new_secondary = "These new packages will be installed: " + names + ".";
  auto* dialog = new Gtk::MessageDialog(*this, m_new_primary, false, Gtk::MESSAGE_QUESTION,
                                        Gtk::BUTTONS_NONE, true);
  dialog->set_secondary_text(m_new_secondary);
  dialog->add_button("_Don't install", Gtk::RESPONSE_CANCEL);
  dialog->add_button("_Install updates", Gtk::RESPONSE_OK);
  dialog->set_default_response(Gtk::RESPONSE_CANCEL);
  m_new_dialog = dialog;
  dialog->signal_response().connect([this](int response) {
    Gtk::MessageDialog* dying = m_new_dialog;
    m_new_dialog = nullptr;
    if (dying != nullptr)
      dying->hide();
    Glib::signal_idle().connect_once([dying]() { delete dying; });
    if (response == Gtk::RESPONSE_OK)
      start_reviewed_install();
  });
  dialog->show_all();
}

void UpdatesWindow::on_install_clicked()
{
  if (m_closing || m_job != Job::None || m_have_pid || m_new_dialog != nullptr)
    return;
  if (m_new_store && !m_new_store->children().empty()) {
    confirm_new_packages();
    return;
  }
  start_reviewed_install();
}

void UpdatesWindow::acquire_inhibitors(const Glib::ustring& reason)
{
  release_inhibitors();
  Gtk::Application* app = nullptr;
  const auto ref = get_application();
  if (ref)
    app = ref.operator->();
  const InhibitHold hold = take_inhibitors(app, *this, reason);
  m_logind_fd = hold.logind_fd;
  m_inhibit_cookie = hold.cookie;
  m_inhibit_mode = m_logind_fd >= 0 ? "block" : "";
}

void UpdatesWindow::release_inhibitors()
{
  if (m_logind_fd >= 0) {
    ::close(m_logind_fd);
    m_logind_fd = -1;
  }
  if (m_inhibit_cookie != 0) {
    if (auto app = get_application())
      app->uninhibit(m_inhibit_cookie);
    m_inhibit_cookie = 0;
  }
  m_inhibit_mode.clear();
}

void UpdatesWindow::update_logout_warning()
{
  const bool show = m_job == Job::Install && m_logind_fd < 0 && m_inhibit_cookie == 0;
  if (show) {
    m_warning.set_text("Don't log out or shut down until the install finishes.");
    m_warning.show();
  } else {
    m_warning.hide();
  }
}

Glib::ustring UpdatesWindow::phase_age_suffix() const
{
  if (m_phase_mark_us <= 0)
    return {};
  const int sec = static_cast<int>((g_get_monotonic_time() - m_phase_mark_us) / G_USEC_PER_SEC);
  if (sec < 1)
    return {};
  if (sec < 60)
    return " (" + std::to_string(sec) + (sec == 1 ? " second)" : " seconds)");
  const int minutes = sec / 60;
  return " (" + std::to_string(minutes) + (minutes == 1 ? " minute)" : " minutes)");
}

void UpdatesWindow::refresh_commit_note()
{
  if (!m_install_committed)
    return;
  Glib::ustring note = "This install will finish on its own.";
  const int sec = m_phase_mark_us > 0
                      ? static_cast<int>((g_get_monotonic_time() - m_phase_mark_us) / G_USEC_PER_SEC)
                      : 0;
  Glib::ustring duration;
  if (sec >= 1) {
    if (sec < 60)
      duration = " for " + std::to_string(sec) + (sec == 1 ? " second" : " seconds");
    else {
      const int minutes = sec / 60;
      duration = " for " + std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
    }
  }
  if (m_dpkg_started) {
    const Glib::ustring pkg = package_being_configured(m_progress_line);
    if (pkg.empty())
      note += " Configuring packages" + duration + ".";
    else
      note += " Configuring " + pkg + duration + ".";
  } else {
    note += " Downloading updates" + duration + ".";
  }
  m_commit_note.set_text(note);
  m_commit_note.show();
}

void UpdatesWindow::apply_phase_status()
{
  if (m_job == Job::None || m_stopping || !m_helper_ready)
    return;
  if (m_install_committed && m_progress_mark_us > 0) {
    const int stalled = static_cast<int>((g_get_monotonic_time() - m_progress_mark_us) / G_USEC_PER_SEC);
    if (stalled >= stall_notice_sec())
      return;
  }
  Glib::ustring text;
  if (m_dpkg_started) {
    const Glib::ustring pkg = package_being_configured(m_progress_line);
    if (pkg.empty())
      text = "Configuring packages…";
    else
      text = "Configuring " + pkg + "…";
  } else if (progress_line_visible(m_progress_line) && !apt_plan_line(m_progress_line))
    text = friendly_progress_text(std::string(m_progress_line), std::string(m_phase));
  else if (m_phase == "refresh")
    text = "Refreshing package lists…";
  else if (m_phase == "download")
    text = "Downloading updates…";
  else if (m_phase == "simulate")
    text = m_job == Job::Install ? "Checking the package list…" : "Checking for updates…";
  else
    return;
  if (!progress_has_count(text))
    text += phase_age_suffix();
  set_status_text(text);
  refresh_commit_note();
}

void UpdatesWindow::note_helper_activity()
{
  if (m_helper_ready)
    m_progress_mark_us = g_get_monotonic_time();
  if (!m_helper_ready || m_dpkg_started || m_install_committed ||
      (m_job != Job::Check && m_job != Job::Install))
    return;
  if (m_phase.empty() && m_progress_line.empty())
    return;
  arm_job_timeout();
}

void UpdatesWindow::arm_phase_timer()
{
  if (m_phase_mark_us <= 0)
    m_phase_mark_us = g_get_monotonic_time();
  if (m_progress_mark_us <= 0)
    m_progress_mark_us = m_phase_mark_us;
  if (m_phase_timer.connected())
    return;
  m_phase_timer = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_phase_tick), 1000);
}

void UpdatesWindow::relax_logind_inhibitor()
{
  if (m_inhibit_relaxed)
    return;
  m_inhibit_relaxed = true;
  const bool had_logind = m_logind_fd >= 0;
  if (m_logind_fd >= 0) {
    ::close(m_logind_fd);
    m_logind_fd = -1;
  }
  if (m_inhibit_cookie != 0) {
    if (auto app = get_application())
      app->uninhibit(m_inhibit_cookie);
    m_inhibit_cookie = 0;
  }
  m_delay_inhibit = false;
  if (had_logind) {
    const int fd = take_logind("Installing updates", "delay");
    if (fd >= 0) {
      m_logind_fd = fd;
      m_delay_inhibit = true;
      m_inhibit_mode = "delay";
    } else {
      m_inhibit_mode = "released";
    }
  } else {
    m_inhibit_mode = "released";
  }
  update_logout_warning();
}

void UpdatesWindow::send_stall_notification(const Glib::ustring& body)
{
  ++m_stall_notifications;
  m_notification = body;
  if (auto app = get_application()) {
    try {
      auto note = Gio::Notification::create(lcos_updates::kProductName);
      note->set_body(body);
      app->send_notification("lcos-updates-stall", note);
    } catch (const Glib::Error&) {
    }
  }
}

bool UpdatesWindow::on_phase_tick()
{
  if (m_job == Job::None || !m_helper_ready)
    return false;
  if (m_stopping)
    return true;
  const int stalled = m_progress_mark_us > 0
                          ? static_cast<int>((g_get_monotonic_time() - m_progress_mark_us) / G_USEC_PER_SEC)
                          : 0;
  const bool stalled_install = m_job == Job::Install && m_install_committed && stalled >= stall_notice_sec();
  if (stalled_install) {
    if (stalled >= stall_inhibit_sec())
      relax_logind_inhibitor();
    const int minutes = stalled / 60;
    Glib::ustring text = "Still installing — no progress for " + std::to_string(minutes) +
                         " minutes. The install is still running; don't turn off the computer.";
    if (m_inhibit_relaxed) {
      if (m_delay_inhibit)
        text += " Shutdown will wait a short time instead of being blocked.";
      else
        text += " Shutdown is no longer blocked.";
    }
    set_status_text(text);
    refresh_commit_note();
    if (!get_visible() || m_background) {
      /* 10 minutes with no progress: tell them the install is still running.
       * The inhibitor is still a block. The 30-minute notice is a second one,
       * sent when shutdown stops being blocked. */
      if (!m_stall_notice_sent) {
        m_stall_notice_sent = true;
        Glib::ustring notice = "Still installing — no progress for " + std::to_string(minutes) +
                               " minutes. The install is still running; don't turn off the computer.";
        send_stall_notification(notice);
      }
      if (m_inhibit_relaxed && !m_stall_inhibit_sent) {
        m_stall_inhibit_sent = true;
        send_stall_notification(text);
      }
    }
    return true;
  }
  apply_phase_status();
  return true;
}

void UpdatesWindow::notify_and_leave(const Glib::ustring& body)
{
  m_notification = body;
  if (auto app = get_application()) {
    try {
      auto note = Gio::Notification::create(lcos_updates::kProductName);
      note->set_body(body);
      app->send_notification("lcos-updates-finished", note);
    } catch (const Glib::Error&) {
    }
  }
  release_inhibitors();
  update_logout_warning();
  if (m_app_held) {
    if (auto app = get_application())
      app->release();
    m_app_held = false;
  }
  m_leave_idle.disconnect();
  m_leave_idle = Glib::signal_idle().connect([this]() {
    m_background = false;
    if (auto app = get_application())
      app->quit();
    return false;
  });
}

bool UpdatesWindow::on_key_press_event(GdkEventKey* event)
{
  if (event != nullptr && event->keyval == GDK_KEY_Escape && m_cancel.get_sensitive()) {
    on_cancel_clicked();
    return true;
  }
  return Gtk::ApplicationWindow::on_key_press_event(event);
}

#ifdef LCOS_UPDATES_TEST
void UpdatesWindow::test_confirm_close()
{
  if (m_close_dialog != nullptr)
    m_close_dialog->response(Gtk::RESPONSE_CLOSE);
}

void UpdatesWindow::test_keep_open()
{
  if (m_close_dialog != nullptr)
    m_close_dialog->response(Gtk::RESPONSE_CANCEL);
}

bool UpdatesWindow::buttons_inside_window_for_test() const
{
  int bottom = 0;
  if (!gtk_widget_translate_coordinates(GTK_WIDGET(m_buttons.gobj()), GTK_WIDGET(gobj()), 0,
                                        m_buttons.get_allocated_height(), nullptr, &bottom))
    return false;
  return bottom <= get_allocated_height() + 2;
}

int UpdatesWindow::button_x_for_test(const Gtk::Widget& widget) const
{
  int x = 0;
  int y = 0;
  if (!gtk_widget_translate_coordinates(GTK_WIDGET(widget.gobj()), GTK_WIDGET(gobj()), 0, 0, &x, &y))
    return -1;
  return x;
}

void UpdatesWindow::test_confirm_restart()
{
  if (m_restart_dialog != nullptr)
    m_restart_dialog->response(Gtk::RESPONSE_OK);
}

void UpdatesWindow::test_cancel_restart()
{
  if (m_restart_dialog != nullptr)
    m_restart_dialog->response(Gtk::RESPONSE_CANCEL);
}

bool UpdatesWindow::restart_default_is_cancel_for_test() const
{
  if (m_restart_dialog == nullptr)
    return false;
  auto* def = dynamic_cast<Gtk::Button*>(m_restart_dialog->get_default_widget());
  return def != nullptr && def->get_label() == "_Don't restart";
}

int UpdatesWindow::package_rows_for_test() const
{
  return static_cast<int>(m_store->children().size());
}

Glib::ustring UpdatesWindow::package_at_row_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.package];
}

int UpdatesWindow::new_package_rows_for_test() const
{
  if (!m_new_store)
    return 0;
  return static_cast<int>(m_new_store->children().size());
}

Glib::ustring UpdatesWindow::new_package_at_row_for_test(int row) const
{
  if (!m_new_store)
    return {};
  const auto children = m_new_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_new_cols.package];
}

bool UpdatesWindow::new_packages_visible_for_test() const
{
  return m_new_heading.get_visible() && m_new_scroller.get_visible();
}

void UpdatesWindow::test_confirm_new_packages()
{
  if (m_new_dialog != nullptr)
    m_new_dialog->response(Gtk::RESPONSE_OK);
}

void UpdatesWindow::test_cancel_new_packages()
{
  if (m_new_dialog != nullptr)
    m_new_dialog->response(Gtk::RESPONSE_CANCEL);
}

bool UpdatesWindow::new_default_is_cancel_for_test() const
{
  if (m_new_dialog == nullptr)
    return false;
  auto* def = dynamic_cast<Gtk::Button*>(m_new_dialog->get_default_widget());
  return def != nullptr && def->get_label() == "_Don't install";
}

Glib::ustring UpdatesWindow::size_at_row_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.size];
}

Glib::ustring UpdatesWindow::security_at_row_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.security];
}

bool UpdatesWindow::columns_fit_for_test() const
{
  const int tree = m_view.get_allocated_width();
  if (tree < 200)
    return false;
  const auto hadj = m_scroller.get_hadjustment();
  if (!hadj || hadj->get_page_size() + 1 < hadj->get_upper())
    return false;
  const int n = m_view.get_n_columns();
  if (n < 5)
    return false;
  for (int i = 0; i < n; ++i) {
    const Gtk::TreeViewColumn* column = m_view.get_column(i);
    if (column == nullptr || column->get_width() <= 0)
      return false;
    if (column->get_x_offset() + column->get_width() > tree + 2)
      return false;
    const auto* cell = dynamic_cast<const Gtk::CellRendererText*>(column->get_first_cell());
    if (cell == nullptr)
      return false;
    if (i == 0) {
      if (cell->property_ellipsize() != Pango::ELLIPSIZE_END)
        return false;
    } else if (i == 1 || i == 2) {
      if (cell->property_ellipsize() != Pango::ELLIPSIZE_MIDDLE || !column->get_resizable())
        return false;
    } else if (cell->property_ellipsize() != Pango::ELLIPSIZE_NONE) {
      return false;
    }
  }
  return true;
}

bool UpdatesWindow::version_columns_ready_for_test() const
{
  for (int index : {1, 2}) {
    const Gtk::TreeViewColumn* column = m_view.get_column(index);
    if (column == nullptr || !column->get_resizable())
      return false;
    const auto* cell = dynamic_cast<const Gtk::CellRendererText*>(column->get_first_cell());
    if (cell == nullptr || cell->property_ellipsize() != Pango::ELLIPSIZE_MIDDLE)
      return false;
  }
  return true;
}

Glib::ustring UpdatesWindow::version_tip_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.version_tip];
}

Glib::ustring UpdatesWindow::old_version_at_row_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.old_version];
}

Glib::ustring UpdatesWindow::new_version_at_row_for_test(int row) const
{
  const auto children = m_store->children();
  if (row < 0 || row >= static_cast<int>(children.size()))
    return {};
  return (*children[static_cast<std::size_t>(row)])[m_cols.new_version];
}

bool UpdatesWindow::ask_default_is_cancel_for_test() const
{
  if (m_close_dialog == nullptr)
    return false;
  auto* def = dynamic_cast<Gtk::Button*>(m_close_dialog->get_default_widget());
  if (def == nullptr)
    return false;
  const Glib::ustring label = def->get_label();
  return label == "_Keep installing" || label == "_Keep open";
}
#endif
