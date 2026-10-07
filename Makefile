# Fallout 2 CE for PS4 (OpenOrbis). Build with ./build.sh, not bare make.
TITLE       := Fallout 2
VERSION     := 1.00
TITLE_ID    := FALL00002
CONTENT_ID  := IV0000-FALL00002_00-FALLOUT2CE000000

LIBS        := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSceSystemService -lSceCommonDialog -lSceImeDialog -lSDL2
EXTRAFLAGS  := -O2 -w -DNDEBUG -D__PS4__ -DZ_HAVE_UNISTD_H -DHAVE_STB_VORBIS=1

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
CEDIR       := external/fallout2-ce
ZLIB_DIR    := external/zlib
FPAT_DIR    := $(CEDIR)/third_party/fpattern
INTDIR      := obj

# Package files taken from the toolchain's SDL2 sample (override ICON to use your own 512x512 PNG).
SAMPLE_DIR  := $(TOOLCHAIN)/samples/SDL2
ICON        := icon0.png
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

ASSETS      := $(shell find -L gamedata -type f 2>/dev/null)

CE_SRCS     := $(addprefix $(CEDIR)/src/,$(shell cat sources.txt))
ZLIB_SRCS   := $(addprefix $(ZLIB_DIR)/,adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c)
LODE_SRCS   := $(CEDIR)/third_party/lodepng/lodepng.cpp
PS4_SRCS    := $(wildcard src/*.cc)
GITVER_H    := $(INTDIR)/gen/platform/git_version.h

CE_OBJS     := $(patsubst $(CEDIR)/src/%.cc,$(INTDIR)/ce/%.o,$(CE_SRCS))
C_OBJS      := $(patsubst external/%.c,$(INTDIR)/%.o,$(ZLIB_SRCS))
FPAT_OBJS   := $(INTDIR)/fpattern/fpattern.o $(INTDIR)/fpattern/fpattern_windows.o
LODE_OBJS   := $(INTDIR)/ce/lodepng.o
PS4_OBJS    := $(patsubst src/%.cc,$(INTDIR)/ps4/%.o,$(PS4_SRCS))
OBJS        := $(CE_OBJS) $(C_OBJS) $(FPAT_OBJS) $(LODE_OBJS) $(PS4_OBJS)

INCS        := -Isrc -I$(TOOLCHAIN)/include/SDL2 -I$(CEDIR)/src -I$(INTDIR)/gen -I$(CEDIR)/third_party/lodepng -I$(CEDIR)/third_party/stb_vorbis -I$(ZLIB_DIR) -I$(FPAT_DIR)/include
CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include $(INCS)
CXXFLAGS    := $(CFLAGS) -std=gnu++17 -include stdlib.h -include cmath -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

CC := clang
CXX := clang++
LD := ld.lld
CDIR := linux

.PHONY: objs all clean
objs: $(OBJS)

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"
	python3 scripts/fix_gp4.py $@

sce_module/%.prx: $(SAMPLE_DIR)/sce_module/%.prx
	@mkdir -p $(dir $@)
	cp $< $@

sce_sys/about/right.sprx: $(SAMPLE_DIR)/sce_sys/about/right.sprx
	@mkdir -p $(dir $@)
	cp $< $@

sce_sys/icon0.png: $(ICON)
	@mkdir -p $(dir $@)
	cp $< $@

sce_sys/param.sfo: Makefile
	@mkdir -p $(dir $@)
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/fallout2.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/fallout2.elf -out=$(INTDIR)/fallout2.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/ce/%.o: $(CEDIR)/src/%.cc | $(GITVER_H)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(INTDIR)/ce/lodepng.o: $(LODE_SRCS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Stand-in for the header CMake's gitver.cmake configures from git_version.h.in.
$(GITVER_H): $(CEDIR)/src/platform/git_version.h.in
	@mkdir -p $(dir $@)
	sed -e "s|@AUTHOR@|$$(git -C $(CEDIR) log -1 --pretty=format:%an 2>/dev/null)|" \
	    -e "s|@BRANCH@|ps4|" \
	    -e "s|@HASH@|$$(git -C $(CEDIR) log -1 --pretty=format:%h 2>/dev/null)|" \
	    -e "s|@LATEST_TAG@|$$(git -C $(CEDIR) describe --tags --abbrev=0 2>/dev/null)|" \
	    -e "s|@DATE@|\"$$(git -C $(CEDIR) log -1 --date=format:'%b %d %Y %H:%M:%S' --pretty=format:%cd 2>/dev/null)\"|" \
	    -e "s|@CI_BUILD@|0|" $< > $@

$(INTDIR)/ps4/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

# fpattern is built twice, as in its CMakeLists: native and Windows-path flavours.
$(INTDIR)/fpattern/fpattern.o: $(FPAT_DIR)/src/fpattern.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Dunix=1 -o $@ $<

$(INTDIR)/fpattern/fpattern_windows.o: $(FPAT_DIR)/src/fpattern.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DFPAT_WINDOWS_PATHS -Dfpattern_isvalid=fpattern_windows_isvalid -Dfpattern_match=fpattern_windows_match -Dfpattern_matchn=fpattern_windows_matchn -o $@ $<

$(INTDIR)/%.o: external/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(INTDIR) eboot.bin pkg.gp4 *.pkg sce_sys sce_module
