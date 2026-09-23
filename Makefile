# =====================================================================
#  WILD 7's — libretro core.  Builds on the Pi in a few seconds.
#    make            -> wild7_libretro.so
#    make clean
# =====================================================================
TARGET := wild7_libretro.so
SRC    := src/wild7_libretro.c
HDR    := src/libretro.h
MODS   := $(wildcard src/w7_*.c src/w7_*.h)
ARCH   := $(shell uname -m)

CFLAGS  := -O3 -ffast-math -fno-math-errno -fPIC -Wall -Wextra -Isrc
LDFLAGS := -shared -lm -lpthread

# Flags common to every target, captured before the host-specific
# tuning below is appended.  The Android cross-build uses these.
BASE_CFLAGS := $(CFLAGS)

ANDROID_TARGET := wild7_libretro_android.so

ifeq ($(ARCH),aarch64)
  CFLAGS += -mcpu=native
endif
ifeq ($(ARCH),armv7l)
  CFLAGS += -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
endif
ifeq ($(ARCH),armv6l)
  CFLAGS += -march=armv6zk -mfpu=vfp -mfloat-abi=hard
endif

.PHONY: all clean
all: $(TARGET)

$(HDR):
	@echo "fetching libretro.h ..."
	@curl -sfL -o $(HDR) \
	  https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h \
	  || (echo "download failed — copy libretro.h into src/ by hand"; exit 1)

$(TARGET): $(SRC) $(HDR) $(MODS)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@echo "built $@ for $(ARCH)"

clean:
	rm -f $(TARGET)

# =====================================================================
#  Android cross-build — for the Arcade1Up cabinet, or any Android cab.
#
#    make android                 both ABIs
#    make android-armeabi-v7a     32-bit only (the P71 board is ARMv7)
#    make clean-android
#
#  Needs an Android NDK: point NDK_HOME at it, or export ANDROID_NDK_HOME.
#  Output lands in android/<abi>/ under the name RetroArch for Android
#  expects, so "Load Core -> Install or Restore a Core" picks it up and
#  pairs it with the .info file.
#
#  Watch the float convention.  armeabi-v7a is softfp; the native Pi build
#  above is hard-float.  You cannot just carry -mfloat-abi=hard across: the
#  NDK clang refuses it outright ("unsupported option") for an Android
#  triple, so the build stops rather than producing something subtly wrong.
#
#  The per-ABI rules are generated rather than written as one pattern rule,
#  because make will not apply a pattern rule to a .PHONY target.
# =====================================================================
NDK_HOME ?= $(ANDROID_NDK_HOME)
NDK_HOST ?= linux-x86_64
NDK_API  ?= 21
NDK_BIN   = $(NDK_HOME)/toolchains/llvm/prebuilt/$(NDK_HOST)/bin

.PHONY: android android-armeabi-v7a android-arm64-v8a clean-android

android: android-armeabi-v7a android-arm64-v8a

define ANDROID_RULE
android-$(1): $$(SRC) $$(HDR) $$(MODS)
	@test -n "$$(NDK_HOME)" || { echo "set NDK_HOME (or ANDROID_NDK_HOME) to an Android NDK"; exit 1; }
	@test -x "$$(NDK_BIN)/$(2)" || { echo "no NDK clang at $$(NDK_BIN)/$(2)"; exit 1; }
	@mkdir -p android/$(1)
	$$(NDK_BIN)/$(2) $$(BASE_CFLAGS) $(3) -shared -o android/$(1)/$$(ANDROID_TARGET) $$(SRC) -lm
	@echo "built android/$(1)/$$(ANDROID_TARGET)"
endef

$(eval $(call ANDROID_RULE,armeabi-v7a,armv7a-linux-androideabi$(NDK_API)-clang,-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp))
$(eval $(call ANDROID_RULE,arm64-v8a,aarch64-linux-android$(NDK_API)-clang,))

clean-android:
	rm -rf android

# =====================================================================
#  Development tools (also build in WSL):
#    make sim     RTP simulator  ->  ./w7sim 5000000 [betIdx]
#    make shot    headless frame dumper -> ./w7shot -n 600 -s 120,599 -o dir
#    make audio   sound bench + WAV stats -> ./w7audio ; ./w7audio stat a.wav
# =====================================================================
.PHONY: sim shot audio
sim: w7sim
shot: w7shot
audio: w7audio
w7sim: src/sim.c $(SRC) $(HDR) $(MODS)
	$(CC) -O2 -Isrc -o $@ src/sim.c -lm -lpthread
w7shot: tools/w7shot.c $(SRC) $(HDR) $(MODS)
	$(CC) -O2 -Isrc -o $@ tools/w7shot.c -lm -lz -lpthread
w7audio: tools/w7audio.c $(SRC) $(HDR) $(MODS)
	$(CC) -O3 -ffast-math -fno-math-errno -Isrc -o $@ tools/w7audio.c -lm -lpthread
