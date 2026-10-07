/* Headless tests for the apt-get -s parser and helper protocol.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "apt-parse.hpp"
#include "startup-id.hpp"
#include "timeouts.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static int g_fails = 0;

static std::string read_file(const std::string& path)
{
  std::ifstream in(path);
  if (!in) {
    std::cerr << "cannot read " << path << "\n";
    ++g_fails;
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static void expect(bool ok, const char* what)
{
  if (!ok) {
    std::cerr << "FAIL: " << what << "\n";
    ++g_fails;
  } else {
    std::cout << "ok: " << what << "\n";
  }
}

int main(int argc, char** argv)
{
  const std::string dir = (argc > 1) ? argv[1] : "tests/fixtures";

  {
    const SimulateResult r = parse_apt_simulate(read_file(dir + "/apt-uptodate.txt"));
    expect(r.status == SimulateResult::UpToDate, "up-to-date fixture status");
    expect(r.packages.empty(), "up-to-date fixture has no packages");
    const std::string proto = format_protocol(r);
    expect(proto == "STATUS up-to-date\n", "up-to-date protocol text");
    const SimulateResult p = parse_protocol(proto);
    expect(p.status == SimulateResult::UpToDate, "up-to-date protocol roundtrip");
  }

  {
    const SimulateResult r = parse_apt_simulate(read_file(dir + "/apt-upgrades.txt"));
    expect(r.status == SimulateResult::Upgrades, "upgrades fixture status");
    expect(r.packages.size() == 3, "upgrades fixture COUNT 3 Inst lines");
    if (r.packages.size() >= 3) {
      expect(r.packages[0].name == "libc6", "pkg0 name");
      expect(r.packages[0].old_version == "2.36-9+deb12u3", "pkg0 old");
      expect(r.packages[0].new_version == "2.36-9+deb12u4", "pkg0 new");
      expect(r.packages[1].name == "libc-bin", "pkg1 name");
      expect(r.packages[2].name == "foo", "pkg2 name (no old version)");
      expect(r.packages[2].old_version == "-", "pkg2 old is dash");
      expect(r.packages[2].new_version == "1.0-1", "pkg2 new");
    }
    const std::string proto = format_protocol(r);
    expect(proto.find("STATUS upgrades\n") == 0, "upgrades protocol STATUS");
    expect(proto.find("COUNT 3\n") != std::string::npos, "upgrades protocol COUNT");
    expect(proto.find("PKG libc6 2.36-9+deb12u3 2.36-9+deb12u4\n") != std::string::npos,
           "upgrades protocol PKG libc6");
    expect(proto.find("PKG foo - 1.0-1\n") != std::string::npos, "upgrades protocol PKG foo");
    const SimulateResult p = parse_protocol(proto);
    expect(p.status == SimulateResult::Upgrades, "upgrades protocol roundtrip");
    expect(p.packages.size() == 3, "upgrades protocol package count");
  }

  {
    const SimulateResult r = parse_apt_simulate(read_file(dir + "/apt-error.txt"));
    expect(r.status == SimulateResult::Error, "error fixture status");
    expect(!r.error_msg.empty(), "error fixture MSG");
    expect(r.error_msg.find("Failed to fetch") != std::string::npos ||
               r.error_msg.find("E:") != std::string::npos,
           "error fixture uses apt E: line");
    const std::string proto = format_protocol(r);
    expect(proto.find("STATUS error\n") == 0, "error protocol STATUS");
    expect(proto.find("MSG ") != std::string::npos, "error protocol MSG");
    const SimulateResult p = parse_protocol(proto);
    expect(p.status == SimulateResult::Error, "error protocol roundtrip");
  }

  {
    PackageUpgrade pkg;
    expect(parse_inst_line("Inst name [old] (new origin [amd64])", pkg), "Inst old+new");
    expect(pkg.name == "name" && pkg.old_version == "old" && pkg.new_version == "new",
           "Inst old+new fields");
    expect(parse_inst_line("Inst other (1.2.3 Debian:12 [amd64])", pkg), "Inst new only");
    expect(pkg.name == "other" && pkg.old_version == "-" && pkg.new_version == "1.2.3",
           "Inst new only fields");
    expect(!parse_inst_line("Conf libc6 (1.0 Debian:12 [amd64])", pkg), "Conf is not Inst");
    expect(!parse_inst_line("Remv unused-pkg [0.1]", pkg), "Remv is not Inst");
  }

  {
    static_assert(kCheckTimeoutSec >= kUpdateTimeoutSec + kSimulateTimeoutSec,
                  "GUI check budget must cover helper update + simulate");
    static_assert(kInstallTimeoutSec >=
                      kUpdateTimeoutSec + kSimulateTimeoutSec + kUpgradeTimeoutSec,
                  "GUI install budget must cover helper update + resimulate + upgrade");
    const char* check_timeout =
        "Timed out waiting for the update check. Check your network and try again.";
    const char* install_timeout = "Timed out while installing updates.";
    const char* offline = "No network connection. Connect to the Internet and try again.";
    expect(friendly_job_error("Timed out while running apt-get update", JobKind::Check) ==
               check_timeout,
           "check update timeout stays a check timeout");
    expect(friendly_job_error("Timed out while simulating apt-get upgrade", JobKind::Check) ==
               check_timeout,
           "check simulate timeout stays a check timeout");
    expect(friendly_job_error("Timed out while installing updates", JobKind::Install) ==
               install_timeout,
           "install timeout is not rewritten as a check timeout");
    expect(friendly_job_error("Connection timed out", JobKind::Check) == offline,
           "connection timed out is offline, not the check-timeout sentence");
    expect(friendly_job_error("E: Failed to fetch http://deb.example/InRelease Connection timed out",
                              JobKind::Install) == offline,
           "connection timed out on a fetch line is offline");
    expect(friendly_job_error("E: Failed to fetch http://deb.example/InRelease", JobKind::Check) ==
               "E: Failed to fetch http://deb.example/InRelease",
           "failed to fetch without a network failure stays the apt line");
    expect(friendly_job_error("E: Failed to fetch http://deb.example/InRelease 404 Not Found",
                              JobKind::Check) ==
               "E: Failed to fetch http://deb.example/InRelease 404 Not Found",
           "404 stays the apt line");
    expect(friendly_job_error("E: Failed to fetch http://deb.example/Packages.gz Hash Sum mismatch",
                              JobKind::Install) ==
               "E: Failed to fetch http://deb.example/Packages.gz Hash Sum mismatch",
           "hash sum mismatch stays the apt line");
    expect(friendly_job_error("NO_PUBKEY 1234567890ABCDEF", JobKind::Check) ==
               "NO_PUBKEY 1234567890ABCDEF",
           "NO_PUBKEY stays the apt line");
    expect(friendly_job_error("Temporary failure resolving 'deb.example'", JobKind::Check) == offline,
           "resolve failure is offline");
    expect(friendly_job_error("Network is unreachable", JobKind::Install) == offline,
           "unreachable network is offline");
    expect(friendly_job_error("E: Unable to connect to deb.example:80", JobKind::Check) == offline,
           "unable to connect is offline");
    expect(friendly_job_error("E: Unable to correct problems, you have held broken packages.",
                              JobKind::Install) ==
               "E: Unable to correct problems, you have held broken packages.",
           "unrelated apt error is kept");
  }

  {
    const char* text =
        "Reading package lists...\n"
        "The following packages have been kept back:\n"
        "  linux-image-amd64 linux-headers-amd64\n"
        "0 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.status == SimulateResult::KeptBack, "kept-back only is not up to date");
    expect(r.packages.empty(), "kept-back only has no Inst packages");
    expect(r.kept_back.size() == 2, "kept-back names both packages");
    expect(r.not_upgraded_count == 2, "kept-back summary count");
    if (r.kept_back.size() >= 2) {
      expect(r.kept_back[0] == "linux-image-amd64", "kept-back pkg0");
      expect(r.kept_back[1] == "linux-headers-amd64", "kept-back pkg1");
    }
    const std::string proto = format_protocol(r);
    expect(proto.find("STATUS kept-back\n") == 0, "kept-back protocol status");
    expect(proto.find("KEPT linux-image-amd64\n") != std::string::npos, "kept-back protocol name");
    expect(proto.find("NOT_UPGRADED 2\n") != std::string::npos, "kept-back protocol count");
    const SimulateResult p = parse_protocol(proto);
    expect(p.status == SimulateResult::KeptBack, "kept-back protocol roundtrip");
    expect(p.kept_back.size() == 2, "kept-back protocol names roundtrip");
    expect(p.not_upgraded_count == 2, "kept-back protocol count roundtrip");
  }

  {
    const char* text =
        "The following packages have been kept back:\n"
        "  linux-image-amd64\n"
        "Inst libc6 [1] (2 Debian:12 [amd64])\n"
        "1 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.status == SimulateResult::Upgrades, "inst plus kept-back is upgrades");
    expect(r.packages.size() == 1 && r.packages[0].name == "libc6", "inst package kept");
    expect(r.kept_back.size() == 1 && r.kept_back[0] == "linux-image-amd64",
           "kept-back package named beside inst");
    expect(r.not_upgraded_count == 1, "mixed summary not-upgraded");
    expect(r.upgraded_count == 1, "mixed summary upgraded");
  }

  {
    const SimulateResult r = parse_apt_simulate(
        "Calculating upgrade...\n"
        "0 upgraded, 0 newly installed, 0 to remove and 3 not upgraded.\n");
    expect(r.status == SimulateResult::KeptBack, "non-zero not upgraded is not up to date");
    expect(r.kept_back.empty(), "summary-only kept-back has no names");
    expect(r.not_upgraded_count == 3, "summary-only not-upgraded count");
  }

  {
    const char* partial =
        "E: Failed to fetch http://deb.example/dists/stable/InRelease 404 Not Found\n"
        "E: Some index files failed to download. They have been ignored, or old ones used instead.\n";
    expect(apt_index_failure_is_partial(partial), "404 with some index files is partial");
    const std::string warning = apt_index_warning(partial);
    expect(warning.find("404 Not Found") != std::string::npos, "partial warning keeps the 404");
    expect(warning.find("Some index files failed") != std::string::npos,
           "partial warning keeps the apt summary");
    expect(!apt_index_failure_is_partial("E: Could not get lock /var/lib/apt/lists/lock\n"),
           "lock failure is not a partial index update");
    expect(apt_index_failure_is_partial(
               "W: GPG error: http://deb.example stable InRelease: NO_PUBKEY 1234ABCD\n"
               "E: Some index files failed to download. They have been ignored, or old ones used instead.\n"),
           "missing key with old indexes is partial");
  }

  {
    SimulateResult r;
    r.status = SimulateResult::Error;
    r.error_msg = "E: Failed to fetch http://deb.example/InRelease 404 Not Found\n"
                  "E: Could not get lock /var/lib/apt/lists/lock";
    const std::string proto = format_protocol(r);
    expect(proto.find("MSG E: Failed to fetch http://deb.example/InRelease 404 Not Found\n") !=
               std::string::npos,
           "newline in error_msg is its own MSG line");
    expect(proto.find("MSG E: Could not get lock /var/lib/apt/lists/lock\n") != std::string::npos,
           "second error line is its own MSG line");
    expect(proto.find("\nE: Could not get lock") == std::string::npos,
           "error continuation is not a raw line");
    const SimulateResult p = parse_protocol(proto);
    expect(p.status == SimulateResult::Error, "multi-line MSG round trip status");
    expect(p.error_msg.find("404 Not Found") != std::string::npos, "round trip keeps the 404");
    expect(p.error_msg.find("Could not get lock") != std::string::npos, "round trip keeps the lock line");
    expect(p.error_msg.find('\n') != std::string::npos, "round trip keeps the newline inside error_msg");
  }

  {
    const std::string blob =
        "E: Failed to fetch http://deb.example/InRelease 404 Not Found\n"
        "E: Connection timed out";
    const std::string got = friendly_job_error(blob, JobKind::Check);
    expect(got.find("404 Not Found") != std::string::npos,
           "a timeout on a later line does not discard the 404");
    expect(got.find("No network connection") != std::string::npos,
           "the timeout line is still mapped on its own");
  }

  {
    const char* text =
        "Configuration file '/etc/ssh/sshd_config'\n"
        " ==> Modified (by you or by a script) since installation.\n"
        " ==> Package distributor has shipped an updated version.\n"
        " ==> Keeping old config file as default.\n"
        "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.conffiles_kept.size() == 1, "kept conffile is parsed");
    if (!r.conffiles_kept.empty())
      expect(r.conffiles_kept[0] == "/etc/ssh/sshd_config", "kept conffile path");
    const std::string proto = format_protocol(r);
    expect(proto.find("CONFKEPT /etc/ssh/sshd_config\n") != std::string::npos, "CONFKEPT protocol line");
    const SimulateResult p = parse_protocol(proto);
    expect(p.conffiles_kept.size() == 1 && p.conffiles_kept[0] == "/etc/ssh/sshd_config",
           "CONFKEPT round trip");
  }

  {
    CaptureBuf cap;
    const std::string early = "E: unique-early-error\n";
    capture_append(cap, early.data(), early.size());
    const std::string junk(2 * kAptCaptureCap, 'x');
    capture_append(cap, junk.data(), junk.size());
    const std::string text = capture_text(cap);
    expect(text.find("unique-early-error") != std::string::npos, "cap keeps the first E: line");
    expect(text.size() < junk.size(), "cap drops the unbounded apt capture");
    expect(text.size() <= kAptCaptureCap + early.size() + 8, "cap stays near 1 MiB");
    CaptureBuf split;
    const std::string a = "E: split-";
    const std::string b = "line\n";
    capture_append(split, a.data(), a.size());
    capture_append(split, b.data(), b.size());
    expect(capture_text(split).find("E: split-line") != std::string::npos,
           "first E: line split across reads is kept");
  }

  {
    unsigned long ts = 99;
    expect(!startup_timestamp_from_id("desktop_TIME0", ts), "timestamp 0 is rejected");
    expect(!startup_timestamp_from_id("desktop_TIME", ts), "empty timestamp is rejected");
    expect(!startup_timestamp_from_id("no-time-here", ts), "missing timestamp is rejected");
    expect(startup_timestamp_from_id("app_TIME42", ts) && ts == 42, "non-zero timestamp is kept");
    expect(startup_timestamp_from_id("app_TIME000", ts) == false, "zero-padded zero is rejected");
  }

  if (g_fails != 0) {
    std::cerr << g_fails << " failure(s)\n";
    return 1;
  }
  std::cout << "all parser tests passed\n";
  return 0;
}
