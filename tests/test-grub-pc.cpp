/* grub-pc install_devices: disk detection and the helper's preseed.
 * Copyright (C) 2026 LCOS
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * argv[1] is the helper. It runs as this (non-root) user with stub tools;
 * root ignores every override used here.
 */

#include "apt-parse.hpp"
#include "grub-pc.hpp"
#include "tmpdir-cleanup.hpp"

#include <cstdio>
#include <cstdlib>
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

static void mkdir_p(const std::string& path)
{
  std::string cur;
  for (char ch : path) {
    cur.push_back(ch);
    if (ch == '/' && cur.size() > 1)
      mkdir(cur.c_str(), 0755);
  }
  mkdir(path.c_str(), 0755);
}

static int count_lines_starting(const std::string& text, const char* prefix)
{
  int n = 0;
  std::size_t i = 0;
  const std::size_t len = std::char_traits<char>::length(prefix);
  while (i < text.size()) {
    std::size_t end = text.find('\n', i);
    if (end == std::string::npos)
      end = text.size();
    if (text.compare(i, len, prefix) == 0)
      ++n;
    i = end < text.size() ? end + 1 : end;
  }
  return n;
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

static const char kStatusInstalled[] =
    "Package: grub-pc\nStatus: install ok installed\nVersion: 1.0\n\n"
    "Package: grub-pc-bin\nStatus: install ok installed\nVersion: 1.0\n";

static const char kStatusHalf[] =
    "Package: grub-pc\nStatus: install ok half-configured\nVersion: 2.0\n";

static const char kStatusOther[] =
    "Package: bash\nStatus: install ok installed\nVersion: 1.0\n";

static const char kConfigEmpty[] = "";

static const char kConfigSet[] =
    "Name: grub-pc/install_devices\nTemplate: grub-pc/install_devices\n"
    "Value: /dev/sdb\nOwners: grub-pc\nFlags: seen\n";

static const char kStubProbe[] = R"SH(#!/bin/sh
printf 'grub-probe' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
case "$LCOS_STUB_ENVFILE" in
  nodedisk)
    echo "grub-probe: error: failed" >&2
    exit 1 ;;
  lvm)
    printf '/dev/mapper/cryptroot\n'
    exit 0 ;;
  twodisk)
    printf '/dev/sda2\n/dev/sdb\n'
    exit 0 ;;
  vda)
    printf '/dev/vda2\n'
    exit 0 ;;
  *)
    printf '/dev/sda1\n'
    exit 0 ;;
esac
)SH";

static const char kStubDebconf[] = R"SH(#!/bin/sh
printf 'debconf-set-selections' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
if [ -n "$1" ] && [ -f "$1" ]; then
  printf 'SELECTIONS ' >> "$LCOS_STUB_ARGVFILE"
  cat "$1" >> "$LCOS_STUB_ARGVFILE"
fi
exit 0
)SH";

static const char kStubDpkg[] = R"SH(#!/bin/sh
printf 'dpkg' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
mode="$LCOS_STUB_STATEFILE"
n=0
if [ -f "$LCOS_STUB_COUNTFILE" ]; then
  n=$(cat "$LCOS_STUB_COUNTFILE" 2>/dev/null || echo 0)
fi
n=$((n + 1))
echo "$n" > "$LCOS_STUB_COUNTFILE"
if [ "$mode" = "failgrub" ] || { [ "$mode" = "failonce" ] && [ "$n" = "1" ]; }; then
  echo "dpkg: error processing package grub-pc (--configure):" >&2
  echo " installed grub-pc package post-installation script subprocess returned error exit status 1" >&2
  echo "Errors were encountered while processing:" >&2
  echo " grub-pc" >&2
  exit 1
fi
if [ "$mode" = "failother" ]; then
  echo "Setting up grub-pc (2.0) ..."
  echo "dpkg: error processing package foo (--configure):" >&2
  echo " installed foo package post-installation script subprocess returned error exit status 1" >&2
  echo "Errors were encountered while processing:" >&2
  echo " foo" >&2
  exit 1
fi
echo "Setting up grub-pc (2.0) ..."
exit 0
)SH";

