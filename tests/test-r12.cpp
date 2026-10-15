/* Round 12 regression tests (lcos-updates 0.9.4-4).
 * U-1 packages that use debconf are named before install, and the questions
 *     the non-interactive install answered are reported afterwards.
 * U-2 new packages carry a size in the helper protocol and the parser.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv[1] is the helper. It runs as this (non-root) user with stub apt-get
 * and apt-cache; root ignores every override used here.
 */

#include "tmpdir-cleanup.hpp"
#include "apt-parse.hpp"

#include <algorithm>
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
    if [ $inst = 1 ] && [ $sim = 0 ] && [ $uris = 0 ] && [ -n "$LCOS_STUB_PIDFILE" ]; then
      cp "$LCOS_STUB_PIDFILE" "$LCOS_STUB_COUNTFILE"
      echo "debconf (developer): frontend running, package name is epsilon" >&2
      echo "debconf (developer): <-- INPUT high epsilon/color" >&2
      echo "debconf (developer): <-- INPUT high epsilon/name" >&2
      echo "debconf (developer): <-- INPUT critical epsilon/secret" >&2
      echo "debconf (developer): --> 0 hunter2" >&2
    fi
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
  *)
    echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded." ;;
esac
exit 0
)SH";

/* apt-cache show: epsilon depends on debconf, delta does not. */
static const char kStubCache[] = R"SH(#!/bin/sh
printf 'apt-cache %s\n' "$*" >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do
  case "$a" in
    epsilon=*) printf 'Package: epsilon\nVersion: 1.0\nDepends: libc6 (>= 2.36), debconf (>= 0.5) | debconf-2.0\n\n' ;;
    delta=*) printf 'Package: delta\nVersion: 2.0\nDepends: epsilon, notdebconf, libdebconfclient0\n\n' ;;
  esac
done
exit 0
)SH";

static const char kTemplates[] = R"(Name: epsilon/color
Default: green
Description: Favourite colour?
Owners: epsilon
Type: select

Name: epsilon/name
Description: Your name?
Owners: epsilon
Type: string

Name: epsilon/secret
Description: Password?
Owners: epsilon
Type: password

Name: epsilon/info
Description: Note
Owners: epsilon
Type: note

Name: epsilon/asked
Description: Asked before
Owners: epsilon
Type: boolean
)";

/* Written by the "install": what debconf's config.dat looks like afterwards. */
static const char kConfigAfter[] = R"(Name: epsilon/asked
Template: epsilon/asked
Value: true
Owners: epsilon
Flags: seen

Name: epsilon/color
Template: epsilon/color
Value: green
Owners: epsilon

Name: epsilon/info
Template: epsilon/info
Owners: epsilon

Name: epsilon/name
Template: epsilon/name
Value:
Owners: epsilon

Name: epsilon/secret
Template: epsilon/secret
Value: hunter2
Owners: epsilon

Name: other/q
Template: other/q
Value: x
Owners: other
)";

