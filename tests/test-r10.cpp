/* Round 10 regression tests: Repair verb (U-1), protocol escaping (U-2),
 * overflow-safe counts (U-3). U-4 is covered by test-policy.py.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv[1] is the helper. The helper runs as this (non-root) user with stub
 * apt-get and dpkg shell scripts; root ignores both overrides.
 */

#include "apt-parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
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

static std::string slurp(const std::string& path)
{
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void write_exec(const std::string& path, const std::string& body)
{
  std::ofstream(path) << body;
  chmod(path.c_str(), 0755);
}

/* ---------------- U-2 ---------------- */
static void test_protocol()
{
  const std::string evil = "zz\nSTATUS success";
  std::vector<std::pair<const char*, std::function<void(SimulateResult&)>>> fields = {
      {"error_msg", [&](SimulateResult& r) { r.error_msg = evil; }},
      {"warning", [&](SimulateResult& r) { r.warning = evil; }},
      {"download_need", [&](SimulateResult& r) { r.download_need = evil; }},
      {"disk_use", [&](SimulateResult& r) { r.disk_use = evil; }},
      {"conffiles_kept", [&](SimulateResult& r) { r.conffiles_kept.push_back(evil); }},
      {"held", [&](SimulateResult& r) { r.held.push_back(evil); }},
      {"phased", [&](SimulateResult& r) { r.phased.push_back(evil); }},
      {"reboot_pkgs", [&](SimulateResult& r) { r.reboot_required = true; r.reboot_pkgs.push_back(evil); }},
      {"reboot_pending", [&](SimulateResult& r) { r.reboot_already = true; r.reboot_pending_pkgs.push_back(evil); }},
      {"unclassified", [&](SimulateResult& r) { r.unclassified.push_back(evil); }},
      {"kept_back", [&](SimulateResult& r) { r.kept_back.push_back(evil); }},
      {"kept_removal.pkg",
       [&](SimulateResult& r) {
         SimulateResult::KeptRemoval k;
         k.package = evil;
         r.kept_removals.push_back(k);
       }},
      {"kept_removal.removes",
       [&](SimulateResult& r) {
         SimulateResult::KeptRemoval k;
         k.package = "p";
         k.removes.push_back(evil);
         r.kept_removals.push_back(k);
       }},
      {"new_pkg.name",
       [&](SimulateResult& r) {
         PackageUpgrade p;
         p.name = evil;
         p.new_version = "2";
         r.new_packages.push_back(p);
       }},
  };
  for (auto& f : fields) {
    SimulateResult r;
    r.status = SimulateResult::Error;
    r.error_msg = "real error";
    f.second(r);
    const std::string wire = format_protocol(r);
    const SimulateResult p = parse_protocol(wire);
    expect(p.status == SimulateResult::Error, std::string("U-2: newline in ") + f.first + " cannot forge STATUS");
    int status_lines = 0;
    std::istringstream in(wire);
    std::string line;
    while (std::getline(in, line))
      status_lines += line.compare(0, 7, "STATUS ") == 0;
    expect(status_lines == 1, std::string("U-2: ") + f.first + " emits exactly one STATUS line");
  }
  /* PKG fields on an Upgrades result. */
  {
    SimulateResult r;
    r.status = SimulateResult::Upgrades;
    PackageUpgrade p;
    p.name = "a b\nSTATUS error";
    p.old_version = "1\t\\x";
    p.new_version = "2\r\x01";
    p.size = "4 kB\nSTATUS error";
    r.packages.push_back(p);
    const SimulateResult back = parse_protocol(format_protocol(r));
    expect(back.status == SimulateResult::Upgrades, "U-2: PKG name cannot change the status");
    expect(back.packages.size() == 1 && back.packages[0].name == p.name &&
               back.packages[0].old_version == p.old_version &&
               back.packages[0].new_version == p.new_version && back.packages[0].size == p.size,
           "U-2: PKG name, versions and size round-trip with spaces and control bytes");
  }
  /* Full round trip of every escaped field. */
  {
    SimulateResult r;
    r.status = SimulateResult::KeptBack;
    const std::string odd = "x\\y\tz\x7f\x1b[31m";
    r.kept_back = {odd, "plain"};
    r.held = {odd};
    r.phased = {odd};
    r.unclassified = {odd};
    r.conffiles_kept = {"/etc/a b\\c"};
    r.disk_use = "After this operation, 1 kB\\s";
    PackageUpgrade n;
    n.name = "sp ace";
    n.new_version = "1:2 3";
    r.new_packages = {n};
    SimulateResult::KeptRemoval k;
    k.package = "pk g";
    k.removes = {"vic tim", odd};
    r.kept_removals = {k};
    r.reboot_required = true;
    r.reboot_pkgs = {odd};
    r.warning = "line one\\n\nline two";
    const SimulateResult b = parse_protocol(format_protocol(r));
    expect(b.status == SimulateResult::KeptBack && b.kept_back == r.kept_back && b.held == r.held &&
               b.phased == r.phased && b.unclassified == r.unclassified &&
               b.conffiles_kept == r.conffiles_kept && b.disk_use == r.disk_use &&
               b.new_packages.size() == 1 && b.new_packages[0].name == n.name &&
               b.new_packages[0].new_version == n.new_version && b.kept_removals.size() == 1 &&
               b.kept_removals[0].package == k.package && b.kept_removals[0].removes == k.removes &&
               b.reboot_pkgs == r.reboot_pkgs && b.warning == r.warning,
           "U-2: format_protocol/parse_protocol round-trip every list and text field");
  }
  {
    const SimulateResult p = parse_protocol("STATUS error\nMSG boom\nSTATUS success\n");
    expect(p.status == SimulateResult::Error && has(p.error_msg, "more than one STATUS"),
           "U-2: a second STATUS line is a protocol error");
  }
  expect(protocol_unescape(protocol_escape(std::string("\0a\\n", 4), true)) == std::string("\0a\\n", 4),
         "U-2: escape/unescape keep NUL and a literal backslash-n");
}

/* ---------------- U-3 ---------------- */
static void test_counts()
{
  const SimulateResult big = parse_apt_simulate(
      "99999999999999999999 upgraded, 0 newly installed, 0 to remove and 300000000000 not upgraded.\n");
  expect(big.upgraded_count == -1 && big.not_upgraded_count == -1,
         "U-3: an overflowing summary count is rejected (-1), not wrapped");
  const SimulateResult ok = parse_apt_simulate("3 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n");
  expect(ok.upgraded_count == 3 && ok.not_upgraded_count == 2, "U-3: normal summary counts still parse");
  const SimulateResult edge =
      parse_apt_simulate("1000000000 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n");
  expect(edge.upgraded_count == 1000000000, "U-3: the largest accepted count parses");
  const SimulateResult proto = parse_protocol("STATUS kept-back\nNOT_UPGRADED 99999999999999999999\n");
  expect(proto.not_upgraded_count == -1, "U-3: NOT_UPGRADED overflow is rejected in the protocol");
  const SimulateResult proto_ok = parse_protocol("STATUS kept-back\nNOT_UPGRADED 7\n");
  expect(proto_ok.not_upgraded_count == 7, "U-3: NOT_UPGRADED 7 parses");
  const InstallPlan plan = parse_install_plan(
      "0 upgraded, 0 newly installed, 99999999999999999999 to remove and 0 not upgraded.\n");
  expect(plan.remove_count == -1, "U-3: an overflowing install-plan count is rejected");
}

/* ---------------- U-1 classifier ---------------- */
static void test_classifier()
{
  expect(error_suggests_repair("The package installer was interrupted. Use Repair to finish configuring packages."),
         "U-1: interrupted dpkg offers Repair");
  expect(error_suggests_repair("E: Unmet dependencies. Try 'apt --fix-broken install' with no packages"),
         "U-1: unmet dependencies offer Repair");
  expect(error_suggests_repair("dpkg: error processing foo\nThe upgrade stopped with a package left unfinished."),
         "U-1: a failed postinst offers Repair");
  expect(!error_suggests_repair("The package installer was interrupted and is still running."),
         "U-1: a dpkg that is still running does not offer Repair");
  expect(!error_suggests_repair("E: Could not get lock /var/lib/dpkg/lock-frontend"),
         "U-1: a lock error does not offer Repair");
  expect(!error_suggests_repair("Temporary failure resolving 'deb.devuan.org'"),
         "U-1: a network error does not offer Repair");
}

/* ---------------- U-1 helper verb ---------------- */
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

static const char kStubApt[] = R"SH(#!/bin/sh
# Stub apt-get. LCOS_STUB_SCRIPT selects the scenario. Every call is logged.
printf 'apt-get' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do
  case "$a" in dist-upgrade|full-upgrade|autoremove|remove|purge) echo "E: forbidden $a"; exit 99;; esac
