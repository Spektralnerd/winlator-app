# PC-Testgeschirr für den Gladio-Server: baut den Server aus app/src/main/cpp/gladiorenderer
# nativ (x86-64, EGL/GLES von Mesa) und den Gladio-Client (libGL.so.1) aus dem Gladio-Repo.
#
#   make                 Server + Client bauen
#   make GLADIO_SRC=...  Pfad zum Gladio-Client-Quelltext (sonst wird er nach build/gladio-src geklont)
#   make DEBUG_MODE=...  z. B. DEBUG_MODE='(DEBUG_MODE_GL_ERROR|DEBUG_MODE_SHADER_INFO)'
#
# Benötigt: gcc, Mesa-Entwicklungsdateien (EGL, GLESv2, GL/glx.h), libX11, ein JDK (jni.h).

CPP_DIR      := $(abspath ../../../app/src/main/cpp)
BUILD        := build
GLADIO_REPO  ?= https://github.com/brunodev85/gladio.git
GLADIO_COMMIT ?= 116c0d14dedbea3bd057f98f1db138bb1efe225e
GLADIO_SRC   ?= $(BUILD)/gladio-src
JNI_INCLUDE  ?= $(shell d=$$(dirname $$(dirname $$(readlink -f $$(command -v javac)))); echo $$d/include)
DEBUG_MODE   ?= (DEBUG_MODE_GL_ERROR|DEBUG_MODE_SHADER_INFO)

SERVER_SRCS := $(filter-out $(CPP_DIR)/gladiorenderer/src/main.c,$(wildcard $(CPP_DIR)/gladiorenderer/src/*.c)) \
               $(CPP_DIR)/winlator/src/arrays.c \
               $(CPP_DIR)/winlator/src/ring_buffer.c
SERVER_OBJS := $(patsubst $(CPP_DIR)/%.c,$(BUILD)/server/%.o,$(SERVER_SRCS)) $(BUILD)/server/server_main.o

SERVER_CFLAGS := -O1 -g -fPIC -std=gnu11 -w -MMD -MP -DGL_SERVER=1 -D__ANDROID_API__=28 "-DDEBUG_MODE=$(DEBUG_MODE)" \
                 -Ishim -I$(JNI_INCLUDE) -I$(JNI_INCLUDE)/linux \
                 -I$(CPP_DIR)/gladiorenderer/include -I$(CPP_DIR)/winlator/include -I$(CPP_DIR)/vortekrenderer/include
SERVER_LIBS := -lEGL -lGLESv2 -lX11 -lpthread -lm

CLIENT_SRCS := main.c gl_calls.c glx_calls.c arrays.c ring_buffer.c gl_buffer.c gl_vao.c
CLIENT_CFLAGS := -O2 -g -fPIC -std=gnu11 -w -DGL_GLEXT_PROTOTYPES -I$(GLADIO_SRC)/include

all: $(BUILD)/gladio-pc-server $(BUILD)/client/libGL.so.1

$(BUILD)/server/%.o: $(CPP_DIR)/%.c
	@mkdir -p $(dir $@)
	gcc $(SERVER_CFLAGS) -c -o $@ $<

$(BUILD)/server/server_main.o: server_main.c
	@mkdir -p $(dir $@)
	gcc $(SERVER_CFLAGS) -Wall -Wno-unused-parameter -c -o $@ $<

$(BUILD)/gladio-pc-server: $(SERVER_OBJS)
	gcc -o $@ $^ $(SERVER_LIBS)

$(GLADIO_SRC)/include/gladio.h:
	@mkdir -p $(BUILD)
	git clone $(GLADIO_REPO) $(GLADIO_SRC)
	git -C $(GLADIO_SRC) checkout -q $(GLADIO_COMMIT)

# Der Client hat den Socket-Pfad fest einkompiliert; für den PC wird er über GLADIO_SOCKET wählbar.
$(BUILD)/client/src/main.c: $(GLADIO_SRC)/include/gladio.h
	@mkdir -p $(BUILD)/client/src
	sed 's|strncpy(server_addr.sun_path, X11_SERVER_PATH,|const char* gladioSock = getenv("GLADIO_SOCKET"); strncpy(server_addr.sun_path, gladioSock ? gladioSock : X11_SERVER_PATH,|' \
	    $(GLADIO_SRC)/src/main.c > $@
	@grep -q GLADIO_SOCKET $@ || { echo "Socket-Pfad im Client nicht ersetzt"; exit 1; }

$(BUILD)/client/libGL.so.1: $(BUILD)/client/src/main.c
	gcc $(CLIENT_CFLAGS) -shared -Wl,-soname,libGL.so.1 -o $@ $(BUILD)/client/src/main.c \
	    $(addprefix $(GLADIO_SRC)/src/,$(filter-out main.c,$(CLIENT_SRCS))) -lX11 -lpthread
	ln -sf libGL.so.1 $(BUILD)/client/libGL.so

clean:
	rm -rf $(BUILD)/server $(BUILD)/client $(BUILD)/gladio-pc-server

-include $(SERVER_OBJS:.o=.d)

.PHONY: all clean
