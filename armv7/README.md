# Winlator für 32-Bit-ARM (armv7)

Dieser Zweig (`armv7`) baut Winlator 11.2 zusätzlich für Geräte mit 32-Bit-Android
(`armeabi-v7a`). Das Original (arm64) bleibt baubar; es bekommt die Fehlerbehebungen an Gladio
mit, die auch 64 Bit betreffen (siehe „Gladio auf 64 Bit“ weiter unten), und den OpenGL-Treiber
„Software“ (siehe „Software auf 64 Bit“), sonst ist es unverändert.

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
  einfachen Rasterer ohne JIT. `GALLIVM_PERF` setzt die App nicht (mehr): Früher stand dort
  `no_aos_sampling`, weil der 8-Bit-Texturpfad (AoS) mit `GL_LINEAR` schwarze Texel lieferte.
  Das kam nur unter qemu vor: LLVM liest für die Zielmerkmale `/proc/cpuinfo`, unter qemu-user
  die des x86-PCs, erzeugt dann Code ohne Gleitkommaeinheit und ruft libm-Funktionen mit der
  falschen Aufrufkonvention auf. Mit der cpuinfo eines Cortex-A9 (`unshare --mount` und
  `mount --bind`) stimmt der AoS-Pfad. Zum Vergleichen lässt sich `GALLIVM_PERF` im Container
  setzen (`no_aos_sampling`, `brilinear`, `rho_approx`, durch Kommas getrennt).
  `patches/mesa/0005-armv7-neon-rounding.patch`: ARMv7-NEON kann Vektoren nicht runden (VRINT
  gibt es erst ab ARMv8); LLVM machte aus `llvm.floor`/`llvm.nearbyint` je Lane einen
  libm-Aufruf, also etwa zwei je Texel (`nearbyintf` sichert dabei jedes Mal FPSCR). Auf
  32-Bit-ARM nimmt gallivm jetzt seine eigenen Rundungen aus reinen Vektorbefehlen;
  `GALLIVM_PERF=arch_rounding` stellt das alte Verhalten her.
  `patches/mesa/0007-armv7-neon-pack.patch`: Den schnellen Weg „4×4×32 → 16×8“ (Fragmentfarbe
  float → RGBA8) gab es nur für SSE2 und AltiVec, weil er sich auf deren sättigende
  Pack-Befehle verlässt. `lp_build_pack2` nimmt auf NEON jetzt `VQMOVN.S`/`VQMOVUN.S` (gleiche
  Bedeutung wie packss/packus), und der schnelle Weg gilt auch auf ARM. Vorher legalisierte
  LLVM die Zwischentypen `<2 x i16>`/`<4 x i8>` Spur für Spur über den Stapel (je 4×4-Block 64
  NEON→ARM-Transfers, auf dem A9 jeweils mit Wartezeit).
  `patches/mesa/0008-armv7-neon-rcp.patch`: NEON kann nicht vektoriell teilen, jede
  `<4 x float>`-Division wurde zu vier VFP-Divisionen (`VDIV.F32`, nicht überlappend). Kehrwert
  und Division nehmen jetzt `VRECPE` und zwei `VRECPS`-Schritte (volle float-Genauigkeit;
  VRECPS behandelt 0·∞, also bleiben 1/0 = ∞ und 1/∞ = 0); `GALLIVM_PERF=exact_div` stellt die
  genaue Division wieder her. Mit beiden Patches ist der Fragment-Shader des Testprogramms um
  ein Drittel kürzer, die Bilder bleiben bitgenau gleich.
  Schalter zum Ausprobieren (Umgebungsvariablen des Containers): llvmpipe bringt `LP_PERF`
  mit (`no_mip_linear` trilinear → eine Mipmap-Stufe, `no_linear` Point-Sampling, `no_mipmap`,
  durch Kommas getrennt) und `GALLIVM_PERF` (`brilinear`, `rho_approx`, `nopt`).
  `patches/mesa/0009-lp-sampler-knobs.patch` ergänzt `WINLATOR_LP_LOD_BIAS=<Zahl>` (Mipmap-Wahl
  aller Texturen verschieben, positiv = kleinere Stufen, weniger Cache-Fehlzugriffe, unschärfer)
  und `WINLATOR_LP_MAG_NEAREST=1` (nur bei Vergrößerung Point-Sampling). Beim SKIDROW-Intro und
  bei Worms 3D auf dem Note 8.0 stecken über 95 % der Zeit der Mal-Threads in den übersetzten
  Fragment-Shadern selbst (rund 120 NEON-Befehle je Pixel: knapp die Hälfte Textur, ein Viertel
  Interpolation, der Rest Tiefentest und Farbausgabe); der größte Hebel bleibt die Pixelzahl.
  Das Winlator-Mesa schaltet softpipe ab, sobald VirGL mitgebaut
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
5. `llvmpipe64` (arm64-Runner, `mesa-llvmpipe-arm64.sh`): Software-Treiber für die 64-Bit-Root-FS,
   zwischengespeichert, bis sich Skript, Patch oder Mesa-Stand ändern
