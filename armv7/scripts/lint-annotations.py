#!/usr/bin/env python3
"""Fasst einen Android-Lint-XML-Bericht als GitHub-Annotation zusammen (eine pro Schweregrad),
damit die Funde auch über die REST-API lesbar sind.

Aufruf: lint-annotations.py <lint-results.xml>
"""
import os
import sys
import xml.etree.ElementTree as ET


def enc(text):
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def main():
    path = sys.argv[1]
    if not os.path.exists(path):
        print(f"::warning title=Lint::kein Bericht ({path})")
        return
    root = ET.parse(path).getroot()
    found = {}
    for issue in root.iter("issue"):
        loc = issue.find("location")
        where = ""
        if loc is not None:
            f = loc.get("file", "")
            f = f[f.find("app/src/"):] if "app/src/" in f else os.path.basename(f)
            where = f"{f}:{loc.get('line', '?')}"
        line = f"{where} [{issue.get('id')}] {issue.get('message')}"
        found.setdefault(issue.get("severity", "Warning"), []).append(line)
    if not found:
        print("::notice title=Lint::keine Funde")
        return
    for severity, lines in found.items():
        level = "error" if severity in ("Error", "Fatal") else "warning"
        text = "\n".join(lines[:80]) + (f"\n… und {len(lines) - 80} weitere" if len(lines) > 80 else "")
        print(f"::{level} title=Lint {severity} ({len(lines)})::{enc(text)}")


if __name__ == "__main__":
    main()
