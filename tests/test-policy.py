#!/usr/bin/env python3
"""Polkit wording and subject defaults.

simulate checks, upgrade installs, repair repairs; argv1 keeps them apart.
allow_any and allow_inactive are no. Round 11: only the check action
(argv1 simulate) may use auth_admin_keep. Install, repair and (round 15)
cleanup stay auth_admin, so a cached authorization never lets another program install,
remove or configure packages (round 10, U-4).
"""

import sys
import xml.etree.ElementTree as ET

PATH_KEY = "org.freedesktop.policykit.exec.path"
ARGV_KEY = "org.freedesktop.policykit.exec.argv1"
GUI_KEY = "org.freedesktop.policykit.exec.allow_gui"
HELPER = "/usr/libexec/lcos-updates-helper"


def main() -> int:
    path = sys.argv[1]
    root = ET.parse(path).getroot()
    matched = []
    for action in root.findall("action"):
        ann = {}
        for node in action.findall("annotate"):
            ann[node.get("key")] = (node.text or "").strip()
        if ann.get(PATH_KEY) != HELPER:
            continue
        defaults = action.find("defaults")
        allow = {}
        if defaults is not None:
            for key in ("allow_any", "allow_inactive", "allow_active"):
                node = defaults.find(key)
                allow[key] = (node.text or "").strip() if node is not None else ""
        matched.append(
            {
                "id": action.get("id"),
                "description": (action.findtext("description") or "").strip(),
                "message": (action.findtext("message") or "").strip(),
                "argv1": ann.get(ARGV_KEY),
                "allow_gui": ann.get(GUI_KEY),
                "allow_any": allow.get("allow_any", ""),
                "allow_inactive": allow.get("allow_inactive", ""),
                "allow_active": allow.get("allow_active", ""),
            }
        )

    if len(matched) != 4:
        print(f"expected 4 helper actions, found {len(matched)}", file=sys.stderr)
        return 1

    for action in matched:
        if action["argv1"] not in ("simulate", "upgrade", "repair", "cleanup"):
            print(f"{action['id']} missing argv1 simulate|upgrade|repair", file=sys.stderr)
            return 1
        if not action["allow_gui"]:
            print(f"{action['id']} lost allow_gui", file=sys.stderr)
            return 1

    simulate = next(a for a in matched if a["argv1"] == "simulate")
    upgrade = next(a for a in matched if a["argv1"] == "upgrade")
    repair = next(a for a in matched if a["argv1"] == "repair")
    cleanup = next(a for a in matched if a["argv1"] == "cleanup")
    if "remove unused" not in cleanup["message"].lower():
        print(f"cleanup message is not about removing unused packages: {cleanup['message']}", file=sys.stderr)
        return 1
    if "repair" not in repair["message"].lower():
        print(f"repair message is not about repairing: {repair['message']}", file=sys.stderr)
        return 1

    if "check" not in simulate["message"].lower() or "install" in simulate["message"].lower():
        print(f"check message is not about checking: {simulate['message']}", file=sys.stderr)
        return 1
    if "install" not in upgrade["message"].lower():
        print(f"install message is not about installing: {upgrade['message']}", file=sys.stderr)
        return 1
    if "force-conf" in upgrade["message"].lower():
        print("install message should stay the authentication sentence", file=sys.stderr)
        return 1

    for action in (simulate, upgrade, repair, cleanup):
        if action["allow_any"] != "no" or action["allow_inactive"] != "no":
            print(
                f"{action['id']} must deny inactive and non-local subjects "
                f"(allow_any={action['allow_any']} allow_inactive={action['allow_inactive']})",
                file=sys.stderr,
            )
            return 1

    if simulate["allow_active"] != "auth_admin_keep":
        print(f"check allow_active should be auth_admin_keep, got {simulate['allow_active']}", file=sys.stderr)
        return 1
    for action in (upgrade, repair, cleanup):
        if action["allow_active"] != "auth_admin":
            print(
                f"{action['id']} allow_active should be auth_admin, got {action['allow_active']}",
                file=sys.stderr,
            )
            return 1
    keeps = 0
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            if "<allow_" in raw and "_keep" in raw:
                keeps += 1
    if keeps != 1:
        print(f"only the check action may keep an authorization, found {keeps} keeps", file=sys.stderr)
        return 1

    print("ok: only check keeps its authorization; install and repair need a fresh password; subjects are pinned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
