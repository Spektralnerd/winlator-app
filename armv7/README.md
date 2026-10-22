# Winlator für 32-Bit-ARM (armv7)

Dieser Zweig (`armv7`) baut Winlator 11.2 zusätzlich für Geräte mit 32-Bit-Android
(`armeabi-v7a`). Das Original (arm64) bleibt unverändert baubar.

## Varianten

| `-PwinlatorArch=` | App | Unterbau | Für |
|---|---|---|---|
| `arm64` (Standard) | arm64-v8a | Box64 + WoW64-Wine (Original) | normale 64-Bit-Geräte |
| `armv7hybrid` | armeabi-v7a | Box64 + WoW64-Wine (Original) | 32-Bit-Android auf 64-Bit-Kernel (`uname -m` = `armv8l`) |
| `armv7` | armeabi-v7a | Box86 + i386-Wine 10.10, Win32-Präfix | alle ARMv7-Geräte mit NEON |

Die App erkennt die Variante über `BuildConfig.WINLATOR_ARCH` / `com.winlator.core.Arch`.
In `armv7` gilt:

- Box86 statt Box64 (Voreinstellungen werden auf `BOX86_*` umgesetzt, reine Box64-Optionen entfallen)
- Wine ist ein reines 32-Bit-Wine, das Präfix ein Win32-Präfix: `system32` enthält die
  32-Bit-DLLs, `syswow64` gibt es nicht. Komponenten, die für ein WoW64-Präfix gepackt sind
  (DXVK, VKD3D, WineD3D, D8VK, D7VK, cnc-ddraw, Windows-Komponenten), werden beim Entpacken
  umgelenkt (`syswow64/` → `system32/`, 64-Bit-Teile entfallen).
- Zusätzlicher OpenGL-Treiber **Software** (Mesa softpipe auf der CPU, steckt in der
  VirGL-Bibliothek; langsam, aber ohne Anforderungen an die GPU)
- adrenotools gibt es nur für arm64 (eigene Qualcomm-Treiber für Vortek entfallen)
- winhandler.exe ist ein 32-Bit-Nachbau (`armv7/winhandler`), winpreexec ein armhf-Nachbau
  (`armv7/winpreexec`), wfm.exe und libcdio.dll werden aus den Quellen von brunodev85 für i686 gebaut

## Bauen

Alles läuft über GitHub Actions (`.github/workflows/armv7.yml`):

1. `plan`: Hash über `armv7/scripts`, die Nachbauten und die Original-Assets; nur wenn er sich
   gegenüber dem Release `armv7-rootfs` geändert hat, wird der Unterbau neu gebaut
   (erzwingen: Workflow von Hand starten, Haken bei „32-Bit-Unterbau neu bauen“)
2. `armhf` (Debian 12, `ci-armhf.sh`): glibc 2.39 mit Termux- und Winlator-Patches, Box86,
   alsa-lib + Winlator-Plugin, Gladio-/Vortek-Client, Mesa (VirGL+softpipe, Zink),
   Laufzeitbibliotheken aus Debian 12 armhf (nur die benötigten), Hilfsprogramme
3. `wine` (Debian 12 i386, `ci-wine.sh`): Wine 10.10 (Winlator-Fassung) als 32-Bit-Wine und
   daraus die Container-Vorlage (Win32-Präfix)
4. `assemble` (`assemble.sh`, Prüfung mit `check-rootfs.py`): Assets → Release `armv7-rootfs`
   (Datei `armv7-assets.tar`, wird nach `app/src/armv7/assets` entpackt)
5. `apk`: beide APKs → Release `armv7-apk`

Warum Debian 12: Box86 reicht Strukturen des i386-Gasts unverändert an die armhf-Bibliotheken
weiter, beide brauchen also dasselbe ABI mit 32-Bit-`time_t`. Neuere Distributionen haben armhf
auf 64-Bit-`time_t` umgestellt (i386 nicht).

Schlägt ein Schritt fehl, stehen die letzten Protokollzeilen als Annotation am Lauf und die
vollständigen Protokolle im Release `armv7-ci-logs`.

Die Bauskripte liegen in `armv7/scripts`, gemeinsame Einstellungen und feste Quellstände in
`armv7/scripts/env.sh`. Einzelne Teile lassen sich auch lokal bauen (Debian 12 bzw. ein
Cross-Compiler für `arm-linux-gnueabihf`), z. B. `bash armv7/scripts/build-box86.sh`;
die Ergebnisse landen in `armv7/work`.

Die APKs sind mit dem Schlüssel aus `armv7/signing/debug.keystore` signiert (Passwort
`android`), damit neue Builds sich über alte installieren lassen. Er ist öffentlich und nur für
Testbuilds gedacht.

## Grenzen

- Nur 32-Bit-Windows-Programme (es gibt kein 64-Bit-Wine auf 32-Bit-ARM)
- Die Bildausgabe der App braucht OpenGL ES 3.0 (Mali-400 & Co. können nur ES 2.0)
- Vortek braucht Vulkan auf dem Gerät, VirGL und Gladio brauchen OpenGL ES 3.x
- PulseAudio gibt es für armv7 noch nicht (ALSA ist ohnehin voreingestellt)
