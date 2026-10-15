/* Round 13 regression tests (lcos-updates 0.9.4-5).
 * U-1 password questions (passwords.dat) are reported, never their values.
 * U-2 answers already there before the install are not listed again.
 * U-3 only questions a normal install would ask (priority), with their text.
 * U-4 a partly failed install still reports its answers.
 * U-5 notice wording: earlier answers kept, defaults for new, Terminal hint.
 * U-6 a new package with templates but no debconf dependency is flagged.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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


static const char kTemplates[] = R"(Name: ask/color
Default: green
Description: Pick a colour
Owners: ask
Type: select

Name: ask/name
Description: Site name
Owners: ask
Type: string

Name: ask/lowq
Description: Enable extra logging?
Owners: ask
Type: boolean

Name: ask/pw
Description: Admin password
Owners: ask
Type: password

Name: ask/pw2
Description: Second password
Owners: ask
Type: password
)";

static const char kTrace[] =
    "Setting up ask (2.0) ...\n"
    "debconf (developer): frontend running, package name is ask\n"
    "debconf (developer): <-- INPUT high ask/color\n"
    "debconf (developer): --> 30 question skipped\n"
    "debconf (developer): <-- INPUT high ask/name\n"
    "debconf (developer): <-- INPUT low ask/lowq\n"
    "debconf (developer): <-- INPUT critical ask/pw\n"
    "debconf (developer): <-- INPUT critical ask/pw2\n"
    "debconf (developer): <-- GET ask/pw2\n"
    "debconf (developer): --> 0 s3cret-value\n";

static void test_parser()
{
  const auto inputs = debconf_inputs(kTrace);
  expect(inputs.size() == 5 && inputs[0].package == "ask" && inputs[2].priority == "low",
         "trace: INPUT lines are read with package and priority");
  const std::string stripped = strip_debconf_trace(kTrace);
  expect(stripped == "Setting up ask (2.0) ...\n", "trace: every debconf trace line is stripped");

  const std::string before_cfg = "Name: ask/color\nTemplate: ask/color\nValue: blue\nOwners: ask\nFlags: seen\n\n"
                                 "Name: ask/name\nTemplate: ask/name\nValue: \nOwners: ask\n";
  const std::string after_cfg = "Name: ask/color\nTemplate: ask/color\nValue: blue\nOwners: ask\nFlags: seen\n\n"
                                "Name: ask/lowq\nTemplate: ask/lowq\nValue: false\nOwners: ask\n\n"
                                "Name: ask/name\nTemplate: ask/name\nValue: \nOwners: ask\n";
  const std::string after_pw = "Name: ask/pw\nTemplate: ask/pw\nValue: \nOwners: ask\n\n"
                               "Name: ask/pw2\nTemplate: ask/pw2\nValue: s3cret-value\nOwners: ask\n";
  const auto before = debconf_snapshot(before_cfg, "");
  const auto after = debconf_snapshot(after_cfg, after_pw);
  bool leaked = false;
  for (const auto& q : after)
    if (q.value.find("s3cret") != std::string::npos)
      leaked = true;
  expect(!leaked, "U-1: the passwords.dat snapshot never keeps a value");

  const auto ans = debconf_auto_answers(inputs, before, after, kTemplates, "high");
  std::vector<std::string> got;
  for (const auto& a : ans)
    got.push_back(a.question + "=" + a.value + "|" + a.text);
  expect(got == std::vector<std::string>({"ask/pw=(password left blank)|Admin password",
                                          "ask/pw2=(password set, not shown)|Second password"}),
         "U-1/U-2/U-3: passwords reported without values; seen color, unchanged name and low lowq are not");

  const auto low = debconf_auto_answers(inputs, before, after, kTemplates, debconf_priority(
      "Name: debconf/priority\nTemplate: debconf/priority\nValue: low\nOwners: debconf\n"));
  bool has_lowq = false;
  for (const auto& a : low)
    if (a.question == "ask/lowq")
      has_lowq = true;
  expect(has_lowq, "U-3: a user who lowered debconf's priority to low sees low questions");
  expect(debconf_priority("") == "high", "U-3: default priority is high");

  /* U-2: an install where nothing changed lists nothing. */
  const auto again = debconf_auto_answers(inputs, after, after, kTemplates, "high");
  expect(again.empty(), "U-2: a later upgrade with the same answers lists nothing again");

  /* U-3/U-1 wording. */
  SimulateResult done;
  done.status = SimulateResult::Success;
  done.upgraded_count = 1;
  done.not_upgraded_count = 0;
  done.debconf_answers = ans;
  const JobOutcome o = outcome_install(done, 0, true);
  expect(has(o.status, "\u201cAdmin password\u201d (ask): (password left blank)") &&
             !has(o.status, "ask/pw") && !has(o.status, "s3cret"),
         "U-3: the result shows the question text, not the internal name, and no password");
  expect(has(o.status, "open Terminal from the applications menu") && has(o.status, "sudo dpkg-reconfigure ask."),
         "U-5: the result has the desktop hint");

  /* QTEXT round-trip. */
  const SimulateResult back = parse_protocol(format_protocol(done));
  expect(back.debconf_answers.size() == 2 && back.debconf_answers[0].text == "Admin password",
         "U-3: question text round-trips the protocol");

  const std::string notice = debconf_review_notice({"ask", "sloppy"});
  expect(has(notice, "answers you gave before are kept") && has(notice, "new questions get the package's suggested answer") &&
             !has(notice, "package defaults will be used") && has(notice, "open Terminal"),
         "U-5: the notice says earlier answers are kept and defaults are for new questions");

  /* U-4: an error result carries answers and the window outcome can show them. */
  SimulateResult err;
  err.status = SimulateResult::Error;
  err.error_msg = "The upgrade stopped with a package left unfinished.";
  err.debconf_answers = ans;
  const SimulateResult eb = parse_protocol(format_protocol(err));
  expect(eb.status == SimulateResult::Error && eb.debconf_answers.size() == 2,
         "U-4: answers survive an error result in the protocol");
}

