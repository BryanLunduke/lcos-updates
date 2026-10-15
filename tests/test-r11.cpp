/* Round 11 regression tests (lcos-updates 0.9.4-3).
 * U-1 unfinished packages block Check/Install and point at Repair.
 * U-2 a held lock during Repair is "another program is installing".
 * U-3 a failed Repair is worded once, with the right package list.
 * U-4 network down leads with one plain sentence.
 * U-6 sizes come from the pinned install when packages were adopted.
 * Check caller verification (auth_admin_keep scope) and repair timeout text.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv[1] is the helper. It runs as this (non-root) user with stub apt-get
 * and dpkg; root ignores every override used here.
 */

#include "tmpdir-cleanup.hpp"
#include "apt-parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

static int g_fails = 0;

static void expect(bool ok, const std::string& what)
{
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_fails;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

static bool has(const std::string& hay, const char* needle)
{
  return hay.find(needle) != std::string::npos;
}

static std::size_t count_of(const std::string& hay, const char* needle)
{
  std::size_t n = 0;
  for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
    ++n;
  return n;
}

static std::string slurp(const std::string& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void write_file(const std::string& path, const std::string& body, mode_t mode = 0644)
{
  std::ofstream(path) << body;
  chmod(path.c_str(), mode);
}

struct HelperRun {
  int status = -1;
  std::string out;
  std::string err;
};

static HelperRun run_helper(const char* helper, const std::vector<std::string>& args,
                            const std::vector<std::pair<std::string, std::string>>& env)
{
  HelperRun run;
  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
    return run;
  const pid_t pid = fork();
  if (pid == 0) {
    for (const auto& kv : env)
      setenv(kv.first.c_str(), kv.second.c_str(), 1);
    const int devnull = open("/dev/null", O_RDONLY);
    dup2(devnull, 0);
    dup2(out_pipe[1], 1);
    dup2(err_pipe[1], 2);
    close(out_pipe[0]);
    close(err_pipe[0]);
    std::vector<char*> av;
    av.push_back(const_cast<char*>(helper));
    for (const auto& a : args)
      av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv(helper, av.data());
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  auto drain = [](int fd) {
    std::string s;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
      s.append(buf, static_cast<std::size_t>(n));
    close(fd);
    return s;
  };
  run.out = drain(out_pipe[0]);
  run.err = drain(err_pipe[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  run.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return run;
}

static const char kStatusBroken[] = R"(Package: alpha
Status: install ok unpacked
Version: 2.0

Package: beta
Status: install ok half-configured
Version: 2.0

Package: heldpkg
Status: hold ok installed
Version: 1.0

Package: old
Status: deinstall ok config-files
Version: 0.5

Package: zeta
Status: install ok installed
Version: 1.0
)";

static const char kStatusClean[] = R"(Package: alpha
Status: install ok installed
Version: 2.0

Package: beta
Status: install ok installed
Version: 2.0
)";

static void test_parser()
{
  const auto names = unfinished_packages(kStatusBroken);
  expect(names.size() == 2 && names[0] == "alpha" && names[1] == "beta",
         "U-1: unpacked and half-configured are unfinished; installed, hold and config-files are not");
  expect(unfinished_packages(kStatusClean).empty(), "U-1: a clean status has nothing unfinished");
  const std::string trig = "Package: t\nStatus: install ok triggers-pending\n";
  expect(unfinished_packages(trig).size() == 1, "U-1: triggers-pending is unfinished, as dpkg --audit says");
  const std::string msg = unfinished_message(names, false);
  expect(error_suggests_repair(msg) && has(msg, "alpha, beta") && has(msg, "did not finish"),
         "U-1: the message names the packages and offers Repair");
  expect(error_suggests_repair(unfinished_message({}, true)), "U-1: an interrupted journal offers Repair");
  const auto states = parse_dpkg_status(kStatusBroken);
  expect(states.size() == 5 && states[1].state == "half-configured" && states[1].version == "2.0",
         "U-5: status parse keeps state and version");

  expect(error_says_locked("dpkg: error: dpkg frontend lock was locked by /usr/bin/apt-get process with pid 7"),
         "U-2: the dpkg frontend lock message is a lock");
  expect(error_says_locked("E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 7"),
         "U-2: apt's could-not-get-lock is a lock");
  expect(!error_suggests_repair(kAnotherInstaller), "U-2: the lock sentence does not offer Repair");

  const std::string offline =
      "Hit:1 file:/srv/repo ./ InRelease\n"
      "Err:2 http://deb.debian.org/debian trixie InRelease\n"
      "  Temporary failure resolving 'deb.debian.org'\n"
      "W: Failed to fetch http://deb.debian.org/debian/dists/trixie/InRelease  Temporary failure resolving 'deb.debian.org'\n"
      "W: Some index files failed to download. They have been ignored, or old ones used instead.\n";
  const UpdateFetch f = classify_apt_update(offline);
  expect(f.kind == UpdateFetchKind::Partial && f.detail.compare(0, std::strlen(kServerUnreachable), kServerUnreachable) == 0,
         "U-4: network down with a local repo leads with the plain sentence");
  expect(has(f.detail, "out of date") && has(f.detail, "W: Failed to fetch"),
         "U-4: it says results may be out of date and keeps apt's lines below");
  const std::string shown = friendly_job_error(f.detail, JobKind::Check);
  expect(shown.compare(0, std::strlen(kServerUnreachable), kServerUnreachable) == 0,
         "U-4: the shown warning still starts with the plain sentence, not W:/Err:");
  const std::string one_mirror = "Hit:1 http://deb.devuan.org/merged excalibur InRelease\n"
                                 "Err:2 http://mirror.example stable InRelease\n"
                                 "  Could not connect to mirror.example:80, connection timed out\n"
                                 "W: Some index files failed to download. They have been ignored, or old ones used instead.\n";
  expect(!has(classify_apt_update(one_mirror).detail, "Can't reach"),
         "U-4: one skipped mirror while another server answered is not called unreachable");
  const std::string all_down = "Err:1 http://deb.debian.org/debian trixie InRelease\n"
                               "  Temporary failure resolving 'deb.debian.org'\n"
                               "W: Failed to fetch http://deb.debian.org/debian/dists/trixie/InRelease\n";
  const UpdateFetch t = classify_apt_update(all_down);
  expect(t.kind == UpdateFetchKind::Total && t.detail.compare(0, 31, "Can't reach the update server. ") == 0,
         "U-4: a total failure also leads with the plain sentence");

  const JobOutcome auth = outcome_repair_timeout(false);
  expect(has(auth.status, "repair") && has(auth.status, "password") && !has(auth.status, "install"),
         "extra: a timeout at the Repair password prompt is worded as a repair");
  expect(has(outcome_repair_timeout(true).status, "repairing"), "extra: a repair that times out says repairing");
}

static const char kStubApt[] = R"SH(#!/bin/sh
printf 'apt-get' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
sim=0; uris=0; inst=0
for a in "$@"; do
  [ "$a" = "-s" ] && sim=1
  [ "$a" = "--print-uris" ] && uris=1
  [ "$a" = "install" ] && inst=1
done
case "$*" in *" update"|update) echo "Hit:1 http://deb.example stable InRelease"; exit 0;; esac
if [ $uris = 1 ]; then
  if [ $inst = 1 ]; then
    echo "Need to get 2048 B of archives."
    echo "After this operation, 12.3 kB of additional disk space will be used."
    echo "'file:/r/delta_2.0_all.deb' delta_2.0_all.deb 1024 MD5Sum:00"
    echo "'file:/r/epsilon_1.0_all.deb' epsilon_1.0_all.deb 1024 MD5Sum:00"
  else
    echo "Need to get 0 B of archives."
    echo "After this operation, 0 B of additional disk space will be used."
  fi
  exit 0
fi
case "$LCOS_STUB_SCRIPT" in
  adopt)
    if [ $inst = 1 ]; then
      echo "The following NEW packages will be installed:"
      echo "  epsilon"
      echo "Inst epsilon (1.0 local [all])"
      echo "Inst delta [1.0] (2.0 local [all])"
      echo "1 upgraded, 1 newly installed, 0 to remove and 0 not upgraded."
    else
      echo "The following packages have been kept back:"
      echo "  delta"
      echo "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded."
    fi ;;
  lockfix)
    if [ $sim = 1 ]; then
      echo "Inst libbar1 (1.2-1 Devuan:6.0/stable [amd64])"
      echo "0 upgraded, 1 newly installed, 0 to remove and 0 not upgraded."
    else
      echo "E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 4242 (apt-get)" >&2
      exit 100
    fi ;;
  *)
    echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded." ;;