done
sim=0
for a in "$@"; do [ "$a" = "-s" ] && sim=1; done
case "$LCOS_STUB_SCRIPT" in
  ok)
    if [ $sim = 1 ]; then
      echo "The following NEW packages will be installed:"
      echo "  libbar1"
      echo "Inst libbar1 (1.2-1 Devuan:6.0/stable [amd64])"
      echo "Inst foo [1.0-1] (1.0-1 Devuan:6.0/stable [amd64])"
      echo "Conf libbar1 (1.2-1 Devuan:6.0/stable [amd64])"
      echo "1 upgraded, 1 newly installed, 0 to remove and 0 not upgraded."
    else
      echo "Setting up libbar1 (1.2-1) ..."
      echo "1 upgraded, 1 newly installed, 0 to remove and 0 not upgraded."
    fi ;;
  nothing)
    echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded." ;;
  remove)
    echo "The following packages will be REMOVED:"
    echo "  lcos-desktop"
    echo "Remv lcos-desktop [0.9.4-1]"
    echo "Inst libbar1 (1.2-1 Devuan:6.0/stable [amd64])"
    echo "0 upgraded, 1 newly installed, 1 to remove and 0 not upgraded." ;;
  fixfail)
    if [ $sim = 1 ]; then
      echo "Inst libbar1 (1.2-1 Devuan:6.0/stable [amd64])"
      echo "0 upgraded, 1 newly installed, 0 to remove and 0 not upgraded."
    else
      echo "E: Unmet dependencies. Try 'apt --fix-broken install' with no packages (or specify a solution)." >&2
      exit 100
    fi ;;
