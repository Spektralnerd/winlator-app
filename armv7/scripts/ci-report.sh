#!/usr/bin/env bash
# Gibt das Ende eines Bauprotokolls als GitHub-Annotationen aus (die sind auch über die REST-API
# lesbar, wenn die Rohprotokolle nicht erreichbar sind).
#   ci-report.sh <protokoll> [titel] [zeilen]
LOG="$1"; TITLE="${2:-Bau fehlgeschlagen}"; LINES="${3:-120}"
[ -f "$LOG" ] || { echo "::error title=$TITLE::kein Protokoll ($LOG)"; exit 0; }
python3 -I - "$LOG" "$TITLE" "$LINES" <<'PY'
import re, sys
log, title, lines = sys.argv[1], sys.argv[2], int(sys.argv[3])
text = open(log, errors="replace").read().splitlines()[-lines:]
# Farbcodes entfernen, in Stücke von höchstens ~3500 Zeichen teilen (max. 9 Annotationen)
text = [re.sub(r"\x1b\[[0-9;]*m", "", l)[:400] for l in text]
chunks, cur = [], ""
for l in text:
    if len(cur) + len(l) > 3500:
        chunks.append(cur); cur = ""
    cur += l + "\n"
chunks.append(cur)
chunks = chunks[-9:]
enc = lambda s: s.replace("%", "%25").replace("\r", "").replace("\n", "%0A")
for i, c in enumerate(chunks, 1):
    t = enc(f"{title} ({i}/{len(chunks)})").replace(":", "%3A").replace(",", "%2C")
    print(f"::error title={t}::{enc(c)}")
PY
