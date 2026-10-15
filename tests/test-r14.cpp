/* Round 14 regression tests (lcos-updates 0.9.4-7).
 * U-1 stale lcos-updates-templates.* folders are removed at helper start.
 * U-2 Check downloads into apt's archive cache (kept for Install), no private folder.
 * U-3 a deb whose download failed (hash mismatch) is never inspected.
 * U-4 Repair reports the setup answers it chose.
 * U-5 password answers have one set of brackets.
 * U-6 the test removes its own /tmp folder.
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
sim=0; uris=0; inst=0; dl=0; fix=0; arch=""
prev=""
for a in "$@"; do
  [ "$a" = "-s" ] && sim=1
  [ "$a" = "--print-uris" ] && uris=1
  [ "$a" = "install" ] && inst=1
  [ "$a" = "--download-only" ] && dl=1
  [ "$a" = "-f" ] && fix=1
  case "$prev$a" in -oDir::Cache::archives=*) arch="${a#Dir::Cache::archives=}";; esac
  prev="$a"
done
d="$LCOS_STUB_PIDFILE"
case "$*" in *" update"|update) echo "Hit:1 http://deb.example stable InRelease"; exit 0;; esac
if [ $uris = 1 ]; then
  echo "Need to get 1024 B of archives."
  echo "After this operation, 1024 B of additional disk space will be used."
  echo "'file:$d/plainnew_1.0_all.deb' plainnew_1.0_all.deb 1024 MD5Sum:00"
  echo "'http://deb.example/quiet_1.0_all.deb' quiet_1.0_all.deb 1024 MD5Sum:00"
  exit 0
fi
if [ $dl = 1 ]; then
  if [ -e "$d/fail-download" ]; then
    echo "E: Failed to fetch http://deb.example/quiet_1.0_all.deb  Hash Sum mismatch" >&2
    exit 100
  fi
  echo "$arch" > "$d/archdir"
  if [ ! -e "${arch}quiet_1.0_all.deb" ]; then
    echo dl >> "$d/downloads"
    printf 'deb' > "${arch}quiet_1.0_all.deb"
  fi
  exit 0
fi
if [ $fix = 1 ]; then
  echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
if [ $inst = 1 ]; then
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

static const char kStubCache[] = R"SH(#!/bin/sh
for a in "$@"; do
  case "$a" in
    *=*) printf 'Package: %s\nVersion: 1.0\n\n' "${a%%=*}" ;;
  esac
done
exit 0
)SH";

static const char kStubDpkgDeb[] = R"SH(#!/bin/sh
echo "$2" >> "$LCOS_STUB_PIDFILE/inspected"
case "$2" in
  */quiet_*) printf 'Template: quiet/q\nType: string\nDescription: Quiet question\n'; exit 0 ;;
esac
exit 2
)SH";

/* dpkg --configure -a answers one question and one password, like a
 * package left unconfigured by an interrupted install. */
static const char kStubDpkg[] = R"SH(#!/bin/sh
c="$LCOS_STUB_COUNTFILE"
if [ "$DEBCONF_NONINTERACTIVE_SEEN" = true ]; then flags='Flags: seen\n'; else flags=''; fi
printf "Name: quiet/q\nTemplate: quiet/q\nValue: yes\nOwners: quiet\n${flags}" > "$c"
printf "Name: quiet/pw\nTemplate: quiet/pw\nValue: TOPSECRET4411\nOwners: quiet\n${flags}" > "$(dirname "$c")/passwords.dat"
echo "Setting up quiet (1.0) ..."
[ -e "$LCOS_STUB_PIDFILE/fail-configure" ] && { echo "dpkg: error processing package quiet (--configure):" >&2; exit 1; }
exit 0
)SH";

static void test_text()
{
  SimulateResult r;
  r.status = SimulateResult::Success;
  SimulateResult::DebconfAnswer a;
  a.package = "ask";
  a.question = "ask/pw";
  a.text = "Admin password";
  a.value = "password set, not shown";
  r.debconf_answers.push_back(a);
  const std::string s = debconf_answer_sentence(r);
  expect(has(s, "\u201cAdmin password\u201d (ask): password set, not shown") && !has(s, "(password"),
         "U-5: a password answer has one set of brackets");
}

