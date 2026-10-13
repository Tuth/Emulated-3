# eiii-standalone: E-mu Emulator III runtime.
#   unmodified MAME device sources (BSD-3-Clause, vendored in third_party/mame by tools/vendor_mame.sh) compiled against
#   shim/emu.h, Berkeley SoftFloat 3e built with MAME's configuration, the EIII motherboard (eiii_host.cpp), the portable
#   eiii-core output hardware (../fchip, ../cem, ../host) and ALSA host I/O.
#
#   make              build/default  -> ./eiii-standalone           (development, used by the check_*.sh regressions)
#   make release      build/release  -> dist/eiii-standalone        (+ README.md)
#   make debug        build/debug    -> build/debug/eiii-standalone (-O0 -g)
#   make asan         build/asan     -> build/asan/eiii-standalone  (AddressSanitizer + UBSan)
#   make check        all regressions (needs the ROMs and the original disk images; see README.md)
#   MAME=~/EIII_RE/mame make ...      build from a live MAME tree instead of the vendored copy

MAME     ?= third_party/mame
CXX      ?= g++
BUILD    ?= build/default
OUT      ?= $(BUILD)/eiii-standalone
OPT      ?= -O2
# -ffp-contract=off: no fused multiply-add contraction, so the analog model is bit-identical on x86-64 and ARM64
CXXFLAGS ?= -std=c++20 $(OPT) -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
SFFLAGS  ?= -x c++ -std=c++17 $(OPT) -ffp-contract=off -w
LDLIBS    = -lasound -lpthread
INC       = -I.. -Ishim -I$(MAME) -I$(MAME)/src/lib -I$(MAME)/src/emu -I$(MAME)/src/osd -I$(MAME)/src/lib/util -I$(MAME)/src/devices \
            -I$(MAME)/src/devices/cpu/ns32000 -I$(MAME)/3rdparty
SFDIR     = $(MAME)/3rdparty/softfloat3
SFDEFS    = -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1
SFINC     = -I$(MAME)/src/osd -I$(SFDIR)/build/MAME -I$(SFDIR)/source -I$(SFDIR)/source/include -I$(SFDIR)/source/8086 -I$(SFDIR)/bochs_ext

SRC_HOST  = eiii_host.cpp eiii_gui.cpp host_io.cpp shim/emu_shim.cpp eiii_scsi.cpp diserial_shim.cpp
SRC_CORE  = ../fchip/eiii_fchip.cpp ../cem/eiii_cem3387.cpp ../cem/eiii_cem_calibration.cpp ../cem/eiii_cem_cv_conditioning.cpp \
            ../cem/eiii_cem3387_v2.cpp ../host/eiii_backend.cpp ../runtime/eiii_runtime.cpp ../runtime/eiii_engine.cpp \
            ../panel/eiii_lcd.cpp ../panel/eiii_panel_model.cpp ../panel/eiii_panel_adapter.cpp
# UI layer (software renderer, panel artwork) — linked by the GUI/plugin/tools only
SRC_UI    = ../ui/raster.cpp ../ui/panel_view.cpp ../ui/panel_menu.cpp ../ui/x11_window.cpp ../ui/panel_window.cpp
LDLIBS_X  = -lX11 -lXext
SRC_MAME  = $(addprefix $(MAME)/src/, devices/cpu/ns32000/ns32000.cpp devices/cpu/ns32000/ns32000d.cpp devices/machine/ns32081.cpp \
            devices/machine/nscsi_bus.cpp devices/machine/ncr5380.cpp devices/machine/nscsi_hle.cpp devices/bus/nscsi/hd.cpp \
            devices/bus/nscsi/cd.cpp devices/machine/z80scc.cpp devices/machine/z80daisy.cpp lib/util/strformat.cpp \
            lib/util/coreutil.cpp lib/util/disasmintf.cpp)
SRC_SF    = $(addprefix $(MAME)/,$(shell cat $(MAME)/softfloat3_files.txt 2>/dev/null))

# TARGET=windows: native MinGW-w64 or cross compilation from Linux.
ifeq ($(TARGET),windows)
CROSS ?= x86_64-w64-mingw32-
CXX = $(CROSS)g++
CC = $(CROSS)gcc
AR = $(CROSS)ar
BUILD = build/windows
OUT = $(BUILD)/eiii-standalone.exe
CXXFLAGS += -D_WIN32_WINNT=0x0601 -DNOMINMAX
SRC_HOST := $(filter-out host_io.cpp,$(SRC_HOST)) host_io_win.cpp
SRC_UI := $(filter-out ../ui/x11_window.cpp,$(SRC_UI)) ../ui/win32_window.cpp
LDLIBS = -lwinmm -lpthread
LDLIBS_X = -lgdi32 -luser32
LDFLAGS += -static
endif
AR ?= ar

OBJ       = $(addprefix $(BUILD)/obj/,$(notdir $(SRC_HOST:.cpp=.o) $(SRC_CORE:.cpp=.o) $(SRC_MAME:.cpp=.o)))
OBJ_SF    = $(addprefix $(BUILD)/softfloat/,$(notdir $(SRC_SF:.c=.o)))
TRACES   ?= $(HOME)/EIII_RE/analysis/traces/standalone_spike