static const char kStubApt[] = R"SH(#!/bin/sh
printf 'apt-get' >> "$LCOS_STUB_ARGVFILE"
for a in "$@"; do printf ' %s' "$a" >> "$LCOS_STUB_ARGVFILE"; done
printf '\n' >> "$LCOS_STUB_ARGVFILE"
last=
sim=0
yes=0
only=0
for a in "$@"; do
  last=$a
  [ "$a" = "-s" ] && sim=1
  [ "$a" = "-y" ] && yes=1
  [ "$a" = "--only-upgrade" ] && only=1
done
if [ "$LCOS_STUB_SCRIPT" != "upgrade" ] && [ "$LCOS_STUB_SCRIPT" != "instfail" ]; then
  echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
if [ "$last" = "update" ]; then
  echo "Hit:1 http://deb.example stable InRelease"
  exit 0
fi
if [ "$last" = "clean" ] || [ "$last" = "autoremove" ]; then
  echo "0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
if [ "$sim" = "0" ] && [ "$yes" = "1" ] && [ "$only" = "1" ]; then
  if [ "$LCOS_STUB_SCRIPT" = "instfail" ]; then
    echo "dpkg: error processing package grub-pc (--configure):" >&2
    echo " installed grub-pc package post-installation script subprocess returned error exit status 1" >&2
    echo "Errors were encountered while processing:" >&2
    echo " grub-pc" >&2
    exit 1
  fi
  echo "Setting up grub-pc (2.0) ..."
  echo "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
  exit 0
fi
echo "Inst grub-pc [1.0] (2.0 Devuan:6.0/stable [amd64])"
echo "1 upgraded, 0 newly installed, 0 to remove and 0 not upgraded."
exit 0
)SH";

