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
`frame-N.ppm` dort (`--frames 1,10` wählt die Bildnummern, `all` alle). Legt man im
Ablageverzeichnis eine Datei `now` an, wird das nächste Bild abgelegt; eine Datei `trace`
schreibt alle Anfragen des nächsten Bilds ins Server-Protokoll (`TRACE name größe` und die ersten bis
zu zehn 32-Bit-Werte der Anfrage), eine Datei `tracecont` jedes Bild, solange sie existiert (etwa um
das Laden eines Levels samt aller Texturen mitzuschneiden).

Umgebungsvariablen des Servers: `GLADIO_SOCKET`, `GLADIO_PC_ES` (2|3), `GLADIO_PC_DUMP`,
`GLADIO_PC_DUMP_FRAMES`, `GLADIO_PC_SIZE` (Fenstergröße ohne `$DISPLAY`), `EGL_PLATFORM`
(`x11` mit laufendem X-Server, sonst `surfaceless`). Vom Server selbst (gelten auch in der
App): `GLADIO_GLES=2|3` erzwingt den Pfad, `GLADIO_GLES_EXTENSIONS="GL_OES_… …"` ersetzt die
Erweiterungsliste des Treibers (z. B. die der Mali-400, um deren Lücken – kein VAO, kein
glMapBuffer, keine 32-Bit-Indizes – mit Mesa nachzustellen), `GLADIO_PBUFFER=1` erzwingt den
Pbuffer-Pfad für Treiber ohne `EGL_KHR_surfaceless_context`, `GLADIO_GLES_LIMITS="varyings=12
vs_uniforms=256 fs_uniforms=256 textures=8 vtextures=0 texture_size=4096 highp_fragment=0"` ersetzt
einzelne Grenzen (so meldet der Server dem Client dieselben Werte wie auf der Mali-400; auch
`attribs`, `cube_size`, `renderbuffer_size`). `GLADIO_PC_TRACE=1` protokolliert alle Anfragen bis
zum ersten Bild (`TRACE`-Zeilen, z. B. die Erkennung von wined3d).

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
* `statetest.c` – Zustandsfilter und Uniform-Wertevergleich des Clients (`patch-gladio-client.py`): 50 Zellen, in
  denen ein unveränderter Zustand noch einmal gesetzt wird oder sich hinter dem Rücken des Filters ändern könnte
  (glPopAttrib, gelöschte und neu angelegte Objekte mit derselben Nummer, Werte hinter dem Arrayende, neu gebundene
  Programme, Arrays des festen Funktionsmodells, Direktmodus, zweiter Kontext). Das Bild muss mit
  `GLADIO_NO_STATE_FILTER=1 GLADIO_NO_UNIFORM_DIFF=1` dasselbe sein wie ohne (und wie Mesa nativ). Bauen:
  `gcc -O1 -o statetest statetest.c -lX11 -lGL -ldl`.
* `glsltest.c` – Shader im wined3d-Stil: GLSL 1.20 mit gl_Vertex/gl_TexCoord/gl_FragData/
  gl_Fog, GLSL 1.30 mit in/out und texture(), VBO/IBO, D3DCOLOR-Attribute, Framebuffer-
  Objekt (EXT), Alphatest, Blending, BGRA-Rücklesen (`./glsltest out.ppm`).
  Bauen: `gcc -O1 -o glsltest glsltest.c -lX11 -lGL -ldl`.
* `fftest.c` – Festfunktions-Beleuchtung in acht Fällen (Richtungslicht unter gedrehter
  Modelview, GL_COLOR_MATERIAL in zwei Modi, Spot mit Abschwächung, zweiseitige Beleuchtung mit
  Vorder- und Rückseiten, GL_NORMALIZE, Material-Alpha mit Blending, Farb-Array ohne
  GL_COLOR_MATERIAL), mit Client-Arrays und glDrawElements (`./fftest out.ppm`).
  Bauen: `gcc -O1 -o fftest fftest.c -lX11 -lGL -lm`.
* `batchtest.c` – Zeichenstapel des Servers: je Fall zwei Quadrate mit Anfragen dazwischen, die
  den Stapel nicht ausgeben (Farb-/Texturkoordinaten-/Normalen-Array an und aus, glColor/glNormal/
  glMultiTexCoord zwischen zwei Zeichenaufrufen, glBegin/glEnd und Arrays gemischt,
  Einheitenwechsel wie bei Worms 3D; zweite Hälfte: Matrix, Licht, Material, Texturumgebung,
  Alphatest, Blending und Textur zwischen zwei Aufrufen geändert oder auf denselben Wert gesetzt).
  `./batchtest out.ppm`; Bauen:
  `gcc -O1 -o batchtest batchtest.c -lX11 -lGL`.
* `matrixtest.c` – Matrix- und Zustandsabfragen (glGetFloatv/glGetDoublev der drei Matrizen,
  GL_MATRIX_MODE, GL_DEPTH_WRITEMASK) nach einer Folge von Matrixoperationen, Push/Pop über die
  Stapelgrenze und glPushAttrib/glPopAttrib; gibt Text aus, mit Mesa vergleichen
  (`./matrixtest > ref.txt`). Bauen: `gcc -O1 -o matrixtest matrixtest.c -lX11 -lGL`.
