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

static int count_phrase(const std::string& hay, const char* needle)
{
  int n = 0;
  const std::string nd(needle);
  for (std::string::size_type pos = 0; (pos = hay.find(nd, pos)) != std::string::npos; pos += nd.size())
    ++n;
  return n;
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
    static_assert(kCheckTimeoutSec >= kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec +
                                     kDownloadSizeHardSec,
                  "GUI check budget must cover update, simulate, kept-back probes, and size");
    static_assert(kInstallTimeoutSec >= kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec +
                                        kUpgradeTimeoutSec,
                  "GUI install budget must cover update, probes, resimulate, and upgrade");
    expect(kCheckTimeoutSec >= kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec +
                                   kDownloadSizeHardSec,
           "check deadline includes kept-back classification and the download size");
    expect(kInstallTimeoutSec >=
               kUpdateTimeoutSec + kSimulateTimeoutSec + kKeptClassifySec + kUpgradeTimeoutSec,
           "install deadline includes kept-back classification");
    const char* check_timeout =
        "Timed out waiting for the update check. Check your network and try again.";
    const char* install_timeout = "Timed out while installing updates.";
    const char* offline = "No network connection. Connect to the Internet and try again.";
    const char* proxy =
        "Could not connect to the update server. If this computer uses a proxy, set it in "
        "/etc/apt/apt.conf.d.";
    expect(friendly_job_error("Timed out while running apt-get update", JobKind::Check) ==
               check_timeout,
           "check update timeout stays a check timeout");
    expect(friendly_job_error("Timed out while simulating apt-get upgrade", JobKind::Check) ==
               check_timeout,
           "check simulate timeout stays a check timeout");
    expect(friendly_job_error("Timed out while installing updates", JobKind::Install) ==
               install_timeout,
           "install timeout is not rewritten as a check timeout");
    {
      const std::string got = friendly_job_error("Connection timed out", JobKind::Check);
      expect(got.find("Connection timed out") != std::string::npos,
             "connection timed out keeps apt's words");
      expect(got.find(proxy) != std::string::npos,
             "connection timed out mentions a proxy as a second hint");
      expect(got.find(offline) == std::string::npos,
             "connection timed out is not the offline sentence");
    }
    {
      const std::string line =
          "E: Failed to fetch http://deb.example/InRelease Connection timed out";
      const std::string got = friendly_job_error(line, JobKind::Install);
      expect(got.find(line) != std::string::npos, "a fetch timeout keeps the apt line and URL");
      expect(got.find(proxy) != std::string::npos,
             "connection timed out on a fetch line mentions a proxy");
      expect(got.find(offline) == std::string::npos, "a fetch timeout is not the offline sentence");
    }
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
    {
      const std::string line = "Temporary failure resolving 'deb.example'";
      const std::string got = friendly_job_error(line, JobKind::Check);
      expect(got.find(line) != std::string::npos, "a resolve failure keeps the hostname");
      expect(got.find(offline) != std::string::npos, "resolve failure adds the offline sentence");
      expect(got.find(proxy) == std::string::npos, "a resolve failure is not a proxy hint");
    }
    {
      const std::string line = "Network is unreachable";
      const std::string got = friendly_job_error(line, JobKind::Install);
      expect(got.find(line) != std::string::npos, "an unreachable network keeps apt's words");
      expect(got.find(offline) != std::string::npos, "unreachable network adds the offline sentence");
    }
    {
      const std::string line = "E: Unable to connect to deb.example:80";
      const std::string got = friendly_job_error(line, JobKind::Check);
      expect(got.find(line) != std::string::npos, "unable to connect keeps the apt line");
      expect(got.find(proxy) != std::string::npos, "unable to connect mentions a proxy");
    }
    {
      const std::string line = "Could not connect to 192.0.2.1:80";
      const std::string got = friendly_job_error(line, JobKind::Install);
      expect(got.find(line) != std::string::npos, "could not connect keeps the address");
      expect(got.find(proxy) != std::string::npos, "could not connect mentions a proxy");
      expect(got.find("Connection refused") == std::string::npos || got.find(line) != std::string::npos,
             "could not connect is not replaced");
    }
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
    const char* no_hit =
        "E: Failed to fetch http://deb.example/dists/stable/InRelease 404 Not Found\n"
        "E: Some index files failed to download. They have been ignored, or old ones used instead.\n";
    expect(!apt_index_failure_is_partial(no_hit),
           "404 with no Hit or Get is a total failure, not a partial one");
    const UpdateFetch total = classify_apt_update(no_hit);
    expect(total.kind == UpdateFetchKind::Total, "a 404 with nothing fetched stops the check");
    expect(total.detail.find("404 Not Found") != std::string::npos,
           "a total 404 keeps the apt line");
    const char* partial =
        "Hit:1 http://deb.example stable InRelease\n"
        "E: Failed to fetch http://deb.example/dists/stable/InRelease 404 Not Found\n"
        "E: Some index files failed to download. They have been ignored, or old ones used instead.\n";
    expect(apt_index_failure_is_partial(partial), "404 after a Hit is partial");
    const std::string warning = apt_index_warning(partial);
    expect(warning.find("404 Not Found") != std::string::npos, "partial warning keeps the 404");
    expect(warning.find("Some index files failed") != std::string::npos,
           "partial warning keeps the apt summary");
    expect(!apt_index_failure_is_partial("E: Could not get lock /var/lib/apt/lists/lock\n"),
           "lock failure is not a partial index update");
    const char* key_no_hit =
        "W: GPG error: http://deb.example stable InRelease: NO_PUBKEY 1234ABCD\n"
        "E: Some index files failed to download. They have been ignored, or old ones used instead.\n";
    expect(!apt_index_failure_is_partial(key_no_hit),
           "a missing key with nothing fetched is not partial");
    expect(apt_index_failure_is_partial(std::string("Get:1 http://deb.example stable InRelease\n") +
                                        key_no_hit),
           "missing key after a Get is partial");
    const char* dead =
        "W: Failed to fetch http://deb.example/InRelease  Connection timed out\n"
        "E: Some index files failed to download. They have been ignored, or old ones used instead.\n";
    const UpdateFetch dead_fetch = classify_apt_update(dead);
    expect(dead_fetch.kind == UpdateFetchKind::Total, "a dead mirror with no Hit is a total failure");
    expect(dead_fetch.detail.find("/etc/apt/apt.conf.d") != std::string::npos,
           "a dead mirror names the apt proxy config");
    expect(dead_fetch.detail.find("http://deb.example/InRelease") != std::string::npos,
           "a dead mirror keeps apt's URL");
    expect(dead_fetch.detail.find("Connection timed out") != std::string::npos,
           "a dead mirror keeps the timeout words");
    expect(dead_fetch.detail.find("No network connection") == std::string::npos,
           "a connection timeout is not the offline sentence");
    const UpdateFetch offline_fetch =
        classify_apt_update("Err:1 http://deb.example stable InRelease\n"
                            "  Temporary failure resolving 'deb.example'\n");
    expect(offline_fetch.kind == UpdateFetchKind::Total &&
               offline_fetch.detail.find("No network connection") != std::string::npos,
           "a resolve failure with nothing fetched adds the offline sentence");
    expect(offline_fetch.detail.find("deb.example") != std::string::npos,
           "a resolve failure still shows the hostname");
    const UpdateFetch refused =
        classify_apt_update("Err:1 http://127.0.0.1:9/debian stable InRelease\n"
                            "  Could not connect to 127.0.0.1:9 (127.0.0.1). - connect (111: "
                            "Connection refused)\n");
    expect(refused.kind == UpdateFetchKind::Total, "connection refused with nothing fetched is total");
    expect(refused.detail.find("Connection refused") != std::string::npos,
           "connection refused keeps apt's words");
    expect(refused.detail.find("127.0.0.1:9") != std::string::npos, "connection refused keeps the URL");
    expect(refused.detail.find("/etc/apt/apt.conf.d") != std::string::npos,
           "connection refused adds the proxy hint");
    const UpdateFetch release =
        classify_apt_update("E: The repository 'http://deb.example stable' does not have a Release "
                            "file.\n");
    expect(release.kind == UpdateFetchKind::Total, "a missing Release file is a total failure");
    expect(release.detail.find("does not have a Release file") != std::string::npos,
           "a missing Release file keeps apt's E: line");
    expect(release.detail.find("could not be refreshed") == std::string::npos,
           "a missing Release file is not the generic sentence");
    expect(release.detail.find("/etc/apt/apt.conf.d") == std::string::npos,
           "a missing Release file is not a proxy hint");
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
    expect(got.find("E: Connection timed out") != std::string::npos,
           "the timeout line is kept");
    expect(got.find("/etc/apt/apt.conf.d") != std::string::npos,
           "the timeout line adds the proxy sentence once");
    expect(got.find("No network connection") == std::string::npos,
           "that timeout line is not the offline sentence");
    const std::string skipped =
        friendly_job_error("E: Failed to fetch http://deb.example/InRelease  Connection timed out\n"
                           "E: Some index files failed to download. They have been ignored, or old "
                           "ones used instead.",
                           JobKind::Check);
    expect(skipped.find("Connection timed out") != std::string::npos,
           "a skipped index does not rewrite the timed-out mirror");
    expect(skipped.find("No network connection") == std::string::npos,
           "a skipped index is not called offline");
    expect(skipped.find("Some index files failed") != std::string::npos,
           "a skipped index keeps apt's own summary");
    expect(skipped.find("/etc/apt/apt.conf.d") == std::string::npos,
           "a skipped index does not add the proxy sentence");
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

  {
    const char* text =
        "The following upgrades have been deferred due to phasing:\n"
        "  shim-signed grub-efi-amd64-signed\n"
        "Some packages may have been kept back due to phasing.\n"
        "0 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.status == SimulateResult::KeptBack, "phasing is not up to date");
    expect(r.phased.size() == 2, "phasing names both packages");
    expect(r.kept_back.empty(), "phasing is not the extra-packages list");
    if (r.phased.size() >= 2) {
      expect(r.phased[0] == "shim-signed", "phased pkg0");
      expect(r.phased[1] == "grub-efi-amd64-signed", "phased pkg1");
    }
    const std::string proto = format_protocol(r);
    expect(proto.find("PHASED shim-signed\n") != std::string::npos, "PHASED protocol line");
    expect(proto.find("KEPT ") == std::string::npos, "phasing does not emit KEPT");
    const SimulateResult p = parse_protocol(proto);
    expect(p.phased.size() == 2 && p.kept_back.empty(), "PHASED round trip");
  }

  {
    const char* text =
        "The following held packages will be changed:\n"
        "  held-pkg\n"
        "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.held.empty() && r.kept_back.empty(),
           "dist-upgrade's held-changed header is not a hold we left behind");
    expect(r.status == SimulateResult::UpToDate, "held-changed with nothing else is up to date");
  }

  {
    SimulateResult r;
    r.kept_back = {"linux-image-amd64", "libc6", "libfoo:i386"};
    const char* status =
        "Package: linux-image-amd64\n"
        "Status: hold ok installed\n"
        "Architecture: amd64\n"
        "\n"
        "Package: libc6\n"
        "Status: install ok installed\n"
        "Architecture: amd64\n"
        "\n"
        "Package: libfoo\n"
        "Status: hold ok installed\n"
        "Architecture: i386\n"
        "\n";
    apply_held_packages(r, status);
    expect(r.held.size() == 2, "hold status moves those names out of kept-back");
    expect(r.kept_back.size() == 1 && r.kept_back[0] == "libc6", "a normal package stays kept back");
    bool saw_image = false;
    bool saw_libfoo = false;
    for (const auto& name : r.held) {
      if (name == "linux-image-amd64")
        saw_image = true;
      if (name == "libfoo:i386")
        saw_libfoo = true;
    }
    expect(saw_image && saw_libfoo, "hold matches the package and the name:arch form");
  }

  {
    SimulateResult r;
    r.status = SimulateResult::Success;
    r.reboot_required = true;
    r.reboot_pkgs.push_back("linux-image-amd64");
    r.summary_missing = true;
    const std::string proto = format_protocol(r);
    expect(proto.find("REBOOT linux-image-amd64\n") != std::string::npos, "REBOOT protocol line");
    expect(proto.find("SUMMARY_MISSING\n") != std::string::npos, "SUMMARY_MISSING protocol line");
    const SimulateResult p = parse_protocol(proto);
    expect(p.reboot_required && p.reboot_pkgs.size() == 1 && p.reboot_pkgs[0] == "linux-image-amd64",
           "REBOOT round trip");
    expect(p.summary_missing, "SUMMARY_MISSING round trip");
  }

  {
    SimulateResult r;
    r.status = SimulateResult::Success;
    r.reboot_already = true;
    r.reboot_pending_pkgs.push_back("linux-image-amd64");
    r.kept_removals.push_back(SimulateResult::KeptRemoval{"foo", {"oldplug"}});
    const std::string proto = format_protocol(r);
    expect(proto.find("REBOOT_ALREADY linux-image-amd64\n") != std::string::npos,
           "REBOOT_ALREADY protocol line");
    expect(proto.find("\nREBOOT ") == std::string::npos, "an old flag is not this install's REBOOT");
    expect(proto.find("REMOVE foo oldplug\n") != std::string::npos, "REMOVE protocol line");
    const SimulateResult p = parse_protocol(proto);
    expect(p.reboot_already && !p.reboot_required, "REBOOT_ALREADY round trip");
    expect(p.reboot_pending_pkgs.size() == 1 && p.reboot_pending_pkgs[0] == "linux-image-amd64",
           "REBOOT_ALREADY names the old package");
    expect(p.kept_removals.size() == 1 && p.kept_removals[0].package == "foo", "REMOVE package");
    expect(p.kept_removals[0].removes.size() == 1 && p.kept_removals[0].removes[0] == "oldplug",
           "REMOVE names the package that would be removed");
    std::vector<std::string> removed;
    std::vector<std::string> newly;
    parse_removal_plan("The following packages will be REMOVED:\n"
                       "  oldplug\n"
                       "The following NEW packages will be installed:\n"
                       "  extralib\n",
                       removed, newly);
    expect(removed.size() == 1 && removed[0] == "oldplug", "removal plan names the removed package");
    expect(newly.size() == 1 && newly[0] == "extralib", "removal plan names a new dependency");
    const InstallPlan down = parse_install_plan(
        "The following packages will be DOWNGRADED:\n"
        "  libc6\n"
        "Inst lcos-base [1.0] (2.0 Debian:12 [amd64])\n"
        "1 upgraded, 0 newly installed, 1 downgraded, 0 to remove and 0 not upgraded.\n");
    expect(down.downgraded.size() == 1 && down.downgraded[0] == "libc6",
           "a downgrade section names the package");
    expect(down.downgrade_count == 1 && down.remove_count == 0, "the summary counts a downgrade");
    expect(down.inst.size() == 1 && down.inst[0].name == "lcos-base", "Inst lines stay on the plan");
    const InstallPlan added = parse_install_plan(
        "The following NEW packages will be installed:\n"
        "  eject\n"
        "Inst eject (2.38.2-5 Debian:12 [amd64])\n"
        "0 upgraded, 1 newly installed, 0 to remove and 0 not upgraded.\n");
    expect(added.newly.size() == 1 && added.newly[0] == "eject", "a new package is named");
    expect(added.removed.empty() && added.remove_count == 0 && added.downgrade_count == -1,
           "an add-only plan has no removal and no downgrade");
    expect(added.inst.size() == 1 && added.inst[0].old_version == "-" &&
               added.inst[0].new_version == "2.38.2-5",
           "a new Inst line has no old version");
  }

  {
    const char* restart = "Restart to finish installing updates.";
    SimulateResult clean;
    clean.status = SimulateResult::Success;
    clean.upgraded_count = 1;
    const JobOutcome empty = outcome_install(clean, 0, false);
    expect(empty.status == "Updates installed. You're up to date.",
           "a finished install with nothing left says updates installed and up to date");
    expect(empty.check_enabled && !empty.install_enabled, "a finished install enables Check only");
    expect(count_phrase(empty.status, restart) == 0, "a clean install has no restart sentence");

    SimulateResult reboot = clean;
    reboot.reboot_required = true;
    reboot.reboot_pkgs.push_back("linux-image-amd64");
    const JobOutcome once = outcome_install(reboot, 0, false);
    expect(once.status.find("Updates installed.") == 0, "a restart still starts with Updates installed");
    expect(count_phrase(once.status, restart) == 1, "the restart sentence is printed once");
    expect(once.status.find("linux-image-amd64") != std::string::npos, "the restart names the package");
    expect(once.status.find("You're up to date.") == std::string::npos,
           "a restart is not the up-to-date sentence");

    SimulateResult phased = clean;
    phased.phased.push_back("shim-signed");
    const JobOutcome waiting = outcome_install(phased, 0, false);
    expect(waiting.status.find("Updates installed.") == 0, "phased leftovers start with Updates installed");
    expect(waiting.status.find("will be offered later: shim-signed.") != std::string::npos,
           "phased leftovers use the Check sentence");
    expect(waiting.status.find("You're up to date.") == std::string::npos,
           "phased leftovers are not up to date");

    SimulateResult held = clean;
    held.held.push_back("vim");
    const JobOutcome hold = outcome_install(held, 0, false);
    expect(hold.status.find("These packages are held: vim.") != std::string::npos,
           "held leftovers use the Check sentence");
    expect(hold.status.find("You're up to date.") == std::string::npos, "holds are not up to date");

    SimulateResult extra = clean;
    extra.kept_back.push_back("foo");
    const JobOutcome needs = outcome_install(extra, 0, false);
    expect(needs.status.find("need extra packages: foo.") != std::string::npos,
           "a new dependency stays the extra-packages sentence");

    SimulateResult removal = clean;
    removal.kept_removals.push_back(SimulateResult::KeptRemoval{"foo", {"oldplug"}});
    const JobOutcome breaks = outcome_install(removal, 0, false);
    expect(breaks.status.find("would remove packages: foo (would remove oldplug).") != std::string::npos,
           "a Breaks keep-back names the package that would be removed");
    expect(breaks.status.find("need extra packages") == std::string::npos,
           "a removal is not described as extra packages");

    SimulateResult conf = clean;
    conf.conffiles_kept.push_back("/etc/ssh/sshd_config");
    const JobOutcome kept_conf = outcome_install(conf, 0, false);
    expect(count_phrase(kept_conf.status, "Existing configuration was kept") == 1,
           "a kept conffile is named once");
    expect(kept_conf.status.find("Updates installed.") == 0, "a conffile success starts with Updates installed");

    SimulateResult pending = clean;
    pending.reboot_already = true;
    pending.reboot_pending_pkgs.push_back("linux-image-amd64");
    const JobOutcome old_flag = outcome_install(pending, 0, false);
    expect(old_flag.status.find("Updates installed.") == 0, "an old reboot flag still says Updates installed");
    expect(old_flag.status.find("A restart was already pending. (needed by linux-image-amd64).") !=
               std::string::npos,
           "an old reboot flag is a separate sentence");
    expect(old_flag.status.find(restart) == std::string::npos,
           "an old reboot flag does not use the restart sentence");

    SimulateResult grew = pending;
    grew.reboot_required = true;
    grew.reboot_pkgs.push_back("linux-image-amd64");
    grew.reboot_pending_pkgs = {"bash"};
    const JobOutcome both = outcome_install(grew, 0, false);
    expect(count_phrase(both.status, restart) == 1, "a grown reboot flag restarts once");
    expect(both.status.find("A restart was already pending. (needed by bash).") != std::string::npos,
           "packages already pending stay on the old-flag sentence");

    SimulateResult missing = clean;
    missing.summary_missing = true;
    const JobOutcome no_summary = outcome_install(missing, 0, false);
    expect(no_summary.status.find("did not report whether any packages were kept back") !=
               std::string::npos,
           "a missing summary is named");
    expect(no_summary.status.find("You're up to date.") == std::string::npos,
           "a missing summary is not up to date");

    SimulateResult failed;
    failed.status = SimulateResult::Error;
    failed.error_msg = "E: Sub-process /usr/bin/dpkg returned an error code (1)";
    const JobOutcome err = outcome_install(failed, 100, true);
    expect(err.check_enabled && err.install_enabled, "a failed install leaves Check and Install");
    expect(err.packages == PackageListAction::Keep, "a failed install keeps the package list");
    expect(err.status.find("returned an error code") != std::string::npos, "a failure keeps the apt line");

    const JobOutcome check_timeout = outcome_timeout(false, true, false);
    expect(check_timeout.status.find("Timed out waiting for the update check") != std::string::npos,
           "a check timeout uses the check sentence");
    expect(check_timeout.check_enabled, "a timeout enables Check");
    const JobOutcome install_timeout = outcome_timeout(true, true, true);
    expect(install_timeout.status == "Timed out while installing updates.",
           "an install timeout uses the install sentence");
    expect(install_timeout.check_enabled && install_timeout.install_enabled,
           "an install timeout enables Check and Install");
    const JobOutcome auth_timeout = outcome_timeout(true, false, false);
    expect(auth_timeout.status.find("Timed out waiting for authentication") != std::string::npos,
           "an authentication timeout says so");

    SimulateResult check_phased;
    check_phased.status = SimulateResult::KeptBack;
    check_phased.phased.push_back("shim-signed");
    const JobOutcome check_only = outcome_check(check_phased, 0, false);
    expect(check_only.status == "These updates are waiting and will be offered later: shim-signed.",
           "Check names a phased update without the kept-back headline");
    expect(check_only.check_enabled && !check_only.install_enabled, "a phased-only check has no Install");
  }

  {
    CaptureBuf cap;
    const std::string head =
        "The following packages have been kept back:\n"
        "  linux-image-amd64\n"
        "0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.\n"
        "Configuration file '/etc/ssh/sshd_config'\n"
        " ==> Keeping old config file as default.\n";
    capture_append(cap, head.data(), head.size());
    std::string junk(1300 * 1024, 'x');
    for (std::size_t i = 80; i < junk.size(); i += 80)
      junk[i] = '\n';
    capture_append(cap, junk.data(), junk.size());
    const std::string text = capture_text(cap);
    expect(text.find("The following packages have been kept back:") != std::string::npos,
           "cap keeps the kept-back header past 1 MiB");
    expect(text.find("linux-image-amd64") != std::string::npos, "cap keeps the kept-back name");
    expect(text.find("0 upgraded, 0 newly installed, 0 to remove and 1 not upgraded.") !=
               std::string::npos,
           "cap keeps the summary line past 1 MiB");
    expect(text.find("/etc/ssh/sshd_config") != std::string::npos, "cap keeps a conffile note");
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.kept_back.size() == 1 && r.not_upgraded_count == 1, "a long log still parses kept-back");
    expect(!r.conffiles_kept.empty() && r.conffiles_kept[0] == "/etc/ssh/sshd_config",
           "a long log still parses the conffile");
  }

  {
    /* Real apt 2.8.3 output with -o quiet=0. Percent lines use a carriage
     * return. Every parser path has to keep working on that text. */
    const std::string kept = read_file(dir + "/apt-quiet0-kept.bin");
    const SimulateResult r = parse_apt_simulate(kept);
    expect(r.status == SimulateResult::KeptBack, "quiet=0 kept-back is not up to date");
    expect(r.packages.empty(), "quiet=0 percent lines are not Inst packages");
    expect(r.kept_back.size() == 2, "quiet=0 kept-back names both packages");
    expect(r.not_upgraded_count == 2, "quiet=0 kept-back summary count");
    if (r.kept_back.size() >= 2) {
      expect(r.kept_back[0] == "heldpkg", "quiet=0 kept-back pkg0");
      expect(r.kept_back[1] == "lcos-fixture-pkg", "quiet=0 kept-back pkg1");
    }
    bool percent_name = false;
    for (const auto& name : r.kept_back) {
      if (name.find('%') != std::string::npos)
        percent_name = true;
    }
    expect(!percent_name, "quiet=0 percent lines are not package names");
    expect(kept.find('\r') != std::string::npos, "quiet=0 kept-back fixture has carriage returns");

    SimulateResult held = r;
    apply_held_packages(held, "Package: heldpkg\nStatus: hold ok installed\nArchitecture: all\n");
    expect(held.held.size() == 1 && held.held[0] == "heldpkg",
           "quiet=0 kept-back still moves a held package");
    expect(held.kept_back.size() == 1 && held.kept_back[0] == "lcos-fixture-pkg",
           "quiet=0 a package that is not held stays kept back");

    const std::string::size_type plan = kept.find("The following packages have been kept back:");
    expect(plan != std::string::npos, "quiet=0 kept-back fixture has the apt heading");
    const std::string phased_text =
        kept.substr(0, plan) +
        "The following upgrades have been deferred due to phasing:\n"
        "  shim-signed grub-efi-amd64-signed\n"
        "0 upgraded, 0 newly installed, 0 to remove and 2 not upgraded.\n";
    const SimulateResult phased = parse_apt_simulate(phased_text);
    expect(phased.phased.size() == 2 && phased.phased[0] == "shim-signed" &&
               phased.phased[1] == "grub-efi-amd64-signed",
           "quiet=0 percent lines do not disturb a phased list");
    expect(phased.kept_back.empty() && phased.packages.empty(),
           "quiet=0 phasing is not extra packages or Inst lines");
  }

  {
    const std::string text = read_file(dir + "/apt-quiet0-removal.bin");
    std::vector<std::string> removed;
    std::vector<std::string> newly;
    parse_removal_plan(text, removed, newly);
    expect(removed.size() == 1 && removed[0] == "oldplug",
           "quiet=0 removal plan names the removed package");
    expect(newly.size() == 1 && newly[0] == "newplug",
           "quiet=0 removal plan names the new package");
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.packages.size() == 1 && r.packages[0].name == "newplug",
           "quiet=0 Inst line survives the percent lines");
    expect(r.upgraded_count == 0 && r.not_upgraded_count == 2,
           "quiet=0 removal summary counts");
    expect(!r.reboot_required, "quiet=0 removal text does not invent a reboot");
  }

  {
    const std::string text = read_file(dir + "/apt-quiet0-print-uris.bin");
    expect(parse_download_need(text) == "Need to get 0 B/478 B of archives.",
           "quiet=0 print-uris keeps apt's download size");
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.upgraded_count == 1, "quiet=0 print-uris still has the summary");
    expect(r.status != SimulateResult::Error, "quiet=0 print-uris is not an error");
    SimulateResult shown;
    shown.status = SimulateResult::Upgrades;
    PackageUpgrade pkg;
    pkg.name = "lcos-fixture-pkg";
    pkg.old_version = "1.0";
    pkg.new_version = "2.0";
    shown.packages.push_back(pkg);
    shown.download_need = parse_download_need(text);
    apply_download_details(shown, text);
    const JobOutcome outcome = outcome_check(shown, 0, true);
    expect(outcome.status.find("1 update.") != std::string::npos,
           "a sized check says how many updates there are");
    expect(outcome.status.find("The packages are already downloaded (478 B).") != std::string::npos,
           "a cached download is not shown as 0 B");
    expect(outcome.status.find("Need to get 0 B/") == std::string::npos,
           "the raw 0 B/N line is not the status");
    expect(outcome.status.find("After this operation, 0 B of additional disk space will be used.") !=
               std::string::npos,
           "the disk-space sentence is kept");
    expect(shown.packages[0].size == "478 B", "print-uris size is attached to the package");
    const std::string proto = format_protocol(shown);
    expect(proto.find("NEED Need to get 0 B/478 B of archives.\n") != std::string::npos,
           "NEED protocol line");
    const SimulateResult back = parse_protocol(proto);
    expect(back.download_need == "Need to get 0 B/478 B of archives.", "NEED round trip");
  }

  {
    const std::string text = read_file(dir + "/apt-quiet0-update.bin");
    const UpdateFetch fetch = classify_apt_update(text);
    expect(fetch.kind == UpdateFetchKind::Partial, "quiet=0 update with a Get and an Err is partial");
    expect(fetch.detail.find("Err:1 ") != std::string::npos, "quiet=0 update keeps the Err line");
    expect(fetch.detail.find("NO_PUBKEY 871920D1991BC93C") != std::string::npos,
           "quiet=0 update keeps NO_PUBKEY");
    expect(fetch.detail.find("W: GPG error:") != std::string::npos, "quiet=0 update keeps the W: line");
    expect(fetch.detail.find("E: The repository ") != std::string::npos,
           "quiet=0 update keeps the E: line");
    const std::string warning = apt_index_warning(text);
    expect(warning.find("NO_PUBKEY 871920D1991BC93C") != std::string::npos,
           "quiet=0 partial warning keeps NO_PUBKEY");
    expect(warning.find("is not signed") != std::string::npos,
           "quiet=0 partial warning keeps the E: line");
    expect(friendly_job_error(fetch.detail, JobKind::Check).find("NO_PUBKEY 871920D1991BC93C") !=
               std::string::npos,
           "quiet=0 Err/W/E lines stay in the check message");
  }

  {
    const std::string text = read_file(dir + "/apt-quiet0-install.bin");
    const SimulateResult r = parse_apt_simulate(text);
    expect(r.status == SimulateResult::Error, "quiet=0 install failure is an error");
    expect(r.error_msg.find("E: Sub-process /usr/bin/dpkg returned an error code (1)") !=
               std::string::npos,
           "quiet=0 install keeps apt's E: line");
    expect(r.upgraded_count == 1, "quiet=0 install keeps the summary");
    expect(r.packages.empty(), "quiet=0 Setting up is not an Inst line");
    expect(!r.reboot_required, "quiet=0 install failure does not invent a reboot");
    expect(parse_download_need(text) == "Need to get 0 B/574 B of archives.",
           "quiet=0 install output still yields a download size");
    expect(friendly_job_error(r.error_msg, JobKind::Install).find("returned an error code (1)") !=
               std::string::npos,
           "quiet=0 dpkg E: line is not rewritten");
    expect(friendly_job_error("dpkg: error processing package lcos-simple (--configure):",
                              JobKind::Install)
                   .find("dpkg: error processing package lcos-simple") != std::string::npos,
           "a dpkg header stays dpkg's own line");
    const std::string crlf =
        "dpkg: error processing package lcos-simple (--configure):\r\n"
        " installed lcos-simple package post-installation script subprocess returned error exit "
        "status 1\r\n"
        "Errors were encountered while processing:\r\n"
        " lcos-simple\r\n"
        "E: Sub-process /usr/bin/dpkg returned an error code (1)\r\n";
    CaptureBuf cap;
    capture_append(cap, crlf.data(), crlf.size());
    const std::string stored = capture_text(cap);
    expect(stored.find("\r") == std::string::npos, "a CRLF capture stores line breaks as newlines");
    expect(stored.find("Errors were encountered while processing:\n lcos-simple\n") != std::string::npos,
           "CRLF does not insert a blank line before the package name");
    expect(stored.find("E: Sub-process /usr/bin/dpkg returned an error code (1)") != std::string::npos,
           "CRLF capture keeps the E: line");
  }

  {
    const std::string burst =
        "12% [Working]\r"
        "47% [1 linux-image-amd64 40 MB/80 MB 50%]\r"
        "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n";
    const SimulateResult r = parse_apt_simulate(burst);
    expect(r.status == SimulateResult::UpToDate, "carriage-return percents still parse the summary");
    expect(r.packages.empty() && r.not_upgraded_count == 0,
           "carriage-return percents are not packages");
  }

  {
    SimulateResult unseen;
    unseen.status = SimulateResult::KeptBack;
    unseen.unclassified.push_back("pkg01");
    unseen.unclassified.push_back("pkg40");
    const std::string sentence = describe_remaining(unseen);
    expect(sentence.find(
               "These updates were not included and will be offered on a later check: pkg01, pkg40.") !=
               std::string::npos,
           "unclassified kept-back packages are left for a later check");
    expect(sentence.find("not classified") == std::string::npos,
           "unclassified packages are not described as classified");
    expect(sentence.find("need extra packages") == std::string::npos,
           "unclassified packages are not extra packages");
    const JobOutcome outcome = outcome_check(unseen, 0, false);
    expect(outcome.status.find(
               "These updates were not included and will be offered on a later check: pkg01, pkg40.") !=
               std::string::npos,
           "a check says those updates will be offered later");
    expect(!outcome.install_enabled, "unclassified kept-back packages are not an install");
    const std::string proto = format_protocol(unseen);
    expect(proto.find("UNCLASSIFIED pkg01\n") != std::string::npos, "UNCLASSIFIED protocol line");
    const SimulateResult back = parse_protocol(proto);
    expect(back.unclassified.size() == 2 && back.kept_back.empty(), "UNCLASSIFIED round trip");
  }

  {
    SimulateResult shown;
    shown.status = SimulateResult::Upgrades;
    shown.packages.push_back(PackageUpgrade{"bash", "1", "2", "", "", false});
    shown.packages.push_back(PackageUpgrade{"lcos-base", "1.0", "2.0", "", "", false});
    shown.new_packages.push_back(PackageUpgrade{"gvfs-backends", "-", "1.54.2-1", "", "", false});
    shown.new_packages.push_back(PackageUpgrade{"eject", "-", "2.38.2-5", "", "", false});
    const std::string proto = format_protocol(shown);
    expect(proto.find("NEW eject 2.38.2-5\n") != std::string::npos, "NEW protocol line");
    expect(proto.find("PKG lcos-base 1.0 2.0\n") != std::string::npos,
           "the kept-back upgrade stays a PKG line");
    const SimulateResult parsed = parse_protocol(proto);
    expect(parsed.new_packages.size() == 2, "NEW packages round trip");
    expect(parsed.packages.size() == 2, "NEW packages are not upgrade rows");
    const JobOutcome outcome = outcome_check(shown, 0, false);
    expect(outcome.install_enabled, "an add-only kept-back upgrade can be installed");
    expect(outcome.status.find("2 updates.") != std::string::npos, "new packages are not upgrade rows");
    expect(outcome.status.find("New packages will also be installed: eject, gvfs-backends.") !=
               std::string::npos,
           "the check names the new packages before install");
    expect(outcome.packages == PackageListAction::Show, "the review list stays available");
  }

  {
    expect(!progress_line_visible(""), "an empty progress segment is dropped");
    expect(!progress_line_visible(" "), "a single space is apt's clear-line");
    expect(!progress_line_visible("   \t  "), "whitespace-only progress is dropped");
    expect(!progress_line_visible("\r"), "a bare carriage return is dropped");
    expect(!progress_line_visible("\n"), "a blank newline is dropped");
    expect(progress_line_visible("28% [1 xz-utils 80.0 kB/267 kB 30%]"),
           "a real percent line is kept");
    expect(progress_percent("28% [1 xz-utils 80.0 kB/267 kB 30%]") == 28,
           "the leading percent is the download percent");
    expect(progress_percent("Reading package lists... 59%") == 59,
           "a trailing percent on Reading package lists is read");
    expect(progress_percent("Calculating upgrade... 50%") == 50,
           "a trailing percent on Calculating upgrade is read");
    expect(progress_percent("Hit:1 http://deb.example stable InRelease") == -1,
           "a hit line has no percent");
    const std::string burst =
        "28% [1 xz-utils 80.0 kB/267 kB 30%]\r"
        "                                        \r"
        "\r"
        "Inst bash [5.1] (5.2 Debian:12/stable [amd64])\n"
        "Inst openssl [1] (2 Debian-Security:12/stable-security [amd64])\n"
        "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n"
        "Need to get 387 MB of archives.\n"
        "After this operation, 37.5 MB of additional disk space will be used.\n";
    const SimulateResult parsed = parse_apt_simulate(burst);
    expect(parsed.packages.size() == 2, "whitespace clear-lines are not packages");
    expect(parsed.packages[0].name == "bash" && !parsed.packages[0].security,
           "a stable archive is not a security update");
    expect(parsed.packages[1].name == "openssl" && parsed.packages[1].security,
           "a Debian-Security archive is a security update");
    expect(parsed.packages[1].archive.find("stable-security") != std::string::npos,
           "the security suite is kept from the Inst line");
    SimulateResult shown = parsed;
    shown.status = SimulateResult::Upgrades;
    apply_download_details(shown, burst);
    const JobOutcome outcome = outcome_check(shown, 0, true);
    expect(outcome.status.find("2 updates.") != std::string::npos, "the count matches the Inst rows");
    expect(outcome.status.find("1 of these is a security update.") != std::string::npos,
           "security updates are counted");
    expect(outcome.status.find("Need to get 387 MB of archives.") != std::string::npos,
           "a real download size is kept");
    expect(outcome.status.find("After this operation, 37.5 MB of additional disk space will be used.") !=
               std::string::npos,
           "a non-zero disk sentence is kept");
    const std::string noble =
        "Inst openjdk-17-jre-headless [21.0.11] (21.0.12.1+1-1~24.04.4 Ubuntu:24.04/noble-security [amd64])\n";
    PackageUpgrade noble_pkg;
    expect(parse_inst_line(noble, noble_pkg) && noble_pkg.security,
           "a suite ending in -security is a security update");
    const std::string release =
        "Origin: Debian-Security\nLabel: Debian-Security\nSuite: stable-security\nCodename: bookworm-security\n";
    expect(release_text_is_security(release), "a Debian-Security Release file is security");
    expect(!release_text_is_security("Origin: Debian\nLabel: Debian\nSuite: stable\nCodename: bookworm\n"),
           "a stable Release file is not security");
    expect(release_text_is_security("Origin: Ubuntu\nLabel: Ubuntu\nSuite: noble-security\nCodename: noble\n"),
           "a suite ending in -security marks the Release file");
    expect(release_text_is_security("Origin: Example\nLabel: Example-Security\nSuite: updates\n"),
           "a label ending in -security marks the Release file");
    std::vector<std::string> ids;
    collect_security_ids(release, ids);
    expect(!ids.empty(), "security ids are collected from the Release file");
    PackageUpgrade plain;
    plain.name = "libssl3";
    plain.archive = "Debian-Security:12/stable-security";
    std::vector<PackageUpgrade> rows = {plain};
    apply_security_ids(rows, ids);
    expect(rows[0].security, "a package is marked from the Release ids");
    SimulateResult reboot;
    reboot.status = SimulateResult::Success;
    reboot.reboot_required = true;
    reboot.reboot_pkgs.push_back("linux-image-amd64");
    const JobOutcome installed = outcome_install(reboot, 0, false);
    expect(installed.offer_restart, "a reboot flag offers Restart");
    expect(installed.status.find("Restart to finish installing updates. (needed by linux-image-amd64).") !=
               std::string::npos,
           "a reboot package is named as needed by that package");
    expect(!outcome_install(SimulateResult{}, 0, false).offer_restart,
           "an install without a reboot flag does not offer Restart");
    const std::string proto = format_protocol(shown);
    expect(proto.find("SEC openssl\n") != std::string::npos, "SEC protocol line");
    expect(proto.find("DISK After this operation, 37.5 MB of additional disk space will be used.\n") !=
               std::string::npos,
           "DISK protocol line");
    const SimulateResult back = parse_protocol(proto);
    expect(back.packages.size() == 2 && back.packages[1].security && !back.packages[0].security,
           "SEC round trip marks only the security package");
    expect(back.disk_use.find("37.5 MB") != std::string::npos, "DISK round trip");
    expect(back.download_need.find("387 MB") != std::string::npos, "NEED round trip keeps apt's line");
  }

  {
    const std::string human = read_file(dir + "/apt-progress-download.txt");
    expect(human.find("[Working]") != std::string::npos, "download fixture records [Working]");
    expect(human.find("[Connecting to") != std::string::npos, "download fixture records Connecting");
    expect(human.find("[Waiting for headers]") != std::string::npos, "download fixture records Waiting");
    expect(human.find("[1 bash") != std::string::npos && human.find("[2 xz-utils") != std::string::npos,
           "download fixture records two files");
    expect(human.find("Err:3 ") != std::string::npos, "download fixture records an error");
    std::string shown;
    int percent = -1;
    std::string rest = human;
    std::string forbidden;
    auto consider = [&](const std::string& line) {
      const AptProgressNote note = apt_progress_note(line, shown);
      if (note.replace_shown)
        shown = note.shown;
      if (note.have_percent)
        percent = note.percent;
      const std::string text = friendly_progress_text(shown, "download");
      const char* bad[] = {"Downloading Working", "Downloading Connecting", "Downloading Waiting",
                           "Downloading Hit",     "Downloading Get",        "Downloading Ign",
                           "Downloading Err",     "Downloading Retrieving", "Downloading dpkg-exec",
                           "Downloading 404",     "Downloading Not"};
      for (const char* needle : bad) {
        if (text.find(needle) != std::string::npos)
          forbidden = needle;
      }
      const std::string named = progress_package_name(line);
      if (named == "working" || named == "Working" || named == "Connecting" || named == "Waiting" ||
          named == "Hit" || named == "Get" || named == "Ign" || named == "Err" || named == "Retrieving" ||
          named == "dpkg-exec")
        forbidden = named;
    };
    while (!rest.empty()) {
      const std::string::size_type cut = rest.find_first_of("\r\n");
      std::string line = cut == std::string::npos ? rest : rest.substr(0, cut);
      rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
      consider(line);
    }
    expect(forbidden.empty(), "a placeholder or error token is not a package name");
    expect(shown.find("xz-utils") != std::string::npos, "the last real download name is kept");
    expect(friendly_progress_text(shown, "download").find("Downloading xz-utils") != std::string::npos,
           "the status still names xz-utils after Hit, Ign, and Err");
    expect(percent == 40, "the last human percent is kept");
    expect(progress_package_name("17% [Working]").empty(), "[Working] has no package");
    expect(progress_package_name("0% [Connecting to deb.debian.org (151.101.2.132)]").empty(),
           "[Connecting] has no package");
    expect(progress_package_name("0% [Waiting for headers]").empty(), "[Waiting] has no package");
    expect(progress_package_name("40% [2 xz-utils 10.0 kB/80.0 kB 12%]") == "xz-utils",
           "a numbered bracket names the package");
    expect(progress_package_name(
               "Get:1 http://deb.example/pool/bash_5.2.15-2%2bb7_amd64.deb [1 kB]") == "bash",
           "Get: names the package in the deb filename");
    expect(friendly_progress_text("17% [Working]", "download") == "Downloading updates…",
           "a lone Working line is not a package");
    expect(friendly_progress_text("Hit:1 http://deb.example stable InRelease", "refresh")
                   .find("Hit:1") != std::string::npos,
           "a refresh still shows the Hit line");

    const std::string machine = read_file(dir + "/apt-progress-status-fd.txt");
    expect(machine.find("dlstatus:") != std::string::npos && machine.find("pmstatus:") != std::string::npos &&
               machine.find("pmerror:") != std::string::npos,
           "status-fd fixture has dlstatus, pmstatus, and pmerror");
    shown.clear();
    percent = -1;
    forbidden.clear();
    rest = machine;
    bool saw_file = false;
    bool saw_bash = false;
    while (!rest.empty()) {
      const std::string::size_type cut = rest.find('\n');
      std::string line = cut == std::string::npos ? rest : rest.substr(0, cut);
      rest = cut == std::string::npos ? std::string() : rest.substr(cut + 1);
      consider(line);
      const std::string text = friendly_progress_text(shown, "download");
      if (text.find("Downloading file 1 of 2") != std::string::npos)
        saw_file = true;
      if (text.find("Configuring bash") != std::string::npos)
        saw_bash = true;
    }
    expect(forbidden.empty(), "status-fd tokens are not package names");
    expect(saw_file, "dlstatus says which file is being retrieved");
    expect(saw_bash, "pmstatus names the package being configured");
    expect(progress_percent("dlstatus:1:17.5000:Retrieving file 1 of 2") == 18,
           "a status-fd percent rounds to the bar");
    expect(progress_percent("dlstatus:2:60.2500:Retrieving file 2 of 2 (1s remaining)") == 60,
           "a status-fd percent keeps the whole number");
    expect(progress_package_name("pmstatus:dpkg-exec:0.0000:Running dpkg").empty(),
           "dpkg-exec is not a package");
    expect(progress_package_name("pmerror:bash:80.0000:subprocess returned error exit status 1").empty(),
           "pmerror is not a download name");
    expect(friendly_progress_text(shown, "download").find("Configuring bash") != std::string::npos,
           "an error line does not replace the package being configured");

    const std::string uris = read_file(dir + "/apt-print-uris-epoch.txt");
    expect(uris.find("%3a") != std::string::npos, "epoch fixture encodes the colon");
    expect(deb_versions_match("1:1.17-4", "1%3a1.17-4"), "an epoch matches its filename encoding");
    expect(deb_versions_match("8:6.9.12.98+dfsg1-5.2", "8%3a6.9.12.98%2bdfsg1-5.2"),
           "a plus in an epoch version is decoded");
    expect(!deb_versions_match("1:1.17-4", "1.17-4"), "a missing epoch is a different version");
    expect(canonical_deb_version("4%3a14.2.0-1") == "4:14.2.0-1", "canonical version has the colon");
    SimulateResult epoch;
    epoch.status = SimulateResult::Upgrades;
    PackageUpgrade automake;
    automake.name = "automake";
    automake.old_version = "1:1.16.5-1.3";
    automake.new_version = "1:1.17-4";
    PackageUpgrade gcc;
    gcc.name = "gcc";
    gcc.old_version = "4:12.2.0-3";
    gcc.new_version = "4:14.2.0-1";
    PackageUpgrade image;
    image.name = "imagemagick";
    image.old_version = "8:6.9.11.60+dfsg-1.6";
    image.new_version = "8:6.9.12.98+dfsg1-5.2";
    PackageUpgrade plain;
    plain.name = "bash";
    plain.old_version = "5.2.15-2";
    plain.new_version = "5.2.21-2";
    epoch.packages = {automake, gcc, image, plain};
    apply_download_details(epoch, uris);
    expect(epoch.packages[0].size == "123.5 kB", "an epoch package gets a size");
    expect(epoch.packages[1].size == "2.3 MB", "gcc's epoch version matches the filename");
    expect(epoch.packages[2].size == "3.5 MB", "a percent-encoded plus still matches");
    expect(epoch.packages[3].size.empty(), "a package with no filename is left blank");
    const std::string inst =
        "Inst automake [1:1.16.5-1.3] (1:1.17-4 Debian:13/stable [all])\n"
        "Inst gcc [4:12.2.0-3] (4:14.2.0-1 Debian:13/stable [amd64])\n";
    SimulateResult parsed = parse_apt_simulate(inst);
    expect(parsed.packages.size() == 2 && parsed.packages[0].new_version == "1:1.17-4" &&
               parsed.packages[1].old_version == "4:12.2.0-3",
           "Inst lines keep epoch versions");
    apply_download_details(parsed, uris);
    expect(parsed.packages[0].size == "123.5 kB" && parsed.packages[1].size == "2.3 MB",
           "sizes attach to Inst versions that contain an epoch");
  }

  if (g_fails != 0) {
    std::cerr << g_fails << " failure(s)\n";
    return 1;
  }
  std::cout << "all parser tests passed\n";
  return 0;
}
