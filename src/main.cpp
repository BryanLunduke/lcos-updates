/* lcos-updates — Check for updates… (GTK3, X11, never runs as root).
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "startup-id.hpp"
#include "window.hpp"

#include <glib.h>
#include <gtkmm.h>

namespace {
constexpr const char* kAppId = "org.lunduke.LcosUpdates";

UpdatesWindow* existing_window(const Glib::RefPtr<Gtk::Application>& app)
{
  const std::vector<Gtk::Window*> windows = app->get_windows();
  for (Gtk::Window* window : windows) {
    if (auto* updates = dynamic_cast<UpdatesWindow*>(window))
      return updates;
  }
  return nullptr;
}

/* Startup-notification ids end in _TIME<x11 timestamp>. Passing that to
 * present() is what lets a second launch raise the window under X11 focus
 * stealing prevention. A parsed timestamp of 0 is refused. */
bool timestamp_from_command_line(const Glib::RefPtr<Gio::ApplicationCommandLine>& cmdline,
                                 unsigned long& timestamp)
{
  if (!cmdline)
    return false;
  try {
    const Glib::RefPtr<Glib::VariantDict> dict =
        Glib::VariantDict::create(cmdline->get_platform_data());
    if (!dict)
      return false;
    Glib::ustring startup_id;
    if (!dict->lookup_value("desktop-startup-id", startup_id))
      return false;
    return startup_timestamp_from_id(static_cast<std::string>(startup_id), timestamp);
  } catch (const Glib::Error&) {
    return false;
  }
}

/* One window per process. --check is taken from this launch's options, not
 * remembered from the process that first opened the window. */
void present_updates(const Glib::RefPtr<Gtk::Application>& app,
                     const Glib::RefPtr<Gio::ApplicationCommandLine>& cmdline, bool check)
{
  UpdatesWindow* window = existing_window(app);
  if (window == nullptr) {
    window = new UpdatesWindow(check);
    app->add_window(*window);
    /* Delete from an idle callback so hide() is not re-entered by the delete. */
    auto* alive = new bool(true);
    window->signal_hide().connect([window, alive]() {
      if (!*alive)
        return;
      /* A committed install hides the window and keeps running. Deleting the
       * window here would close the cancel pipe and drop the result. */
      if (window->retains_background_job())
        return;
      *alive = false;
      Glib::signal_idle().connect_once([window, alive]() {
        delete window;
        delete alive;
      });
    });
  } else if (check) {
    window->request_check();
  }
  unsigned long timestamp = 0;
  if (timestamp_from_command_line(cmdline, timestamp))
    window->present(static_cast<guint32>(timestamp));
  else
    window->present();
}
} // namespace

int main(int argc, char* argv[])
{
  if (g_getenv("GDK_BACKEND") == nullptr)
    g_setenv("GDK_BACKEND", "x11", FALSE);
  g_set_prgname("lcos-updates");

  auto app = Gtk::Application::create(kAppId, Gio::APPLICATION_HANDLES_COMMAND_LINE);
  /* WM / title-bar icon (xfwm4 etc.): desktop Icon= alone is not enough. */
  Gtk::Window::set_default_icon_name(lcos_updates::kIconName);

  app->add_main_option_entry(Gio::Application::OPTION_TYPE_BOOL, "check", '\0',
                             "Check for updates immediately after opening");

  /* after=false: command-line uses first-wins, and the default handler returns 1
   * without showing a window. Our handler must run first. */
  app->signal_command_line().connect(
      [app](const Glib::RefPtr<Gio::ApplicationCommandLine>& cmdline) -> int {
        bool check = false;
        if (cmdline) {
          const Glib::RefPtr<Glib::VariantDict> options = cmdline->get_options_dict();
          if (options)
            options->lookup_value("check", check);
        }
        present_updates(app, cmdline, check);
        return 0;
      },
      false);

  /* Activation without a command line (no --check) must not open a second window. */
  app->signal_activate().connect([app]() { present_updates(app, {}, false); });

  return app->run(argc, argv);
}
