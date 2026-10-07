/* lcos-updates — GTK3 window (never runs as root).
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.hpp"

#include "timeouts.hpp"

#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
const char kHelperPath[] = "/usr/libexec/lcos-updates-helper";
/* 120s update + 120s simulate + the same 30s slack install already used.
 * Restarted when the helper announces HELPER_READY, so polkit time does not
 * eat the budget and a check still inside the helper's limits is not killed. */
const int kCheckTimeoutMs = kCheckTimeoutSec * 1000;
const int kInstallTimeoutMs = kInstallTimeoutSec * 1000;
const int kPulseIntervalMs = 100;

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
  return strip_helper_marker(std::move(err), "DPKG_STARTED\n");
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

Glib::ustring kept_sentence(const SimulateResult& result)
{
  if (!result.kept_back.empty()) {
    Glib::ustring sentence("This tool will not install updates that need extra packages: ");
    sentence += join_names(result.kept_back);
    sentence += ".";
    return sentence;
  }
  if (result.not_upgraded_count > 0)
    return "This tool will not install updates that need extra packages.";
  return {};
}

bool result_has_kept(const SimulateResult& result)
{
  return result.status == SimulateResult::KeptBack || !result.kept_back.empty() ||
         result.not_upgraded_count > 0;
}

Glib::ustring append_warning(Glib::ustring status, const std::string& warning, JobKind kind)
{
  if (warning.empty())
    return status;
  const std::string shown = friendly_job_error(warning, kind);
  if (shown.empty())
    return status;
  if (!status.empty())
    status += "\n";
  status += shown;
  return status;
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

  auto* help_menu = Gtk::manage(new Gtk::Menu());
  auto* about_item = Gtk::manage(new Gtk::MenuItem("_About…", true));
  about_item->signal_activate().connect(sigc::mem_fun(*this, &UpdatesWindow::on_about));
  help_menu->append(*about_item);
  auto* help_item = Gtk::manage(new Gtk::MenuItem("_Help", true));
  help_item->set_submenu(*help_menu);
  m_menubar.append(*help_item);
  m_menubar.show_all();

  m_status.set_line_wrap(true);
  m_status.set_xalign(0.0f);
  m_status.set_text("Check for updates for your Computer.");

  /* Spinner lives beside status (not in ButtonBox — GtkButtonBox is for buttons). */
  m_spinner.set_no_show_all(true);
  m_spinner.set_size_request(18, 18);
  m_status_row.pack_start(m_spinner, Gtk::PACK_SHRINK, 0);
  m_status_row.pack_start(m_status, Gtk::PACK_EXPAND_WIDGET, 0);

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
  m_buttons.pack_start(m_install, Gtk::PACK_SHRINK);
  m_buttons.pack_start(m_check, Gtk::PACK_SHRINK);
  m_check.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_check_clicked));
  m_install.signal_clicked().connect(sigc::mem_fun(*this, &UpdatesWindow::on_install_clicked));

  m_content.set_border_width(12);
  m_content.pack_start(m_status_row, Gtk::PACK_SHRINK);
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
  m_retry.disconnect();
  m_timeout.disconnect();
  m_pulse.disconnect();
  /* dpkg is already configuring. Closing the window must not signal it.
   * The helper ignores cancel once it has announced DPKG_STARTED. */
  if (m_leave_helper_running || m_dpkg_started) {
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
  if (m_job != Job::None || m_have_pid) {
    m_check_after_job = true;
    return;
  }
  if (m_check_queued)
    return;
  m_check_queued = true;
  Glib::signal_idle().connect_once(sigc::mem_fun(*this, &UpdatesWindow::on_check_idle));
}

void UpdatesWindow::on_check_idle()
{
  m_check_queued = false;
  on_check_clicked();
}

void UpdatesWindow::set_busy(bool busy, const Glib::ustring& status)
{
  /* Always replace prior error/status text — never leave a stale string. */
  m_status.set_text(status);
  m_check.set_sensitive(!busy);
  if (busy)
    m_install.set_sensitive(false);
  m_pulse.disconnect();
  if (busy) {
    m_spinner.show();
    m_spinner.start();
    m_progress.set_fraction(0.0);
    m_progress.pulse();
    m_progress.show();
    m_pulse = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_pulse_tick),
                                             kPulseIntervalMs);
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
  /* Closing the pipe is the cancel. The helper stops apt-get during
   * download. It will not signal dpkg. Do not walk /proc from here. */
  if (m_cancel_fd >= 0) {
    ::close(m_cancel_fd);
    m_cancel_fd = -1;
  }
  const Job job = m_stopped_job != Job::None ? m_stopped_job : m_job;
  m_check.set_sensitive(false);
  m_install.set_sensitive(false);
  m_status.set_text(job == Job::Install ? "Stopping the update…" : "Stopping the update check…");
  if (m_helper_ready || !m_have_pid)
    return;
  const pid_t child = static_cast<pid_t>(m_pid);
  if (child > 1)
    ::kill(child, SIGTERM);
  m_retry.disconnect();
  m_retry = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_stop_retry), 250);
}

