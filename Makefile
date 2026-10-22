# Savage Wind build
# macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer.
# Windows: MSYS2 UCRT64 shell (see docs/BUILD-windows.md), clang + Vulkan renderer.
# Linux: clang + Vulkan renderer, x86-64 or aarch64 (see docs/linux.md).
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of
# Homebrew's (make deps). `make app` sets all three (build/mac15, 15.0, build/deps); plain `make` uses none.
B         ?= build
MACOS_MIN ?=
DEPS      ?=
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := clang $(MINFLAG) $(CROSS)
CXX      := clang++ $(MINFLAG) $(CROSS)
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it. $(GEN)/layout.c, then a folder per module (main, subsdk*, sdk)
GEN_SRC  := $(wildcard $(GEN)/*.c) $(wildcard $(GEN)/*/*.c)
GEN_OBJ  := $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),macos)
SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
EXE      := $(B)/savagewind
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/swgame.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the start screen's build: zig cc)
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/savagewind
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
# Debian / Ubuntu name it libshaderc.so, Arch, Fedora and a shaderc built from source libshaderc_shared.so: use the one
# this system has.
SHADERC  ?= $(shell $(CC) -x c /dev/null -lshaderc -o /dev/null 2>/dev/null && echo -lshaderc || echo -lshaderc_shared)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/swgame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else
EXE      := $(B)/savagewind.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lvulkan-1 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows)  # release: no console window (log -> savagewind.log)
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/swgame.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/savagewind.dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/savagewind.dll.a
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
# "Savage Wind Configuration.exe" in the release: starts savagewind.exe --config (runtime/platform/windows_config.c)
CONFIG_EXE := $(B)/savagewind-config.exe
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_SRCS += tools/layout.py
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC))
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn -isystem third_party $(DEPINC) $(OSFLAGS)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn -isystem third_party -Ithird_party/stb $(DEPINC)

all: $(EXE) $(CONFIG_EXE) $(if $(strip $(GEN_SRC)),$(GAMELIB))  # (GEN_SRC is a space when there is no generated/)

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h  # (cpu.h includes both)
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@
$(B)/obj/layout.o: runtime/gamelib.h

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own): on Windows the import library records the name the game code library loads it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp $^
	mv -f $@.tmp $@
	echo $(GAMELIB_ID) > $@.id