* `readtest.c` – glReadPixels: GL_BGRA zweimal hintereinander (Lesecache des Servers), GL_RGBA
  danach, Lesen in einen Pack-Puffer mit glMapBuffer/glGetBufferSubData, auch gleich nach
  glXSwapBuffers; gibt je Fall das erste Pixel aus, mit Mesa vergleichen (`./readtest`).
  Bauen: `gcc -O1 -o readtest readtest.c -lX11 -lGL`.

* `bgratest.c` – GL_BGRA-Eckpunktfarben aus Puffern (`ARB_vertex_array_bgra`, wie wined3d ab GL 3.2
  D3DCOLOR angibt) und Hochladen ohne Antwort des Servers, zwölf Fälle: glDrawArrays mit first > 0,
  Indexpuffer, Basis-Eckpunkt, Indizes im Client-Speicher, Teilbereich abgebildet mit
  GL_MAP_FLUSH_EXPLICIT_BIT (Versatz ab Bereichsanfang), zwei Aufrufe aus einem Streaming-Puffer,
  Attribut ab- und ohne neuen Zeiger wieder angeschaltet, glMapBuffer(GL_WRITE_ONLY) über schon
  gezeichnete Daten, derselbe Bereich dreimal mit glBufferSubData beschrieben und gezeichnet,
  Eckpunkte nach dem Zeichnen überschrieben, aus einer Kopie (glCopyBufferSubData) gezeichnet und
  zurückgelesen; vorher ein Abbilden nur zum Lesen ab einer Farbe (`./bgratest out.ppm`, mit Mesa
  vergleichen). `./bgratest --wow64 out.ppm` stellt dabei Wine im WoW64-Betrieb nach
  (`dlls/opengl32/unix_wgl.c`: Abbilden über eine Kopie, deren Adresse in den ersten vier Byte des echten
  Bereichs steht, Rückkopie erst in glUnmapBuffer) – das Bild muss dasselbe sein. Bauen:
  `gcc -O1 -o bgratest bgratest.c -lX11 -lGL -ldl`.
* `streamtest.c` – Streaming-Puffer des Servers auf dem ES-2.0-Pfad, acht Fälle: GL_STREAM_DRAW mit
  wachsendem Versatz, GL_STATIC_DRAW fünfmal im Bild neu beschrieben, Eckpunkte und Indizes aus
  Streaming-Puffern, Puffer nach dem Zeichnen gelöscht und durch einen festen ersetzt, vergrößert,
  mit glBufferData (GL_STREAM_DRAW) neu befüllt, Position aus Streaming- und Farbe aus festem Puffer,
  glDrawElementsBaseVertex (`./streamtest out.ppm`, mit Mesa vergleichen). Bauen:
  `gcc -O1 -o streamtest streamtest.c -lX11 -lGL -ldl`.

* `gettest.c` – Zustandsabfragen: Werte, Typen und Antwortgrößen von glGetIntegerv/Floatv/Booleanv
  (jede Antwort landet in einem vorbelegten Feld, danach die belegten Bytes), der Pixelspeicher in der
  Reihenfolge von GLU (`retrieveStoreModes` vor `gluBuild2DMipmaps`), Proxy-Texturen wie in GLUs
  `closestFit`, glIsEnabled für Fähigkeiten, die ES nicht kennt; gibt Text aus, mit Mesa vergleichen
  (`./gettest > ref.txt`). Bauen: `gcc -O1 -o gettest gettest.c -lX11 -lGL`.

* `d3d9test.c` – Direct3D 9 über wined3d (Texturformate A8R8G8B8/R5G6B5/A1R5G5B5/A4R4G4B4/L8/
  DXT1 mit Mipmaps, festes Licht, Nebel, Alphatest, Blending, vs_2_0/ps_2_0, Render-to-Texture,
  Rücklesen als PPM). Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9test.exe d3d9test.c -ld3d9
  -lgdi32 -luser32`; laufen lassen mit dem Wine 10.10 (i386) aus der Root-FS des Releases
  `armv7-rootfs` (`armv7-assets.tar` → `rootfs.tzst` → `opt/wine/bin/wine`), das auf dem PC
  direkt startet – so prüft man genau das wined3d, das auch auf dem Gerät läuft.

* `d3d9caps.c` – Direct3D-9-Formatabfragen (`CheckDeviceFormat` je Format für Textur, Filter,
  Renderziel, Mischen), wie Spiele sie beim Start stellen; gibt eine Tabelle aus. Bauen:
  `i686-w64-mingw32-gcc -O1 -o d3d9caps.exe d3d9caps.c -ld3d9`.
* `d3d9stride0.c` – konstante Vertexfarbe aus einem Strom mit Schrittweite 0 (D3DCOLOR), wie die Unreal
  Engine 3 ihn für Modelle ohne Vertexfarben bindet; zwei Vierecke, erwartete und gelesene Farben. Bauen:
  `i686-w64-mingw32-gcc -O1 -o d3d9stride0.exe d3d9stride0.c -ld3d9 -lgdi32 -luser32`.
* `d3d9hdr.c` – Gleitkomma-Renderziel (A16B16G16R16F) wie die Szenenfarbe der Unreal Engine 3: Werte über 1
  und Alpha-Mischen hinein, dann mit Faktor 0,25 in den Hintergrundpuffer; gibt die erwarteten und die
  gelesenen Farben aus. Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9hdr.exe d3d9hdr.c -ld3d9 -lgdi32 -luser32`.
