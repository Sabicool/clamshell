# 3DS build (devkitPro devkitARM + libctru). From the repo root:
#   make -C 3ds            -> 3ds/clamshell.3dsx (copy to sdmc:/3ds/)
#   make -C 3ds clean
#
# Library sources are compiled straight from ../third_party so nothing needs
# installing beyond devkitARM and libctru.
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
CC := $(PREFIX)gcc
CXX := $(PREFIX)g++
TOOLS := $(DEVKITPRO)/tools/bin

ROOT := ..
TP := $(ROOT)/third_party
BUILD := build
TARGET := clamshell
APP_TITLE := Clamshell
APP_DESC := Anki-compatible flashcards (0.2 alpha)
APP_AUTHOR := Sabiqul

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# -ffp-contract=off keeps float rounding identical to desktop Anki's scheduler.
COMMON := $(EXTRA_FLAGS) -MMD -MP -O2 -g -mword-relocations -ffunction-sections -fdata-sections -ffp-contract=off $(ARCH) -D__3DS__ \
	-I$(DEVKITPRO)/libctru/include -I$(ROOT)/app -I$(ROOT)/core -I$(ROOT)/core/render -I$(ROOT)/core/js -I$(TP)/quickjs -I$(TP)/libwebp/src -I$(TP)/libwebp -I$(TP) -I$(TP)/sqlite -I$(TP)/zstd \
	-I$(TP)/litehtml/include -I$(TP)/litehtml/include/litehtml \
	-I$(TP)/litehtml/src/gumbo/include -I$(TP)/litehtml/src/gumbo/include/gumbo \
	-I$(TP)/mbedtls/include -Isource \
	-DMBEDTLS_USER_CONFIG_FILE='"mbedtls_3ds_config.h"'
CFLAGS := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++17
SQLITE_FLAGS := -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_DEFAULT_CACHE_SIZE=-4000
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -L$(DEVKITPRO)/libctru/lib
LIBS := -lctru -lm

APP_SRC := $(wildcard source/*.cpp) $(wildcard source/*.c)
CORE_SRC := $(wildcard $(ROOT)/core/*.cpp $(ROOT)/core/render/*.cpp $(ROOT)/core/js/*.cpp $(ROOT)/app/*.cpp)
LH_SRC := $(wildcard $(TP)/litehtml/src/*.cpp)
GUMBO_SRC := $(wildcard $(TP)/litehtml/src/gumbo/*.c)
QJS_SRC := $(wildcard $(TP)/quickjs/*.c)
WEBP_SRC := $(wildcard $(TP)/libwebp/src/dec/*.c $(TP)/libwebp/src/dsp/*.c $(TP)/libwebp/src/utils/*.c $(TP)/libwebp/src/demux/*.c)
ZSTD_SRC := $(wildcard $(TP)/zstd/common/*.c $(TP)/zstd/compress/*.c $(TP)/zstd/decompress/*.c)
MBED_SRC := $(filter-out %/net_sockets.c %/timing.c,$(wildcard $(TP)/mbedtls/library/*.c))

obj = $(patsubst %,$(BUILD)/%.o,$(subst ../,,$(1)))
OBJS := $(call obj,$(APP_SRC)) $(call obj,$(CORE_SRC)) $(call obj,$(LH_SRC)) $(call obj,$(GUMBO_SRC)) \
	$(call obj,$(QJS_SRC)) $(call obj,$(WEBP_SRC)) $(call obj,$(ZSTD_SRC)) $(call obj,$(MBED_SRC)) $(BUILD)/third_party/sqlite/sqlite3.c.o

all: $(TARGET).3dsx

# Self-test build for emulator runs: make -C 3ds selftest
selftest:
	$(MAKE) BUILD=build-selftest TARGET=clamshell-selftest EXTRA_FLAGS=-DANKI3DS_SELFTEST

$(BUILD)/source/%.cpp.o: source/%.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/source/%.c.o: source/%.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/app/%.cpp.o: $(ROOT)/app/%.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/core/%.cpp.o: $(ROOT)/core/%.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/third_party/litehtml/%.cpp.o: $(TP)/litehtml/%.cpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -w -c $< -o $@
$(BUILD)/third_party/sqlite/sqlite3.c.o: $(TP)/sqlite/sqlite3.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) $(SQLITE_FLAGS) -w -c $< -o $@
$(BUILD)/third_party/zstd/%.c.o: $(TP)/zstd/%.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) -DZSTD_DISABLE_ASM -DZSTD_MULTITHREAD=0 -w -c $< -o $@
$(BUILD)/third_party/%.c.o: $(TP)/%.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) -w -c $< -o $@

$(TARGET).elf: $(OBJS)
	@echo linking $@
	@$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(TARGET).smdh: icon.png
	@$(TOOLS)/smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" icon.png $@

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh $(shell find romfs -type f)
	@$(TOOLS)/3dsxtool $(TARGET).elf $@ --smdh=$(TARGET).smdh --romfs=romfs
	@echo built $@

-include $(OBJS:.o=.d)

clean:
	rm -rf build build-selftest *.elf *.3dsx *.smdh

.PHONY: all clean selftest
