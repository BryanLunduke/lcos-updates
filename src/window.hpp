/* lcos-updates — GTK3 window (never runs as root).
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LCOS_UPDATES_WINDOW_HPP
#define LCOS_UPDATES_WINDOW_HPP

#include "apt-parse.hpp"

#include <gtkmm.h>
#include <string>

namespace lcos_updates {
constexpr const char* kVersion = "0.9";
constexpr const char* kProductName = "LCOS Updates";
constexpr const char* kAppId = "org.lunduke.LcosUpdates";
/* Real hicolor artwork is not in the tree. The SVG only references a missing
 * PNG, so the menu and window use the stock software-update icon. */
constexpr const char* kIconName = "system-software-update";
}  // namespace lcos_updates

class UpdatesWindow : public Gtk::ApplicationWindow {
public:
  explicit UpdatesWindow(bool check_on_start);
  ~UpdatesWindow() override;

  /* Start a check for this launch. A later call is not implied by an earlier one. */
  void request_check();

protected:
  void on_check_clicked();
  void on_install_clicked();
  void on_about();
  bool on_delete_event(GdkEventAny* event) override;

private:
  enum class Job { None, Check, Install };

  void set_busy(bool busy, const Glib::ustring& status);
  void set_idle_status(const Glib::ustring& status);
  void start_helper(const char* helper_arg, Job job, int timeout_ms);
  /* Close the cancel pipe and, before authentication, signal pkexec.
   * Does not walk /proc and does not sleep. The child watch finishes the job. */
  void request_stop();
  void finish_job();
  void release_job_io();
  void maybe_rearm_job_timeout();
  void maybe_note_dpkg();
  void show_timeout_message(Job job, bool helper_ready);
  void use_check_as_default();
  void use_install_as_default();
  void on_check_idle();
  bool on_stdout(Glib::IOCondition cond);
  bool on_stderr(Glib::IOCondition cond);
  bool on_timeout();
  bool on_stop_retry();
  bool on_pulse_tick();
  void on_child_exited(Glib::Pid pid, int wait_status);
  void apply_check_result(const SimulateResult& result, int wait_status);
  void apply_install_result(const SimulateResult& result, int wait_status);
  void show_packages(const std::vector<PackageUpgrade>& packages);
  void show_package_list();
  void hide_package_list();

  Gtk::Box m_vbox{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::MenuBar m_menubar;
  Gtk::Box m_content{Gtk::ORIENTATION_VERTICAL, 10};
  Gtk::Box m_status_row{Gtk::ORIENTATION_HORIZONTAL, 8};
  Gtk::Spinner m_spinner;
  Gtk::Label m_status;
  Gtk::ProgressBar m_progress;
  Gtk::ScrolledWindow m_scroller;
  Gtk::TreeView m_view;
  Gtk::ButtonBox m_buttons{Gtk::ORIENTATION_HORIZONTAL};
  Gtk::Button m_check{"Check for updates"};
  Gtk::Button m_install{"Install updates"};

  class ModelColumns : public Gtk::TreeModel::ColumnRecord {
  public:
    ModelColumns()
    {
      add(package);
      add(old_version);
      add(new_version);
    }
    Gtk::TreeModelColumn<Glib::ustring> package;
    Gtk::TreeModelColumn<Glib::ustring> old_version;
    Gtk::TreeModelColumn<Glib::ustring> new_version;
  };

  ModelColumns m_cols;
  Glib::RefPtr<Gtk::ListStore> m_store;

  Job m_job = Job::None;
  Glib::Pid m_pid = 0;
  bool m_have_pid = false;
  int m_out_fd = -1;
  int m_err_fd = -1;
  Glib::RefPtr<Glib::IOChannel> m_out_ch;
  Glib::RefPtr<Glib::IOChannel> m_err_ch;
  sigc::connection m_out_watch;
  sigc::connection m_err_watch;
  sigc::connection m_child_watch;
  sigc::connection m_timeout;
  sigc::connection m_pulse;
  sigc::connection m_retry;
  std::string m_stdout;
  std::string m_stderr;
  int m_cancel_fd = -1;
  bool m_helper_ready = false;
  bool m_dpkg_started = false;
  bool m_leave_helper_running = false;
  bool m_check_queued = false;
  bool m_check_after_job = false;
  bool m_stop_was_timeout = false;
  Job m_stopped_job = Job::None;
};

#endif
