# Optional official Steinberg SDK (MIT). Pinned build subset is vendored in third_party/vst3sdk.
VST3_SDK ?= ../third_party/vst3sdk
VST_DEFS = -DRELEASE=1 -I$(VST3_SDK)
SDK_SOURCES = pluginterfaces/base/funknown.cpp pluginterfaces/base/ustring.cpp pluginterfaces/base/coreiids.cpp \
 base/source/baseiids.cpp base/source/fobject.cpp base/source/fstring.cpp base/source/fbuffer.cpp \
 base/source/fdebug.cpp base/source/timer.cpp base/source/updatehandler.cpp base/thread/source/flock.cpp \
 public.sdk/source/common/commoniids.cpp public.sdk/source/common/pluginview.cpp \
 public.sdk/source/main/pluginfactory.cpp public.sdk/source/vst/vstinitiids.cpp \
 public.sdk/source/vst/vstcomponentbase.cpp public.sdk/source/vst/vstbus.cpp \
 public.sdk/source/vst/vstparameters.cpp \
 public.sdk/source/vst/vstsinglecomponenteffect.cpp
SDK_OBJ = $(addprefix $(BUILD)/sdk/,$(SDK_SOURCES:.cpp=.o))
$(BUILD)/sdk/%.o: $(VST3_SDK)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VST_DEFS) -w -MMD -MP -c $< -o $@
$(BUILD)/obj/eiii_vst3.o: ../plugin/eiii_vst3.cpp | $(BUILD)/obj
	$(CXX) $(CXXFLAGS) $(VST_DEFS) -I.. -MMD -MP -c $< -o $@
VST_MODULE = $(BUILD)/EIII-Rack.vst3/Contents/x86_64-win/EIII-Rack.vst3
$(VST_MODULE): $(BUILD)/obj/eiii_vst3.o $(SDK_OBJ) $(BUILD)/sdk/public.sdk/source/main/dllmain.o $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a
	mkdir -p $(dir $@)
	$(CXX) -shared -static -o $@ $^ -lcomdlg32 -lole32 -luuid $(LDLIBS_X) $(LDLIBS)
windows-vst3:
	$(MAKE) TARGET=windows build/windows/EIII-Rack.vst3/Contents/x86_64-win/EIII-Rack.vst3
.PHONY: windows-vst3
-include $(SDK_OBJ:.o=.d) $(BUILD)/obj/eiii_vst3.d

HOST_SDK_OBJ = $(BUILD)/sdk/public.sdk/source/common/memorystream.o $(BUILD)/sdk/public.sdk/source/vst/hosting/eventlist.o $(BUILD)/sdk/public.sdk/source/vst/hosting/parameterchanges.o
$(BUILD)/tools/vst3_host_smoke: ../tools/vst3_host_smoke.cpp ../plugin/eiii_vst3.cpp $(SDK_OBJ) $(HOST_SDK_OBJ) $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a | $(BUILD)/tools
	$(CXX) $(CXXFLAGS) $(VST_DEFS) -I.. $< $(SDK_OBJ) $(HOST_SDK_OBJ) $(BUILD)/libeiiiui.a $(BUILD)/libeiiiruntime.a $(BUILD)/libsoftfloat3.a $(LDLIBS) $(LDLIBS_X) -o $@
