# gladio-pc – der Gladio-Server auf dem PC

Gladio ist Winlators OpenGL-Weiterleitung: Das Windows-Programm (bzw. Wine) ruft in der
Wine-Umgebung eine `libGL`, die jeden GL-Befehl in einen Ringpuffer im geteilten Speicher
schreibt (**Client**, Quelltext: github.com/brunodev85/gladio). In der App führt
`libgladiorenderer.so` die Befehle auf einem echten EGL/GLES-Kontext aus und kopiert das
fertige Bild in die Textur des X-Fensters (**Server**, Quelltext:
`app/src/main/cpp/gladiorenderer`). Der Handschlag läuft über den X11-Socket: Der Client
schickt GLX-Anfragen mit dem Haupt-Opcode −106 an Winlators Java-X-Server
(`GLXExtension.java`), der den nativen Server per JNI aufruft.

Dieses Verzeichnis baut beide Seiten nativ für den PC (x86-64, Mesa), damit man den
Server ohne Gerät testen und umbauen kann – insbesondere für den GLES-2.0-Pfad
(Mali-400 im Galaxy Note 8.0, kein GLES 3). Ein kleiner X-Server-Nachbau in C
(`server_main.c`) ersetzt dabei den Java-Teil:

* beantwortet die X11-Verbindungsaufnahme und die GLX-Anfragen byteidentisch zu
  `GLXExtension.java`,
* stellt dem Server die JNI-Rückrufe bereit (`getWindowSize` fragt den echten X-Server
  unter `$DISPLAY`, `updateWindowContent` liest das Bild mit `glReadPixels` aus und
  schreibt auf Wunsch PPM-Dateien, `getGLXContextPtr`),
* legt den „App-Kontext“ an, mit dem die GLX-Kontexte ihre Objekte teilen (auf dem Gerät
  der Kontext der `GLSurfaceView`).

Der Server-Quelltext wird unverändert aus `app/src/main/cpp` übersetzt (`-DGL_SERVER=1`,
`shim/android/log.h` ersetzt `<android/log.h>`), der Client aus dem Gladio-Repo mit denselben
Anpassungen wie der armhf-Client in der Root-FS (`armv7/scripts/patch-gladio-client.py`:
8-Byte-`long` im Protokoll, Socket-Pfad über `GLADIO_SOCKET`, GL-Funktionen über
`glXGetProcAddressARB`, Visuals in der Standardtiefe, `glLightModelfv`). `make client32` baut
zusätzlich einen i386-Client für Wine (32 Bit) mit auf den 64-Bit-Server ausgerichtetem
Ringpufferkopf (`--align64`).

## Bauen

Voraussetzungen: gcc, Mesa-Entwicklungsdateien (EGL, GLESv2, `GL/glx.h`), libX11-dev, ein
JDK (für `jni.h`), Xvfb oder ein anderer X-Server für das Fenster des Testprogramms.
Auf Arch/CachyOS: `mesa libglvnd libx11 jdk-openjdk xorg-server-xvfb`.

    make                              # klont den Gladio-Client nach build/gladio-src
    make GLADIO_SRC=/pfad/zu/gladio   # vorhandener Klon
    make DEBUG_MODE=0                 # ohne glGetError-/Shader-Protokoll (schneller)

Ergebnis: `build/gladio-pc-server` und `build/client/libGL.so.1` (`make client32`:
`build/client32/libGL.so.1`, braucht `gcc -m32` und `libX11.so.6` für i386).

## Ausführen

    gcc -O1 -o glrender2 glrender2.c -lX11 -lGL          # Testprogramm (Textur, Alphatest, Licht, Nebel)
    Xvfb :0 -screen 0 1024x768x24+32 &                    # falls kein X-Server läuft
    DISPLAY=:0 ./run.sh --es 3 --dump /tmp/bilder -- ./glrender2 1 out.ppm 2
    DISPLAY=:0 ./run.sh --es 2 -- ./glrender2 1 out.ppm 2

`run.sh` startet den Server, setzt `LD_LIBRARY_PATH` auf den Gladio-Client und
`GLADIO_SOCKET`, führt das Programm aus und gibt zum Schluss das Serverprotokoll aus.
`--es 2` erzwingt bei Mesa einen reinen ES-2.0-Kontext (`MESA_GLES_VERSION_OVERRIDE=2.0`,
`MESA_GLSL_VERSION_OVERRIDE=100`) – das ist die Mali-400-Situation.
Mit `--dump VERZ` landen die Bilder, die der Server bei `glXSwapBuffers` fertigstellt, als
`frame-N.ppm` dort (`--frames 1,10` wählt die Bildnummern, `all` alle).

