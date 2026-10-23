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
- Zusätzlicher OpenGL-Treiber **Software** (Mesa llvmpipe auf der CPU, steckt in der
  VirGL-Bibliothek; ohne Anforderungen an die GPU). llvmpipe übersetzt die Shader mit LLVM 15
  (aus Debian armhf, statisch gelinkt) und rastert mit `LP_NUM_THREADS` Threads (die App setzt
  die Zahl der Kerne). `GALLIUM_DRIVER=softpipe` in den Container-Umgebungsvariablen nimmt den
  einfachen Rasterer ohne JIT. Die App setzt für „Software“ `GALLIVM_PERF=no_aos_sampling`: Der
  8-Bit-Pfad (AoS) von llvmpipe liefert auf 32-Bit-ARM mit LINEAR gefilterte Texturen schwarz
  (unter qemu nachgestellt: `GL_NEAREST` stimmt, `GL_LINEAR` gibt (0,0,0,255)); der
  Gleitkomma-Pfad rechnet richtig. Ein eigener Wert in den Umgebungsvariablen hat Vorrang. Das Winlator-Mesa schaltet softpipe ab, sobald VirGL mitgebaut
  wird; `patches/mesa/0001-softpipe-with-virgl.patch` nimmt das zurück (mit `virpipe` bleibt es
  weiterhin bei VirGL, ohne Ausweichen).
  Ohne den Patch scheiterte jeder OpenGL-Kontext („couldn't initialize OpenGL“), und Programme,
  die trotzdem weitermachen, stürzten z. B. in glu32 mit 0xc0000417 ab.
- adrenotools gibt es nur für arm64 (eigene Qualcomm-Treiber für Vortek entfallen)
- winhandler.exe ist ein 32-Bit-Nachbau (`armv7/winhandler`), winpreexec ein armhf-Nachbau
  (`armv7/winpreexec`), wfm.exe und libcdio.dll werden aus den Quellen von brunodev85 für i686 gebaut

## Bauen

Alles läuft über GitHub Actions (`.github/workflows/armv7.yml`):

1. `plan`: Hash über `armv7/scripts`, die Nachbauten und die Original-Assets; nur wenn er sich
   gegenüber dem Release `armv7-rootfs` geändert hat, wird der Unterbau neu gebaut
   (erzwingen: Workflow von Hand starten, Haken bei „32-Bit-Unterbau neu bauen“)
2. `armhf` (Debian 12, `ci-armhf.sh`): glibc 2.39 mit Termux- und Winlator-Patches, Box86,
   alsa-lib + Winlator-Plugin, Gladio-/Vortek-Client, Mesa (VirGL+llvmpipe/softpipe, Zink),
   Laufzeitbibliotheken aus Debian 12 armhf (nur die benötigten), Hilfsprogramme
3. `wine` (Debian 12 i386, `ci-wine.sh`): Wine 10.10 (Winlator-Fassung) als 32-Bit-Wine und
   daraus die Container-Vorlage (Win32-Präfix)
4. `assemble` (`assemble.sh`, Prüfung mit `check-rootfs.py`): Assets → Release `armv7-rootfs`
   (Datei `armv7-assets.tar`, wird nach `app/src/armv7/assets` entpackt)
5. `apk`: beide APKs → Release `armv7-apk`

Warum Debian 12: Box86 reicht Strukturen des i386-Gasts unverändert an die armhf-Bibliotheken
weiter, beide brauchen also dasselbe ABI mit 32-Bit-`time_t`. Neuere Distributionen haben armhf
auf 64-Bit-`time_t` umgestellt (i386 nicht).

Eigene Patches liegen in `armv7/patches` (werden beim Bauen angewendet):
- `box86/0001-wine10.patch`: was Box86 0.3.9 für Wine 10 fehlt, beides beendete jeden
  Containerstart sofort:
  - `posix_spawn` ging ungeprüft an die armhf-libc; Wine 10 startet wineserver damit, und der
    Kernel kann das i386-Programm nicht ausführen („could not exec wineserver“). Jetzt startet
    Box86 x86-Programme dort wie bei `execv` über sich selbst.
  - `__epoll_pwait2_time64` (wineserver) und `__nanosleep64` (winebus) hatten keinen Wrapper
    („PltResolver: Symbol … not found“). epoll_pwait2 liefert ENOSYS (gibt es unter Android
    nicht), wineserver nimmt dann epoll_wait.

  `check-rootfs.py` prüft seitdem jede glibc-Funktion, die Wine importiert, gegen die Wrapper
  von Box86 (`box86-libc-symbols.txt` aus `build-box86.sh`).
- `glibc/android_seccomp.c`: siehe „Android 8 und 9“ unten
- `glibc/0001-arc4random-enosys.patch` (aus dem 2.39-Wartungszweig, BZ 31612): ohne
  `getrandom` (Kernel vor 3.17, z. B. 3.0 auf dem Galaxy Note 8.0) weicht arc4random auf
  /dev/urandom aus, statt jedes Programm mit „cannot get entropy for arc4random“ abzubrechen

Schlägt ein Schritt fehl, stehen die letzten Protokollzeilen als Annotation am Lauf und die
vollständigen Protokolle im Release `armv7-ci-logs`.

Die Bauskripte liegen in `armv7/scripts`, gemeinsame Einstellungen und feste Quellstände in
`armv7/scripts/env.sh`. Einzelne Teile lassen sich auch lokal bauen (Debian 12 bzw. ein
Cross-Compiler für `arm-linux-gnueabihf`), z. B. `bash armv7/scripts/build-box86.sh`;
die Ergebnisse landen in `armv7/work`.

Die APKs sind mit dem Schlüssel aus `armv7/signing/debug.keystore` signiert (Passwort
`android`), damit neue Builds sich über alte installieren lassen. Er ist öffentlich und nur für
Testbuilds gedacht.

## Ältere Geräte (Etappe 6)

Die 32-Bit-Builds laufen ab **Android 7.0** (API 24, das Original ab 8.0):

- `@CriticalNative` wirkt erst ab Android 8; auf Android 7 registriert `libwinlator` beim Laden
  Wrapper mit normaler JNI-Signatur (`critical_native_compat.c`)
- AHardwareBuffer (ab Android 8) wird zur Laufzeit gesucht (`ahb_compat.c`); ohne ihn bleiben
  Fenster gewöhnliche Texturen, Vortek kann dann nicht ausgeben
- geteilter Speicher fällt ohne `memfd` (Kernel älter als 3.17) auf ashmem zurück
- Pointer-Capture, Benachrichtigungskanäle, Vibrationsstärke und der AudioTrack-Leistungsmodus
  nur ab Android 8

**Android 8 und 9 (seccomp):** Android legt jedem App-Prozess einen seccomp-Filter an, der für
32-Bit-ARM alle Systemaufrufe ab Linux 4.14 (darunter die time64-Aufrufe) und einige alte
(`link`, `lchown32`, `pause`, SysV-Semaphoren …) mit SIGSYS beantwortet, statt ENOSYS zu liefern.
Erst Android 10/11 erlauben die time64-Aufrufe. Die glibc des Root-FS
- ruft `link`, `lchown32` und `pause` gar nicht mehr auf (sie nimmt `linkat`, `fchownat`, `ppoll`),
- richtet in jedem Prozess einen SIGSYS-Handler ein (`patches/glibc/android_seccomp.c`), der
  gesperrte Aufrufe auf erlaubte umlenkt oder mit ENOSYS zurückkehren lässt, damit glibc wie bei
  einem alten Kernel auf ältere Aufrufe ausweicht. Jeder Fall steht einmal pro Prozess in der
  Ausgabe (`winlator-seccomp: …`, landet in `start.txt`).

`scripts/check-syscalls.py` prüft beim Bauen, dass ld.so nichts Gesperrtes aufruft (es läuft vor
dem Handler) und libc die drei alten Aufrufe nicht mehr nutzt. Die Sperrliste stammt aus bionic
(LineageOS 15.1/16.0, `libc/seccomp/arm_*policy.cpp`).

Das Root-FS trägt eine Build-Kennung (`.winlator/rootfs-id`, daneben `assets/rootfs-id.txt`);
weicht die installierte von der mitgelieferten ab, entpackt die App das Root-FS neu (Container
in `home/` bleiben erhalten).

Bildausgabe mit **OpenGL ES 2.0** (z. B. Mali-400): Hat das Gerät kein ES 3, nimmt die App einen
ES-2-Kontext und übersetzt ihre Shader nach GLSL ES 1.00. Neue Container bekommen dann
„Software“ als OpenGL-Treiber und, ohne Vulkan-Treiber, WineD3D statt DXVK (alles umstellbar).

**Stapel des Hauptthreads (Kernel mit 3G/1G-Aufteilung, z. B. Android 9 auf dem Galaxy Note 8.0):**
Box86 führt den nativen Code des Hauptthreads (eingebundene Bibliotheken wie die libGL mit
llvmpipe/LLVM) auf dem nativen Hauptstapel des Prozesses aus; Wine sieht nur Box86s eigenen
x86-Stapel. Liegt der native Stapel oberhalb von 2 GB (bei 3G/1G knapp unter 0xbf000000),
reservierte Wine alles bis 64 KB unter ihn, der Stapel konnte nicht mehr wachsen und hatte etwa
200 KB. LLVM lief darüber hinaus (SIGSEGV in `memset` knapp unter dem Stapel); das Programm fing
die Ausnahme ab, der GL-Aufruf brach jedes Mal mitten in der Shader-Übersetzung ab, llvmpipe
legte bei jedem Zeichenaufruf eine neue Variante an, kein Bild wurde fertig, und nach etwa 1400
Varianten war der Speicher voll. `patches/wine/0002-native-main-stack-room.patch` lässt unter dem
nativen Hauptstapel (`[stack]` in `/proc/self/maps`) so viel Platz frei, wie `RLIMIT_STACK`
erlaubt (mindestens 8 MB). Unter qemu liegt der Stapel unter 2 GB, dort trat das nie auf.

## Testen auf dem Gerät

1. APK aus dem Release `armv7-apk` installieren: `Winlator-11.2-armv7.apk` für alle 32-Bit-Geräte,
   `Winlator-11.2-armv7hybrid.apk` nur bei 64-Bit-Kernel (`uname -m` zeigt `armv8l` oder `aarch64`)
2. Beim ersten Start wird das Root-FS entpackt (einige Minuten), danach einen Container anlegen
   (Vorgaben passen sich an: ohne OpenGL ES 3 „Software“, ohne Vulkan WineD3D)
3. Container starten: es sollte der Wine-Desktop mit dem Dateimanager (wfm) erscheinen
4. Klappt etwas nicht, schreiben die 32-Bit-Builds ohne weitere Einstellungen nach
   `Documents/Winlator` (ohne Speicherberechtigung nach `Android/data/com.winlator/files`):
   - `start.txt`: jeder Containerstart mit Gerät, CPU-Merkmalen, OpenGL ES, Speicher,
     Einstellungen, Befehl, Ausgabe des Gastprogramms und Endstatus (Signale ausgeschrieben)
   - `crash.txt`: Java-Abstürze der App
   - `logcat.txt`: das Android-Protokoll der App, gesichert, wenn das Gastprogramm endet, und beim
     nächsten App-Start, falls die App während eines Containers verschwunden ist (nativer Absturz,
     vom System beendet)

   Mehr Ausgabe: in den Einstellungen „Enable Wine debug“ und „Box86 Logs“ einschalten.
   Mit „Save logs to file“ landet das Protokoll zusätzlich in `Documents/Winlator/logs.txt`.

   Stürzt ein Windows-Programm ab („Unhandled exception …“), kann Box86 den Wine-Debugger nicht
   starten. Das Wine des Root-FS (`patches/wine/0001-kernelbase-backtrace.patch`) gibt darum
   selbst die Register und die Aufrufkette aus (Rücksprungadressen auf dem Stapel als
   `modul+offset`), die Zeilen beginnen mit `wine:` und stehen in `start.txt`.

   Scheitert in der libGL (llvmpipe/LLVM) eine Speicheranforderung, endete das bisher nur mit
   „terminate called after throwing an instance of 'std::bad_alloc'“. Die libGL ersetzt darum
   `operator new` (`patches/mesa/0002-oom-report.patch`) und schreibt vorher Zeilen `[diag] …`
   nach `start.txt`: Größe, Speicherstand, Grenzen (overcommit, max_map_count, rlimit), die
   malloc-Statistik, den Adressraum (größte Lücken und Bereiche, Summen je Datei, Belegung je
   16 MB) und die Rücksprungadressen in die libGL. Die Offsets löst
   die Symboldatei `libGL-virgl-symbole.txt.xz` aus dem Release `armv7-rootfs` auf (erste Zeile:
   build-id, muss zur Diagnosezeile passen). `WINLATOR_OOM_TEST=n` lässt zum Ausprobieren die
   n-te Anforderung scheitern. Eine nicht abgefangene C++-Ausnahme beendet den Prozess danach
   sofort (`_exit(134)`): Über `abort()` machte Wine unter Box86 daraus eine Windows-Ausnahme,
   und das Programm hing in einer Endlosschleife (Ton lief weiter, Fenster blieb schwarz).
   `start.txt` fasst gleiche Zeilen hintereinander zusammen und übernimmt `[diag]`-Zeilen
   immer, auch jenseits der ersten 600 Zeilen.

   Dazu schreibt die libGL den Fortschritt von llvmpipe mit (`patches/mesa/0003-diag-progress.patch`):
   die ersten 40 übersetzten Shader mit Dauer (danach nur langsame und jeden hundertsten), das
   erste Bild und alle 30 s die Bilder pro Sekunde (nach 5 Minuten alle 5 Minuten). Ein
   Wachhund-Thread meldet Stillstände – eine Übersetzung dauert über 15 s, seit 15 s kein neues
   Bild, 30 s nach dem ersten Shader noch kein Bild – und gibt dann für jeden Thread Zustand,
   Wartestelle im Kernel, CPU-Zeit, Programmzähler und die Rücksprünge in die libGL aus
   (höchstens dreimal je Prozess; Kernel 3.0 liefert dafür Stapel- und Befehlszeiger in
   `/proc/self/task/*/stat`). Bekommt ein Fragment-Shader mehr als vier Varianten, steht
   für die nächsten (höchstens 40 Meldungen) da, in welchen Bytes sich der Variantenschlüssel
   von der ähnlichsten vorhandenen Variante unterscheidet (Aufbau des Schlüssels im Kommentar
   von `winlator_diag_fs_variant`); auf dem Galaxy Note 8.0 bekam beim SKIDROW-Intro ein
   einziger Shader über 1400 Varianten, bis der Speicher ausging (Ursache: zu kleiner Stapel des
   Hauptthreads, siehe „Stapel des Hauptthreads“). Bricht eine Übersetzung ab, steht beim
   nächsten Beginn, nach welchem Schritt (JIT-Engine, Optimierung, Maschinencode …). Stürzt
   Code in der libGL ab (Signal mit Programmzähler oder Rücksprungadresse in der libGL), meldet
   sie Signal, Adresse und Aufrufkette und reicht das Signal dann an Box86 weiter (höchstens
   dreimal je Prozess); Box86 selbst zeigt mit `BOX86_SHOWSEGV=1` jeden Speicherzugriffsfehler,
   auch wenn das Programm einen Handler hat. `WINLATOR_DIAG=0` schaltet den Fortschritt und den
   Signalabfang ab.

Bildreste beim Scrollen (Einstellungen auf Android 9): Das Original setzt den Fensterhintergrund
auf `?attr/colorPrimarySurface`, eine reine Farbe. Android bis 9 liest `android:windowBackground`
nur als Ressourcen-ID und bekommt dann gar keinen Hintergrund; das Fenster gilt trotzdem als
deckend, beim Scrollen bleibt der alte Inhalt stehen. Der Hintergrund ist jetzt eine
Farbressource (`values/colors.xml`); erst Android 10 liest auch reine Farben.

Beide APKs haben dieselbe Paketkennung und lassen sich übereinander installieren. Beim Wechsel
der Variante entpackt die App ihr Root-FS neu; Container aus der anderen Variante (WoW64- statt
Win32-Präfix) funktionieren dann nicht mehr und sollten neu angelegt werden.

## Grenzen

- Nur 32-Bit-Windows-Programme (es gibt kein 64-Bit-Wine auf 32-Bit-ARM)
- Vortek braucht Vulkan auf dem Gerät (und Android 8), VirGL und Gladio brauchen OpenGL ES 3.x
- Software-Rendering bleibt Software-Rendering: llvmpipe ist um ein Vielfaches schneller als
  softpipe (auf dem Galaxy Note 8.0 schaffte softpipe beim SKIDROW-Intro, 1024×768, etwa
  0,5 Bilder pro Sekunde), ersetzt aber keine GPU
- PulseAudio gibt es für armv7 noch nicht (ALSA ist ohnehin voreingestellt)
