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
`shim/android/log.h` ersetzt `<android/log.h>`), der Client aus dem Gladio-Repo; nur der
fest einkompilierte Socket-Pfad wird im Client durch die Umgebungsvariable
`GLADIO_SOCKET` wählbar gemacht.

## Bauen

Voraussetzungen: gcc, Mesa-Entwicklungsdateien (EGL, GLESv2, `GL/glx.h`), libX11-dev, ein
JDK (für `jni.h`), Xvfb oder ein anderer X-Server für das Fenster des Testprogramms.
Auf Arch/CachyOS: `mesa libglvnd libx11 jdk-openjdk xorg-server-xvfb`.

    make                              # klont den Gladio-Client nach build/gladio-src
    make GLADIO_SRC=/pfad/zu/gladio   # vorhandener Klon
    make DEBUG_MODE=0                 # ohne glGetError-/Shader-Protokoll (schneller)

Ergebnis: `build/gladio-pc-server` und `build/client/libGL.so.1`.

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
(`x11` mit laufendem X-Server, sonst `surfaceless`).

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
  Rot und Blau falsch (In-place-Tausch).
