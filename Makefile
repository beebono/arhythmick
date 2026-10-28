# Arhythmick libretro core
#
#   make                         native shared core for this host
#   make platform=win            Windows .dll (use with a MinGW CC)
#   make platform=osx            macOS .dylib
#   make platform=unix CC=arm-linux-gnueabihf-gcc   cross-compile for ARM handhelds
#   make platform=static         static archive for frontends that link cores statically
#   make test                    unit tests
#   make tools                   build tools/analyze
#   make install                 copy core + .info into RetroArch user dirs (CORE_DIR, INFO_DIR)

TARGET_NAME := arhythmick

ifeq ($(platform),)
   platform = unix
   ifeq ($(shell uname -s),)
      platform = win
   else ifneq ($(findstring Darwin,$(shell uname -s)),)
      platform = osx
   else ifneq ($(findstring MINGW,$(shell uname -s)),)
      platform = win
   endif
endif

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -D_POSIX_C_SOURCE=200112L
WARN    := -Wall -Wextra
LIBS    := -lm

ifeq ($(platform),unix)
   TARGET  := $(TARGET_NAME)_libretro.so
   CFLAGS  += -fPIC
   LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined
else ifeq ($(platform),osx)
   TARGET  := $(TARGET_NAME)_libretro.dylib
   CFLAGS  += -fPIC
   LDFLAGS += -dynamiclib
else ifeq ($(platform),win)
   TARGET  := $(TARGET_NAME)_libretro.dll
   LDFLAGS += -shared -static-libgcc -Wl,--version-script=link.T
else ifneq (,$(findstring static,$(platform)))
   TARGET  := $(TARGET_NAME)_libretro_$(platform).a
   STATIC_LINKING := 1
else
   $(error Unknown platform '$(platform)')
endif

CORE_SOURCES := src/antirhythm.c src/bpm.c src/fft.c src/decode.c src/render.c src/libretro.c
LOGIC_SOURCES := src/antirhythm.c src/bpm.c src/fft.c
THIRD_PARTY  := src/decoders_impl.c

BUILD := build/$(platform)
CORE_OBJS  := $(CORE_SOURCES:%.c=$(BUILD)/%.o)
THIRD_OBJS := $(THIRD_PARTY:%.c=$(BUILD)/%.o)

all: $(TARGET)

ifeq ($(STATIC_LINKING),1)
$(TARGET): $(CORE_OBJS) $(THIRD_OBJS)
	$(AR) rcs $@ $^
else
$(TARGET): $(CORE_OBJS) $(THIRD_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)
endif

$(BUILD)/src/decoders_impl.o: src/decoders_impl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -c -o $@ $<

# Host-only helpers (not cross-compiled).
HOST_CC ?= cc
HOST_CFLAGS := -std=c99 -O2 -D_POSIX_C_SOURCE=200112L

tools: build/analyze

build/analyze: tools/analyze.c $(LOGIC_SOURCES) src/decode.c src/decoders_impl.c
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -w -c -o build/decoders_impl_host.o src/decoders_impl.c
	$(HOST_CC) $(HOST_CFLAGS) $(WARN) -o $@ tools/analyze.c $(LOGIC_SOURCES) src/decode.c build/decoders_impl_host.o -lm

build/test_antirhythm: tests/test_antirhythm.c $(LOGIC_SOURCES)
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) $(WARN) -o $@ $^ -lm

test: build/test_antirhythm
	./build/test_antirhythm

CORE_DIR ?= $(HOME)/.config/retroarch/cores
INFO_DIR ?= $(HOME)/.config/retroarch/info

install: $(TARGET)
	mkdir -p $(CORE_DIR) $(INFO_DIR)
	cp $(TARGET) $(CORE_DIR)/
	cp $(TARGET_NAME)_libretro.info $(INFO_DIR)/

clean:
	rm -rf build $(TARGET_NAME)_libretro.*.a $(TARGET_NAME)_libretro.so $(TARGET_NAME)_libretro.dll $(TARGET_NAME)_libretro.dylib

.PHONY: all clean test tools install