.DEFAULT_GOAL := eiii-standalone

eiii-standalone: $(BUILD)/eiii-standalone
	cp $< $@

# everything except the standalone's main: the runtime library shared by the standalone, the tests and the plugin
LIB_OBJS  = $(filter-out $(BUILD)/obj/eiii_host.o $(BUILD)/obj/eiii_gui.o,$(OBJ))
$(BUILD)/libeiiiruntime.a: $(LIB_OBJS)
	rm -f $@; $(AR) rcs $@ $^

$(OUT): $(BUILD)/obj/eiii_host.o $(BUILD)/obj/eiii_gui.o $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a
	$(CXX) $(LDFLAGS) -o $@ $(BUILD)/obj/eiii_host.o $(BUILD)/obj/eiii_gui.o $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a $(LDLIBS) $(LDLIBS_X)

# UI library: renderer + panel artwork + embedded OFL fonts
OBJ_UI    = $(addprefix $(BUILD)/obj/,$(notdir $(SRC_UI:.cpp=.o))) $(BUILD)/obj/fonts.o
$(BUILD)/obj/fonts.o: ../ui/fonts.S ../third_party/fonts/*.ttf | $(BUILD)/obj
	$(CC) -c $< -Wa,-I../third_party/fonts -o $@
$(BUILD)/libeiiiui.a: $(OBJ_UI)
	rm -f $@; $(AR) rcs $@ $^
$(BUILD)/tools/clap_host_test: ../tools/clap_host_test.cpp | $(BUILD)/tools
	$(CXX) $(CXXFLAGS) -I../third_party/clap/include $< -o $@ -lX11 -ldl -lpthread
$(BUILD)/tools/%: ../tools/%.cpp $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a | $(BUILD)/tools
	$(CXX) $(CXXFLAGS) -I.. -MMD -MP $< $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a $(LDLIBS) $(LDLIBS_X) -o $@
$(BUILD)/tools:
	mkdir -p $@
-include $(OBJ_UI:.o=.d)

# CLAP plugin: the same sources built position-independent with hidden visibility; only clap_entry is exported
plugin:
	$(MAKE) BUILD=build/pic OPT="-O2 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden" build/pic/EIII-Rack.clap
	@echo "CLAP plugin: $(CURDIR)/build/pic/EIII-Rack.clap"
$(BUILD)/EIII-Rack.clap: $(BUILD)/obj/eiii_clap.o $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a ../plugin/exports.map
	$(CXX) -shared $(PLUGIN_LDFLAGS) -o $@ $(BUILD)/obj/eiii_clap.o $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a \
	       -Wl,--version-script=../plugin/exports.map -Wl,--exclude-libs,ALL -Wl,-z,defs $(LDLIBS_X) -lasound -lpthread
-include $(BUILD)/obj/eiii_clap.d

# panel view tests (link the UI library)
$(BUILD)/tests/display_themes: ../tests/display_themes.cpp $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a | $(BUILD)/tests
	$(CXX) $(CXXFLAGS) -I.. -MMD -MP $< $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a $(LDLIBS) $(LDLIBS_X) -o $@

# runtime tests (no MAME headers needed by the test sources)
$(BUILD)/tests/%: ../tests/%.cpp $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a | $(BUILD)/tests
	$(CXX) $(CXXFLAGS) -I.. -MMD -MP $< $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a $(LDLIBS) -o $@
$(BUILD)/tests:
	mkdir -p $@

$(BUILD)/libsoftfloat3.a: $(OBJ_SF)
	rm -f $@; $(AR) rcs $@ $^

define cxx_rule
$(BUILD)/obj/$(notdir $(1:.cpp=.o)): $(1) | $(BUILD)/obj
	$$(CXX) $$(CXXFLAGS) $(2) $$(INC) -MMD -MP -c $$< -o $$@
endef
$(foreach s,$(filter-out eiii_scsi.cpp,$(SRC_HOST)) $(SRC_CORE),$(eval $(call cxx_rule,$(s),)))
$(foreach s,$(SRC_UI),$(eval $(call cxx_rule,$(s),)))
$(eval $(call cxx_rule,../plugin/eiii_clap.cpp,-I../third_party/clap/include))
# eiii_scsi.cpp is extracted verbatim from emu3.cpp and includes MAME headers: treat like upstream code
$(eval $(call cxx_rule,eiii_scsi.cpp,-w))
# upstream MAME files are compiled unmodified; their warnings are not ours to fix
$(foreach s,$(SRC_MAME),$(eval $(call cxx_rule,$(s),-w)))
define sf_rule
$(BUILD)/softfloat/$(notdir $(1:.c=.o)): $(1) | $(BUILD)/softfloat
	$$(CXX) $$(SFFLAGS) $$(SFDEFS) $$(SFINC) -c $$< -o $$@
endef
$(foreach s,$(SRC_SF),$(eval $(call sf_rule,$(s))))

$(BUILD)/obj $(BUILD)/softfloat:
	mkdir -p $@

-include $(OBJ:.o=.d)

release:
	$(MAKE) BUILD=build/release OPT="-O2 -DNDEBUG" LDFLAGS="-static-libstdc++ -static-libgcc" OUT=build/release/eiii-standalone build/release/eiii-standalone
	mkdir -p dist
	cp build/release/eiii-standalone dist/eiii-standalone
	strip dist/eiii-standalone
	cp README.md dist/README.md
	cd dist && sha256sum eiii-standalone README.md > SHA256SUMS
	@ls -la dist; echo "release binary: $(CURDIR)/dist/eiii-standalone"
# Release archive: CLAP plugin + standalone (C++ runtime linked statically: needs glibc >= 2.38, libX11, libXext,
# libasound), docs, licences.  -> dist/Emulated-3-v$(VERSION)-linux-x86_64.tar.gz
VERSION  ?= 0.1.0-beta
PKG       = Emulated-3-v$(VERSION)-linux-x86_64
package: release
	$(MAKE) BUILD=build/pic-release OPT="-O2 -DNDEBUG -fPIC -fvisibility=hidden -fvisibility-inlines-hidden" \
	        PLUGIN_LDFLAGS="-static-libstdc++ -static-libgcc" build/pic-release/EIII-Rack.clap
	rm -rf dist/$(PKG) && mkdir -p dist/$(PKG)/licenses
	cp build/pic-release/EIII-Rack.clap dist/$(PKG)/ && strip --strip-unneeded dist/$(PKG)/EIII-Rack.clap
	cp dist/eiii-standalone dist/$(PKG)/
	cp ../README.md ../INSTALL.md ../LICENSE ../docs/eiii.conf.example dist/$(PKG)/
	cp README.md dist/$(PKG)/STANDALONE.md
	cp ../third_party/clap/LICENSE dist/$(PKG)/licenses/CLAP-MIT.txt
	cp ../third_party/stb/LICENSE dist/$(PKG)/licenses/stb.txt
	cp ../third_party/fonts/OFL-ArchivoNarrow.txt ../third_party/fonts/OFL-NotoSans.txt dist/$(PKG)/licenses/
	sed 's|/home/[^`]*/mame|a local MAME checkout|' $(MAME)/PROVENANCE.md > dist/$(PKG)/licenses/MAME-PROVENANCE.md
	cp $(MAME)/3rdparty/softfloat3/COPYING.txt dist/$(PKG)/licenses/SoftFloat-COPYING.txt
	printf '%s\n' 'MAME device sources used by this build (CPU, FPU, SCSI, serial): BSD-3-Clause.' \
	  'Copyright (c) the MAME authors (see https://github.com/mamedev/mame).' '' \
	  'Redistribution and use in source and binary forms, with or without modification, are permitted provided that the' \
	  'following conditions are met: (1) Redistributions of source code must retain the above copyright notice, this list' \
	  'of conditions and the following disclaimer. (2) Redistributions in binary form must reproduce the above copyright' \
	  'notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided' \
	  'with the distribution. (3) Neither the name of the copyright holder nor the names of its contributors may be used' \
	  'to endorse or promote products derived from this software without specific prior written permission.' '' \
	  'THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,' \
	  'INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE' \
	  'DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,' \
	  'SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR' \
	  'SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,' \
	  'WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE' \
	  'USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.' > dist/$(PKG)/licenses/MAME-BSD-3-Clause.txt
	cd dist/$(PKG) && sha256sum EIII-Rack.clap eiii-standalone > SHA256SUMS
	cd dist && tar czf $(PKG).tar.gz $(PKG) && sha256sum $(PKG).tar.gz > $(PKG).tar.gz.sha256
	@ls -la dist/$(PKG) dist/$(PKG).tar.gz