static void test_parser()
{
  /* Detection from apt-cache show. */
  const auto deps = packages_depending_on_debconf(
      "Package: a\nDepends: debconf (>= 0.5) | debconf-2.0\n\n"
      "Package: b\nPre-Depends: debconf-2.0\n\n"
      "Package: c\nDepends: libdebconfclient0, debconf-utils, po-debconf\n\n"
      "Package: d\nDepends: foo (>= 1.0-debconf)\n");
  expect(deps == std::vector<std::string>({"a", "b"}),
         "U-1: Depends/Pre-Depends on debconf or debconf-2.0 are detected, look-alikes are not");
  expect(debconf_question_owners(kConfigAfter) == std::vector<std::string>({"epsilon", "other"}),
         "U-1: question owners come from config.dat");

  const auto ans = unseen_debconf_answers(kConfigAfter, kTemplates, {"epsilon"});
  std::vector<std::string> qs;
  for (const auto& a : ans)
    qs.push_back(a.question + "=" + a.value);
  expect(qs == std::vector<std::string>({"epsilon/color=green", "epsilon/name=", "epsilon/secret=(password not shown)"}),
         "U-1: only unseen asking questions are reported; seen and notes are skipped; passwords hidden");

  SimulateResult r;
  r.status = SimulateResult::Success;
  r.debconf_pkgs = {"epsilon"};
  r.debconf_answers = ans;
  /* A value with a newline and a space must survive the protocol. */
  r.debconf_answers.push_back({"epsilon", "epsilon/evil", "a b\nSTATUS error", ""});
  r.new_packages.push_back(PackageUpgrade{});
  r.new_packages.back().name = "epsilon";
  r.new_packages.back().old_version = "-";
  r.new_packages.back().new_version = "1.0";
  r.new_packages.back().size = "1,024 B";
  const std::string wire = format_protocol(r);
  expect(wire.find("SIZE epsilon 1,024 B\n") != std::string::npos, "U-2: SIZE is emitted for new packages");
  expect(wire.find("\nSTATUS error") == std::string::npos, "U-1: a value cannot inject a protocol line");
  const SimulateResult back = parse_protocol(wire);
  expect(back.new_packages.size() == 1 && back.new_packages[0].size == "1,024 B",
         "U-2: the parser gives the new package its size");
  expect(back.debconf_pkgs == std::vector<std::string>({"epsilon"}), "U-1: ASKS round-trips");
  expect(back.debconf_answers.size() == 4 && back.debconf_answers[1].value.empty() &&
             back.debconf_answers[3].value == "a b\nSTATUS error",
         "U-1: ANSWER round-trips blank and escaped values");

  const std::string notice = debconf_review_notice({"epsilon"});
  /* Round 13 (U-5) reworded the notice; see test-r13. */
  expect(has(notice, "epsilon may ask setup questions") && has(notice, "suggested answer") &&
             has(notice, "sudo dpkg-reconfigure epsilon"),
         "U-1: review notice names the package, the defaults and the fix");
  expect(debconf_review_notice({}).empty(), "U-1: no notice without debconf packages");

  SimulateResult done;
  done.status = SimulateResult::Success;
  done.upgraded_count = 1;
  done.not_upgraded_count = 0;
  done.debconf_answers = ans;
  const JobOutcome o = outcome_install(done, 0, true);
  /* Round 13 (U-3) shows question text when known, the ID otherwise. */
  expect(has(o.status, "Setup questions were answered automatically: \u201cepsilon/color\u201d (epsilon): green; "
                       "\u201cepsilon/name\u201d (epsilon): left blank; "
                       "\u201cepsilon/secret\u201d (epsilon): (password not shown).") &&
             has(o.status, "sudo dpkg-reconfigure epsilon."),
         "U-1: install result lists the answers and how to change them");
  done.debconf_answers.clear();
  expect(!has(outcome_install(done, 0, true).status, "Setup questions"),
         "U-1: no sentence when nothing was auto-answered");
}