void UpdatesWindow::finish_job()
{
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
  m_stop_was_timeout = false;
}

void UpdatesWindow::start_helper(const char* helper_arg, Job job, int timeout_ms)
{
  if (m_job != Job::None || m_have_pid)
    return;

  m_stdout.clear();
  m_stderr.clear();
  m_helper_ready = false;
  m_dpkg_started = false;
  m_stop_was_timeout = false;
  m_job = job;

  int cancel_pipe[2] = {-1, -1};
  if (pipe(cancel_pipe) != 0) {
    m_job = Job::None;
    set_idle_status("Could not run pkexec. Install policykit-1 and try again.");
    m_install.set_sensitive(!m_store->children().empty());
    return;
  }
  const int cancel_read = cancel_pipe[0];
  const int cancel_write = cancel_pipe[1];
  /* Child must not keep the write end across pkexec's exec, or the helper
   * never observes EOF. FD_CLOEXEC drops it even if child setup is skipped. */
  fcntl(cancel_write, F_SETFD, FD_CLOEXEC);

  std::vector<std::string> argv;
  argv.push_back("pkexec");
  argv.push_back(kHelperPath);
  argv.push_back(helper_arg);

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
    Glib::spawn_async_with_pipes(
        std::string(), argv,
        Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD, child_setup, &pid, nullptr,
        &out_fd, &err_fd);
  } catch (const Glib::SpawnError& e) {
    ::close(cancel_read);
    ::close(cancel_write);
    m_job = Job::None;
    set_idle_status("Could not run pkexec. Install policykit-1 and try again.");
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

bool UpdatesWindow::on_stdout(Glib::IOCondition cond)
{
  if (!m_out_ch)
    return false;
  if (cond & (Glib::IO_IN | Glib::IO_HUP | Glib::IO_ERR)) {
    gchar buf[4096];
    gsize n = 0;
    const Glib::IOStatus st = m_out_ch->read(buf, sizeof buf, n);
    if (n > 0)
      m_stdout.append(buf, n);
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
      m_stderr.append(buf, n);
    if (n > 0) {
      maybe_rearm_job_timeout();
      maybe_note_dpkg();
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
  if (m_stderr.find("HELPER_READY\n") == std::string::npos)
    return;
  m_helper_ready = true;
  if (m_dpkg_started)
    return;
  const int ms = m_job == Job::Install ? kInstallTimeoutMs : kCheckTimeoutMs;
  m_timeout.disconnect();
  m_timeout = Glib::signal_timeout().connect(sigc::mem_fun(*this, &UpdatesWindow::on_timeout), ms);
}

void UpdatesWindow::maybe_note_dpkg()
{
  if (m_dpkg_started)
    return;
  if (m_stderr.find("DPKG_STARTED\n") == std::string::npos)
    return;
  m_dpkg_started = true;
  /* Configuration has started. The deadline must not close the cancel pipe. */
  m_timeout.disconnect();
  m_retry.disconnect();
  m_stop_was_timeout = false;
  m_status.set_text("Configuring packages…");
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
  if (m_dpkg_started) {
    m_status.set_text(
        "Packages are being configured. Closing this window will not stop the installer.");
    return false;
  }
  m_stopped_job = m_job == Job::None ? m_stopped_job : m_job;
  m_stop_was_timeout = true;
  request_stop();
  return false;
}

bool UpdatesWindow::on_stop_retry()
{
  /* One signal to the pkexec pid. No /proc walk and no sleep. */
  if (!m_have_pid || m_helper_ready || m_dpkg_started)
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
        m_stdout.append(buf, n);
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
        m_stderr.append(buf, n);
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
  const std::string out = m_stdout;
  const std::string err = strip_helper_markers(m_stderr);
  finish_job();

  if (timed_out) {
    show_timeout_message(job, helper_ready);
    if (check_again)
      request_check();
    return;
  }

  SimulateResult parsed = parse_protocol(out);
  if (parsed.status == SimulateResult::Error && parsed.error_msg == "No STATUS from helper") {
    if (!err.empty()) {
      parsed.error_msg = err;
      /* pkexec cancel / dismiss */
      const Glib::ustring el = Glib::ustring(err).lowercase();
      if (el.find("dismiss") != Glib::ustring::npos || el.find("not authorized") != Glib::ustring::npos ||
          el.find("authentication") != Glib::ustring::npos)
        parsed.error_msg = "Authentication was cancelled or failed.";
    }
  }

  if (job == Job::Check)
    apply_check_result(parsed, wait_status);
  else if (job == Job::Install)
    apply_install_result(parsed, wait_status);
  else
    set_idle_status(m_status.get_text());

  if (check_again)
    request_check();
}

void UpdatesWindow::apply_check_result(const SimulateResult& result, int wait_status)
{
  const int exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : -1;
  if ((result.status == SimulateResult::UpToDate || result.status == SimulateResult::Success) &&
      !result_has_kept(result)) {
    hide_package_list();
    m_install.set_sensitive(false);
    /* Clear any prior error; show a definite idle/success string. */
    set_idle_status(append_warning("You're up to date.", result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }
  if (result.status == SimulateResult::Upgrades && !result.packages.empty()) {
    show_packages(result.packages);
    m_install.set_sensitive(true);
    Glib::ustring status = "Updates are available.";
    const Glib::ustring kept = kept_sentence(result);
    if (!kept.empty())
      status += " " + kept;
    set_idle_status(append_warning(status, result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }
  if (result.status == SimulateResult::KeptBack ||
      (result.packages.empty() && result_has_kept(result))) {
    hide_package_list();
    m_install.set_sensitive(false);
    Glib::ustring status = "Some updates were kept back. " + kept_sentence(result);
    set_idle_status(append_warning(status, result.warning, JobKind::Check));
    m_check.set_sensitive(true);
    return;
  }

  /* Not a definitive result: leave the previous rows in place. */
  const bool keep = !m_store->children().empty();
  if (!keep)
    hide_package_list();
  std::string msg = result.error_msg;
  if (msg.empty() || msg == "No STATUS from helper") {
    if (exit_code == 127 || exit_code == 126)
      msg = "Could not run the update helper (pkexec). Authentication may have been cancelled.";
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
  if (result.status != SimulateResult::Error && result_has_kept(result)) {
    hide_package_list();
    m_install.set_sensitive(false);
    Glib::ustring status = result.status == SimulateResult::Success
                               ? "Updates installed. "
                               : "Some updates were kept back. ";
    status += kept_sentence(result);
    set_idle_status(append_warning(status, result.warning, JobKind::Install));
    m_check.set_sensitive(true);
    return;
  }
  if (result.status == SimulateResult::Success || result.status == SimulateResult::UpToDate ||
      (result.status != SimulateResult::Error && exit_code == 0)) {
    hide_package_list();
    m_install.set_sensitive(false);
    /* Clear any prior error left from a failed check/install. */
    set_idle_status(append_warning("Updates installed successfully. You're up to date.",
                                   result.warning, JobKind::Install));
    m_check.set_sensitive(true);
    return;
  }
  std::string msg = result.error_msg;
  if (msg.empty() || msg == "No STATUS from helper") {
    if (exit_code == 127 || exit_code == 126)
      msg = "Could not run the update helper (pkexec). Authentication may have been cancelled.";
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

bool UpdatesWindow::on_delete_event(GdkEventAny* event)
{
  if (m_dpkg_started && m_have_pid && !m_leave_helper_running) {
    Gtk::MessageDialog dialog(*this, "Packages are being configured.", false, Gtk::MESSAGE_WARNING,
                              Gtk::BUTTONS_NONE, true);
    dialog.set_secondary_text(
        "Closing this window will not stop the installer. "
        "If the install is interrupted, run dpkg --configure -a.");
    dialog.add_button("_Keep open", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Close", Gtk::RESPONSE_CLOSE);
    dialog.set_default_response(Gtk::RESPONSE_CANCEL);
    if (dialog.run() != Gtk::RESPONSE_CLOSE)
      return true;
    m_leave_helper_running = true;
  }
  return Gtk::ApplicationWindow::on_delete_event(event);
}

void UpdatesWindow::on_check_clicked()
{
  if (m_job != Job::None || m_have_pid)
    return;
  /* Keep the current rows until this check returns a definitive result. */
  set_busy(true, "Checking for updates…");
  start_helper("simulate", Job::Check, kCheckTimeoutMs);
}

void UpdatesWindow::on_install_clicked()
{
  if (m_job != Job::None || m_have_pid)
    return;
  set_busy(true, "Installing updates…");
  start_helper("upgrade", Job::Install, kInstallTimeoutMs);
}