esac
exit 0
)SH";

static const char kStubDpkg[] = R"SH(#!/bin/sh
printf 'dpkg' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
[ -t 0 ] && echo "stdin is a tty" >> "$LCOS_STUB_ARGVFILE"
if [ "$LCOS_STUB_STATEFILE" = "fail" ]; then
  echo "Setting up foo (2.0-1) ..."
  echo "dpkg: error processing package foo (--configure):" >&2
  echo " installed foo package post-installation script subprocess returned error exit status 1" >&2
  echo "Errors were encountered while processing:" >&2
  echo " foo" >&2
  exit 1
fi
echo "Setting up foo (2.0-1) ..."
exit 0
)SH";

static void test_repair(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r10-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    expect(false, "mkdtemp");
    return;
  }
  const std::string dir = tmpl;
  const std::string apt = dir + "/apt-get";
  const std::string dpkg = dir + "/dpkg";
  write_exec(apt, kStubApt);
  write_exec(dpkg, kStubDpkg);
  int n = 0;
  auto run = [&](const char* scenario, const char* dpkg_mode, std::string& log) {
    log = dir + "/argv" + std::to_string(++n);
    return run_helper(helper, {"repair"},
                      {{"LCOS_UPDATES_APT_GET", apt},
                       {"LCOS_UPDATES_DPKG", dpkg},
                       {"LCOS_STUB_SCRIPT", scenario},
                       {"LCOS_STUB_STATEFILE", dpkg_mode},
                       {"LCOS_STUB_ARGVFILE", log}});
  };

  {
    std::string log;
    const HelperRun r = run("ok", "ok", log);
    const std::string calls = slurp(log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.status == SimulateResult::Success, "U-1: repair succeeds on a repairable system");
    expect(has(calls, "dpkg --force-confdef --force-confold --configure -a\n"),
           "U-1: repair runs dpkg --configure -a with confdef/confold");
    const std::string dpkg_line = calls.substr(0, calls.find('\n'));
    expect(dpkg_line.compare(0, 5, "dpkg ") == 0 && !has(dpkg_line, "APT::"),
           "U-1: dpkg is not given apt-only options");
    expect(!has(calls, "stdin is a tty"), "U-1: maintainer scripts get no terminal");
    const auto cfg = calls.find("dpkg ");
    const auto sim = calls.find(" -s ");
    const auto fix = calls.find("--no-remove -f install");
    expect(cfg != std::string::npos && sim != std::string::npos && fix != std::string::npos && cfg < sim &&
               sim < fix,
           "U-1: order is configure -a, then simulate -f install, then -f install");
    expect(has(calls, "APT::Install-Recommends=false") && has(calls, "--no-remove") &&
               has(calls, "Dpkg::Options::=--force-confold"),
           "U-1: -f install runs without Recommends, with --no-remove and confold");
    expect(!has(calls, "dist-upgrade") && !has(calls, "full-upgrade") && !has(calls, "upgrade\n"),
           "U-1: repair never runs dist-upgrade, full-upgrade or upgrade");
    expect(has(r.err, "HELPER_READY") && has(r.err, "PHASE configure") && has(r.err, "PHASE repair") &&
               has(r.err, "DPKG_STARTED"),
           "U-1: repair reports its phases and is committed once dpkg starts");
  }
  {
    std::string log;
    const HelperRun r = run("nothing", "ok", log);
    const std::string calls = slurp(log);
    expect(r.status == 0 && parse_protocol(r.out).status == SimulateResult::Success &&
               !has(calls, "--no-remove"),
           "U-1: with nothing left to fix, repair stops after configure -a and the simulation");
  }
  {
    std::string log;
    const HelperRun r = run("remove", "ok", log);
    const std::string calls = slurp(log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && p.status == SimulateResult::Error && has(p.error_msg, "remove lcos-desktop") &&
               has(p.error_msg, "Nothing was removed"),
           "U-1: repair refuses when -f install would remove a package, and names it");
    expect(!has(calls, "--no-remove"), "U-1: a refused repair never runs the real -f install");
    expect(!error_suggests_repair(p.error_msg), "U-1: a refused removal does not offer Repair again");
  }
  {
    std::string log;
    const HelperRun r = run("ok", "fail", log);
    const std::string calls = slurp(log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && p.status == SimulateResult::Error &&
               has(p.error_msg, "Repair could not finish setting up") &&
               has(p.error_msg, "post-installation script"),
           "U-1: a failing postinst is reported with dpkg's reason");
    expect(!has(calls, "apt-get"), "U-1: apt is not run after configure -a fails");
    expect(error_suggests_repair(p.error_msg), "U-1: a failed configure can be retried");
  }
  {
    std::string log;
    const HelperRun r = run("fixfail", "ok", log);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && has(p.error_msg, "Repair could not fix missing dependencies") &&
               has(p.error_msg, "Unmet dependencies"),
           "U-1: a failing -f install is reported with apt's error");
  }
  {
    const HelperRun r = run_helper(helper, {"repair", "foo"}, {});
    expect(r.status == 2 && has(r.out, "refused"), "U-1: repair takes no arguments");
  }
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_protocol();
  test_counts();
  test_classifier();
  if (argc >= 2)
    test_repair(argv[1]);
  else
    expect(false, "helper path argument");
  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  return 0;
}