static const char kStubApt[] = R"SH(#!/bin/sh
printf 'apt-get' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
sim=0; uris=0; inst=0; dl=0
for a in "$@"; do
  [ "$a" = "-s" ] && sim=1
  [ "$a" = "--print-uris" ] && uris=1
  [ "$a" = "install" ] && inst=1
  [ "$a" = "--download-only" ] && dl=1
done
case "$*" in *" update"|update) echo "Hit:1 http://deb.example stable InRelease"; exit 0;; esac
if [ $uris = 1 ]; then
  echo "Need to get 1024 B of archives."
  echo "After this operation, 1024 B of additional disk space will be used."
  echo "'file:$LCOS_STUB_PIDFILE/quiet_1.0_all.deb' quiet_1.0_all.deb 1024 MD5Sum:00"
  echo "'file:$LCOS_STUB_PIDFILE/plainnew_1.0_all.deb' plainnew_1.0_all.deb 1024 MD5Sum:00"
  exit 0
fi
[ $dl = 1 ] && exit 0
if [ $inst = 1 ]; then
  if [ $sim = 0 ]; then
    env > "$LCOS_STUB_ENVFILE"
    # Like apt, copy what dpkg's children print into term.log. Real debconf
    # prints its protocol trace, answers included, only with DEBCONF_DEBUG.
    if [ -n "$DEBCONF_DEBUG" ]; then
      echo "debconf (developer): <-- SET quiet/pw TOPSECRET4411" | tee -a "$LCOS_STUB_STATEFILE" >&2
      echo "debconf (developer): --> 0 TOPSECRET4411" | tee -a "$LCOS_STUB_STATEFILE" >&2
    fi
    if [ "$DEBCONF_NONINTERACTIVE_SEEN" = true ]; then flags='Flags: seen\n'; else flags=''; fi
    printf "Name: quiet/q\nTemplate: quiet/q\nValue: yes\nOwners: quiet\n${flags}\nName: quiet/skip\nTemplate: quiet/skip\nValue: no\nOwners: quiet\n" > "$LCOS_STUB_COUNTFILE"
    printf "Name: quiet/pw\nTemplate: quiet/pw\nValue: TOPSECRET4411\nOwners: quiet\n${flags}" > "$(dirname "$LCOS_STUB_COUNTFILE")/passwords.dat"
    echo "dpkg: error processing package plainnew (--configure):"
    echo "Errors were encountered while processing:"
    echo " plainnew"
    echo "E: Sub-process /usr/bin/dpkg returned an error code (1)"
    exit 100
  fi
  echo "The following NEW packages will be installed:"
  echo "  plainnew quiet"
  echo "Inst plainnew (1.0 local [all])"
  echo "Inst quiet (1.0 local [all])"
  echo "Inst host [1.0] (2.0 local [all])"
  echo "1 upgraded, 2 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
echo "The following packages have been kept back:"
echo "  host"
echo "Inst host [1.0] (2.0 local [all])"
echo "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded."
exit 0
)SH";

/* No package depends on debconf. */
static const char kStubCache[] = R"SH(#!/bin/sh
for a in "$@"; do
  case "$a" in
    *=*) printf 'Package: %s\nVersion: 1.0\n\n' "${a%%=*}" ;;
  esac
done
exit 0
)SH";

/* dpkg-deb --info DEB templates: only quiet ships templates. */
static const char kStubDpkgDeb[] = R"SH(#!/bin/sh
case "$2" in
  */quiet_*) printf 'Template: quiet/q\nType: string\nDescription: Quiet question\n'; exit 0 ;;
