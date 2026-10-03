#!/usr/bin/env python3
"""Polkit wording: simulate checks, upgrade installs, and argv1 keeps them apart."""

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
        matched.append(
            {
                "id": action.get("id"),
                "description": (action.findtext("description") or "").strip(),
                "message": (action.findtext("message") or "").strip(),
                "argv1": ann.get(ARGV_KEY),
                "allow_gui": ann.get(GUI_KEY),
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

    print("ok: simulate asks to check, upgrade asks to install")
    return 0


if __name__ == "__main__":
    sys.exit(main())