Umgebungsvariablen des Servers: `GLADIO_SOCKET`, `GLADIO_PC_ES` (2|3), `GLADIO_PC_DUMP`,
`GLADIO_PC_DUMP_FRAMES`, `GLADIO_PC_SIZE` (Fenstergröße ohne `$DISPLAY`), `EGL_PLATFORM`
(`x11` mit laufendem X-Server, sonst `surfaceless`). Vom Server selbst (gelten auch in der
App): `GLADIO_GLES=2|3` erzwingt den Pfad, `GLADIO_GLES_EXTENSIONS="GL_OES_… …"` ersetzt die
Erweiterungsliste des Treibers (z. B. die der Mali-400, um deren Lücken – kein VAO, kein
glMapBuffer, keine 32-Bit-Indizes – mit Mesa nachzustellen), `GLADIO_PBUFFER=1` erzwingt den
Pbuffer-Pfad für Treiber ohne `EGL_KHR_surfaceless_context`.

Windows-Programme über Wine: `make client32`, dann z. B.

    GLADIO_SOCKET=/tmp/w3d.sock GLADIO_PC_ES=2 GLADIO_GLES=2 GLADIO_PBUFFER=1 EGL_PLATFORM=x11 \
        MESA_GLES_VERSION_OVERRIDE=2.0 MESA_GLSL_VERSION_OVERRIDE=100 ./build/gladio-pc-server &
    GLADIO_SOCKET=/tmp/w3d.sock LD_LIBRARY_PATH=$PWD/build/client32 WINEARCH=win32 wine bin/Worms3D.exe

(Wine lädt seine `libGL.so.1` über `LD_LIBRARY_PATH`; die Xlib-Seite geht an den X-Server
unter `$DISPLAY`. Worms 3D muss aus seinem Spielverzeichnis gestartet werden, sonst stürzt es
beim Start ab. Mit `GLADIO_GLES_EXTENSIONS="$(cat mali-exts.txt)"` – der Erweiterungsliste
aus dem Geräteprotokoll – sieht der Server dieselben Lücken wie auf der Mali-400.)

Wer eigene Programme testet, muss im Client-Programm nur `LD_LIBRARY_PATH` auf
`build/client` und `GLADIO_SOCKET` auf den Socket des Servers zeigen lassen; Xlib geht
weiter an den echten X-Server unter `$DISPLAY`, nur die GL-Befehle laufen durch Gladio.

## Testprogramme

* `glrender2.c` – festes Funktionsmodell: Textur mit Mipmaps, Alphatest, Licht, Nebel
  (`./glrender2 MODUS out.ppm FILTER`, Modus 0–3).
* `glsltest.c` – Shader im wined3d-Stil: GLSL 1.20 mit gl_Vertex/gl_TexCoord/gl_FragData/
  gl_Fog, GLSL 1.30 mit in/out und texture(), VBO/IBO, D3DCOLOR-Attribute, Framebuffer-
  Objekt (EXT), Alphatest, Blending, BGRA-Rücklesen (`./glsltest out.ppm`).
  Bauen: `gcc -O1 -o glsltest glsltest.c -lX11 -lGL -ldl`.

