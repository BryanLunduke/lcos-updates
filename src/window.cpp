/* lcos-updates — GTK3 window (never runs as root).
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.hpp"

#include "timeouts.hpp"

#include <cstdlib>
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

std::string join_names(const std::vector<std::string>& names)
{
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0)
      out += ", ";
    out += names[i];
  }
  return out;
}

Glib::ustring explain_lists(const SimulateResult& result)
{
  Glib::ustring out;
  auto add = [&](const Glib::ustring& sentence) {
    if (sentence.empty())
      return;
    if (!out.empty())
      out += " ";
    out += sentence;
  };
  if (!result.phased.empty())
    add("These updates are waiting and will be offered later: " + join_names(result.phased) + ".");
  if (!result.held.empty())
    add("These packages are held: " + join_names(result.held) + ".");
  if (!result.kept_back.empty())
    add("This tool will not install updates that need extra packages: " + join_names(result.kept_back) +
        ".");
  else if (result.phased.empty() && result.held.empty() && result.not_upgraded_count > 0)
    add("This tool will not install updates that need extra packages.");
  return out;
}

Glib::ustring reboot_sentence(const SimulateResult& result)
{
  if (!result.reboot_required)
    return {};
  Glib::ustring sentence("Restart to finish installing updates.");
  if (!result.reboot_pkgs.empty()) {
    sentence += " ";
    sentence += join_names(result.reboot_pkgs);
    sentence += ".";
  }
  return sentence;
}

Glib::ustring conf_sentence(const SimulateResult& result)
{
  if (result.conffiles_kept.empty())
    return {};
  Glib::ustring sentence("Existing configuration was kept for ");
  sentence += join_names(result.conffiles_kept);
  sentence += ".";
  return sentence;
}

bool result_has_kept(const SimulateResult& result)
{
  return result.status == SimulateResult::KeptBack || !result.kept_back.empty() ||
         !result.phased.empty() || !result.held.empty() || result.not_upgraded_count > 0;
}

/* Index warnings go above the headline so a partial refresh is not "up to date" first. */
Glib::ustring with_warning(Glib::ustring status, const std::string& warning, JobKind kind)
{
  if (warning.empty())
    return status;
  const std::string shown = friendly_job_error(warning, kind);
  if (shown.empty())
    return status;
  if (status.empty())
    return shown;
  return Glib::ustring(shown) + "\n" + status;
}

struct InhibitHold {
  int logind_fd = -1;
  guint cookie = 0;
};

/* elogind's login1 block inhibitor, plus Gtk's session inhibit. Either may
 * fail; the caller shows a logout warning when both do. */