static void test_helper(const char* helper)
{
  char tmpl[] = "/tmp/lcos-r14-XXXXXX";
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
  write_file(dir + "/status", "Package: host\nStatus: install ok installed\nVersion: 1.0\n\n");
  write_file(debconf + "/templates.dat", "Name: quiet/q\nType: string\nDescription: Quiet question\nOwners: quiet\n\n"
                                         "Name: quiet/pw\nType: password\nDescription: Quiet password\nOwners: quiet\n");
  write_file(debconf + "/config.dat", "");
  write_file(dir + "/argv", "");
  /* U-1: a folder a killed 0.9.4-6 helper left, plus look-alikes to keep. */
  const std::string stale = state + "/lcos-updates-templates.Ab12Cd";
  mkdir(stale.c_str(), 0700);
  mkdir((stale + "/partial").c_str(), 0700);
  write_file(stale + "/old_1.0_all.deb", "x");
  write_file(stale + "/partial/half.deb", "x");
  mkdir((state + "/keep-me").c_str(), 0755);
  write_file(state + "/lcos-updates-templates.file", "not a folder");
  symlink(dir.c_str(), (state + "/lcos-updates-templates.Link01").c_str());
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
      {"LCOS_STUB_PIDFILE", dir},
      {"LCOS_STUB_COUNTFILE", debconf + "/config.dat"},
      {"LCOS_STUB_ARGVFILE", dir + "/argv"}};
  struct stat st {};
  {
    const HelperRun r = run_helper(helper, {"simulate"}, env);
    const SimulateResult p = parse_protocol(r.out);
    expect(lstat(stale.c_str(), &st) != 0, "U-1: a stale lcos-updates-templates folder is removed at start");
    expect(lstat((state + "/keep-me").c_str(), &st) == 0 &&
               lstat((state + "/lcos-updates-templates.file").c_str(), &st) == 0 &&
               lstat((state + "/lcos-updates-templates.Link01").c_str(), &st) == 0 &&
               lstat(dir.c_str(), &st) == 0,
           "U-1: other entries, plain files and symlinks are left alone");
    bool quiet = false;
    for (const auto& n : p.debconf_pkgs)
      quiet = quiet || n == "quiet";
    expect(quiet, "U-6 r13 still holds: the new package with templates is flagged");
    expect(slurp(dir + "/archdir") == state + "/archives/\n",
           "U-2: Check downloads into apt's archive cache, not a private folder");
    expect(lstat((state + "/archives/quiet_1.0_all.deb").c_str(), &st) == 0,
           "U-2: the downloaded deb stays in the cache for Install");
    bool priv = false;
    if (FILE* f = popen(("ls " + state).c_str(), "r")) {
      char b[256];
      while (fgets(b, sizeof b, f) != nullptr)
        priv = priv || (std::strstr(b, "lcos-updates-templates.") != nullptr &&
                           std::strstr(b, ".file") == nullptr && std::strstr(b, "Link01") == nullptr);
      pclose(f);
    }
    expect(!priv, "U-1: no new private download folder is made");
    const HelperRun r2 = run_helper(helper, {"simulate"}, env);
    (void)r2;
    expect(slurp(dir + "/downloads") == "dl\n", "U-2: a second Check downloads nothing again");
  }
  {
    write_file(dir + "/inspected", "");
    write_file(dir + "/fail-download", "");
    unlink((state + "/archives/quiet_1.0_all.deb").c_str());
    const HelperRun r = run_helper(helper, {"simulate"}, env);
    (void)r;
    expect(slurp(dir + "/inspected").empty(),
           "U-3: after a failed (hash mismatch) download no deb is inspected, file: ones included");
    unlink((dir + "/fail-download").c_str());
  }
  {
    const HelperRun r = run_helper(helper, {"repair"}, env);
    const SimulateResult p = parse_protocol(r.out);
    std::vector<std::string> got;
    for (const auto& a : p.debconf_answers)
      got.push_back(a.question + "=" + a.value);
    expect(p.status == SimulateResult::Success &&
               got == std::vector<std::string>({"quiet/q=yes", "quiet/pw=password set, not shown"}),
           "U-4: Repair reports the setup answers it chose");
    expect(!has(r.out, "TOPSECRET4411") && !has(r.err, "TOPSECRET4411"), "U-4: never the password value");
  }
  {
    write_file(debconf + "/config.dat", "");
    unlink((debconf + "/passwords.dat").c_str());
    write_file(dir + "/fail-configure", "");
    const HelperRun r = run_helper(helper, {"repair"}, env);
    const SimulateResult p = parse_protocol(r.out);
    expect(p.status == SimulateResult::Error && p.debconf_answers.size() == 2,
           "U-4: a failed Repair still reports its answers");
  }
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_text();
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