static void test_detection()
{
  expect(partition_parent_name("sda1") == "sda" && partition_parent_name("sda") == "" &&
             partition_parent_name("sda12") == "sda",
         "sd partition reduces to the whole disk");
  expect(partition_parent_name("vda2") == "vda" && partition_parent_name("vda") == "" &&
             partition_parent_name("hda1") == "hda" && partition_parent_name("xvda3") == "xvda",
         "vd, hd and xvd partitions reduce to the whole disk");
  expect(partition_parent_name("nvme0n1p2") == "nvme0n1" && partition_parent_name("nvme0n1") == "" &&
             partition_parent_name("nvme10n2p12") == "nvme10n2",
         "nvme partition reduces to the namespace, not to a shorter name");
  expect(partition_parent_name("mmcblk0p1") == "mmcblk0" && is_whole_disk_name("mmcblk0") &&
             !is_whole_disk_name("mmcblk0p1") && !is_whole_disk_name("dm-0"),
         "mmcblk partition reduces to the disk; dm is not a boot disk");
  expect(is_whole_disk_name("sda") && is_whole_disk_name("vda") && is_whole_disk_name("nvme0n1") &&
             !is_whole_disk_name("sda1") && !is_whole_disk_name("nvme0n1p1"),
         "only whole disks are install targets");

  DiskTopology none;
  const auto sda = grub_boot_disk_devices("/dev/sda1\n", none);
  expect(sda.size() == 1 && sda[0] == "/dev/sda", "a partition probe becomes /dev/sda with no by-id");
  const auto both = grub_boot_disk_devices("/dev/sdb\n/dev/sda1\n", none);
  expect(both.size() == 2 && both[0] == "/dev/sdb" && both[1] == "/dev/sda",
         "several probe disks are all kept, in probe order");
  expect(grub_boot_disk_devices("/dev/vda2", none) == std::vector<std::string>{"/dev/vda"},
         "virtio partition becomes the whole virtio disk");
  expect(grub_boot_disk_devices("/dev/nvme0n1p2", none) == std::vector<std::string>{"/dev/nvme0n1"},
         "nvme partition becomes the nvme disk");

  DiskTopology id;
  id.by_id = {{"ata-DISK", "sda"}, {"wwn-0x1", "sda"}, {"ata-DISK-part1", "sda1"},
              {"dm-name-root", "dm-0"}};
  const auto preferred = grub_boot_disk_devices("/dev/sda1", id);
  expect(preferred.size() == 1 && preferred[0] == "/dev/disk/by-id/ata-DISK",
         "the stable ata by-id name is used, not wwn, the partition, or dm-name");

  DiskTopology lvm;
  lvm.alias = {{"cryptroot", "dm-1"}};
  lvm.slaves = {{"dm-1", {"dm-0"}}, {"dm-0", {"nvme0n1p3"}}};
  lvm.by_id = {{"nvme-eui.abc", "nvme0n1"},
               {"nvme-SAMSUNG_SSD", "nvme0n1"},
               {"nvme-SAMSUNG_SSD-part3", "nvme0n1p3"}};
  const auto luks = grub_boot_disk_devices("/dev/mapper/cryptroot\n", lvm);
  expect(luks.size() == 1 && luks[0] == "/dev/disk/by-id/nvme-SAMSUNG_SSD",
         "LUKS on nvme resolves to the whole disk's nvme by-id name");

  DiskTopology two;
  two.slaves = {{"dm-2", {"sdb3", "sda2"}}};
  two.by_id = {{"ata-B", "sdb"}, {"ata-A", "sda"}, {"wwn-0x2", "sdb"}};
  const auto pvs = grub_boot_disk_devices("/dev/dm-2", two);
  expect(pvs.size() == 2 && pvs[0] == "/dev/disk/by-id/ata-A" && pvs[1] == "/dev/disk/by-id/ata-B",
         "LVM on two disks uses both whole disks, ata names, slave order");
  for (const auto& dev : pvs)
    expect(dev.find("part") == std::string::npos && partition_parent_name(dev.substr(dev.find_last_of('/') + 1)).empty(),
           "resolved devices are not partitions");

  expect(grub_boot_disk_devices("/dev/dm-0", none).empty(),
         "a device-mapper node with no slaves is not used as a disk");
  expect(grub_boot_disk_devices("(hd0)\n", none).empty(), "a non-device probe line is ignored");

  const std::string line = grub_pc_debconf_line({"/dev/sda"});
  expect(line == "grub-pc grub-pc/install_devices multiselect /dev/sda",
         "one disk is the debconf-set-selections line that was tested by hand");
  const std::string multi = grub_pc_debconf_line(pvs);
  expect(multi ==
             "grub-pc grub-pc/install_devices multiselect /dev/disk/by-id/ata-A, /dev/disk/by-id/ata-B",
         "several disks are one multiselect value");
  expect(line.find("install_devices_empty") == std::string::npos &&
             multi.find("install_devices_empty") == std::string::npos && grub_pc_debconf_line({}).empty(),
         "install_devices_empty is never set");
  expect(grub_pc_debconf_line({"/dev/sda", "not-a-device"}).empty(),
         "a path outside /dev is not written");

  expect(grub_pc_package_present(kStatusInstalled) && grub_pc_package_present(kStatusHalf) &&
             !grub_pc_package_present(kStatusOther) &&
             !grub_pc_package_present("Package: grub-pc\nStatus: deinstall ok config-files\n"),
         "grub-pc counts while installed or half-configured, not as config-files, and grub-pc-bin does not");
  expect(grub_pc_install_devices_value(kConfigSet) == "/dev/sdb", "a stored disk is kept");
  expect(grub_pc_install_devices_value("Name: grub-pc/install_devices\nValue:   \n").empty() &&
             grub_pc_install_devices_value("").empty(),
         "a missing or blank install_devices is empty");

  GrubPcFacts facts;
  facts.package_present = true;
  facts.bios_core_img = false;
  facts.probe_ok = true;
  facts.probe_output = "/dev/sda";
  GrubPcDecision d = decide_grub_pc(facts);
  expect(d.action == GrubPcAction::Skip && d.log.empty() && d.devices.empty(),
         "UEFI-only (no i386 core.img) is left alone");
  facts.bios_core_img = true;
  facts.install_devices = "/dev/sdb";
  d = decide_grub_pc(facts);
  expect(d.action == GrubPcAction::Skip && d.devices.empty() && has(d.log, "left unchanged"),
         "a non-empty install_devices is not replaced");
  facts.install_devices.clear();
  facts.package_present = false;
  d = decide_grub_pc(facts);
  expect(d.action == GrubPcAction::Skip && d.log.empty(), "without grub-pc nothing is changed");
  facts.package_present = true;
  facts.probe_ok = false;
  d = decide_grub_pc(facts);
  expect(d.action == GrubPcAction::Unknown && d.devices.empty() && has(d.log, "could not determine"),
         "a failed probe does not invent a disk");
  facts.probe_ok = true;
  facts.probe_output = "/dev/sda1";
  facts.topo = id;
  d = decide_grub_pc(facts);
  expect(d.action == GrubPcAction::Set && d.devices.size() == 1 &&
             d.devices[0] == "/dev/disk/by-id/ata-DISK" && has(d.log, d.devices[0].c_str()),
         "an empty BIOS install_devices is set to the by-id disk");

  expect(grub_pc_configure_failed(
             "dpkg: error processing package grub-pc (--configure):\n"
             " installed grub-pc package post-installation script subprocess returned error exit status 1\n"
             "Errors were encountered while processing:\n grub-pc\n") &&
             !grub_pc_configure_failed("Setting up grub-pc (2.0) ...\n"
                                      "dpkg: error processing package foo (--configure):\n foo\n") &&
             !grub_pc_configure_failed("dpkg: error processing package grub-pc-bin (--configure):\n"),
         "only a failed configure of grub-pc itself counts");
  const std::string manual = grub_pc_manual_message();
  expect(has(manual, "sudo DEBIAN_FRONTEND=dialog dpkg --configure grub-pc") && has(manual, "whole disk") &&
             !error_suggests_repair(manual),
         "an unknown disk tells the user the command and does not offer Repair again");
  expect(error_suggests_repair("Click Repair to try again.") &&
             error_suggests_repair("The upgrade stopped with a package left unfinished.\n"
                                  "Click Repair to finish configuring packages."),
         "the failure text that says to click Repair still shows the Repair button");
}

