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
#include <vector>

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

  /* True while a committed install is finishing with the window hidden. */
  bool retains_background_job() const { return m_background; }

#ifdef LCOS_UPDATES_TEST
  Glib::ustring status_text_for_test() const { return m_status.get_text(); }
  bool progress_visible_for_test() const { return m_progress.get_visible(); }
  bool cancel_sensitive_for_test() const { return m_cancel.get_sensitive(); }
  bool check_sensitive_for_test() const { return m_check.get_sensitive(); }
  bool install_sensitive_for_test() const { return m_install.get_sensitive(); }
  void test_click_check() { on_check_clicked(); }
  void test_click_install() { on_install_clicked(); }
  void test_click_cancel() { on_cancel_clicked(); }
  void test_quit() { on_quit(); }
  void test_confirm_close();
  Glib::ustring check_label_for_test() const { return m_check.get_label(); }
  Glib::ustring install_label_for_test() const { return m_install.get_label(); }
  Glib::ustring cancel_label_for_test() const { return m_cancel.get_label(); }
  bool button_mnemonics_for_test() const
  {
    return m_check.get_use_underline() && m_install.get_use_underline() && m_cancel.get_use_underline();
  }
  bool cancel_is_default_for_test() const { return get_default_widget() == &m_cancel; }
  bool status_selectable_for_test() const { return m_status.get_selectable(); }
  Glib::ustring notification_text_for_test() const { return m_notification; }
  bool background_for_test() const { return m_background; }
  bool warning_visible_for_test() const { return m_warning.get_visible(); }
  Glib::ustring warning_text_for_test() const { return m_warning.get_text(); }
  Glib::ustring commit_note_for_test() const { return m_commit_note.get_text(); }
  bool commit_note_visible_for_test() const { return m_commit_note.get_visible(); }
  Glib::ustring close_primary_for_test() const { return m_close_primary; }
  Glib::ustring close_secondary_for_test() const { return m_close_secondary; }
  int allocated_height_for_test() const { return get_allocated_height(); }
  int allocated_width_for_test() const { return get_allocated_width(); }
  bool buttons_inside_window_for_test() const;
#endif

protected:
  void on_check_clicked();
  void on_install_clicked();
  void on_about();
  bool on_delete_event(GdkEventAny* event) override;
  bool on_key_press_event(GdkEventKey* event) override;

private:
  enum class Job { None, Check, Install };

  void set_busy(bool busy, const Glib::ustring& status);
  void set_idle_status(const Glib::ustring& status);
  void start_helper(const std::vector<std::string>& helper_args, Job job, int timeout_ms);
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
  bool on_check_idle();
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
  void on_cancel_clicked();
  void on_quit();
  /* True when the window should hide now. False when a close dialog is up. */
  bool prepare_close();
  void show_close_dialog();
  void append_stdout(const char* data, std::size_t n);
  void append_stderr(const char* data, std::size_t n);
  std::string stderr_text() const;
  void show_job_progress();
  void arm_job_timeout();
  void acquire_inhibitors(const Glib::ustring& reason);
  void release_inhibitors();
  void update_logout_warning();
  void apply_phase_status();
  void note_helper_activity();
  void notify_and_leave(const Glib::ustring& body);
  void use_cancel_as_default();

  Gtk::Box m_vbox{Gtk::ORIENTATION_VERTICAL, 0};
  Gtk::MenuBar m_menubar;
  Gtk::Box m_content{Gtk::ORIENTATION_VERTICAL, 10};
  Gtk::Box m_status_row{Gtk::ORIENTATION_HORIZONTAL, 8};
  Gtk::Spinner m_spinner;
  Gtk::ScrolledWindow m_status_scroll;
  Gtk::Label m_status;
  Gtk::Label m_commit_note;
  Gtk::Label m_warning;
  Gtk::ProgressBar m_progress;
  Gtk::ScrolledWindow m_scroller;
  Gtk::TreeView m_view;
  Gtk::ButtonBox m_buttons{Gtk::ORIENTATION_HORIZONTAL};
  Gtk::Button m_cancel{"_Cancel", true};
  Gtk::Button m_check{"_Check for updates", true};
  Gtk::Button m_install{"_Install updates", true};

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
  sigc::connection m_check_idle;
  sigc::connection m_leave_idle;
  std::string m_stdout;
  std::string m_stderr;
  CaptureBuf m_out_cap;
  std::string m_markers;
  std::string m_err_partial;
  std::string m_stderr_first_error;
  int m_cancel_fd = -1;
  bool m_helper_ready = false;
  bool m_dpkg_started = false;
  /* Latched the first time this install's dpkg starts. Cancel stays off. */
  bool m_install_committed = false;
  bool m_leave_helper_running = false;
  bool m_background = false;
  bool m_app_held = false;
  int m_logind_fd = -1;
  guint m_inhibit_cookie = 0;
  bool m_check_queued = false;
  bool m_check_after_job = false;
  bool m_stop_was_timeout = false;
  bool m_closing = false;
  Job m_stopped_job = Job::None;
  unsigned m_spawn_gen = 0;
  unsigned m_retry_gen = 0;
  Glib::Pid m_retry_pid = 0;
  Gtk::MessageDialog* m_close_dialog = nullptr;
  Glib::ustring m_close_primary;
  Glib::ustring m_close_secondary;
  Glib::ustring m_notification;
  Glib::ustring m_phase;
  Glib::ustring m_progress_line;
};

#endif
