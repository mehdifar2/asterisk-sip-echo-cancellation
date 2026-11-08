# Build func_lec.so (Asterisk module) and lec_replay (offline test tool).
#
#   make                 build both
#   make install         install func_lec.so into the Asterisk modules directory
#   make check-deps      show what was found
#
# Override any variable on the command line, for example:
#   make WEBRTC_LIBS=/usr/lib64/libwebrtc_audio_processing.so.1 SPEEXDSP_CFLAGS=-I/path/include

CC              ?= gcc
ASTERISK_CFLAGS ?= -I/usr/include
SPEEXDSP_CFLAGS ?= $(shell pkg-config --cflags speexdsp 2>/dev/null)
SPEEXDSP_LIBS   ?= $(shell pkg-config --libs speexdsp 2>/dev/null || echo -lspeexdsp)
# webrtc-audio-processing 0.3.x only: the 1.x series removed the AEC2 C API used here.
WEBRTC_LIBS     ?= $(shell pkg-config --libs webrtc-audio-processing 2>/dev/null || echo -lwebrtc_audio_processing)
MODULES_DIR     ?= $(firstword $(wildcard /usr/lib64/asterisk/modules /usr/lib/asterisk/modules /usr/lib/x86_64-linux-gnu/asterisk/modules))

WARN   = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
HARDEN = -fstack-protector-strong -D_FORTIFY_SOURCE=2
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu99 $(WARN) $(HARDEN) -pthread -D_GNU_SOURCE -D_REENTRANT

MOD_DEFS = -DAST_MODULE=\"func_lec\" -DAST_MODULE_SELF_SYM=__internal_func_lec_self

all: func_lec.so lec_replay

func_lec.so: src/func_lec.c src/lec_core_webrtc.c src/lec_core.h
	$(CC) $(CFLAGS) -fPIC -shared $(MOD_DEFS) $(ASTERISK_CFLAGS) $(SPEEXDSP_CFLAGS) -Isrc \
		-o $@ src/func_lec.c src/lec_core_webrtc.c $(WEBRTC_LIBS) $(SPEEXDSP_LIBS) -lm

lec_replay: tools/lec_replay.c src/lec_core_webrtc.c src/lec_core.h
	$(CC) $(CFLAGS) -Isrc -o $@ tools/lec_replay.c src/lec_core_webrtc.c $(WEBRTC_LIBS) -lm

install: func_lec.so
	@test -n "$(MODULES_DIR)" || { echo "MODULES_DIR not found; run: make install MODULES_DIR=/path/to/asterisk/modules"; exit 1; }
	install -m 0755 func_lec.so $(DESTDIR)$(MODULES_DIR)/func_lec.so

check-deps:
	@echo "asterisk headers : $$(ls $(patsubst -I%,%,$(ASTERISK_CFLAGS))/asterisk.h 2>/dev/null || echo MISSING)"
	@echo "speexdsp         : $$(pkg-config --modversion speexdsp 2>/dev/null || echo 'no pkg-config entry (install speexdsp-devel / libspeexdsp-dev)')"
	@echo "webrtc-audio-proc: $$(pkg-config --modversion webrtc-audio-processing 2>/dev/null || echo 'no pkg-config entry (need 0.3.x devel package)')"
	@echo "modules dir      : $(MODULES_DIR)"

clean:
	rm -f func_lec.so lec_replay

.PHONY: all install check-deps clean
