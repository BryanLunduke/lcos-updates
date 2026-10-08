# Check for updates… (`lcos-updates`)

GTK3 / gtkmm-3.0 tool for LCOS. Checks for and installs `apt-get upgrade`
updates from Devuan and LCOS. No daemon, no tray, no PackageKit, no
`dist-upgrade`.

The GUI (`/usr/bin/lcos-updates`) never runs as root. Privileged work is
`pkexec /usr/libexec/lcos-updates-helper` with argv `simulate` or `upgrade`.

The apt child starts from a cleared environment. `http_proxy`,
`https_proxy`, and `no_proxy` are not passed through, and a caller cannot
point the root apt at another config. Set a proxy in `/etc/apt/apt.conf.d`,
for example:

```
Acquire::http::Proxy "http://proxy.example:8080/";
```

A check or install asks the session to block logout, shutdown, and sleep.
After packages have started unpacking, closing the window leaves that
install running. If the session cannot block logout or shutdown, the window
says not to log out or shut down until the install finishes.

License: GPL-3.0-or-later
