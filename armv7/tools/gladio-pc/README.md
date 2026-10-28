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