* `d3d9occl.c` – Verdeckungsabfragen (`D3DQUERYTYPE_OCCLUSION`) wie die Unreal Engine 3: eine Fläche hinter
  der Tiefe (0 Abtastwerte), ein Quadrat davor (> 0), eine leere Abfrage (0), Ergebnisse mit
  `GetData(D3DGETDATA_FLUSH)`. `wine d3d9occl.exe out.txt` (ES 3) bzw. `out.txt es2` (unter ES 2 ist alles
  sichtbar). Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9occl.exe d3d9occl.c -ld3d9 -luser32`.

* `d3d9inst.c` – Instanzen mit `SetStreamSourceFreq` (geometrische Instanzen, Basis-Eckpunkt 4): Versatz
  und D3DCOLOR je Instanz aus einem zweiten Strom, dazu ein Aufruf ohne Instanzen; erwartet rot/grün/blau/gelb
  und weiß in der Mitte. Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9inst.exe d3d9inst.c -ld3d9 -luser32`.
* `d3d9occlmany.c` – 64 Verdeckungsabfragen je Bild, Ergebnisse drei Bilder später mit `GetData(D3DGETDATA_FLUSH)`
  abgeholt (wie die Unreal Engine 3); gibt Bilder/s aus, die Rundreisen je Bild stehen in der Server-Statistik
  (`wine d3d9occlmany.exe [sekunden] [ausgabe.txt]`). Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9occlmany.exe
  d3d9occlmany.c -ld3d9 -luser32`.
* `d3d9const.c` – Shader-Konstanten wie die Unreal Engine 3: 100 Zeichenaufrufe je Bild mit je 118 Pixel- und 96
  Vertexshader-Konstanten, von denen sich nur wenige ändern; gibt Bilder/s aus und schreibt ein Bild (gleich mit
  `GLADIO_NO_UNIFORM_DIFF=1`), die Statistik des Servers zeigt die verglichenen und gesendeten Uniform-Bytes
  (`wine d3d9const64.exe [sekunden] [bild.ppm] [ausgabe.txt]`). Bauen: `x86_64-w64-mingw32-gcc -O1 -o
  d3d9const64.exe d3d9const.c -ld3d9 -luser32` (32 Bit entsprechend mit i686).
* `d3d9link.c` – ein Vertex-Shader (vs_3_0) mit drei Pixel-Shadern (ps_3_0), die verschiedene Ausgaben lesen
  (TEXCOORD0/1/2); erwartet rot/grün/blau. Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9link.exe d3d9link.c -ld3d9
  -luser32`.