esac
echo "dpkg-deb: error: no such control component" >&2
exit 2
)SH";

static void test_helper(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r13-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    expect(false, "mkdtemp");
    return;
  }
  const std::string dir = tmpl;
  const std::string debconf = dir + "/debconf";
  const std::string state = dir + "/state";
  for (const auto& d : {dir + "/updates", dir + "/lists", state, debconf})
    mkdir(d.c_str(), 0755);
  write_file(dir + "/apt-get", kStubApt, 0755);
  write_file(dir + "/apt-cache", kStubCache, 0755);
  write_file(dir + "/dpkg-deb", kStubDpkgDeb, 0755);
  write_file(dir + "/status", "Package: host\nStatus: install ok installed\nVersion: 1.0\n\n");
  write_file(debconf + "/templates.dat", "Name: quiet/q\nType: string\nDescription: Quiet question\nOwners: quiet\n\n"
                                         "Name: quiet/skip\nType: string\nDescription: Skipped question\nOwners: quiet\n\n"
                                         "Name: quiet/pw\nType: password\nDescription: Quiet password\nOwners: quiet\n");
  write_file(dir + "/term.log", "");
  write_file(dir + "/env", "");
  write_file(debconf + "/config.dat", "");
  write_file(dir + "/argv", "");
  std::vector<std::pair<std::string, std::string>> env = {
      {"LCOS_UPDATES_APT_GET", dir + "/apt-get"},
      {"LCOS_UPDATES_APT_CACHE", dir + "/apt-cache"},
      {"LCOS_UPDATES_DPKG_DEB", dir + "/dpkg-deb"},
      {"LCOS_UPDATES_DPKG_STATUS", dir + "/status"},
      {"LCOS_UPDATES_DPKG_UPDATES_DIR", dir + "/updates"},
      {"LCOS_UPDATES_LISTS_DIR", dir + "/lists"},
      {"LCOS_UPDATES_STATE_DIR", state},
      {"LCOS_UPDATES_DEBCONF_DIR", debconf},
      {"LCOS_STUB_PIDFILE", dir},
      {"LCOS_STUB_COUNTFILE", debconf + "/config.dat"},
      {"LCOS_STUB_ARGVFILE", dir + "/argv"},
      {"LCOS_STUB_ENVFILE", dir + "/env"},
      {"LCOS_STUB_STATEFILE", dir + "/term.log"}};
  {
    const HelperRun r = run_helper(helper, {"simulate"}, env);
    const SimulateResult p = parse_protocol(r.out);
    bool quiet = false;
    bool plain = false;
    for (const auto& n : p.debconf_pkgs) {
      quiet = quiet || n == "quiet";
      plain = plain || n == "plainnew";
    }
    expect(quiet && !plain, "U-6: a new package with templates and no debconf dependency is flagged; one without is not");
    expect(has(slurp(dir + "/argv"), "--download-only"), "U-6: Check downloads the new package to inspect it");
    struct stat st {};
    bool leftover = false;
    std::string ls = state;
    if (FILE* f = popen(("ls " + state).c_str(), "r")) {
      char b[256];
      while (fgets(b, sizeof b, f) != nullptr)
        if (std::strstr(b, "lcos-updates-templates") != nullptr)
          leftover = true;
      pclose(f);
    }
    (void)st;
    expect(!leftover, "U-6: the private download directory is removed");
  }
  {
    const HelperRun r = run_helper(helper, {"upgrade", "host=2.0", "plainnew=1.0", "quiet=1.0"}, env);
    const SimulateResult p = parse_protocol(r.out);
    std::vector<std::string> got;
    for (const auto& a : p.debconf_answers)
      got.push_back(a.question + "=" + a.value);
    expect(p.status == SimulateResult::Error &&
               got == std::vector<std::string>({"quiet/q=yes", "quiet/pw=(password set, not shown)"}) &&
               p.debconf_answers[0].text == "Quiet question",
           "U-4: a failed install still reports the answers it chose; unseen quiet/skip is not listed");
    const std::string envs = slurp(dir + "/env");
    expect(!has(envs, "DEBCONF_DEBUG") && has(envs, "DEBCONF_NONINTERACTIVE_SEEN=true"),
           "r13b: the install runs without DEBCONF_DEBUG and with DEBCONF_NONINTERACTIVE_SEEN=true");
    expect(!has(slurp(dir + "/term.log"), "TOPSECRET4411"),
           "r13b: no password value reaches apt's term.log");
    expect(!has(r.out, "TOPSECRET4411") && !has(r.err, "TOPSECRET4411"),
           "r13b: no password value reaches the helper's stdout or stderr");
    expect(!has(r.out, "debconf (developer)") && !has(r.err, "debconf (developer)"),
           "trace lines never reach the window");
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