InhibitHold take_inhibitors(Gtk::Application* app, Gtk::Window& window, const Glib::ustring& reason)
{
  InhibitHold hold;
  try {
    auto conn = Gio::DBus::Connection::get_sync(Gio::DBus::BUS_TYPE_SYSTEM);
    auto proxy = Gio::DBus::Proxy::create_sync(conn, "org.freedesktop.login1",
                                               "/org/freedesktop/login1",
                                               "org.freedesktop.login1.Manager");
    GError* error = nullptr;
    GUnixFDList* out_fds = nullptr;
    GVariant* result = g_dbus_proxy_call_with_unix_fd_list_sync(
        G_DBUS_PROXY(proxy->gobj()), "Inhibit",
        g_variant_new("(ssss)", "shutdown:sleep", "LCOS Updates", reason.c_str(), "block"),
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &out_fds, nullptr, &error);
    if (error != nullptr) {
      g_error_free(error);
    } else if (result != nullptr && out_fds != nullptr) {
      gint32 index = -1;
      g_variant_get(result, "(h)", &index);
      if (index >= 0)
        hold.logind_fd = g_unix_fd_list_get(out_fds, index, nullptr);
    }
    if (result != nullptr)
      g_variant_unref(result);
    if (out_fds != nullptr)
      g_object_unref(out_fds);
  } catch (const Glib::Error&) {
  }
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

Glib::ustring skipped_headline(const SimulateResult& result)
{
  const bool only_phased = !result.phased.empty() && result.kept_back.empty() && result.held.empty();
  const bool only_held = !result.held.empty() && result.kept_back.empty() && result.phased.empty();
  if (only_phased || only_held)
    return explain_lists(result);
  return "Some updates were kept back. " + explain_lists(result);
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
  m_status.set_selectable(true);
  m_status.set_xalign(0.0f);
  m_status.set_yalign(0.0f);
  m_status.set_text("Check for updates for your Computer.");

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
  m_view.set_headers_visible(true);
  m_view.get_selection()->set_mode(Gtk::SELECTION_NONE);

  m_scroller.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
  m_scroller.set_shadow_type(Gtk::SHADOW_IN);
  m_scroller.set_min_content_height(180);
  m_scroller.set_no_show_all(true);
  m_scroller.add(m_view);
  m_scroller.hide();

  m_buttons.set_layout(Gtk::BUTTONBOX_END);
  m_buttons.set_spacing(8);
  m_install.set_sensitive(false);
  m_install.set_can_default(true);
  m_check.set_can_default(true);
  m_cancel.set_sensitive(false);
  m_cancel.set_can_default(true);
  m_buttons.pack_start(m_cancel, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_install, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_check, Gtk::PACK_SHRINK);
  m_check.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_check_clicked));
  m_install.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_install_clicked));
  m_cancel.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_cancel_clicked));

  m_content.set_border_width(12);
  m_content.pack_start(m_status_row, Gtk::PACK_SHRINK);
  m_content.pack_start(m_commit_note, Gtk::PACK_SHRINK);
  m_content.pack_start(m_warning, Gtk::PACK_SHRINK);
  m_content.pack_start(m_progress, Gtk::PACK_SHRINK);
  m_content.pack_start(m_scroller, Gtk::PACK_EXPAND_WIDGET);
  m_content.pack_start(m_buttons, Gtk::PACK_SHRINK);

  m_vbox.pack_start(m_menubar, Gtk::PACK_SHRINK);
  /* SHRINK while the list is hidden so expand space does not open under the
   * buttons. show_package_list() switches this to EXPAND when the list appears. */
  m_vbox.pack_start(m_content, Gtk::PACK_SHRINK);
  add(m_vbox);
  show_all_children();
  m_check.grab_default();
  set_default(m_check);

  if (check_on_start)
    request_check();
}

