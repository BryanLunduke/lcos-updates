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
    expect(friendly_job_error("Connection timed out", JobKind::Check) == proxy,
           "connection timed out mentions a proxy, not the offline sentence");
    expect(friendly_job_error("E: Failed to fetch http://deb.example/InRelease Connection timed out",
                              JobKind::Install) == proxy,
           "connection timed out on a fetch line mentions a proxy");
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
    expect(friendly_job_error("E: Unable to connect to deb.example:80", JobKind::Check) == proxy,
           "unable to connect mentions a proxy");
    expect(friendly_job_error("Could not connect to 192.0.2.1:80", JobKind::Install) == proxy,
           "could not connect mentions a proxy");
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
    expect(dead_fetch.detail.find("No network connection") == std::string::npos,
           "a connection timeout is not the offline sentence");
    const UpdateFetch offline_fetch =
        classify_apt_update("Err:1 http://deb.example stable InRelease\n"
                            "  Temporary failure resolving 'deb.example'\n");
    expect(offline_fetch.kind == UpdateFetchKind::Total &&
               offline_fetch.detail.find("No network connection") != std::string::npos,
           "a resolve failure with nothing fetched is the offline sentence");
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
    expect(got.find("/etc/apt/apt.conf.d") != std::string::npos,
           "the timeout line is mapped to the proxy sentence on its own");
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

  if (g_fails != 0) {
    std::cerr << g_fails << " failure(s)\n";
    return 1;
  }
  std::cout << "all parser tests passed\n";
  return 0;
}