* `d3d9test.c` – Direct3D 9 über wined3d (Texturformate A8R8G8B8/R5G6B5/A1R5G5B5/A4R4G4B4/L8/
  DXT1 mit Mipmaps, festes Licht, Nebel, Alphatest, Blending, vs_2_0/ps_2_0, Render-to-Texture,
  Rücklesen als PPM). Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9test.exe d3d9test.c -ld3d9
  -lgdi32 -luser32`; laufen lassen mit dem Wine 10.10 (i386) aus der Root-FS des Releases
  `armv7-rootfs` (`armv7-assets.tar` → `rootfs.tzst` → `opt/wine/bin/wine`), das auf dem PC
  direkt startet – so prüft man genau das wined3d, das auch auf dem Gerät läuft.

Vergleich zweier PPM-Bilder: `python3 ppmdiff.py a.ppm b.ppm` (größte Abweichung je
Kanal und Anteil abweichender Pixel).

## Stand

* ES 3.2 (Kontrolllauf, llvmpipe): `glrender2` Modus 0 und 1 liefern durch Gladio
  pixelgenau dasselbe Bild wie direkt mit llvmpipe (`frame-N.ppm` gegen `out.ppm`).
  Modus 2 (festes Licht) weicht ab: Gladio liefert für beleuchtete Pixel Schwarz – ein
  Verhalten des Gladio-Servers selbst, unabhängig von dieser Portierung (Wine/wined3d
  rechnet Beleuchtung in eigenen GLSL-Shadern, nutzt das feste GL-Licht also nicht).
* ES 2.0 (`GLADIO_GLES=2`, Mesa auf ES 2.0 gezwungen): beide Testprogramme liefern dasselbe
  Bild wie auf ES 3.2 bzw. direkt mit llvmpipe (Abweichung höchstens 1/255). Der Server
  meldet dem Client OpenGL 2.1 / GLSL 1.20; die Shader werden nach GLSL ES 1.00 übersetzt.
  Einzelheiten zum Pfad: `app/src/main/cpp/gladiorenderer/include/gl_es2.h`.
* Nebenbefunde aus den Kontrollläufen (in beiden Pfaden behoben): festes Licht war schwarz,
  wenn die Anwendung kein glMaterial aufrief (kein Vorgabematerial); das Umgebungslicht der
  Szene (GL_LIGHT_MODEL_AMBIENT, Vorgabe 0.2) fehlte; glReadPixels mit GL_BGRA vertauschte
  Rot und Blau falsch (In-place-Tausch); glLightModelfv/-iv wurden vom Client nicht übertragen.
* Mit der Erweiterungsliste der Mali-400 (Galaxy Note 8.0) und erzwungenem Pbuffer-Pfad liefern
  beide Testprogramme weiterhin dasselbe Bild; Worms 3D (eigener OpenGL-Renderer des Spiels,
  Wine 9 i386 über `build/client32`) zeigt über ES 2.0 den Titelbildschirm korrekt (llvmpipe:
  ~20 Bilder/s, wie über ES 3.2).
* Auf dem Galaxy Note 8.0 selbst scheiterte der erste Versuch an `eglMakeCurrent` ohne
  Oberfläche (`EGL_BAD_MATCH`: die Mali-400-EGL kennt `EGL_KHR_surfaceless_context` nicht) –
  daher der Pbuffer-Pfad. Mit ihm läuft Worms 3D auf dem Gerät durch (ES-2.0-Kontext, keine
  Shader-Fehler), war aber langsamer als llvmpipe. Dagegen (Statistik alle 5 s im Protokoll,
  siehe `armv7/README.md`): spezialisierte Fixed-Function-Shader statt Uniform-Verzweigungen
  (`shader_material_es2.c`; `glrender2` Modus 0–2 bleiben pixelgenau gegen Mesa) und kein
  `glFlush()` mehr nach jedem Zeichenstapel (bei Worms 3D ~60 je Bild – auf einem
  Kachel-Renderer rendert jeder Flush das Bild bis dahin). Worms 3D sendet je Bild ~1950
  Anfragen mit ~1,3 MB Vertexdaten (62 Zeichenaufrufe mit Client-Arrays, vom Client je Index
  ausgepackt), ~500 glActiveTexture/glClientActiveTexture und 10 Antworten (glGetFloatv).
* wined3d (Wine 10.10, `d3d9test`): Mit `GL_ARB_buffer_storage` in der Erweiterungsliste legt
  wined3d alle Puffer als `GL_PIXEL_UNPACK_BUFFER` an, für die der Server nur Schattenspeicher
  hat – nichts wurde gezeichnet (auch auf dem unveränderten ES-3-Pfad). Ohne die Erweiterung
  rendert der ES-3-Pfad pixelgenau wie Mesa; dazu kamen zwei Formatfehler (L8-Texturen rot:
  `glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA)` übertrug nur einen Wert; DXT-Texturen ohne
  Filterung: `glGetInternalformativ` meldete für komprimierte und alte Formate FILTER=NONE).
  Der ES-2.0-Pfad braucht außerdem getrennte Lese-/Zeichen-Framebuffer, `glBlitFramebuffer`
  (Bildausgabe), gleich große FBO-Anhänge (eigener Tiefenpuffer für kleine Render-Targets,
  Löschungen werden weitergereicht), `GL_TEXTURE_MAX_LEVEL` (sonst unvollständige Texturen,
  schwarz), begrenzte Vertexfarben (altes GL klemmt gl_FrontColor auf [0,1]) – alles in
  `gl_framebuffer.c`/`gl_renderer.c`/`shader_converter.c` nachgebildet; Ergebnis pixelgenau
  bis auf den Alphatest, den wined3d 10.10 auf alten Kontexten nie einschaltet
  (`armv7/patches/wine/0002-wined3d-legacy-alpha-test.patch`).