6. `apk`: alle drei APKs → Release `armv7-apk`

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
- Einfügen in eine Laufwerkswurzel (wfm, „Paste“ auf C:) beendete den Dateimanager: wfm gab das
  Ziel als `C:` weiter, Wines `SHFileOperationW` lässt bei einem Pfad ohne Backslash Verzeichnis
  und Dateinamen leer (NULL) und kopiert sie dann mit `wcscpy` (Absturz in ucrtbase, aufgerufen
  aus `do_copy`). `wfm/0001-paste-into-drive-root.patch` gibt `C:\` weiter;
  `wine/0003-shell32-drive-root-target.patch` nimmt ein bloßes `C:` als Wurzel und lässt die
  beiden Felder nie mehr leer. Gilt genauso im Original (gleiche wfm- und Wine-Quellen): für
  armv7hybrid und arm64 baut `scripts/rootfs-patches-x64.sh` im CI wfm.exe mit `patches/wfm` als
  x86-64-Programm (gegen die libcdio.dll des Originals) und tauscht es in
  `rootfs_patches.tzst` aus. `rootfs_patches.id` (Prüfsumme) lässt bestehende Container die
  geänderte Datei auch ohne neue Versionsnummer entpacken. Brunos x86-64-wfm.exe übergibt
  `wcstok` außerdem einen Nullzeiger als drittes Argument (neuere MinGW-Laufzeit): eingetippte
  Pfade im Adressfeld landeten dort immer bei „Computer“; der Neubau hat das nicht.
- `wfm/0002-iso-extract-chunks-progress-cancel.patch`: „Load ISO image“ entpackte das Abbild in
  2-KB-Blöcken (je Block ein Aufruf in libcdio und ein `fwrite`) und zeigte dabei nur das
  Wartesymbol; ließ sich das Abbild nicht öffnen (NULL aus `iso9660_open_ext`), blieb der Dialog
  endlos stehen, und „Cancel“ wirkte beim Entpacken nicht. Jetzt 128-KB-Stücke, Dateiname und
  bisher entpackte Menge im Dialog, Abbrechen wirkt, und ein nicht lesbares Abbild gibt die
  Meldung „ungültiges ISO-Abbild“ statt zu hängen. Am PC (Winlators x86-64-Wine, 161-MB-Abbild):
  gleicher Inhalt wie vorher, 2 s.

### Korrigierte DLLs für das originale 64-Bit-Wine (arm64, armv7hybrid)

Das 64-Bit-Wine der Originalvariante bleibt Brunos Bau; einzelne DLLs daraus baut
`scripts/wine-dlls-x64.sh` (CI-Job `wine64dlls`, zwischengespeichert) aus denselben Quellen
(`WINE_REPO`/`WINE_COMMIT`) mit Korrekturen neu, jeweils als x86_64- und i386-PE (WoW64), und
`rootfs-patches-x64.sh` legt sie unter `opt/wine/lib/wine/...` in `rootfs_patches.tzst`:

- `dbghelp.dll`, `patches-x64/wine/0001-dbghelp-minidump-pseudo-handle.patch`: Brunos Wine lässt
  `SymInitialize(GetCurrentProcess(), ...)` absichtlich leer laufen (der Pseudo-Handle ist
  `INVALID_HANDLE_VALUE`), `MiniDumpWriteDump` griff danach auf einen Nullzeiger zu. Damit starb
  die Absturzbehandlung jedes Programms, das einen Minidump schreibt (Unreal Engine 3, Unity …),
  bevor es seine Fehlermeldung ausgeben konnte (0-Byte-`.dmp`, leeres Log; Outlast). Jetzt
  liefert `MiniDumpWriteDump` dort FALSE, und das Programm meldet seinen Fehler selbst.
  Für das 32-Bit-Wine (armv7) liegt derselbe Patch als `patches/wine/0004-dbghelp-minidump-pseudo-handle.patch`
  bei und geht mit dem nächsten Bau des Unterbaus hinein.
- `shell32.dll` mit `patches/wine/0003-shell32-drive-root-target.patch` (siehe oben), damit auch
  andere Programme als wfm in eine Laufwerkswurzel kopieren können.

Auf dem PC mit Winlators x86_64-Wine geprüft (Exporte, Importe und Ressourcen wie im Original;
gettext muss beim Bauen da sein, sonst fehlen die Übersetzungen in den Ressourcen).

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

**Gladio auf OpenGL ES 2.0** (OpenGL-Treiber „Gladio“ auf einem ES-2-Gerät): Der Gladio-Server
der App (`app/src/main/cpp/gladiorenderer`) ist für ES 3.2 geschrieben; die armv7-Variante hat
einen ES-2.0-Pfad (`gl_es2.c`, `shader_converter_es2.c`): Dem Windows-Programm wird OpenGL 2.1 /
GLSL 1.20 gemeldet (wined3d nimmt dann seinen alten Shader-Pfad), die Shader werden nach GLSL
ES 1.00 übersetzt, Texturformate, Framebuffer, Puffer und Zeichenaufrufe auf das abgebildet, was
ES 2.0 und die Erweiterungen des Treibers hergeben (Mali-400: kein VAO, kein glMapBuffer, keine
32-Bit-Indizes, kein highp im Fragment-Shader, 12 Varyings). Ohne `EGL_KHR_surfaceless_context`
(Mali-400) bekommt jeder GLX-Kontext einen 1×1-Pbuffer. Mit dem PC-Testgeschirr in
`armv7/tools/gladio-pc` lässt sich der Server samt Client auf dem Rechner mit Mesa prüfen
(ES 2.0 erzwungen, Erweiterungsliste und Pbuffer-Pfad wählbar). Der Gladio-Client in der Root-FS
wird dafür mit `armv7/scripts/patch-gladio-client.py` angepasst. Das Startprotokoll zeigt die
ES-Erweiterungen und Grenzen des Geräts (`gladio: ES 2.0 Erweiterungen …`).

Geschwindigkeit auf der Mali-400 (ES-2.0-Pfad):
- Alle 5 s stehen zwei Statistikzeilen im Protokoll: `gladio: 5.0 s: N Bilder (…/s); je Bild …
  Anfragen, … Antworten, … Zeichenaufrufe, … KB (Vertexdaten … KB), Texturen … KB` und
  `gladio: Zeit je Bild: Warten auf den Client … ms, Bildausgabe … ms, Ausführen … ms (davon
  Zeichnen … ms); Shader-Programme neu …, gesamt …`. „Warten“ ist die Zeit, in der der
  Server-Thread auf Anfragen wartet (das Programm selbst ist dann die Bremse), „Bildausgabe“
  das Kopieren des fertigen Bilds ins Fenster (auf einem Kachel-Renderer wartet der Treiber
  hier auf die GPU – ist dieser Wert groß, ist die GPU die Bremse), „Ausführen“ der Rest
  (GL-Aufrufe samt dem, was der Treiber dabei synchron erledigt), „Zeichnen“ der Anteil davon
  für Zeichenaufrufe und das Ausgeben der Stapel (Vertexdaten umsetzen, Puffer füllen).
  Dazu kommen `gladio: Stapel je Bild: … ausgegeben mit … Treiber-Zeichenaufrufen in … ms
  (Shader, Uniforms, Vertexzeiger … ms; Treiber-Zeichnen … ms)` – wie viele Stapel der Server
  tatsächlich an den Treiber gibt und wie lange der Treiber dafür braucht –,
  `gladio: Antworten je Bild außer SWAP: …` (Anfragen, auf deren Antwort das Programm warten
  muss; währenddessen arbeiten Client und Server nicht gleichzeitig) und
  `gladio: häufigste Anfragen je Bild: …`.
- Fixed-Function-Zeichnen (klassisches OpenGL ohne eigene Shader, z. B. Worms 3D): Der allgemeine
  Material-Shader des Servers entscheidet Texturumgebung, Alphatest, Nebel und Lichtzahl pro
  Pixel über Uniforms; die Mali-400 rechnet dabei praktisch alle Zweige aus. Im ES-2.0-Pfad
  erzeugt `shader_material_es2.c` deshalb für jede vorkommende Kombination (Texturmodus je
  Einheit samt GL_COMBINE-Einstellungen, Alphafunktion, Nebelmodus, Lichter mit ihren Eigenschaften,
  Punkte) einen eigenen, geradlinigen Shader; die Kombination (`FFSpec`) ist Teil des
  Material-Schlüssels. Auf dem PC liefern die Shader dasselbe Bild wie Mesa direkt.
- Der Server rief nach jedem Stapel von Zeichenaufrufen `glFlush()` auf (bei Worms 3D ~60-mal je
  Bild). Auf einem Kachel-Renderer wie der Mali-400 rendert der Treiber dann das Bild bis dahin
  und schreibt Farbe und Tiefe in den Speicher; im ES-2.0-Pfad entfällt der Aufruf (für die
  Richtigkeit ist er nicht nötig, der arm64-Pfad bleibt unverändert). Messung auf dem Note 8.0
  (Worms-3D-Titelbild, 1024×768): „Ausführen“ 340 ms → 46 ms je Bild durch Shader und Flush.
- Bildübergabe: Bisher kopierte der X-Server bei jedem `glXSwapBuffers` das fertige Bild mit
  `glCopyTexImage2D` in die Fenstertextur – auf der Mali-400 läuft das über den Prozessor und
  kostet bei 1024×768 rund 300 ms je Bild (auch bei leeren Bildern; „Bildausgabe“ in der
  Statistik), die Obergrenze lag damit bei gut 3 Bildern/s. Im ES-2.0-Pfad zeichnet der Server
  jetzt in drei Farbtexturen im Wechsel und übergibt die fertige dem Fenster direkt
  (`presentWindowTexture` in `GLXExtension.java`, `Texture.setExternalTextureId`); X-Server-Renderer
  und GLX-Kontexte teilen sich die Texturen, kopiert wird nichts mehr. Verschwindet die Textur
  (Fenstergröße, Kontextende), gibt der Server sie vorher frei (`releaseWindowTexture`).
  Messung danach (Note 8.0): Titelbild 20 Bilder/s statt 1,4, Menüs 28–65/s, „Bildausgabe“
  unter 2 ms.
- Zeichenstapel (klassisches OpenGL mit Client-Arrays, `glBegin`/`glEnd`): Der Server legte je
  Stapel für jedes Attribut ein neues Pufferobjekt an (`glBufferData`, bei Worms 3D ~250-mal je
  Bild – auf der Mali-400 je eine GPU-Speicherzuteilung über den Kernel) und setzte die
  Vertexdaten Element für Element um. Im ES-2.0-Pfad gehen die Daten jetzt als Client-Arrays
  direkt an `glVertexAttribPointer`/`glDrawElements` (der Treiber kopiert sie beim Zeichnen in
  seinen Bildspeicher), und Float-Arrays werden im Block umgesetzt
  (`readLegacyFloatArrayBulk` in `gl_context.c`).
- Indizierte Zeichenaufrufe mit Client-Arrays (`glDrawElements` im festen Funktionsmodell): Der
  Gladio-Client schickte je Index einen Vertex, gemeinsame Ecken also mehrfach (Worms-3D-Titelbild:
  1,3 MB je Bild). Der Client der Root-FS (`patch-gladio-client.py`) schickt jetzt den
  Vertexbereich einmal und die Indizes dazu, gekennzeichnet mit `GLADIO_DRAW_RANGED` im Modus;
  der Server baut daraus den Stapel mit den echten Indizes (`readRangedLegacyArrays`). Auf dem
  PC: 417 statt 1319 KB je Bild. Der Client des Original-Winlator kennt das Format nicht und
  schickt wie bisher; der Server versteht beides.
- Zeichenstapel: Der Server fasst aufeinanderfolgende Zeichenaufrufe zu einem Stapel zusammen,
  solange dazwischen nur Anfragen kommen, die an der Zeichnung nichts ändern. Im Match von Worms 3D
  lag vor jedem Zeichenaufruf ein Durchschalten aller acht Textureinheiten
  (glActiveTexture/glClientActiveTexture), das den Stapel jedes Mal ausgab: ~500 Treiberaufrufe je
  Bild. Diese Wechsel zählen nicht mehr als Grenze (PC: ~150 statt ~430 Stapel).
  Vorher nötig: Feste Farbe/Normale/Texturkoordinate zwischen zusammengefassten Aufrufen und
  Aufrufe mit und ohne Array im selben Stapel wurden falsch gezeichnet (`batchtest.c`, sechs von
  acht Fällen); die Attribute bleiben jetzt auf die Vertices ausgerichtet (`gl_renderer.c`).
  Außerdem geben Zustandsänderungen, die nur Server-Zustand betreffen (Matrizen, Licht, Material,
  Texturumgebung, Alphatest, Schalter des festen Funktionsmodells), den Stapel nicht sofort aus:
  Der alte Inhalt wird gemerkt und erst vor dem nächsten Zeichenaufruf verglichen
  (`deferStateChange` in `gl_context.c`). Worms 3D setzt vor fast jedem Objekt Matrix, Licht und
  Material neu, oft auf dieselben Werte; dann läuft der Stapel weiter. Treiberzustand
  (`glEnable(GL_BLEND)`, `glBindTexture`) wird nur übersprungen, wenn er schon so ist. Die
  Statistikzeile `gladio: Stapel ausgegeben vor: …` zeigt, was die Stapel noch trennt (Worms 3D:
  vor allem Texturwechsel).
- Client (`patch-gladio-client.py`): Einheitenwechsel gehen erst vor der nächsten anderen Anfrage
  und nur bei Änderung hinaus (Worms 3D: ~8400 von 13000 Anfragen je Bild entfallen);
  Matrixabfragen (`glGetFloatv(GL_MODELVIEW_MATRIX)`, 29-mal je Bild) beantwortet der Client aus
  mitgeführten Matrixstapeln, statt jedes Mal auf den Server zu warten; `glXSwapBuffers` wartet
  nur noch auf das vorige Bild, so dass das Spiel das nächste Bild vorbereitet, während der Server
  das letzte zeichnet (`GLADIO_SYNC_SWAP=1` in den Umgebungsvariablen des Containers: wieder auf
  jedes Bild warten). Nebenbei: `glGetDoublev` schrieb Matrizen (16 Werte) in eine einzelne
  Variable, `glGetIntegerv(GL_MATRIX_MODE)` lieferte unbestimmte Werte (ES kennt die Abfrage nicht).
- Bildwechsel-Takt (Vsync-Nachbildung): Wine gibt jedem Fenster Swap-Intervall 1 vor, wined3d setzt 0
  nur bei `D3DPRESENT_INTERVAL_IMMEDIATE`; winex11 gibt das aber nur weiter, wenn GLX
  `GLX_EXT_swap_control` (oder SGI/MESA) nennt. Der Gladio-Server nannte keine davon, jedes Bild
  ging sofort hinaus – 2D-Spiele, die je Bild weiterzählen, liefen so schnell wie die GPU hergab
  (Tiny Tiger: der Abspann rast). Jetzt nennt der Server (`GLXExtension.java`, PC: `server_main.c`)
  die drei Erweiterungen, der Client beantwortet `glXSwapIntervalEXT/SGI/MESA` selbst und hält in
  `glXSwapBuffers` bei Intervall n höchstens Bildrate/n Bilder je Sekunde ein (`GLADIO_REFRESH_HZ`,
  Standard 60; `GLADIO_NO_VSYNC=1` in den Umgebungsvariablen des Containers lässt wieder jedes Bild
  sofort hinaus). Spiele, die Vsync selbst abschalten, bleiben ungebremst. PC-Geschirr: Direct3D-9-
  Testprogramm mit `D3DPRESENT_INTERVAL_ONE` 60 Bilder/s (30 mit `GLADIO_REFRESH_HZ=30`, 209 mit
  `GLADIO_NO_VSYNC=1`), mit `INTERVAL_IMMEDIATE` unverändert 200+; Regressionslauf gleich.
- Unveränderte Zustände und Uniform-Werte (Outlast): Von 41500 Anfragen je Bild setzten 38 % nur den
  schon gültigen Zustand (wined3d bindet vor jedem Zeichenaufruf Texturen, Sampler, Zeiger und
  Schalter neu), und `glUniform4fv` trug 6,5 MB je Bild – wined3d lädt bei jedem Zeichenaufruf die
  Shader-Konstanten vom kleinsten bis zum größten seit dem letzten Laden gesetzten Register am
  Stück (118 bzw. 244 vec4), auch wenn sich fast nichts geändert hat. Der Client
  (`patch-gladio-client.py`) lässt jetzt auch `glBindTexture` je Einheit, `glVertexAttribPointer`
  aus einem Puffer je Vertex-Array-Objekt, `glEnable/glDisable(i)`, `glColorMask(i)` und
  `glVertexAttrib4f/4Nubv` mit unverändertem Wert weg und vergleicht `glUniform*`-Werte je
  Programm mit den zuletzt gesendeten: nur die geänderten Läufe gehen hinaus (PC-Testprogramm
  `d3d9const.c`, 100 Zeichenaufrufe mit je 118+96 Konstanten: 3 statt 334 KB je Bild). Die
  Arraygrenzen kennt er aus der Uniform-Tabelle (GL ignoriert Werte dahinter); `glLinkProgram` und
  Löschen verwerfen die Werte. Unbekannt werden die Zustände, wo der Server sie anders ändern kann:
  `glPopAttrib`/`glPopClientAttrib`, gelöschte Objekte (Zähler, auch für andere Kontexte der
  Gruppe), Arrays des festen Funktionsmodells und Direktmodus (dafür setzt der Server beim Zeichnen
  selbst Treiberattribute). `GLADIO_NO_STATE_FILTER=1` bzw. `GLADIO_NO_UNIFORM_DIFF=1` in den
  Umgebungsvariablen des Containers schalten das ab; die Laufstatistik zeigt „Client je Bild: …
  gleiche Zustände nicht gesendet; Uniforms: … verglichen, davon … gesendet“. Testprogramm
  `statetest.c` (PC-Geschirr): 50 Zellen, die je einen Filter und seine Ungültigmachung prüfen –
  gleiches Bild mit und ohne Filter und wie Mesa nativ. Dazu im Server: Texturbindungen werden für
  32 statt 8 Einheiten geführt (`MAX_TEXTURE_UNITS`; vorher teilten sich alle Einheiten ab 7 den
  Platz 7, ein DSA-Aufruf oder `glDeleteTextures` konnte dort die falsche Textur zurückbinden), und
  Ansicht/Schere stehen nach einem Kontextwechsel nicht mehr auf dem ganzen Fenster (der
  Anzeigepuffer wird dann neu angelegt; in GL gelten die Werte des Programms weiter).
- Outlast-Intro grau (Bink-Videos, Unreal-Logo) mit dem Bindungsfilter: Bei der Bildausgabe bindet die
  Anzeige-Seite (`Texture.java`) ihre Fenstertextur im Kontext des Programms. Legt sie sie gerade
  erst an (erstes Bild nach `glXMakeCurrent`, neue Fenstergröße), schaltet sie dazu auf
  Textureinheit 0 um, bindet dort und löst die Bindung und stellt `GL_UNPACK_ALIGNMENT` auf 1 – das
  galt danach weiter: der Server band die Textur der vermeintlich aktiven Einheit (bei den Videos
  die Chroma-Ebene der Einheit 2) auf Einheit 0, deren eigene (die Y-Ebene) war weg, und weil
  wined3d sie jedes Bild gleich bindet, schickte der Client die Bindung nicht mehr. Vorher deckten
  die unnötigen `glBindTexture` je Zeichenaufruf das ab. `Texture.copyFromReadBuffer` stellt
  Einheit, Ausrichtung und die Bindung der Einheit 0 jetzt selbst wieder her, und der Server
  (`swapDisplayBuffers`) ebenso aus seiner Buchführung. Das PC-Geschirr führt die Fenstertextur
  seither wie das Gerät (`server_main.c`), Testprogramm `d3d9units.c` (drei Sampler wie der
  YUV-Shader, erwartet jedes Bild Rot).
- Objekte, die ein Bild lang verschwinden (Outlast: Torflügel, Mauern): Die Unreal Engine 3 lässt
  ein Objekt weg, wenn seine Verdeckungsabfrage des vorigen Bilds 0 Abtastwerte meldet. Zum
  Eingrenzen zwei Schalter im Client (Umgebungsvariablen des Containers):
  `GLADIO_QUERY_ALWAYS_VISIBLE=1` beantwortet Verdeckungsabfragen ohne Rundreise mit „fertig, 255“
  (die Engine hält alles für sichtbar – verschwinden Objekte trotzdem, liegt es nicht an den
  Abfragen), `GLADIO_NO_QUERY_SHORTCUT=1` schickt jede Abfrage und jede Fence als Rundreise zum
  Server wie vor den Abkürzungen (verschwinden sie dann nicht mehr, steckt es in der Abkürzung).
  Der Server sieht vor `GL_QUERY_RESULT` jetzt selbst nach, ob das Ergebnis vorliegt (höchstens
  50 ms), und ein Ergebnis, das der Treiber nicht liefert (Fehler: Abfrage noch aktiv, fremder
  Kontext), gilt als sichtbar statt als 0.
- Mausbewegungen: Jedes MotionNotify/RawMotion kostet unter Box86 Umläufe zum wineserver (alle
  emuliert); bei 100+ Ereignissen je Sekunde wurde Worms 3D im Menü deutlich langsamer („Warten
  auf den Client“ 6 → 40 ms). Die 32-Bit-Variante fasst Bewegungen im X-Server auf höchstens
  ~60 Ereignisse je Sekunde zusammen (`XServer.java`, `BuildConfig.X86_ONLY`).
- Bild zusammensetzen bei Mausbewegungen: Der X-Server-Renderer (`GLRenderer.java`) zeichnete
  bei jeder Bewegung das ganze Bild neu (Swap-Intervall 0, also auch öfter als 60-mal je
  Sekunde), auch wenn kein Mauszeiger zu sehen ist – Worms 3D blendet den X-Zeiger aus und
  zeichnet seinen eigenen. Während der Bewegungen stieg auf der Mali-400 die Zeit der
  Zeichenaufrufe des Spiels (Menü: „Zeichnen“ 6,7 → 22 ms, 42 → 22 Bilder/s) – vermutlich, weil
  sich beide die GPU teilen (noch nicht nachgemessen). Die 32-Bit-Variante zeichnet bei Bewegungen
  nur noch neu, wenn ein sichtbarer Zeiger im Spiel ist (an der neuen Position oder im zuletzt
  gezeigten Bild) oder Lupe/Bildversatz dem Zeiger folgen. Alle 5 s steht im Log eine Zeile
  `xserver: … Bilder zusammengesetzt …, Mausbewegungen … (davon … ohne Neuzeichnen)`.
- Beim Ende einer Client-Verbindung (auch nach einem Absturz des Programms) stehen die letzten
  64 GL-Anfragen im Protokoll (`gladio: letzte Anfragen …`).
- Stürzt der Server selbst ab (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT in der App), schreibt er vorher
  einen Bericht ins Protokoll (`gladio-crash: …`, landet in `logcat.txt`): Fehleradresse, Register,
  Bibliothek+Versatz von PC/LR mit dem nächsten exportierten Symbol, Rücksprungadressen vom Stack,
  im Thread einer Verbindung die laufende und die letzten Anfragen, im X-Server-Thread die Phase
  (`createGLXContext`/`destroyGLXContext`). Androids `crash_dump` liefert auf dem 3.0-Kernel des
  Note 8.0 keinen Stack. Der ungekürzte Server (`libgladiorenderer-armv7-symbole.so.xz` im Release
  `armv7-apk`, gleiche Build-ID wie im APK) löst die Versätze mit `llvm-addr2line -f -e` auf.

**Gladio auf 64 Bit (`arm64`, `armv7hybrid`):** Mehrere der oben beschriebenen Fehler stecken im
Original-Gladio und betreffen auch 64-Bit-Geräte (ES-3-Pfad). Behoben sind dort jetzt: die
Festfunktions-Beleuchtung (`fftest`), die Zeichenstapel mit verrutschten oder falschen Attributen
(`batchtest`, vorher auch auf ES 3 sechs von acht Fällen falsch) samt aufgeschobenem
Zustandsvergleich und Einheitenwechseln ohne Stapelgrenze, `glGetIntegerv(GL_MATRIX_MODE)`. Den
64-Bit-Client baut die CI mit denselben Anpassungen wie den armhf-Client
(`scripts/gladio-client-arm64.sh`, aarch64-gcc von Ubuntu 24.04, glibc 2.39 wie die Root-FS des
Originals) und ersetzt damit `graphics_driver/gladio-1.1.tzst`; daneben liegt eine Kennung
(`gladio-1.1.id`), damit ein neuer Client auch bei gleicher Versionsnummer entpackt wird
(`XServerDisplayActivity.extractGraphicsDriverFiles`). Die CI veröffentlicht das 64-Bit-APK als
`Winlator-11.2-arm64.apk` im Release `armv7-apk`; es ist mit demselben festen Schlüssel signiert
wie die 32-Bit-APKs (ein installiertes Original-Winlator muss dafür vorher entfernt werden).
Was nur die Mali-400 braucht (spezialisierte ES-2.0-Shader, Bildübergabe ohne Kopie, kein
`glFlush` je Stapel, Client-Arrays statt Pufferobjekte), bleibt auf den ES-2.0-Pfad beschränkt.

**Software auf 64 Bit (`arm64`, `armv7hybrid`):** Der OpenGL-Treiber „Software“ (Mesa llvmpipe)
gibt es auch mit der originalen 64-Bit-Root-FS, vor allem als Gegenprobe: Sieht ein Programm mit
Gladio anders aus als mit Software, liegt der Fehler in Gladio oder im GPU-Treiber. Das Paket
`graphics_driver/llvmpipe-23.1.9.tzst` (`./usr/lib/libGL.so.1.7.0`, daneben `.id` mit der
Prüfsumme) baut `scripts/mesa-llvmpipe-arm64.sh` aus demselben Mesa-Stand wie die VirGL-Bibliothek
des Originals (mesa3d-custom, `virgl-23.1.9`, GLX über Xlib), aber nur mit swrast (llvmpipe und
softpipe), eingebautem glapi und statisch gelinktem LLVM 15; dazu `patches/mesa/0009` (die
`WINLATOR_LP_*`-Stellschrauben). Die VirGL-Bibliothek des Originals bleibt unverändert. Gebaut wird
nativ auf dem arm64-Runner (Ubuntu 24.04: glibc 2.39 und libstdc++ 6.0.32 wie die Root-FS); das
Skript prüft jede benötigte Bibliothek samt Symbolversionen gegen die Root-FS des Originals und
zeichnet zur Probe auf Xvfb ein Dreieck. Die Root-FS hat keine libtinfo, LLVM fragt darüber nur
nach Terminalfarben: ein Ersatz ohne Terminal kommt statisch in die libGL. Fehlt das Paket (Build
ohne CI), bietet die App den Treiber nicht an und nimmt für so eingestellte Container Gladio.

**Stapel des Hauptthreads (Kernel mit 3G/1G-Aufteilung, z. B. Android 9 auf dem Galaxy Note 8.0):**
Box86 führt den nativen Code des Hauptthreads (eingebundene Bibliotheken wie die libGL mit
llvmpipe/LLVM) auf dem nativen Hauptstapel des Prozesses aus; Wine sieht nur Box86s eigenen
x86-Stapel. Liegt der native Stapel oberhalb von 2 GB (bei 3G/1G knapp unter 0xbf000000),
reservierte Wine alles bis 64 KB unter ihn, der Stapel konnte nicht mehr wachsen und hatte etwa
200 KB. LLVM lief darüber hinaus (SIGSEGV in `memset` knapp unter dem Stapel); das Programm fing
die Ausnahme ab, der GL-Aufruf brach jedes Mal mitten in der Shader-Übersetzung ab, llvmpipe
legte bei jedem Zeichenaufruf eine neue Variante an, kein Bild wurde fertig, und nach etwa 1400
Varianten war der Speicher voll. Unter qemu liegt der Stapel unter 2 GB und ist von Anfang an
8 MB groß, dort trat das nie auf. In Wine selbst lässt sich das nicht beheben: Box86 zeigt dem
x86-Code in `/proc/self/maps` nur das Programm und den x86-Stapel, der native Stapel ist für
Wine unsichtbar. Darum zwei Absicherungen:
- `patches/box86/0002-native-main-stack.patch`: Box86 lässt beim Start von Wine-Prozessen den
  nativen Stapel auf 8 MB wachsen (knapp unter `RLIMIT_STACK`), bevor Wine reserviert. Dafür
  liest der Kernel einmal an der untersten Adresse (`write()` aus dem Speicher in eine Pipe);
  belegt wird nur Adressraum, Speicher kostet erst eine tatsächlich beschriebene Seite. Mit
  „Box86 Logs“ steht das Ergebnis im Box86-Protokoll (`BOX86: native stack grown from … KB to
  … KB`). `BOX86_NATIVE_STACK=<MB>` ändert die Größe, `0` schaltet es ab.
- `patches/mesa/0004-llvm-stack-thread.patch`: llvmpipe prüft vor jeder Übersetzung den freien
  Stapel des aufrufenden Threads (`pthread_getattr_np`, beim Hauptthread durch den Bereich
  darunter begrenzt). Sind es unter 2 MB, übersetzt LLVM in einem Hilfsthread mit 8 MB Stapel,
  der Aufrufer wartet. Das hilft auch bei Wine-Threads mit kleinem Stapel (z. B. dem
  Befehls-Thread von WineD3D). `WINLATOR_LLVM_THREAD=1` erzwingt den Hilfsthread, `0` schaltet
  ihn ab. Beim ersten Mal steht eine Zeile `[diag] llvmpipe: … Hilfsthread …` in `start.txt`.

Beim Laden schreibt die libGL außerdem `[diag] libGL geladen: Hauptstapel <von>-<bis> (<Größe>)`
nach `start.txt`; mit Box86s Vergrößerung stehen dort knapp 8 MB, ohne sie etwa 190 KB.

## Testen auf dem Gerät

1. APK aus dem Release `armv7-apk` installieren: `Winlator-11.2-armv7.apk` für alle 32-Bit-Geräte,
   `Winlator-11.2-armv7hybrid.apk` nur bei 64-Bit-Kernel (`uname -m` zeigt `armv8l` oder `aarch64`)
2. Beim ersten Start wird das Root-FS entpackt (einige Minuten), danach einen Container anlegen
   (Vorgaben passen sich an: ohne OpenGL ES 3 „Software“, ohne Vulkan WineD3D)
3. Container starten: es sollte der Wine-Desktop mit dem Dateimanager (wfm) erscheinen.
   Spiele brauchen meist einen **beschreibbaren** Spielordner (Spielstände, Protokolle). Auf der
   SD-Karte darf eine App unter Android ab 4.4 nur in `Android/data/com.winlator/files/` schreiben;
   ein Laufwerk, das woanders auf die Karte zeigt, ist für Wine schreibgeschützt. Beispiel
   Worms 3D: Menüs laufen, beim Laden eines Matches Absturz (`Unhandled page fault … at address
   0061B6F4`), weil `AIPlanLog.txt` nicht angelegt werden kann – auf dem PC mit schreibgeschütztem
   Spielordner genau so nachgestellt.
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
   `modul+offset`), die Zeilen beginnen mit `wine:` und stehen in `start.txt`. Dazu die Bytes
   rund um den Absturzbefehl (`code`, ein `|` markiert die Absturzadresse), der Speicher, auf
   den die Register zeigen (Text oft gleich lesbar, z. B. Erweiterungsnamen), der Stapel und
   der Windows-Adressraum (belegt, reserviert, frei, größter freier Block), um
   Speicheranforderungen, die an fehlendem Adressraum scheitern, zu erkennen.

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
   erste Bild und alle 30 s die Bilder pro Sekunde (nach 5 Minuten alle 5 Minuten), jeweils mit
   einer Zeile `CPU je Thread`: die Threads mit dem größten Anteil eines Kerns seit der letzten
   Zeile (z. B. das Spiel samt Box86, `llvmpipe-0` … `llvmpipe-3`), der Prozess zusammen und,
   falls `/proc/stat` lesbar ist, das ganze System (auch X-Server, wineserver, Ton). Dazu
   Ranglisten, wo die Threads gerade rechnen (`… stecken in, N Stichproben: …`): ein Thread
   `winlator-prof` liest 50-mal pro Sekunde den Programmzähler der Mal-Threads und der übrigen
   Threads mit mindestens 5 % CPU (Kernel 3.0: Feld kstkeip in `/proc/self/task/*/stat`; liefert
   der Kernel nur 0, holt ein Echtzeitsignal den Programmzähler aus dem Kontext). Einträge:
   übersetzte Funktion (`fs319_variant2:whole`, Namen meldet `patches/mesa/0006`), `libGL+Offset`
   (256-Byte-Abschnitte, über die Symboldatei den Funktionen zuzuordnen),
   Bibliothek mit Symbol, `anonym` (meist Box86-Dynarec). `WINLATOR_PROFILE=0` schaltet das ab.
   Für die drei meistgenannten übersetzten Funktionen folgt je eine Zeile mit den heißen
   Stellen in 32-Byte-Abschnitten (`+0x1a40 12 %`), die `tools/jithot.py` (Sitzungswerkzeug)
   mit dem Code-Dump zu Befehlsfolgen auflöst.
   `WINLATOR_JIT_DUMP=1` (oder ein absoluter Ordner) schreibt für jede übersetzte Funktion das
   LLVM-IR nach der Optimierung (`<Modul>.ll`) und den Maschinencode (`<Modul>.<Funktion>.bin`,
   bis 64 KB, `index.txt` mit Adressen) nach `Documents/Winlator/jit` und schaltet dafür den
   Shader-Cache ab; den Code zeigt `arm-linux-gnueabihf-objdump -D -b binary -marm`. Ein
   Wachhund-Thread meldet Stillstände – eine Übersetzung dauert über 15 s, seit 15 s kein neues
   Bild, 30 s nach dem ersten Shader noch kein Bild – und gibt dann für jeden Thread Zustand,
   Wartestelle im Kernel, CPU-Zeit, Programmzähler und die Rücksprünge in die libGL aus
   (höchstens dreimal je Prozess; Kernel 3.0 liefert dafür Stapel- und Befehlszeiger in
   `/proc/self/task/*/stat`). Bekommt ein Fragment-Shader mehr als vier Varianten, steht
   für die nächsten (höchstens 40 Meldungen) da, in welchen Bytes sich der Variantenschlüssel
   von der ähnlichsten vorhandenen Variante unterscheidet (Aufbau des Schlüssels im Kommentar
   von `winlator_diag_fs_variant`); auf dem Galaxy Note 8.0 bekam beim SKIDROW-Intro ein
   einziger Shader über 1400 Varianten, bis der Speicher ausging (Ursache: zu kleiner Stapel des
   Hauptthreads, LLVM lief über; siehe „Stapel des Hauptthreads“). Bricht eine Übersetzung ab, steht beim
   nächsten Beginn, nach welchem Schritt (JIT-Engine, Optimierung, Maschinencode …). Stürzt
   Code in der libGL ab (Signal mit Programmzähler oder Rücksprungadresse in der libGL), meldet
   sie Signal, Adresse und Aufrufkette und reicht das Signal dann an Box86 weiter (höchstens
   dreimal je Prozess); Box86 selbst zeigt mit `BOX86_SHOWSEGV=1` jeden Speicherzugriffsfehler,
   auch wenn das Programm einen Handler hat. `WINLATOR_DIAG=0` schaltet den Fortschritt und den
   Signalabfang ab.

   Mit dem OpenGL-Treiber **Gladio** meldet der Server (alle Varianten, auch arm64) in
   `logcat.txt`, was das Programm selbst nie erfährt: Shader, die der Treiber nicht übersetzt,
   und Programme, die er nicht bindet (Fehlertext; der Quelltext nach der Umsetzung für OpenGL ES
   landet als `gladio-shaderfehler-N.txt` bzw. `gladio-bindefehler-N.txt` neben `start.txt`),
   GL-Fehler (geprüft nach jedem Zeichenaufruf und beim Bildwechsel; nach einem Fund zwei Bilder
   lang nach jeder Anfrage, dann mit der auslösenden Anfrage und ihren Argumenten) und
   unvollständige Zeichenziele (FBO mit Anhängen, Formaten und Größen). Die 5-s-Statistik zählt
   diese Fehler mit. Fehlt etwas im Bild, zeichnet der Menüpunkt **„Bild aufzeichnen (Gladio)“**
   (seitliches Menü im laufenden Container) das nächste vollständige Bild nach
   `Documents/Winlator/gladio-JJJJMMTT-HHMMSS/` auf: `anfragen.txt` mit jeder Anfrage samt
   Argumenten und GL-Fehlern, zu jedem Zeichenaufruf Programm, Sampler mit Textur, Format und
   Filter, Vertexattribute, Zustand (Mischen, Masken, Tiefe, Stencil, Ausschnitt) und Uniforms;
   `bild-NNN.png` mit dem Ergebnis jedes Durchgangs (links Farbe, rechts Alpha, bei Werten über 1
   logarithmisch; Wertebereiche je Kanal in `anfragen.txt`); `shader.txt` mit den Quelltexten der
   benutzten Programme. Das ersetzt am Gerät ein Werkzeug wie RenderDoc; am PC dasselbe mit
   `GLADIO_PC_CAPTURE_AT` (`tools/gladio-pc/README.md`).

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
- Vortek braucht Vulkan auf dem Gerät (und Android 8), VirGL braucht OpenGL ES 3.x; Gladio läuft
  auf ES 2.0 über den beschriebenen ES-2.0-Pfad (mit dessen Grenzen)
- Software-Rendering bleibt Software-Rendering: llvmpipe ist um ein Vielfaches schneller als
  softpipe (auf dem Galaxy Note 8.0 schaffte softpipe beim SKIDROW-Intro, 1024×768, etwa
  0,5 Bilder pro Sekunde), ersetzt aber keine GPU
- PulseAudio gibt es für armv7 noch nicht (ALSA ist ohnehin voreingestellt)