UpdatesWindow::~UpdatesWindow()
{
  m_leave_idle.disconnect();
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

void UpdatesWindow::set_busy(bool busy, const Glib::ustring& status)
{
  /* Always replace prior error/status text — never leave a stale string. */
  m_status.set_text(status);
  m_check.set_sensitive(!busy);
  m_cancel.set_sensitive(busy && !m_install_committed);
  if (busy)
    m_install.set_sensitive(false);
  if (busy && !m_install_committed)
    use_cancel_as_default();
  m_pulse.disconnect();
  if (busy) {
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
  m_progress.pulse();
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
  m_store->clear();
  for (const auto& pkg : packages) {
    Gtk::TreeModel::Row row = *(m_store->append());
    row[m_cols.package] = pkg.name;
    row[m_cols.old_version] = pkg.old_version;
    row[m_cols.new_version] = pkg.new_version;
  }
  show_package_list();
}

void UpdatesWindow::show_package_list()
{
  /* Explicit show(): show_all() is a no-op while no_show_all is set on the scroller. */
  m_view.show();
  m_scroller.show();
  /* gtkmm Box no longer wraps this; expand content so the scroller absorbs growth. */
  gtk_box_set_child_packing(m_vbox.gobj(), GTK_WIDGET(m_content.gobj()), TRUE, TRUE, 0,
                            GTK_PACK_START);
  resize(560, 420);
  use_install_as_default();
}

void UpdatesWindow::hide_package_list()
{
  m_store->clear();
  m_scroller.hide();
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

void UpdatesWindow::use_check_as_default()
{
  m_check.set_can_default(true);
  set_default(m_check);
  m_check.grab_default();
}

void UpdatesWindow::use_install_as_default()
{
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
      m_status.set_text("Configuring packages…");
    return;
  }
  /* Closing the pipe is the cancel. The helper stops apt-get during
   * download. It will not signal a configuring dpkg. Do not walk /proc. */
  if (m_cancel_fd >= 0) {
    ::close(m_cancel_fd);
    m_cancel_fd = -1;
  }
  const Job job = m_job;
  m_check.set_sensitive(false);
  m_install.set_sensitive(false);
  m_status.set_text(job == Job::Install ? "Stopping the update…" : "Stopping the update check…");
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
  m_stopped_job = Job::None;
  m_markers.clear();
  m_err_partial.clear();
  m_phase.clear();
  m_progress_line.clear();
  m_commit_note.hide();
  release_inhibitors();
  update_logout_warning();
}

void UpdatesWindow::arm_job_timeout()
{
  if (m_dpkg_started || (m_job != Job::Check && m_job != Job::Install))
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
  m_phase.clear();
  m_progress_line.clear();
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
    else if (line.compare(0, 6, "PHASE ") == 0)
      m_phase = line.substr(6);
    else if (line.compare(0, 9, "PROGRESS ") == 0) {
      m_progress_line = line.substr(9);
      if (m_progress_line.size() > 240)
        m_progress_line.resize(240);
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
      maybe_rearm_job_timeout();
      maybe_note_dpkg();
      apply_phase_status();
      note_helper_activity();
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
  show_job_progress();
  if (!m_dpkg_started && m_phase.empty() && m_progress_line.empty())
    m_status.set_text(m_job == Job::Install ? "Installing updates…" : "Checking for updates…");
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
    if (m_job == Job::Install)
      m_install_committed = true;
    m_timeout.disconnect();
    m_retry.disconnect();
    m_stop_was_timeout = false;
    m_status.set_text("Configuring packages…");
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
  arm_job_timeout();
  apply_phase_status();
  if (m_progress_line.empty() && m_phase.empty())
    m_status.set_text(m_job == Job::Install ? "Installing updates…" : "Checking for updates…");
}

void UpdatesWindow::show_timeout_message(Job job, bool helper_ready)
{
  const bool keep = !m_store->children().empty();
  if (!helper_ready) {
    if (!keep)
      hide_package_list();
    set_idle_status("Timed out waiting for authentication. Try again.");
  } else if (job == Job::Check) {
    if (!keep)
      hide_package_list();
    set_idle_status("Timed out waiting for the update check. Check your network and try again.");
  } else {
    set_idle_status("Timed out while installing updates.");
  }
  m_check.set_sensitive(true);
  m_install.set_sensitive(keep);
}

bool UpdatesWindow::on_timeout()
{
  if (m_job == Job::None && !m_have_pid)
    return false;
  if (m_dpkg_started || m_install_committed) {
    m_cancel.set_sensitive(false);
    m_commit_note.show();
    if (m_dpkg_started)
      m_status.set_text("Configuring packages…");
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

  if (job == Job::Check)
    apply_check_result(parsed, wait_status);
  else if (job == Job::Install)
    apply_install_result(parsed, wait_status);
  else
    set_idle_status(m_status.get_text());

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
  const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  if ((result.status == SimulateResult::UpToDate || result.status == SimulateResult::Success) &&
      !result_has_kept(result)) {
    hide_package_list();
    m_install.set_sensitive(false);
    /* Clear any prior error; show a definite idle/success string. */
    set_idle_status(with_warning("You're up to date.", result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }
  if (result.status == SimulateResult::Upgrades && !result.packages.empty()) {
    show_packages(result.packages);
    m_install.set_sensitive(true);
    Glib::ustring status = "Updates are available.";
    const Glib::ustring kept = explain_lists(result);
    if (!kept.empty())
      status += " " + kept;
    set_idle_status(with_warning(status, result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }
  if (result.status == SimulateResult::KeptBack ||
      (result.packages.empty() && result_has_kept(result))) {
    hide_package_list();
    m_install.set_sensitive(false);
    Glib::ustring status = skipped_headline(result);
    set_idle_status(with_warning(status, result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }

  /* Not a definitive result: leave the previous rows in place. */
  const bool keep = !m_store->children().empty();
  if (!keep)
    hide_package_list();
  std::string msg = result.error_msg;
  if (msg.empty() || msg == "No STATUS from helper") {
    const std::string auth = authentication_message({}, exit_code);
    if (!auth.empty())
      msg = auth;
    else if (exit_code == 127 || exit_code == 126)
      msg = "Could not run the update helper (pkexec).";
    else
      msg = "The update check failed.";
  }
  set_idle_status(friendly_job_error(msg, JobKind::Check));
  m_check.set_sensitive(true);
  m_install.set_sensitive(keep);
}

void UpdatesWindow::apply_install_result(const SimulateResult& result, int wait_status)
{
  const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  const Glib::ustring conf = conf_sentence(result);
  if (result.install_skipped) {
    Glib::ustring status = "The update list changed. Nothing was installed.";
    if (result.status == SimulateResult::Upgrades && !result.packages.empty()) {
      show_packages(result.packages);
      m_install.set_sensitive(true);
      const Glib::ustring kept = explain_lists(result);
      if (!kept.empty())
        status += " " + kept;
      if (!conf.empty())
        status += " " + conf;
      set_idle_status(with_warning(status, result.warning, JobKind::Install));
      m_check.set_sensitive(true);
      return;
    }
    hide_package_list();
    m_install.set_sensitive(false);
    if (result_has_kept(result))
      status += " " + explain_lists(result);
    else if (result.status == SimulateResult::UpToDate || result.status == SimulateResult::Success)
      status += " You're up to date.";
    if (!conf.empty())
      status += " " + conf;
    set_idle_status(with_warning(status, result.warning, JobKind::Install));
    m_check.set_sensitive(true);
    return;
  }
  const Glib::ustring reboot = reboot_sentence(result);
  auto finish_installed = [&](Glib::ustring status) {
    if (!reboot.empty()) {
      if (!status.empty())
        status += " ";
      status += reboot;
    }
    hide_package_list();
    m_install.set_sensitive(false);
    set_idle_status(with_warning(status, result.warning, JobKind::Install));
    m_check.set_sensitive(true);
  };
  if (result.summary_missing && result.status != SimulateResult::Error) {
    Glib::ustring status =
        "Updates installed. Apt did not report whether any packages were kept back.";
    if (!conf.empty())
      status += " " + conf;
    finish_installed(status);
    return;
  }
  if (result.status != SimulateResult::Error && result_has_kept(result)) {
    Glib::ustring status = result.status == SimulateResult::Success ? "Updates installed. "
                                                                    : skipped_headline(result);
    if (result.status == SimulateResult::Success)
      status += explain_lists(result);
    if (!conf.empty())
      status += " " + conf;
    finish_installed(status);
    return;
  }
  if (!conf.empty() && result.status != SimulateResult::Error) {
    finish_installed("Updates installed. " + conf);
    return;
  }
  if (result.status == SimulateResult::Success || result.status == SimulateResult::UpToDate ||
      (result.status != SimulateResult::Error && exit_code == 0)) {
    if (!reboot.empty())
      finish_installed(reboot);
    else
      finish_installed("Updates installed successfully. You're up to date.");
    return;
  }
  std::string msg = result.error_msg;
  if (msg.empty() || msg == "No STATUS from helper") {
    const std::string auth = authentication_message({}, exit_code);
    if (!auth.empty())
      msg = auth;
    else if (exit_code == 127 || exit_code == 126)
      msg = "Could not run the update helper (pkexec).";
    else
      msg = "apt-get upgrade failed.";
  }
  set_idle_status(friendly_job_error(msg, JobKind::Install));
  m_check.set_sensitive(true);
  /* Offer Check again; keep Install if we still had a list. */
  if (!m_store->children().empty())
    m_install.set_sensitive(true);
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
  m_closing = true;
  return true;
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

void UpdatesWindow::on_install_clicked()
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
  if (args.size() < 2)
    return;
  set_busy(true, "Waiting for authentication…");
  start_helper(args, Job::Install, gui_timeout_ms(kAuthTimeoutMs));
  if (m_job != Job::None)
    acquire_inhibitors("Installing updates");
  update_logout_warning();
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

void UpdatesWindow::apply_phase_status()
{
  if (m_job == Job::None || m_dpkg_started || !m_helper_ready)
    return;
  if (!m_progress_line.empty()) {
    m_status.set_text(m_progress_line);
    return;
  }
  if (m_phase == "refresh")
    m_status.set_text("Refreshing package lists…");
  else if (m_phase == "download")
    m_status.set_text("Downloading updates…");
  else if (m_phase == "simulate")
    m_status.set_text(m_job == Job::Install ? "Checking the package list…" : "Checking for updates…");
}

void UpdatesWindow::note_helper_activity()
{
  if (!m_helper_ready || m_dpkg_started || (m_job != Job::Check && m_job != Job::Install))
    return;
  if (m_phase.empty() && m_progress_line.empty())
    return;
  arm_job_timeout();
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

bool UpdatesWindow::buttons_inside_window_for_test() const
{
  int bottom = 0;
  if (!gtk_widget_translate_coordinates(GTK_WIDGET(m_buttons.gobj()), GTK_WIDGET(gobj()), 0,
                                        m_buttons.get_allocated_height(), nullptr, &bottom))
    return false;
  return bottom <= get_allocated_height() + 2;
}
#endif
