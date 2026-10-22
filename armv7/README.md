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
- Zusätzlicher OpenGL-Treiber **Software** (Mesa llvmpipe/softpipe auf der CPU)
- adrenotools gibt es nur für arm64 (eigene Qualcomm-Treiber für Vortek entfallen)
- winhandler.exe ist ein 32-Bit-Nachbau (`armv7/winhandler`), winpreexec ein armhf-Nachbau
  (`armv7/winpreexec`), wfm.exe und libcdio.dll werden aus den Quellen von brunodev85 für i686 gebaut

## Bauen

Alles läuft über GitHub Actions (`.github/workflows/armv7.yml`):

1. Unterbau (glibc mit Termux- und Winlator-Patches, Box86, Wine, Mesa, Bibliotheken) →
   Release `armv7-rootfs` (Datei `armv7-assets.tar`)
2. APKs → Release `armv7-apk`

Die Bauskripte liegen in `armv7/scripts`, gemeinsame Einstellungen und feste Quellstände in
`armv7/scripts/env.sh`.

Die APKs sind mit dem Schlüssel aus `armv7/signing/debug.keystore` signiert (Passwort
`android`), damit neue Builds sich über alte installieren lassen. Er ist öffentlich und nur für
Testbuilds gedacht.

## Grenzen

- Nur 32-Bit-Windows-Programme (es gibt kein 64-Bit-Wine auf 32-Bit-ARM)
- Die Bildausgabe der App braucht OpenGL ES 3.0 (Mali-400 & Co. können nur ES 2.0)
- Vortek braucht Vulkan auf dem Gerät, VirGL und Gladio brauchen OpenGL ES 3.x
- PulseAudio gibt es für armv7 noch nicht (ALSA ist ohnehin voreingestellt)