$(B)/rt/ui/i18n.o: $(wildcard runtime/ui/i18n/*.inc) runtime/ui/i18n.h
$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# Python: tools/venv (macOS, Linux: bin/python, Windows: Scripts/python.exe)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

gen:  # every module of the exefs, at the addresses tools/layout.py gives them
	cd tools && ../$(PY) gen.py all ../extracted/exefs ../$(GEN) --hle ../runtime/hle/hle_symbols.txt $(GENSIMD)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -O coff $< -o $@

$(B)/rt/platform/windows_config.o: runtime/platform/windows_config.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall $(OSFLAGS) -c $< -o $@
$(B)/savagewind-config.exe: $(B)/rt/platform/windows_config.o $(B)/rt/platform/windows_rc.o
	$(CC) -o $@ $^ -fuse-ld=lld -Wl,--subsystem,windows

VERSION  := $(shell sed -n 's/^\#define SAVAGEWIND_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)

# macOS: build/Savage Wind.app, runs on macOS $(APP_MIN)+: the game rebuilt for that in build/mac15 against build/deps
# (`make deps`: SDL3, LGPL FFmpeg, Python, from pinned sources), the generator helper when built
# (`make gen-helper` after `make deps`), packaged by tools/bundle_macos.sh. No game files or fonts.
APP_MIN  := 15.0
APP_B    := build/mac15
deps:
	MACOS_MIN=$(APP_MIN) tools/build_deps_macos.sh
app:
	@[ -f build/deps/lib/libSDL3.0.dylib ] || { echo "make deps first"; exit 1; }
	$(MAKE) B=$(APP_B) MACOS_MIN=$(APP_MIN) DEPS=$(CURDIR)/build/deps $(APP_B)/savagewind
	MACOS_MIN=$(APP_MIN) tools/bundle_macos.sh $(APP_B)/savagewind

# macOS release disk image (the .app + an Applications shortcut to drag it onto): build/dist/Savage Wind-<v>-macOS.dmg
dmg: app
	tools/make_dmg.sh

# Windows: build/dist/SavageWind-<version>-windows.zip, top folder "Savage Wind/": the exe, what the start screen
# builds the game code with on the player's machine (make gen-helper, tools/fetch_zig.sh slimmed by
# tools/zig_slim.sh: its runtime built ahead of time, gamelib-src/, the exe's import library)
# and the DLLs they load (runs without MSYS2). No game code. The exe is a release build in $(WIN_B):
# linked against the LGPL FFmpeg from `make deps-windows` (MSYS2's is a GPLv3 build) and without a console window
# (RELEASE=1: the log goes to savagewind.log next to the exe, fatal errors show a message box). Its DLLs are looked up
# with build/deps-win/bin first (the FFmpeg DLL names are the same as MSYS2's GPL ones). vulkan-1.dll is left
# out: every Vulkan driver installs the loader in System32. Saves and game files go next to the exe at run time.
# Needs: Windows 10+ x64, a Vulkan 1.3 driver, an AVX2 CPU (GENARCH x86-64-v3; checked at startup).
DIST     := build/dist/Savage Wind
WIN_B    := build/win-release
deps-windows:
	tools/build_deps_windows.sh
dist:
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -d build/helper/swgen ] || { echo "make gen-helper first (the game code generator)"; exit 1; }
	@! find tools -maxdepth 1 -name '*.py' -newer build/helper/swgen/swgen.exe | grep -q . || \
	    { echo "make gen-helper first: tools/*.py changed since build/helper/swgen was frozen"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 $(WIN_B)/savagewind.exe $(WIN_B)/savagewind-config.exe
	! nm $(WIN_B)/savagewind.exe | grep ' f_70' >/dev/null  # (no game code inside)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/savagewind.exe $(WIN_B)/savagewind.dll.a "$(DIST)/"
	cp $(WIN_B)/savagewind-config.exe "$(DIST)/Savage Wind Configuration.exe"
	tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	cp -R build/helper/swgen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	cp docs/icon/icon_1024.png "$(DIST)/icon.png"
	cp third_party/fonts/FiraSans/FiraSans-SemiBold.ttf third_party/fonts/FiraSans/FiraSans-Medium.ttf "$(DIST)/"  # (menus, OFL)
	cp third_party/fonts/FiraSans/OFL.txt "$(DIST)/FiraSans-OFL.txt"
	cp README.md LICENSE "$(DIST)/"  # (licenses and source links of the bundled components: README "License")
	PATH="$(CURDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/savagewind.exe | awk '(/ucrt64/ || /deps-win/) && !/vulkan-1/ {print $$3}' | sort -u | \
	    xargs -I{} cp -u {} "$(DIST)/"
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (no GPL codec libraries in the release)
	! ls "$(DIST)" | grep -i splash  # (no start screen picture: the player's own, never shipped)
	cd build/dist && if command -v zip >/dev/null; then zip -qr "SavageWind-$(VERSION)-windows.zip" "Savage Wind"; \
	    else /c/Windows/System32/tar.exe -a -cf "SavageWind-$(VERSION)-windows.zip" "Savage Wind"; fi
	@echo "build/dist/SavageWind-$(VERSION)-windows.zip"

# The game code generator as a standalone helper, so players need no Python: build/helper/swgen/ (swgen[.exe] +
# _internal/), frozen by PyInstaller from tools/swgen.spec in its own venv (build/gen-venv). Shipped as swgen/ next to
# the exe (Windows, Linux) or in "Savage Wind.app"/Contents/Resources. The start screen's setup runs it on the player's own
# game files (runtime/gamebuild.c). PyInstaller only builds for the OS it runs on.
# macOS arm64: the relocatable Python 3.12 from `make deps` (macOS 11+ / 15+: the helper runs where the app does);
#   without it Homebrew's python@3.12 (then only on the macOS it was built for).
# Windows x86-64: from the MSYS2 shell with a python.org CPython 3.12 (prebuilt unicorn / capstone wheels), e.g.
#   make gen-helper GEN_PY="/c/Users/<you>/AppData/Local/Programs/Python/Python312/python.exe"
ifeq ($(PLATFORM),macos)
GEN_PY    ?= $(firstword $(wildcard build/deps/python/bin/python3.12) python3.12)
GEN_VPY   := build/gen-venv/bin/python
else
GEN_PY    ?= py -3.12
GEN_VPY   := build/gen-venv/Scripts/python.exe
endif

gen-helper: build/gen-venv/.ok
	$(if $(filter windows,$(PLATFORM)),USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)") \
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/swgen.spec
	$(if $(filter windows,$(PLATFORM)),$(GEN_VPY) tools/pe_no_cfg.py build/helper/swgen/swgen.exe)
	@du -sh build/helper/swgen

build/gen-venv/.ok:
	$(GEN_PY) -m venv build/gen-venv
	$(GEN_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
	touch $@

# Linux: build/dist/Savage_Wind-<version>-<arch>.AppImage, built in Ubuntu 22.04 containers (podman) with SDL3, LGPL
# FFmpeg, shaderc and the game code generator helper inside; the Vulkan loader and GPU drivers are the system's.
# `make appimage` (ARCH: this machine's, or aarch64 cross-compiled), `make appimages` both. See docs/linux.md.
appimage:
	tools/linux/appimage.sh $(ARCH)
appimages:
	tools/linux/appimage.sh x86_64
	tools/linux/appimage.sh aarch64

.PHONY: dmg all gen gen-helper gen-objs runtime-objs deps deps-windows app dist appimage appimages

build/test_shaders: tools/test_shaders.m runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
	clang -O1 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shaders.m runtime/nvn/maxwell/translate.c