static void test_helper(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r12-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    expect(false, "mkdtemp");
    return;
  }
  lcos_test::cleanup_at_exit(tmpl);
  const std::string dir = tmpl;
  const std::string apt = dir + "/apt-get";
  const std::string cache = dir + "/apt-cache";
  const std::string status = dir + "/status";
  const std::string updates = dir + "/updates";
  const std::string lists = dir + "/lists";
  const std::string state = dir + "/state";
  const std::string debconf = dir + "/debconf";
  for (const auto& d : {updates, lists, state, debconf})
    mkdir(d.c_str(), 0755);
  write_file(apt, kStubApt, 0755);
  write_file(cache, kStubCache, 0755);
  write_file(status, "Package: delta\nStatus: install ok installed\nVersion: 1.0\n\n");
  write_file(debconf + "/templates.dat", kTemplates);
  write_file(debconf + "/config.dat", "");
  write_file(dir + "/config-after", kConfigAfter);
  int n = 0;
  auto run = [&](std::vector<std::string> args, std::string& log, bool with_cache) {
    log = dir + "/argv" + std::to_string(++n);
    std::vector<std::pair<std::string, std::string>> env = {
        {"LCOS_UPDATES_APT_GET", apt},
        {"LCOS_UPDATES_DPKG_STATUS", status},
        {"LCOS_UPDATES_DPKG_UPDATES_DIR", updates},
        {"LCOS_UPDATES_LISTS_DIR", lists},
        {"LCOS_UPDATES_STATE_DIR", state},
        {"LCOS_UPDATES_DEBCONF_DIR", debconf},
        {"LCOS_STUB_SCRIPT", "adopt"},
        {"LCOS_STUB_PIDFILE", dir + "/config-after"},
        {"LCOS_STUB_COUNTFILE", debconf + "/config.dat"},
        {"LCOS_STUB_ARGVFILE", log}};
    if (with_cache)
      env.push_back({"LCOS_UPDATES_APT_CACHE", cache});
    return run_helper(helper, args, env);
  };

  {
    std::string log;
    const HelperRun r = run({"simulate"}, log, true);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.new_packages.size() == 1 && p.new_packages[0].name == "epsilon" &&
               p.new_packages[0].size == "1.0 kB" && has(r.out, "SIZE epsilon 1.0 kB"),
           "U-2: Check emits a size for the new dependency");
    expect(p.debconf_pkgs == std::vector<std::string>({"epsilon"}),
           "U-1: Check names the package that depends on debconf (and not delta)");
    expect(has(slurp(log), "apt-cache show --no-all-versions delta=2.0 epsilon=1.0"),
           "U-1: detection asks apt-cache about the exact reviewed versions");
  }
  {
    /* Already-installed questions count even without apt-cache. */
    write_file(debconf + "/config.dat", "Name: delta/q\nTemplate: delta/q\nOwners: delta\n");
    std::string log;
    const HelperRun r = run({"simulate"}, log, false);
    const SimulateResult p = parse_protocol(r.out);
    expect(p.debconf_pkgs == std::vector<std::string>({"delta"}),
           "U-1: a package that already owns debconf questions is named");
    write_file(debconf + "/config.dat", "");
  }
  {
    /* Round 13b: the install runs with DEBCONF_NONINTERACTIVE_SEEN=true, so
     * debconf marks the questions it would have shown; epsilon/asked was
     * already seen before the install. */
    write_file(debconf + "/config.dat",
               "Name: epsilon/asked\nTemplate: epsilon/asked\nValue: true\nOwners: epsilon\nFlags: seen\n");
    std::string after = kConfigAfter;
    for (const char* q : {"epsilon/color\n", "epsilon/name\n", "epsilon/secret\n"}) {
      const std::size_t at = after.find(std::string("Name: ") + q);
      const std::size_t end = after.find("\n\n", at);
      after.insert(end + 1, "Flags: seen\n");
    }
    write_file(dir + "/config-after", after);
    std::string log;
    const HelperRun r = run({"upgrade", "delta=2.0", "epsilon=1.0"}, log, true);
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 0 && p.status == SimulateResult::Success, "U-1: install succeeds");
    std::vector<std::string> qs;
    for (const auto& a : p.debconf_answers)
      qs.push_back(a.package + ":" + a.question + "=" + a.value);
    expect(qs == std::vector<std::string>({"epsilon:epsilon/color=green", "epsilon:epsilon/name=",
                                           "epsilon:epsilon/secret=password set, not shown"}),
           "U-1: install reports the auto-answered questions with their values");
    expect(!has(r.out, "hunter2"), "U-1: the password value never leaves the helper");
    const std::string calls = slurp(log);
    expect(has(calls, "Dpkg::Options::=--force-confold install delta=2.0 epsilon=1.0") ||
               has(calls, "install delta=2.0 epsilon=1.0"),
           "U-1: the install itself is unchanged (non-interactive pinned install)");
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