struct World {
  std::string dir;
  std::string apt;
  std::string dpkg;
  std::string probe;
  std::string debconf;
  std::string status;
  std::string updates;
  std::string confdir;
  std::string core;
  std::string byid;
  std::string sys;
  std::string mapper;
  std::string lists;
  std::string count;
  std::string reboot;
};

static World make_world()
{
  char tmpl[] = "/tmp/lcos-grub-XXXXXX";
  if (mkdtemp(tmpl) == nullptr)
    return {};
  lcos_test::cleanup_at_exit(tmpl);
  World w;
  w.dir = tmpl;
  w.apt = w.dir + "/apt-get";
  w.dpkg = w.dir + "/dpkg";
  w.probe = w.dir + "/grub-probe";
  w.debconf = w.dir + "/debconf-set-selections";
  w.status = w.dir + "/status";
  w.updates = w.dir + "/updates";
  w.confdir = w.dir + "/debconf";
  w.core = w.dir + "/core.img";
  w.byid = w.dir + "/by-id";
  w.sys = w.dir + "/sys";
  w.mapper = w.dir + "/mapper";
  w.lists = w.dir + "/lists";
  w.count = w.dir + "/count";
  w.reboot = w.dir + "/no-reboot";
  mkdir(w.updates.c_str(), 0755);
  mkdir(w.confdir.c_str(), 0755);
  mkdir(w.byid.c_str(), 0755);
  mkdir(w.mapper.c_str(), 0755);
  mkdir(w.lists.c_str(), 0755);
  write_file(w.apt, kStubApt, 0755);
  write_file(w.dpkg, kStubDpkg, 0755);
  write_file(w.probe, kStubProbe, 0755);
  write_file(w.debconf, kStubDebconf, 0755);
  write_file(w.core, "core");
  write_file(w.confdir + "/config.dat", kConfigEmpty);
  write_file(w.confdir + "/templates.dat", "");
  write_file(w.confdir + "/passwords.dat", "");
  write_file(w.status, kStatusHalf);
  return w;
}