debug:
	$(MAKE) BUILD=build/debug OPT="-O0 -g" OUT=build/debug/eiii-standalone build/debug/eiii-standalone
asan:
	$(MAKE) BUILD=build/asan OPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" LDFLAGS="-fsanitize=address,undefined" \
	        OUT=build/asan/eiii-standalone build/asan/eiii-standalone

# all regressions (MAME equivalence, original-UI factory load, runtime, audio); see README.md for what each proves
check: eiii-standalone
	./check_boot.sh
	./check_panel.sh
	./check_factory_load.sh
	./check_runtime.sh
	./check_audio.sh
	./check_cem_v2.sh
	$(MAKE) $(BUILD)/tests/lcd_mapping $(BUILD)/tests/panel_functional $(BUILD)/tests/engine_instances $(BUILD)/tests/display_themes $(BUILD)/tests/media_swap $(BUILD)/tests/sampling $(BUILD)/tests/hd_changes $(BUILD)/tools/clap_host_test
	./check_panel_ui.sh

clean:
	rm -rf build eiii-standalone
distclean: clean
	rm -rf dist
.PHONY: eiii-standalone release package debug asan check clean distclean plugin

windows:
	$(MAKE) TARGET=windows build/windows/eiii-standalone.exe
.PHONY: windows

include vst3.mk