esac
exit 0
)SH";

static const char kStubDpkg[] = R"SH(#!/bin/sh
printf 'dpkg' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
case "$LCOS_STUB_STATEFILE" in
  lock)
    echo "dpkg: error: dpkg frontend lock was locked by /usr/bin/apt-get process with pid 4242" >&2
    echo "Note: removing the lock file is always wrong, and can end up damaging the" >&2
    echo "locked area and the entire system. See <https://wiki.debian.org/Teams/Dpkg/FAQ#db-lock>." >&2
    exit 2 ;;
  fail)
    echo "Setting up beta (2.0) ..."
    echo "dpkg: error processing package beta (--configure):" >&2
    echo " installed beta package post-installation script subprocess returned error exit status 1" >&2
    echo "Errors were encountered while processing:" >&2
    echo " beta" >&2
    exit 1 ;;
esac
echo "Setting up beta (2.0) ..."
exit 0
)SH";

static void test_helper(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r11-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    expect(false, "mkdtemp");
    return;
  }
  lcos_test::cleanup_at_exit(tmpl);
  const std::string dir = tmpl;
  const std::string apt = dir + "/apt-get";
  const std::string dpkg = dir + "/dpkg";
  const std::string broken = dir + "/status-broken";
  const std::string clean = dir + "/status-clean";
  const std::string updates = dir + "/updates";
  const std::string updates_empty = dir + "/updates-empty";
  const std::string lists = dir + "/lists";
  const std::string state = dir + "/state";
  mkdir(updates.c_str(), 0755);
  mkdir(updates_empty.c_str(), 0755);
  mkdir(lists.c_str(), 0755);
  mkdir(state.c_str(), 0755);
  write_file(apt, kStubApt, 0755);
  write_file(dpkg, kStubDpkg, 0755);
  write_file(broken, kStatusBroken);
  write_file(clean, kStatusClean);
  int n = 0;
  auto run = [&](std::vector<std::string> args, const std::string& status, const std::string& upd,
                 const char* script, const char* dpkg_mode, std::string& log,
                 std::vector<std::pair<std::string, std::string>> extra = {}) {
    log = dir + "/argv" + std::to_string(++n);
    std::vector<std::pair<std::string, std::string>> env = {
        {"LCOS_UPDATES_APT_GET", apt},
        {"LCOS_UPDATES_DPKG", dpkg},
        {"LCOS_UPDATES_DPKG_STATUS", status},
        {"LCOS_UPDATES_DPKG_UPDATES_DIR", upd},
        {"LCOS_UPDATES_LISTS_DIR", lists},
        {"LCOS_UPDATES_STATE_DIR", state},
        {"LCOS_STUB_SCRIPT", script},
        {"LCOS_STUB_STATEFILE", dpkg_mode},
        {"LCOS_STUB_ARGVFILE", log}};
    for (auto& kv : extra)
      env.push_back(kv);
    return run_helper(helper, args, env);
  };

  /* U-1: the killed-install state (unpacked + half-configured). */
  {
    std::string log;
    const HelperRun r = run({"simulate"}, broken, updates_empty, "none", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && p.status == SimulateResult::Error && has(p.error_msg, "alpha, beta") &&
               error_suggests_repair(p.error_msg),
           "U-1: Check reports unfinished packages as an error with Repair");
    expect(!has(r.out, "STATUS kept-back"), "U-1: Check no longer says kept back over a broken system");
  }
  {
    std::string log;
    const HelperRun r = run({"upgrade", "alpha=2.0"}, broken, updates_empty, "none", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && error_suggests_repair(p.error_msg) && !has(slurp(log), "install"),
           "U-1: Install refuses before running apt and points at Repair");
  }
  {
    write_file(updates + "/0001", "x");
    std::string log;
    const HelperRun r = run({"simulate"}, clean, updates, "none", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && has(p.error_msg, "interrupted") && error_suggests_repair(p.error_msg),
           "U-1: a leftover dpkg journal (dpkg killed) is reported with Repair");
  }
  {
    std::string log;
    const HelperRun r = run({"simulate"}, clean, updates_empty, "none", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.status != SimulateResult::Error, "U-1: a clean system checks as before");
  }

  /* U-2 */
  {
    std::string log;
    const HelperRun r = run({"repair"}, broken, updates_empty, "none", "lock", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && p.error_msg == kAnotherInstaller,
           "U-2: Repair while dpkg is locked says another program is installing");
    expect(!error_suggests_repair(p.error_msg) && !has(p.error_msg, "left unfinished"),
           "U-2: the lock case does not claim a package is unfinished");
  }
  {
    std::string log;
    const HelperRun r = run({"repair"}, broken, updates_empty, "lockfix", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && p.error_msg == kAnotherInstaller,
           "U-2: a lock taken before the -f install step gets the same sentence");
  }

  /* U-3 */
  {
    std::string log;
    const HelperRun r = run({"repair"}, broken, updates_empty, "none", "fail", log);
    const SimulateResult p = parse_protocol(r.out);
    const std::string& m = p.error_msg;
    expect(r.status == 1 && has(m, "Repair could not finish setting up alpha, beta."),
           "U-3: a failed Repair names the packages that are still unfinished");
    expect(!has(m, "The upgrade stopped") && count_of(m, "unfinished") == 0,
           "U-3: no install wording and no repeated 'left unfinished'");
    expect(has(m, "Errors were encountered while processing: beta") && !has(m, "beta, Setting up"),
           "U-3: dpkg's failed list is just the package names");
    expect(has(m, "post-installation script") && error_suggests_repair(m),
           "U-3: dpkg's reason is kept and Repair can be tried again");
  }

  /* U-6 */
  {
    std::string log;
    const HelperRun r = run({"simulate"}, clean, updates_empty, "adopt", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    const std::string calls = slurp(log);
    expect(has(calls, "--print-uris -y -o APT::Install-Recommends=false install delta=2.0 epsilon=1.0"),
           "U-6: the size query is the pinned install including the new dependency");
    bool delta_size = false;
    for (const auto& pkg : p.packages)
      if (pkg.name == "delta" && !pkg.size.empty())
        delta_size = true;
    expect(delta_size, "U-6: the adopted package has a size");
    expect(has(p.disk_use, "12.3 kB"), "U-6: the disk figure includes the new dependency");
  }

  /* Check caller (auth_admin_keep scope). */
  {
    std::string log;
    const HelperRun r = run({"simulate"}, clean, updates_empty, "none", "ok", log,
                            {{"PKEXEC_UID", std::to_string(getuid())},
                             {"LCOS_UPDATES_CALLER_EXE", "/usr/bin/lcos-updates"}});
    expect(r.status == 2 && has(r.out, "refused: checks can only be started from Updates") &&
               slurp(log).empty(),
           "keep: a check from another program under pkexec is refused before apt runs");
  }
  {
    char self[4096];
    const ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    self[len > 0 ? len : 0] = '\0';
    std::string log;
    const HelperRun r = run({"simulate"}, clean, updates_empty, "none", "ok", log,
                            {{"PKEXEC_UID", std::to_string(getuid())}, {"LCOS_UPDATES_CALLER_EXE", self}});
    expect(r.status == 0, "keep: a check from the expected program is allowed");
    const HelperRun other = run({"simulate"}, clean, updates_empty, "none", "ok", log,
                                {{"PKEXEC_UID", std::to_string(getuid() + 1)}, {"LCOS_UPDATES_CALLER_EXE", self}});
    expect(other.status == 2, "keep: the expected program run by another user is refused");
  }
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_parser();
  if (argc >= 2)
    test_helper(argv[1]);
  else
    expect(false, "helper path argument");
  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  return 0;
}