static HelperRun run_repair(const char* helper, const World& w, const std::string& log, const char* probe_mode,
                            const char* dpkg_mode)
{
  return run_helper(helper, {"repair"},
                    {{"LCOS_UPDATES_APT_GET", w.apt},
                     {"LCOS_UPDATES_DPKG", w.dpkg},
                     {"LCOS_UPDATES_DPKG_STATUS", w.status},
                     {"LCOS_UPDATES_DPKG_UPDATES_DIR", w.updates},
                     {"LCOS_UPDATES_DEBCONF_DIR", w.confdir},
                     {"LCOS_UPDATES_GRUB_PROBE", w.probe},
                     {"LCOS_UPDATES_DEBCONF_SET_SELECTIONS", w.debconf},
                     {"LCOS_UPDATES_GRUB_CORE", w.core},
                     {"LCOS_UPDATES_DISK_BY_ID", w.byid},
                     {"LCOS_UPDATES_SYS_BLOCK", w.sys},
                     {"LCOS_UPDATES_DEV_MAPPER", w.mapper},
                     {"LCOS_UPDATES_REBOOT_FILE", w.reboot},
                     {"LCOS_STUB_ARGVFILE", log},
                     {"LCOS_STUB_SCRIPT", "repair"},
                     {"LCOS_STUB_ENVFILE", probe_mode},
                     {"LCOS_STUB_STATEFILE", dpkg_mode},
                     {"LCOS_STUB_COUNTFILE", w.count}});
}