* `d3d9depth.c` – Tiefe aus einer INTZ-Textur lesen (Tiefenpuffer-Textur, die die Unreal Engine 3 und andere
  abtasten), mit Punktfilter und bilinear; erwartet 64/191. Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9depth.exe
  d3d9depth.c -ld3d9 -luser32`.
* `d3d9srgb.c` – sRGB-Lesen (`D3DSAMP_SRGBTEXTURE`) mit DXT1/3/5, A8R8G8B8 und X8R8G8B8: Fähigkeiten, Grau 0x84
  ohne sRGB (~132) und mit (~59), danach eine nur mit sRGB benutzte Textur zurücklesen (Bytes unverändert).
  Bauen: `i686-w64-mingw32-gcc -O1 -o d3d9srgb.exe d3d9srgb.c -ld3d9 -luser32`.
* `d3d9srgbstream.c` – DXT-Texturen mit allen Mip-Stufen wie beim Textur-Streaming der Unreal Engine 3:
  `D3DPOOL_DEFAULT` über `UpdateTexture`, mit `D3DSAMP_SRGBTEXTURE` gelesen (wined3d kopiert jede Stufe über
  `glGetCompressedTexImage` in die sRGB-Fassung), neu hochgeladen, zum Teil freigegeben und neu angelegt; jede Stufe
  einzeln (`D3DSAMP_MAXMIPLEVEL`) mit und ohne sRGB gegen die erwartete Farbe geprüft (216 Stufen). Bauen:
  `x86_64-w64-mingw32-gcc -O1 -o d3d9srgbstream64.exe d3d9srgbstream.c -ld3d9 -luser32 -lm`.

Vergleich zweier PPM-Bilder: `python3 ppmdiff.py a.ppm b.ppm` (größte Abweichung je
Kanal und Anteil abweichender Pixel).

## Bild aufzeichnen

`GLADIO_PC_CAPTURE_AT=N` (Umgebung des Servers) zeichnet das Bild nach Bild N auf, nach
`GLADIO_PC_CAPTURE_DIR` (sonst ins Ablageverzeichnis bzw. `/tmp`), genau wie der Menüpunkt „Bild
aufzeichnen (Gladio)“ in der App (`app/src/main/cpp/gladiorenderer/src/gl_diag.c`): Ordner
`gladio-JJJJMMTT-HHMMSS` mit `anfragen.txt` (jede Anfrage mit Argumenten und GL-Fehlern, zu
jedem Zeichenaufruf Zustand, Sampler, Attribute und Uniforms), `bild-NNN.png` (Ergebnis jedes
Durchgangs, links Farbe, rechts Alpha) und `shader.txt`. Mit `make BUILD=build-rel DEBUG_MODE=0
GLADIO_SRC=build/gladio-src build-rel/gladio-pc-server` entsteht ein Server wie in der App (ohne
`glGetError` nach jeder Anfrage); dessen Diagnose (GL-Fehler nach Zeichenaufrufen, unvollständige
Zeichenziele, Shader-Fehler) steht im Server-Protokoll.

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
  Auf dem Gerät brachte das „Ausführen“ von 340 auf 46 ms je Bild; die Bildrate blieb bei
  ~3/s, weil die Bildübergabe (`glCopyTexImage2D` im X-Server) ~300 ms je Bild kostete – daher
  jetzt die Übergabe ohne Kopie (drei Texturen im Wechsel, siehe `armv7/README.md`). Das
  PC-Testgeschirr liest das Bild dabei über ein Hilfs-FBO aus der übergebenen Textur.
* Festfunktions-Beleuchtung (`fftest`, Worms 3D): Der Server multiplizierte bei Beleuchtung immer
  mit der Vertexfarbe (in GL zählt sie nur mit GL_COLOR_MATERIAL) – Würmer wurden schwarz, sobald
  das Spiel vorher eine schwarze Farbe gesetzt hatte (nach einem Sprung). Außerdem: Richtungslichter
  und Spot-Richtungen wurden mit der Inversen der Modelview statt mit ihr ins Augensystem gebracht,
  Spots mit Exponent 0 hatten keinen Kegel, das Rückseitenmaterial galt nach Normalenrichtung statt
  nur bei GL_LIGHT_MODEL_TWO_SIDE nach Orientierung des Dreiecks, das Material-Alpha fehlte,
  Normalen wurden nicht normiert, alle Lichter hatten Umgebungslicht 0.2 statt 0. Jetzt rechnen
  beide Pfade (allgemeiner Shader für ES 3, spezialisierte Shader für ES 2) wie GL; alle acht
  Fälle von `fftest` sind mit Mesa pixelgleich (Abweichung höchstens 1/255).
* Zeichenstapel (`batchtest`): Der Server fasst Zeichenaufrufe zusammen, solange dazwischen nur
  Client-Arrays, aktuelle Farbe/Normale/Texturkoordinate oder glBegin/glEnd kommen. Dabei bekamen
  zwei Aufrufe mit verschiedener fester Farbe beide die letzte, Arrays verrutschten gegeneinander,
  wenn ein Aufruf ein Array mitbrachte und der andere nicht, und feste Normalen/Texturkoordinaten
  (ohne Array) kamen gar nicht an – sechs von acht Fällen falsch, auf beiden Pfaden. Jetzt
  bleiben die Attribute auf die Vertices ausgerichtet (`GLRenderer_prepareBatchArrays` in
  `gl_renderer.c`, ES 2 und ES 3); alle Fälle sind mit Mesa gleich (höchstens 2/255). Danach
  zählen auch glActiveTexture/glClientActiveTexture nicht mehr als Stapelgrenze: Worms 3D (Match)
  gibt statt ~430 nur noch ~150 Stapel je Bild an den Treiber; mit dem aufgeschobenen
  Zustandsvergleich (`deferStateChange`) und dem angepassten Client noch ~80 bei ~105
  Zeichenaufrufen. Alle 16 Fälle von `batchtest` sind auf beiden Pfaden mit Mesa gleich.
* Matrixabfragen (`matrixtest`): Der angepasste Client beantwortet sie aus mitgeführten Stapeln; alle
  17 Schritte stimmen mit Mesa überein (auch Push über die Grenze von 32 Einträgen hinaus).
* glReadPixels (`readtest`): Mit gebundenem Pack-Puffer ist `pixels` ein Versatz in den Puffer, der
  Lesecache des Servers kopierte aber dorthin bzw. von dort (Absturz, memcpy an Adresse 0, sobald
  vorher gleich groß an 0,0 gelesen worden war – auf beiden Pfaden). Der Cache gab außerdem für jedes
  Format die vor der Umwandlung gemerkten RGBA-Pixel zurück (zweites GL_BGRA-Lesen als RGBA), und
  seine Struktur lief bei jedem ungecachten Lesen aus. Der Client gab bei glMapBuffer und
  glGetBufferSubData den geteilten Speicher eines Pack-Puffers zurück, bevor der Server die Pixel
  hineingeschrieben hatte (beim ersten Mal Nullen); jetzt wartet er dort erst auf den Server (nach
  den ausstehenden Bildantworten). Alle acht Fälle stimmen auf beiden Pfaden mit Mesa überein.
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
* wined3d auf armv7 (LEGO Racers, DirectDraw/Direct3D 6): Die App stürzte bei der Formaterkennung
  von wined3d ab (SIGSEGV an Adresse 0 im Thread der Verbindung). wined3d fragt
  `glGetInternalformativ(…, GL_SAMPLES, count, …)` mit der Anzahl aus `GL_NUM_SAMPLE_COUNTS` ab, hier
  0. Der Server schrieb den Wert trotzdem in ein Feld der Länge 0 auf dem Stack; im armv7-Build liegt
  dort der gemerkte Zeiger der Stack-Schutzprüfung (`__stack_chk_guard`), die beim Rücksprung dann
  Adresse 0 liest. Auf x86-64 traf der Wert nur Füllbytes, darum lief das Spiel auf dem PC. Gefunden
  mit einem AddressSanitizer-Bau des Servers (`-fsanitize=address -fsanitize-recover=address`,
  `DEBUG_MODE=0` wie auf dem Gerät, `ASAN_OPTIONS=detect_leaks=0:halt_on_error=0`), der außerdem
  einen 4-Byte-Überlauf beim Vorgabelicht (`initLight`) meldete; danach ist der Start von LEGO Racers
  ohne Befund. Die Info-Logs (`glGetShaderInfoLog`/`glGetProgramInfoLog`) bekommen jetzt mindestens
  ein Byte Platz.
* ES-3.0-Pfad mit wined3d (LEGO Racers, Direct3D 6): Der Server meldet GL 3.3, damit gilt
  `ARB_vertex_array_bgra` für wined3d als Kern, und D3DCOLOR-Farben kommen als
  `glVertexAttribPointer(…, GL_BGRA, …)` aus dem Streaming-Puffer. Der Client schickte dafür bei jedem
  Zeichenaufruf den Puffer vom Anfang bis zum letzten Eckpunkt mit (wined3d zeichnet mit wachsendem
  Basis-Eckpunkt) – bis 140 MB je Bild, 1–3 Bilder/s. Jetzt kennzeichnet der Client das Attribut nur
  (0x80 im normalized-Byte); der Server liest die Farben beim Zeichnen aus dem geteilten Speicher,
  stellt nur die gezeichneten Eckpunkte auf RGBA um und hängt sie an einen Ringpuffer an
  (`GLContext_prepareBgraArrays`). Danach blieben je Zeichenaufruf vier Rundreisen: wined3d bildet
  ohne `ARB_buffer_storage` bei jedem Hochladen den ganzen 512-KB-Puffer mit
  GL_MAP_FLUSH_EXPLICIT_BIT ab, und glUnmapBuffer übertrug trotzdem den ganzen Bereich noch einmal;
  glFlushMappedBufferRange wartete auf den Server. Jetzt überträgt glUnmapBuffer mit dem Bit nichts
  mehr, glFlushMappedBufferRange zählt den Versatz wie GL ab dem Bereichsanfang, und
  glFlushMappedBufferRange und glBufferSubData warten nicht mehr: der Server liest beim Abarbeiten
  aus dem geteilten Speicher, der Client merkt sich je Puffer, was der Server dort noch lesen wird
  (auch BGRA-Farben beim Zeichnen und Kopien), und wartet nur, bevor er selbst hineinschreibt –
  synchronisiertes Abbilden zum Schreiben wartet immer. glCopyBufferSubData führt jetzt auch den
  geteilten Speicher des Ziels nach (vorher lasen glGetBufferSubData und die BGRA-Farben dort
  Nullen). Im Rennen am PC (2 Kerne, llvmpipe, `WINEDEBUG=-all`): ES 3 mit CSMT von 2 auf
  ~20 Bilder/s, ohne CSMT ~55; ES 2 mit CSMT von ~45 auf ~62 (vorher 235 Antworten je Bild für
  glBufferSubData, jetzt eine). Was ES 3 mit CSMT fehlt, kostet wined3d selbst: ohne
  `ARB_buffer_storage` wartet der Anwendungsthread bei jedem Abbilden auf den CSMT-Thread.
  `bgratest` ist auf beiden Pfaden pixelgleich mit Mesa (der alte Client scheitert an drei Fällen:
  das ab- und ohne neuen Zeiger wieder angeschaltete BGRA-Attribut bekam keine Daten mehr, aus
  einer Kopie wurde schwarz bzw. nichts gezeichnet); `d3d9test`, `glsltest`, `batchtest`, `fftest`,
  `matrixtest`, `readtest` und `glrender2` liefern dasselbe wie vorher. Nebenbefund: Pufferziele wie
  `GL_COPY_READ_BUFFER`/`GL_COPY_WRITE_BUFFER` kennen Client und Server nicht (`indexOfGLTarget`
  liefert 255, Zugriff hinter `vao->buffer`).
* Mali-400 (Note 8.0, LEGO Racers im Rennen ~2,8 Bilder/s): Der Server brauchte 330–430 ms je Bild zum
  Ausführen, davon 4–5 ms Zeichnen. wined3d lädt über ES 2.0 vor jedem Zeichenaufruf mit glBufferSubData
  in seinen 512-KB-Streaming-Puffer (~140 Mal je Bild), aus dem im selben Bild schon gezeichnet wurde –
  der Mali-400-Treiber kopiert dafür jedes Mal den ganzen Puffer. Jetzt führt der Server
  Streaming-Puffer nur noch als eigene Kopie (in der Reihenfolge der Anfragen) und zeichnet daraus mit
  Client-Arrays, die der Treiber nur im benutzten Bereich übernimmt (`gl_es2.c`). Streaming-Puffer sind
  Puffer mit GL_STREAM_DRAW (wined3d: alle dynamischen Puffer; der Client meldet den Hinweis beim
  Anlegen als GL_CLIENT_STORAGE_BIT) und andere Puffer, sobald sie in einem Bild viermal teilweise neu
  beschrieben werden. Nach jedem Zeichenaufruf stellt der Server die Attribute wieder auf die
  GL-Puffer, damit GL nie einen Zeiger in eine freigegebene Kopie behält. Die Statistik zeigt jetzt
  auch die Zeit fürs Hochladen. Am PC (llvmpipe) zeichnet LEGO Racers so pixelgleich; die Mali-Kopien
  selbst lassen sich hier nicht messen. `streamtest` ist auf beiden Pfaden pixelgleich mit Mesa.
* WoW64 (64-Bit-Variante, 32-Bit-Programm): Liegt der geteilte Speicher über 4 GB, bildet Wine über
  eine eigene Kopie ab, schreibt deren Adresse in die ersten vier Byte des Bereichs und kopiert erst in
  glUnmapBuffer zurück. Box64 legt Abbildungen nativer Bibliotheken mit `BOX64_MMAP32=1` (Vorgabe)
  unter 2 GB, dann entfällt das; sonst galt: Die Übertragungen ohne Rundreise lasen die Daten vor der
  Rückkopie, glGetBufferPointerv lieferte den Pufferanfang statt des Bereichsanfangs, nach glMapBuffer
  meldete der Client GL_BUFFER_MAP_LENGTH 0 und GL_BUFFER_ACCESS_FLAGS ohne Schreib-Bit (Wine kopierte
  nichts zurück). Jetzt gehen ausdrückliche Übertragungen erst beim Freigeben hinaus, die Abfragen
  stimmen, und der 64-Bit-Client wartet vor dem Abbilden, bis der Server die ersten vier Byte nicht mehr
  lesen muss (LEGO Racers: 0,2 zusätzliche Rundreisen je Bild, eine je neuem Streaming-Puffer); nach
  einem Abbilden nur zum Lesen stellt er sie wieder her. BGRA-Farben merkt sich der Client dafür genau
  im gezeichneten Bereich vor statt im ganzen Puffer. `bgratest --wow64` ist auf beiden Pfaden
  pixelgleich mit Mesa; der vorige Client verlor dort Fall 4 (Teilbereich) und Fall 7 (glMapBuffer).
* Zustandsabfragen (SKIDROW-Intro, festes Funktionsmodell mit GLU): Auf dem Note 8.0 blieb das Bild
  schwarz, der Anfang der Musik lief, dann war Schluss; auf dem CMF Phone 1 (ES 3) lief es. Zwei Fehler
  des Servers, beide über `gluBuild2DMipmaps`: GLU fragt dort mit einer Proxy-Textur nach, ob die Textur
  passt (`glTexImage2D(GL_PROXY_TEXTURE_2D, …)`, dann `glGetTexLevelParameteriv`). Für Proxy-Ziele
  liefert `indexOfGLTarget` 255, und der Server nahm den Wert aus `clientState.texture[einheit][255]`
  als gebundene Textur – das liegt hinter dem Renderer im Heap (auf armv7 148 Byte nach seinem Ende,
  auf 64 Bit 848 Byte). Mit `gettest` stürzte der unveränderte Server auf dem ES-2.0-Pfad genau dort
  ab (SIGSEGV, gelesen wurde ein Stück Text aus dem Heap); jetzt haben Ziele ohne Platz keine Bindung,
  und Proxy-Anfragen beantwortet der Server selbst (die Maße, wenn sie in die gemeldeten Grenzen
  passen, sonst 0). Außerdem liest GLU vorher den ganzen Pixelspeicher (`retrieveStoreModes`), auch
  GL_UNPACK_ROW_LENGTH und die Versätze, die ES 2.0 nicht kennt, und GL_UNPACK_SWAP_BYTES/LSB_FIRST,
  die ES gar nicht kennt. Der Treiber ließ diese Abfragen unbeantwortet, und der Server schickte den
  uninitialisierten Antwortpuffer vom Stack zurück (unter ES 3 kamen 3313 und 3312 an – die Kennung
  der Abfrage selbst, die dort noch lag). Mit unbestimmter Zeilenlänge und unbestimmten Versätzen liest GLU
  hinter die Bilddaten. Jetzt beantwortet der Server den Pixelspeicher selbst (unter ES 2.0 aus dem
  eigenen Zustand), belegt alle Antworten vor und schreibt im angefragten Typ: vorher kamen bei
  glGetIntegerv auf Licht, Nebel oder GL_STEREO 1 Byte (der Rest des Programmfelds blieb, wie er war),
  Gleitkommawerte als Bitmuster (GL_FOG_DENSITY, GL_CURRENT_COLOR), glGetBooleanv auf Matrizen 64 statt
  16 Byte (Überlauf beim Programm), GL_MAX_VIEWPORT_DIMS und GL_COLOR_WRITEMASK als Ganzzahl nur der
  erste Wert, GL_BLEND_SRC/GL_BLEND_DST blieben unbeantwortet (glPopAttrib stellte GL_ZERO/GL_ZERO her). `gettest`
  stimmt auf beiden Pfaden mit Mesa überein (bis auf die Grenzen des Servers); das Intro läuft über
  ES 2.0 mit den Mali-400-Grenzen und über ES 3, mit Vollbild und im Fenster.
* Gleitkomma-Renderziele (Outlast, Unreal Engine 3, auf dem CMF Phone 1): Das Spiel brach mit „Your video
  card does not support alpha blending with floating point render targets (D3DFMT_A16B16G16R16F)“ ab.
  wined3d fragt Renderbarkeit und Mischen über `ARB_internalformat_query2` ab (`GL_FRAMEBUFFER_RENDERABLE`,
  `GL_FRAMEBUFFER_BLEND`), und die Tabelle des Servers (`gl_formats.c`) beantwortete das nach ES 3.0:
  Gleitkommaformate nicht renderbar. ES 3.2 (Mali-G615, Mesa) und `GL_EXT_color_buffer_float` können das;
  jetzt fragt der Server den Treiber (`GL_EXT_color_buffer_half_float` nur für 16 Bit, Mischen in 32 Bit nur
  mit `GL_EXT_float_blend`) und kennt die Formate auch als Renderbuffer. `d3d9caps` zeigt über ES 3 jetzt
  alle Gleitkommaformate als Renderziel mit Mischen, `d3d9hdr` liefert die erwarteten Farben (der vorige
  Server: Abfrage nein, Anlegen des Renderziels scheitert). Über ES 2.0 (Mali-400) bleibt es bei keinen
  Gleitkomma-Renderzielen.

* Konstante Vertexattribute: wined3d setzt Ströme mit Schrittweite 0 nicht als Array, sondern mit
  `glVertexAttrib4Nubv` (D3DCOLOR) bzw. `glVertexAttrib4Nub`/`4Nsv`/`4Nusv` (feste Farbe, normierte Formate).
  Der Server verwarf diese Aufrufe („not implemented yet“ im Protokoll, bei Outlast nach den Intro-Videos),
  das Attribut blieb (0,0,0,0) – Modelle ohne Vertexfarben wären schwarz. Jetzt rechnet der Server sie wie
  GL 4.2 in Gleitkomma um. `d3d9stride0` zeigt auf beiden Pfaden die erwarteten Farben (der vorige Server:
  beide Vierecke schwarz). `glProvokingVertex` (ES hat keine Wahl des bestimmenden Eckpunkts) wird ohne
  Meldung verworfen.

* Verdeckungsabfragen (Outlast im Hauptmenü: „CreateQuery(D3DQUERYTYPE_OCCLUSION) failed with error
  D3DERR_NOTAVAILABLE“, für die Unreal Engine 3 fatal): wined3d fragt beim Start
  `glGetQueryiv(GL_SAMPLES_PASSED, GL_QUERY_COUNTER_BITS)` ab und schaltet die Abfragen bei 0 ab. Der Server
  beantwortete das nur bei einer gerade laufenden Abfrage dieser Art, sonst blieb die Antwort 0 – auch im
  Original. Jetzt meldet er für Verdeckungsabfragen immer 32 Bit; die Anzahl wird nachgebildet (ES 3 über
  `GL_ANY_SAMPLES_PASSED`: 0 oder 255, ES 2: immer 255). `GL_QUERY_RESULT_AVAILABLE` fragt unter ES 3 den
  Treiber, statt immer „fertig“ zu melden (sonst wartet jedes Ergebnis auf die GPU). `d3d9occl` liefert über
  ES 3 (64 und 32 Bit) 0/255/0, über ES 2 255/255/255; vorher `D3DERR_NOTAVAILABLE`.

* Instanzen und D3DCOLOR (Outlast: wined3d zeichnet jedes `DrawIndexedPrimitive` als
  `glDrawElementsInstancedBaseVertex`, Instanzzahl = Strom-Frequenz): Die Umstellung der BGRA-Farben las
  auch Attribute mit Teiler (`glVertexAttribDivisor`) im Eckpunktbereich des Aufrufs statt in den
  Instanzen 0 … (Anzahl − 1)/Teiler. `d3d9inst` zeigte die vier Instanzen an der richtigen Stelle, aber
  schwarz/schwarz/weiß/weiß statt rot/grün/blau/gelb; jetzt stimmen alle fünf Farben. Instanzierte
  Zeichenaufrufe mit Basis-Eckpunkt zählen jetzt auch in der Statistik als Zeichenaufrufe (vorher
  „4 Zeichenaufrufe“ je Bild bei Outlast).
* Tiefentexturen (`GL_DEPTH_TEXTURE_MODE` alter Kontexte): INTENSITY lieferte unter ES (D,0,0,1) statt
  (D,D,D,D), ALPHA (0,0,0,1) statt (0,0,0,D); jetzt per Swizzle wie GL, und nur für Tiefenformate.
* Diagnose: siehe „Bild aufzeichnen“. Nebenbefund beim Start von wined3d (einmalig, harmlos):
  `glTexImage2D(GL_TEXTURE_1D_ARRAY)` (ES kennt keine 1D-Texturen) und `glEnable(GL_MULTISAMPLE)` melden
  `GL_INVALID_ENUM`; GL_MULTISAMPLE & Co. verwirft der Server jetzt still.
* Mehrere Shader einer Stufe in einem Programm (Outlast: nach dem Hauptmenü statt der Szene ein gleichmäßig
  brauner bzw. orange flackernder Bildschirm): wined3d übersetzt einen Vertex-Shader einmal und hängt je
  Programm einen kleinen zweiten Vertex-Shader mit `setup_vs_output()` an, der die Ausgaben nach den Eingängen
  des jeweiligen Pixel-Shaders ordnet (Desktop-GL bindet mehrere Shader derselben Stufe, ES nur einen). Der
  Server fügte den zweiten in den gemeinsamen Vertex-Shader selbst ein und übersetzte den nur einmal – jedes
  weitere Programm mit diesem Vertex-Shader bekam die Zuordnung des ersten. Die Bildaufzeichnung am Gerät zeigte
  die Szene im Gleitkomma-Szenenbild korrekt, erst die letzte Nachbearbeitung (Tonwerte, Farbkorrektur) las statt
  der Pixelposition die Clip-Koordinaten und tastete für alle Pixel dieselbe Ecke ab. Jetzt bekommt jedes
  Programm eine eigene Fassung aus dem unveränderten Quelltext des Hauptshaders und seinen Hilfs-Shadern (gleiche
  Hilfs-Shader teilen sich eine Fassung). `d3d9link` vorher: rot/rot/rot, jetzt rot/grün/blau.
* Abfragen und Fences (Outlast: ~2000 Rundreisen je Bild für `GetQueryObjectuiv`, dazu `ClientWaitSync`): wined3d
  fragt mit CSMT alle 100 Befehle jede offene Abfrage und jeden Fence ab, jede Frage wartet auf den Server. Der
  Client beantwortet „fertig?“ jetzt selbst, wo die Reihenfolge es erlaubt (GPU arbeitet sie in der Reihenfolge
  ihres Endes ab; „nicht fertig“ gilt 1 ms), Ergebnisse holt er weiter beim Server. `d3d9occlmany` am PC: 289 statt
  185 Bilder/s, 129 statt 205 Antworten je Bild, gleiche Ergebnisse (51 % sichtbar).
* sRGB-Texturen (Outlast: Texturen zu hell, vermutlich daher auch das grünliche Armaturenbrett im Auto): die Unreal
  Engine 3 liest Farbtexturen mit `D3DSAMP_SRGBTEXTURE` und rechnet linear. DXT meldete kein sRGB-Lesen, wined3d las
  sie also linear (dunkle Töne bis 13-mal zu hell); A8R8G8B8 meldete es, der Server legte die sRGB-Fassung aber als
  lineare BGRA-Textur an, und wined3ds Blit in die sRGB-Fassung wandelte unter ES um. Jetzt: alle sRGB-Formate als
  `GL_SRGB8_ALPHA8` (BGRA-Daten getauscht, DXT ausgepackt), sRGB-DXT gemeldet, ungestreckte Farb-Blits in eine
  sRGB-Textur roh über `glReadPixels`/`glTexSubImage2D`. Dabei behoben: `glGetTexImage`/`glGetCompressedTexImage` lasen
  immer Stufe 0 in deren Größe (eine kleinere Stufe schrieb über das Pufferende) und konnten aus dem Lesecache das
  Bild einer anderen Textur liefern. `d3d9srgb`: vorher alles 132, jetzt 132/59.
* Rundreisen (Outlast nach dem sRGB-Umbau: ~160 je Bild – Verdeckungsabfragen, sRGB-Kopien der DXT-Texturen über
  `glGetCompressedTexImage`, dazu ~730 `glGetUniformLocation` je neuem Shader-Programm): Der Server merkt sich beendete
  Verdeckungsabfragen in der Reihenfolge ihres Endes und schickt mit jeder Frage die Ergebnisse aller inzwischen
  fertigen mit; hat er noch viel abzuarbeiten, fragt der Client bei jungen Abfragen gar nicht erst. Uniform-Positionen
  kommen je Programm in einer Antwort (Tabelle aller aktiven Uniforms), das Zurücklesen komprimierter Stufen beantwortet
  der Client aus einer Kopie der hochgeladenen Daten. `d3d9occlmany` (ohne Wine-Protokoll): 341 → 381 Bilder/s,
  66 → 3 Antworten je Bild, gleiche Ergebnisse; `d3d9link`: 2193 → 3 `GetUniformLocation` (alle Positionen gegen den
  Server geprüft: gleich); `d3d9srgbstream`: alle 90 Rückleseaufrufe im Client, Ausgabe gleich wie vorher. Dazu gehen
  unveränderte `glVertexAttribDivisor`/`glBindSampler` nicht mehr hinaus (Outlast: 5600/1400 je Bild), und der Client
  meldet seine Wartezeiten mit jedem Bild (Zeile „Client je Bild“ in der Laufstatistik).
