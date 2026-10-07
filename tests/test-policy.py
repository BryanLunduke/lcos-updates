#!/usr/bin/env python3
"""Polkit wording and subject defaults.

simulate checks, upgrade installs, argv1 keeps them apart.
allow_any and allow_inactive are no. Upgrade does not use auth_admin_keep.
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

    if len(matched) != 2:
        print(f"expected 2 helper actions, found {len(matched)}", file=sys.stderr)
        return 1

    for action in matched:
        if action["argv1"] not in ("simulate", "upgrade"):
            print(f"{action['id']} missing argv1 simulate|upgrade", file=sys.stderr)
            return 1
        if not action["allow_gui"]:
            print(f"{action['id']} lost allow_gui", file=sys.stderr)
            return 1

    simulate = next(a for a in matched if a["argv1"] == "simulate")
    upgrade = next(a for a in matched if a["argv1"] == "upgrade")

    if "check" not in simulate["message"].lower() or "install" in simulate["message"].lower():
        print(f"check message is not about checking: {simulate['message']}", file=sys.stderr)
        return 1
    if "install" not in upgrade["message"].lower():
        print(f"install message is not about installing: {upgrade['message']}", file=sys.stderr)
        return 1
    if "force-conf" in upgrade["message"].lower():
        print("install message should stay the authentication sentence", file=sys.stderr)
        return 1

    for action in (simulate, upgrade):
        if action["allow_any"] != "no" or action["allow_inactive"] != "no":
            print(
                f"{action['id']} must deny inactive and non-local subjects "
                f"(allow_any={action['allow_any']} allow_inactive={action['allow_inactive']})",
                file=sys.stderr,
            )
            return 1

    if simulate["allow_active"] != "auth_admin_keep":
        print(
            f"check allow_active should be auth_admin_keep, got {simulate['allow_active']}",
            file=sys.stderr,
        )
        return 1
    if upgrade["allow_active"] != "auth_admin":
        print(
            f"upgrade allow_active should be auth_admin, got {upgrade['allow_active']}",
            file=sys.stderr,
        )
        return 1
    if "keep" in upgrade["allow_active"]:
        print("upgrade must not use auth_admin_keep", file=sys.stderr)
        return 1

    print("ok: simulate asks to check, upgrade asks to install, subjects are pinned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