static void test_helper(const char* helper)
{
  /* BIOS /dev/sda1 with an ata by-id link. Repair sets it before configure. */
  {
    const World w = make_world();
    expect(!w.dir.empty(), "mkdtemp");
    if (w.dir.empty())
      return;
    symlink("../../sda", (w.byid + "/ata-TESTDISK").c_str());
    symlink("../../sda", (w.byid + "/wwn-0xabc").c_str());
    symlink("../../sda1", (w.byid + "/ata-TESTDISK-part1").c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "ok");
    const std::string calls = slurp(log);
    expect(r.status == 0 && parse_protocol(r.out).status == SimulateResult::Success,
           "repair succeeds after the boot disk is recorded");
    expect(has(calls, "grub-probe -t disk /boot/grub\n"),
           "repair runs grub-probe -t disk /boot/grub");
    expect(has(calls, "SELECTIONS grub-pc grub-pc/install_devices multiselect /dev/disk/by-id/ata-TESTDISK\n"),
           "repair sets install_devices to the whole disk's by-id name");
    expect(!has(calls, "install_devices_empty") && !has(calls, "part1") && !has(calls, "wwn-"),
           "the part link, the wwn link and install_devices_empty are not used");
    const auto probe = calls.find("grub-probe ");
    const auto sel = calls.find("debconf-set-selections ");
    const auto dpkg = calls.find("dpkg ");
    expect(probe != std::string::npos && sel != std::string::npos && dpkg != std::string::npos && probe < sel &&
               sel < dpkg,
           "the disk is recorded before dpkg --configure -a");
    expect(has(r.err, "NOTE grub-pc: set grub-pc/install_devices to /dev/disk/by-id/ata-TESTDISK\n"),
           "the helper logs the disk it recorded");
  }

  /* No by-id link: /dev/sda, not the partition. */
  {
    const World w = make_world();
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "ok");
    expect(r.status == 0 && has(slurp(log), "SELECTIONS grub-pc grub-pc/install_devices multiselect /dev/sda\n"),
           "without a by-id link the whole disk /dev/sda is used, not /dev/sda1");
  }

  /* Already set: do not probe, do not rewrite. */
  {
    const World w = make_world();
    write_file(w.confdir + "/config.dat", kConfigSet);
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "ok");
    const std::string calls = slurp(log);
    expect(r.status == 0 && !has(calls, "grub-probe") && !has(calls, "debconf-set-selections"),
           "a non-empty install_devices is not probed or rewritten");
    expect(has(r.err, "left unchanged"), "leaving an existing value is logged");
  }

  /* UEFI: core image absent. */
  {
    const World w = make_world();
    unlink(w.core.c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "ok");
    const std::string calls = slurp(log);
    expect(r.status == 0 && !has(calls, "grub-probe") && !has(calls, "debconf-set-selections") &&
               !has(r.err, "NOTE grub-pc"),
           "without i386 core.img nothing is changed");
  }

  /* grub-pc not installed. */
  {
    const World w = make_world();
    write_file(w.status, kStatusOther);
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "ok");
    expect(r.status == 0 && !has(slurp(log), "grub-probe"), "without grub-pc the probe is not run");
  }

  /* LUKS/LVM: mapper -> dm -> nvme partition -> whole nvme disk. */
  {
    const World w = make_world();
    symlink("../dm-0", (w.mapper + "/cryptroot").c_str());
    mkdir_p(w.sys + "/dm-0/slaves");
    write_file(w.sys + "/dm-0/slaves/nvme0n1p3", "");
    symlink("../../nvme0n1", (w.byid + "/nvme-SAMSUNG_SSD").c_str());
    symlink("../../nvme0n1", (w.byid + "/nvme-eui.abc").c_str());
    symlink("../../nvme0n1p3", (w.byid + "/nvme-SAMSUNG_SSD-part3").c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "lvm", "ok");
    expect(r.status == 0 &&
               has(slurp(log),
                   "SELECTIONS grub-pc grub-pc/install_devices multiselect /dev/disk/by-id/nvme-SAMSUNG_SSD\n"),
           "an LVM/LUKS boot disk is the underlying whole nvme disk");
  }

  /* Two disks. */
  {
    const World w = make_world();
    symlink("../../sda", (w.byid + "/ata-A").c_str());
    symlink("../../sdb", (w.byid + "/ata-B").c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "twodisk", "ok");
    expect(r.status == 0 &&
               has(slurp(log),
                   "SELECTIONS grub-pc grub-pc/install_devices multiselect "
                   "/dev/disk/by-id/ata-A, /dev/disk/by-id/ata-B\n"),
           "several probe disks are all recorded");
  }

  /* virtio, from the probe, no by-id. */
  {
    const World w = make_world();
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "vda", "ok");
    expect(r.status == 0 && has(slurp(log), "multiselect /dev/vda\n"),
           "a virtio partition becomes /dev/vda");
  }

  /* Configure fails once, then the same detection lets the retry finish. */
  {
    const World w = make_world();
    symlink("../../sda", (w.byid + "/ata-TESTDISK").c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "failonce");
    const std::string calls = slurp(log);
    expect(r.status == 0 && parse_protocol(r.out).status == SimulateResult::Success,
           "a grub-pc configure that fails once is retried and then finishes");
    expect(count_lines_starting(calls, "dpkg ") == 2, "configure is retried once, not in a loop");
    expect(has(calls, "debconf-set-selections"), "the retry still records the disk");
  }

  /* Disk cannot be determined: one configure, then the hand command. No Repair loop. */
  {
    const World w = make_world();
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "nodedisk", "failgrub");
    const SimulateResult p = parse_protocol(r.out);
    const std::string calls = slurp(log);
    expect(r.status == 1 && has(p.error_msg, "sudo DEBIAN_FRONTEND=dialog dpkg --configure grub-pc") &&
               has(p.error_msg, "whole disk") && !error_suggests_repair(p.error_msg) &&
               !has(p.error_msg, "Click Repair"),
           "an unknown disk shows the hand command instead of asking for Repair again");
    expect(count_lines_starting(calls, "dpkg ") == 1 && !has(calls, "debconf-set-selections"),
           "an unknown disk does not record a device and does not configure twice");
  }

  /* Some other package failed. Record the disk, do not treat it as the grub loop. */
  {
    const World w = make_world();
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_repair(helper, w, log, "sda", "failother");
    const SimulateResult p = parse_protocol(r.out);
    const std::string calls = slurp(log);
    expect(r.status == 1 && has(p.error_msg, "Click Repair to try again.") && error_suggests_repair(p.error_msg) &&
               !has(p.error_msg, "sudo DEBIAN_FRONTEND=dialog"),
           "a non-grub configure failure says to click Repair");
    expect(count_lines_starting(calls, "dpkg ") == 1 && has(calls, "multiselect /dev/sda\n"),
           "the disk is still recorded, and configure is not retried for a different package");
  }

  /* Install records the disk before apt-get install, and a postinst failure says to click Repair. */
  {
    const World w = make_world();
    write_file(w.status, kStatusInstalled);
    symlink("../../sda", (w.byid + "/ata-TESTDISK").c_str());
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_helper(
        helper, {"upgrade", "grub-pc=2.0"},
        {{"LCOS_UPDATES_APT_GET", w.apt},
         {"LCOS_UPDATES_DPKG", w.dpkg},
         {"LCOS_UPDATES_DPKG_STATUS", w.status},
         {"LCOS_UPDATES_DPKG_UPDATES_DIR", w.updates},
         {"LCOS_UPDATES_DEBCONF_DIR", w.confdir},
         {"LCOS_UPDATES_GRUB_PROBE", w.probe},
         {"LCOS_UPDATES_DEBCONF_SET_SELECTIONS", w.debconf},
         {"LCOS_UPDATES_GRUB_CORE", w.core},
         {"LCOS_UPDATES_DISK_BY_ID", w.byid},
         {"LCOS_UPDATES_SYS_BLOCK", w.sys},
         {"LCOS_UPDATES_DEV_MAPPER", w.mapper},
         {"LCOS_UPDATES_LISTS_DIR", w.lists},
         {"LCOS_UPDATES_REBOOT_FILE", w.reboot},
         {"LCOS_STUB_ARGVFILE", log},
         {"LCOS_STUB_SCRIPT", "upgrade"},
         {"LCOS_STUB_ENVFILE", "sda"},
         {"LCOS_STUB_STATEFILE", "ok"},
         {"LCOS_STUB_COUNTFILE", w.count}});
    const std::string calls = slurp(log);
    const auto sel = calls.find("debconf-set-selections ");
    const auto inst = calls.find("--only-upgrade");
    expect(r.status == 0 && sel != std::string::npos && inst != std::string::npos && sel < inst &&
               has(calls, "multiselect /dev/disk/by-id/ata-TESTDISK\n"),
           "install records the boot disk before apt-get install");
  }
  {
    const World w = make_world();
    write_file(w.status, kStatusInstalled);
    const std::string log = w.dir + "/argv";
    const HelperRun r = run_helper(
        helper, {"upgrade", "grub-pc=2.0"},
        {{"LCOS_UPDATES_APT_GET", w.apt},
         {"LCOS_UPDATES_DPKG", w.dpkg},
         {"LCOS_UPDATES_DPKG_STATUS", w.status},
         {"LCOS_UPDATES_DPKG_UPDATES_DIR", w.updates},
         {"LCOS_UPDATES_DEBCONF_DIR", w.confdir},
         {"LCOS_UPDATES_GRUB_PROBE", w.probe},
         {"LCOS_UPDATES_DEBCONF_SET_SELECTIONS", w.debconf},
         {"LCOS_UPDATES_GRUB_CORE", w.core},
         {"LCOS_UPDATES_DISK_BY_ID", w.byid},
         {"LCOS_UPDATES_SYS_BLOCK", w.sys},
         {"LCOS_UPDATES_DEV_MAPPER", w.mapper},
         {"LCOS_UPDATES_LISTS_DIR", w.lists},
         {"LCOS_UPDATES_REBOOT_FILE", w.reboot},
         {"LCOS_STUB_ARGVFILE", log},
         {"LCOS_STUB_SCRIPT", "instfail"},
         {"LCOS_STUB_ENVFILE", "sda"},
         {"LCOS_STUB_STATEFILE", "ok"},
         {"LCOS_STUB_COUNTFILE", w.count}});
    const SimulateResult p = parse_protocol(r.out);
    expect(r.status == 1 && has(p.error_msg, "The upgrade stopped with a package left unfinished.") &&
               has(p.error_msg, "Click Repair to finish configuring packages.") && error_suggests_repair(p.error_msg),
           "a postinst failure during install says to click Repair");
  }
}

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  test_detection();
  if (argc >= 2)
    test_helper(argv[1]);
  else
    expect(false, "helper path argument");
  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all grub-pc tests passed\n");
  return 0;
}
