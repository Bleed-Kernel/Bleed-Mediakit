LIB_TARGET_64 = libmediakit.a

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S)$(UNAME_M),Darwinarm64)
CC 	:= x86_64-elf-gcc
AR 	:= x86_64-elf-ar
else
CC 	:= gcc
AR 	:= ar
endif

ELF_BIN_DIR_64 = bin
ELF_OBJ_DIR_64 = $(ELF_BIN_DIR_64)/obj

BLIBC_REPO = https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR  ?= $(if $(wildcard ../blibc/include/bleed),../blibc,external/blibc)

# a sibling checkout is used as it is, only our own clone gets pulled
AUDIO_REPO = https://github.com/Bleed-Kernel/libaudio.git
AUDIO_DIR  ?= $(if $(wildcard ../libaudio/Makefile),../libaudio,external/libaudio)
AUDIO_OUT  = libaudio
LIBAUDIO   = $(AUDIO_OUT)/libaudio.a

COMMON_CFLAGS = \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra \
	-O2 \
	-nostdlib \
	-no-pie \
	-m64 \
	-Iinclude \
	-I$(AUDIO_OUT)/include \
	-Isysroot/include \
	-Isysroot/include/bleed \
	-nostdinc \
	-U__linux__ -U__linux -Ulinux -U__gnu_linux__

SRCS        = $(shell find src -name '*.c' -not -path 'src/vendor/*')
LIB_OBJS_64 = $(patsubst %.c,$(ELF_OBJ_DIR_64)/%.o,$(SRCS))
LIBMEDIAKIT = $(ELF_BIN_DIR_64)/$(LIB_TARGET_64)

all: $(LIBMEDIAKIT)

$(LIBMEDIAKIT): $(LIB_OBJS_64)
	@mkdir -p $(ELF_BIN_DIR_64)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS_64)

$(ELF_OBJ_DIR_64)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

-include $(LIB_OBJS_64:.o=.d)

$(LIB_OBJS_64): | blibc audio

blibc:
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	elif [ "$(BLIBC_DIR)" = "external/blibc" ]; then \
		cd $(BLIBC_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(BLIBC_DIR)
	@mkdir -p sysroot
	@cp -Rp $(BLIBC_DIR)/sysroot/* sysroot/

# programs link libaudio themselves, we only need the header but build it so they can take it from here
audio: blibc
	@echo "[AUDIO] Preparing libaudio"
	@if [ ! -d "$(AUDIO_DIR)" ]; then \
		git clone $(AUDIO_REPO) $(AUDIO_DIR); \
	elif [ "$(AUDIO_DIR)" = "external/libaudio" ]; then \
		cd $(AUDIO_DIR) && git pull --ff-only; \
	fi
	@$(MAKE) -C $(AUDIO_DIR) bin/libaudio.a BLIBC_DIR=$(abspath $(BLIBC_DIR))
	@rm -rf $(AUDIO_OUT)/include
	@mkdir -p $(AUDIO_OUT)/include
	@cp -p $(AUDIO_DIR)/include/bleed_audio.h $(AUDIO_OUT)/include/
	@cmp -s $(AUDIO_DIR)/bin/libaudio.a $(LIBAUDIO) || cp -p $(AUDIO_DIR)/bin/libaudio.a $(LIBAUDIO)

build: all

clean:
	rm -rf bin

distclean:
	rm -rf bin sysroot external $(AUDIO_OUT)

.PHONY: all build clean distclean blibc audio
