/* Round 15 regression tests (lcos-updates 0.9.5-1).
 * U-1 kernel/firmware/microcode installs offer Restart without the marker;
 *     an existing marker also offers Restart.
 * U-2 downloaded debs are not kept; unused packages are offered, removed only
 *     after review, exactly the reviewed set.
 * U-3 AppStream metainfo.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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


static const char kStubApt[] = R"SH(#!/bin/sh
printf 'apt-get' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
sim=0; uris=0; inst=0; dl=0; auto=0; clean=0
for a in "$@"; do
  [ "$a" = "-s" ] && sim=1
  [ "$a" = "--print-uris" ] && uris=1
  [ "$a" = "install" ] && inst=1
  [ "$a" = "--download-only" ] && dl=1
  [ "$a" = "autoremove" ] && auto=1
  [ "$a" = "clean" ] && clean=1
done
d="$LCOS_STUB_PIDFILE"
case "$*" in *" update"|update) echo "Hit:1 http://deb.example stable InRelease"; exit 0;; esac
if [ $clean = 1 ]; then echo clean >> "$d/cleaned"; exit 0; fi
if [ $auto = 1 ]; then
  if [ $sim = 1 ]; then
    [ -e "$d/no-unused" ] && { echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."; exit 0; }
    echo "The following packages will be REMOVED:"
    echo "  libold1 oldtool"
    echo "Remv libold1 [1.0]"
    echo "Remv oldtool [2.0]"
    echo "0 upgraded, 0 newly installed, 2 to remove and 0 not upgraded."
    exit 0
  fi
  echo "Removing libold1 (1.0) ..."
  echo "Removing oldtool (2.0) ..."
  touch "$d/no-unused"
  exit 0
fi
if [ $uris = 1 ]; then
  echo "Need to get 1024 B of archives."
  exit 0
fi
[ $dl = 1 ] && exit 0
K="$(cat "$d/pkg")"
if [ $inst = 1 ] && [ $sim = 0 ]; then
  echo "Setting up $K ..."
  echo "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
echo "Inst $K [1.0] (2.0 local [amd64])"
echo "Conf $K (2.0 local [amd64])"
echo "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
exit 0
)SH";

static const char kStubCache[] = "#!/bin/sh\nexit 0\n";
static const char kStubDpkgDeb[] = "#!/bin/sh\nexit 2\n";
static const char kStubDpkg[] = "#!/bin/sh\nexit 0\n";

static void test_pure()
{
  expect(needs_reboot_package("linux-image-7.2.6-lunduke") && needs_reboot_package("linux-image-amd64"),
         "U-1: kernel packages need a restart");
  expect(needs_reboot_package("firmware-misc-nonfree") && needs_reboot_package("intel-microcode") &&
             needs_reboot_package("amd64-microcode") && needs_reboot_package("firmware-amd-graphics:amd64"),
         "U-1: firmware and microcode need a restart");
  expect(!needs_reboot_package("linux-headers-amd64") && !needs_reboot_package("firmware") &&
             !needs_reboot_package("lunduke-paint") && !needs_reboot_package("microcode") &&
             !needs_reboot_package("libc6"),
         "U-1: ordinary packages do not");
  expect(reboot_trigger_packages({"lunduke-paint=1.0", "intel-microcode=3", "linux-image-amd64=7"}) ==
             std::vector<std::string>({"intel-microcode", "linux-image-amd64"}),
         "U-1: names come from name=version pins");
  expect(parse_autoremove("Remv a [1]\nRemv b:i386 [2]\nInst c (1)\nRemv a [1]\n") ==
             std::vector<std::string>({"a", "b:i386"}),
         "U-2: autoremove simulation names");
  SimulateResult r;
  r.status = SimulateResult::Success;
  r.upgraded_count = 1;
  r.not_upgraded_count = 0;
  r.reboot_required = true;
  r.reboot_pkgs = {"linux-image-7.2.6-lunduke"};
  JobOutcome o = outcome_install(r, 0, false);
  expect(o.offer_restart && has(o.status, "Restart this computer to finish installing updates (needed by "
                                          "linux-image-7.2.6-lunduke).") && !has(o.status, ". ("),
         "U-1: clear restart sentence and Restart offered");
  SimulateResult pend;
  pend.status = SimulateResult::Success;
  pend.upgraded_count = 1;
  pend.not_upgraded_count = 0;
  pend.reboot_already = true;
  o = outcome_install(pend, 0, false);
  expect(o.offer_restart && has(o.status, "A restart is still needed for an earlier update."),
         "U-1: an existing /var/run/reboot-required still offers Restart");
  SimulateResult u;
  u.status = SimulateResult::UpToDate;
  u.unused = {"libold1", "oldtool"};
  o = outcome_check(u, 0, false);
  expect(has(o.status, "2 packages are no longer needed"), "U-2: Check mentions unused packages");
  const SimulateResult back = parse_protocol(format_protocol(u));
  expect(back.unused == u.unused, "U-2: UNUSED round-trips the protocol");
  SimulateResult c;
  c.status = SimulateResult::Success;
  c.removed_unused = 2;
  expect(parse_protocol(format_protocol(c)).removed_unused == 2 &&
             has(outcome_cleanup(c, 0).status, "Removed 2 unused packages."),
         "U-2: cleanup result wording");
}

static void test_helper(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r15-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    expect(false, "mkdtemp");
    return;
  }
  lcos_test::cleanup_at_exit(tmpl);
  const std::string dir = tmpl;
  const std::string debconf = dir + "/debconf";
  const std::string state = dir + "/state";
  for (const auto& d : {dir + "/updates", dir + "/lists", state, debconf})
    mkdir(d.c_str(), 0755);
  write_file(dir + "/apt-get", kStubApt, 0755);
  write_file(dir + "/apt-cache", kStubCache, 0755);
  write_file(dir + "/dpkg-deb", kStubDpkgDeb, 0755);
  write_file(dir + "/dpkg", kStubDpkg, 0755);
  write_file(debconf + "/templates.dat", "");
  write_file(debconf + "/config.dat", "");
  write_file(dir + "/argv", "");
  std::vector<std::pair<std::string, std::string>> env = {
      {"LCOS_UPDATES_APT_GET", dir + "/apt-get"},
      {"LCOS_UPDATES_APT_CACHE", dir + "/apt-cache"},
      {"LCOS_UPDATES_DPKG_DEB", dir + "/dpkg-deb"},
      {"LCOS_UPDATES_DPKG", dir + "/dpkg"},
      {"LCOS_UPDATES_DPKG_STATUS", dir + "/status"},
      {"LCOS_UPDATES_DPKG_UPDATES_DIR", dir + "/updates"},
      {"LCOS_UPDATES_LISTS_DIR", dir + "/lists"},
      {"LCOS_UPDATES_STATE_DIR", state},
      {"LCOS_UPDATES_DEBCONF_DIR", debconf},
      {"LCOS_UPDATES_REBOOT_FILE", dir + "/reboot-required"},
      {"LCOS_STUB_PIDFILE", dir},
      {"LCOS_STUB_ARGVFILE", dir + "/argv"}};
  auto install = [&](const std::string& pkg) {
    write_file(dir + "/pkg", pkg);
    write_file(dir + "/status", "Package: " + pkg + "\nStatus: install ok installed\nVersion: 1.0\n\n");
    return run_helper(helper, {"upgrade", pkg + "=2.0"}, env);
  };
  {
    const HelperRun r = install("linux-image-7.2.6-lunduke");
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.reboot_required &&
               p.reboot_pkgs == std::vector<std::string>({"linux-image-7.2.6-lunduke"}),
           "U-1: a kernel install asks for a restart with no reboot-required marker");
    expect(outcome_install(p, 0, false).offer_restart, "U-1: the window offers Restart");
  }
  {
    const HelperRun r = install("lunduke-paint");
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && !p.reboot_required && !p.reboot_already,
           "U-1: an ordinary install does not ask for a restart");
  }
  {
    write_file(dir + "/reboot-required", "*** System restart required ***\n");
    write_file(dir + "/reboot-required.pkgs", "linux-image-7.2.6-lunduke\n");
    const HelperRun r = install("lunduke-paint");
    const SimulateResult p = parse_protocol(r.out);
    expect(p.reboot_already && outcome_install(p, 0, false).offer_restart,
           "U-1: an existing reboot-required marker still offers Restart");
    unlink((dir + "/reboot-required").c_str());
    unlink((dir + "/reboot-required.pkgs").c_str());
  }
  {
    unlink((dir + "/cleaned").c_str());
    write_file(dir + "/argv", "");
    const HelperRun r = install("lunduke-paint");
    const SimulateResult p = parse_protocol(r.out);
    const std::string argv = slurp(dir + "/argv");
    expect(has(argv, "APT::Keep-Downloaded-Packages=false"), "U-2: Install does not keep downloaded debs");
    expect(slurp(dir + "/cleaned") == "clean\n", "U-2: apt-get clean runs after a successful install");
    expect(p.unused == std::vector<std::string>({"libold1", "oldtool"}),
           "U-2: unused packages are reported after Install");
    expect(!has(argv, "-y -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold autoremove"),
           "U-2: Install never removes them by itself");
  }
  {
    const HelperRun r = run_helper(helper, {"cleanup", "libold1"}, env);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status != 0 && p.status == SimulateResult::Error && has(p.error_msg, "changed") &&
               !has(slurp(dir + "/argv"), "confold autoremove"),
           "U-2: cleanup refuses when the list differs from what was shown");
    const HelperRun bad = run_helper(helper, {"cleanup", "-o"}, env);
    expect(bad.status == 2, "U-2: cleanup refuses option-looking names");
    const HelperRun none = run_helper(helper, {"cleanup"}, env);
    expect(none.status == 2, "U-2: cleanup needs the reviewed list");
  }
  {
    unlink((dir + "/cleaned").c_str());
    const HelperRun r = run_helper(helper, {"cleanup", "oldtool", "libold1"}, env);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.status == SimulateResult::Success && p.removed_unused == 2 && p.unused.empty(),
           "U-2: cleanup removes exactly the reviewed unused packages");
    expect(has(slurp(dir + "/argv"), "confold autoremove") && !has(slurp(dir + "/argv"), "--purge"),
           "U-2: cleanup runs apt-get autoremove, never purge");
    expect(slurp(dir + "/cleaned") == "clean\n", "U-2: cleanup also cleans the archive cache");
  }
}

static void test_metainfo(const char* root)
{
  const std::string m = slurp(std::string(root) + "/data/org.lunduke.LcosUpdates.metainfo.xml");
  expect(has(m, "<id>org.lunduke.LcosUpdates</id>") && has(m, "<release version=\"0.9.5\"") &&
             has(m, "org.lunduke.LcosUpdates.desktop"),
         "U-3: AppStream metainfo with a 0.9.5 release entry");
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_pure();
  if (argc >= 3) {
    test_helper(argv[1]);
    test_metainfo(argv[2]);
  } else
    expect(false, "helper path and source root arguments");
  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  return 0;
}
