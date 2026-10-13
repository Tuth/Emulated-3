// Native host-side regression for the exact VST3 implementation (Windows view excluded).
// Uses original OS panel operations to sample the host input, then plays it through VST3 events.
#include "../plugin/eiii_vst3.cpp"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include <cstdio>
#include <functional>
static int failures=0;
#define CHECK(c,label) do{bool ok=(c);printf("%s %s\n",ok?"PASS":"FAIL",label);fflush(stdout);if(!ok)++failures;}while(0)
int main(){
    if(env("EIII_HD").empty() || env("EIII_ROM_DIR").empty()){puts("SKIP: EIII_HD and EIII_ROM_DIR required");return 77;}
    std::string ansi="C:\\K";ansi+=char(0xf6);ansi+="nyvtar\\E-mu ";ansi+=char(0xe9);ansi+=".iso";
    CHECK(eiii::path_leaf(ansi)==ansi.substr(ansi.find_last_of('\\')+1),"native ANSI accented CD filename is split without conversion");
    CHECK(eiii::path_parent(ansi)==ansi.substr(0,ansi.find_last_of('\\')),"native ANSI accented directory is preserved");
    CHECK(eiii::path_parent("C:\\ROM.bin")=="C:\\" && eiii::path_parent("ROM.bin")==".","drive-root and relative ROM paths");
    CHECK(view_rect(1104).getHeight()==423 && view_rect(1).getWidth()==736 && view_rect(5000).getWidth()==2944,"editor sizes preserve rack aspect and bounds");
    auto *factory=GetPluginFactory();CHECK(factory && factory->countClasses()==1,"factory exposes one instrument class");
    PClassInfo info{};factory->getClassInfo(0,&info);IComponent *component=nullptr;
    CHECK(factory->createInstance(info.cid,IComponent::iid.toTUID(),reinterpret_cast<void **>(&component))==kResultOk,"factory instantiates component");
    if(!component)return 1;
    auto *p=dynamic_cast<RackPlugin *>(component);if(!p)return 1;
    IAudioProcessor *processor=nullptr;IEditController *controller=nullptr;IMidiMapping *mapping=nullptr;
    CHECK(component->queryInterface(IAudioProcessor::iid.toTUID(),reinterpret_cast<void **>(&processor))==kResultOk,"IAudioProcessor interface");
    CHECK(component->queryInterface(IEditController::iid.toTUID(),reinterpret_cast<void **>(&controller))==kResultOk,"combined IEditController interface");
    CHECK(component->queryInterface(IMidiMapping::iid.toTUID(),reinterpret_cast<void **>(&mapping))==kResultOk,"IMidiMapping interface");
    p->media(eiii::media_slot::rom,env("EIII_ROM_DIR"));
    auto rom=env("EIII_ROM_DIR");setenv("EIII_ROM_DIR","",1);
    {RackPlugin fresh;CHECK(fresh.config.rom_dir==rom,"selected ROM is remembered for a fresh instance without Save defaults");}
    setenv("EIII_ROM_DIR",rom.c_str(),1);
    auto boot_floppy=env("EIII_BOOT_FLOPPY");
    if(!boot_floppy.empty()){p->config.floppy_path=boot_floppy;p->config.hd_path.clear();}
    CHECK(component->initialize(nullptr)==kResultOk,"initialize");
    CHECK(component->getBusCount(kAudio,kOutput)==1 && component->getBusCount(kAudio,kInput)==1 && component->getBusCount(kEvent,kInput)==1,"stereo output + sampling input + MIDI bus");
    ParamID bend=0;CHECK(mapping->getMidiControllerAssignment(0,0,kPitchBend,bend)==kResultTrue,"pitch bend mapping");
    bool cc_mapped=true;for(int ch=0;ch<16;++ch)for(int cc=0;cc<130;++cc){ParamID id=0;cc_mapped=cc_mapped && mapping->getMidiControllerAssignment(0,ch,cc,id)==kResultTrue;}
    CHECK(cc_mapped,"CC, pitch bend and channel pressure map on all 16 MIDI channels");
    {
        RackPlugin state_only; state_only.config.floppy_path="disk-test.img";
        MemoryStream v2;CHECK(state_only.getState(&v2)==kResultOk,"write v2 floppy project state");
        state_only.config.floppy_path.clear();v2.seek(0,IBStream::kIBSeekSet,nullptr);
        CHECK(state_only.setState(&v2)==kResultOk && state_only.config.floppy_path=="disk-test.img","floppy path round-trips per project");
        char magic[8]={'E','I','I','I','V','S','T',1};MemoryStream legacy;
        transfer(&legacy,magic,8,true);std::string empty,model="v1";
        for(auto p:{&empty,&empty,&empty,&model,&empty})string_state(&legacy,*p,true,1u<<20);
        for(int i=1;i<8;++i)string_state(&legacy,empty,true,256u<<20);
        legacy.seek(0,IBStream::kIBSeekSet,nullptr);
        CHECK(state_only.setState(&legacy)==kResultOk && state_only.config.floppy_path.empty(),"legacy v1 project state remains readable and has an empty floppy drive");
    }
    ProcessSetup setup{kOffline,kSample32,256,48000.0};
    CHECK(processor->setupProcessing(setup)==kResultOk,"offline 48 kHz / 256 setup");
    CHECK(processor->getLatencySamples()==1024,"reports 1024-frame engine latency");
    component->activateBus(kAudio,kInput,0,true);component->activateBus(kAudio,kOutput,0,true);component->activateBus(kEvent,kInput,0,true);
    CHECK(component->setActive(true)==kResultOk && processor->setProcessing(true)==kResultOk,"activate and start");
    float il[256]{},ir[256]{},ol[256]{},orr[256]{};float *ip[]{il,ir},*op[]{ol,orr};
    AudioBusBuffers input,output;input.numChannels=2;input.silenceFlags=3;input.channelBuffers32=ip;output.numChannels=2;output.silenceFlags=0;output.channelBuffers32=op;
    EventList events;ParameterChanges changes;ProcessData data{};data.processMode=kOffline;data.symbolicSampleSize=kSample32;data.numSamples=256;data.numInputs=1;data.numOutputs=1;data.inputs=&input;data.outputs=&output;data.inputEvents=&events;data.inputParameterChanges=&changes;
    uint64 frame=0;bool tone=false;double sum=0;size_t count=0;
    auto block=[&]{for(int i=0;i<256;++i){il[i]=tone?float(0.5*std::sin(6.283185307179586*1000.0*(frame+i)/48000.0)):0.f;ir[i]=il[i]*0.5f;}input.silenceFlags=tone?0:3;
        if(processor->process(data)!=kResultOk){++failures;return;}
        for(float v:ol){if(!std::isfinite(v))++failures;sum+=double(v)*v;++count;}frame+=256;events.clear();changes.clearQueue();p->panel.poll();};
    auto pump=[&](double seconds){for(size_t i=0;i<size_t(seconds*48000/256);++i)block();};
    auto lcd=[&]{p->panel.poll();eiii::lcd_view v;p->panel.read_lcd(v);std::string s;for(int r=0;r<4;++r)s+=eiii::lcd_row_text(v,r)+"\n";return s;};
    auto wait=[&](char const *text,double seconds){for(size_t i=0;i<size_t(seconds*48000/256);++i){if(lcd().find(text)!=std::string::npos)return true;block();}return lcd().find(text)!=std::string::npos;};
    auto press=[&](char const *name){int c=eiii::panel_find_control(name);p->panel.press_button(c);pump(.08);p->panel.release_button(c);pump(.3);};
    CHECK(wait("P00 Untitled Preset",25),"OS 2.42 boots through VST3 audio processing");
    pump(1.0);press("sample");std::printf("After SAMPLE: %s\n",lcd().c_str());CHECK(wait("S01 Empty Sample",5),"original SAMPLE module");
    press("keypad_7");CHECK(wait("Sampling Armed",5),"original OS arms sampling");
    tone=true;CHECK(wait("Sampling...",5),"VST3 stereo input triggers ADC sampling");pump(1.0);press("enter");tone=false;
    CHECK(wait("S01 Untitled Sample",10),"OS creates sample from VST3 input");
    press("keypad_6");for(int i=0;i<5;++i)press("enter");press("sample");wait("P00",5);pump(.5);
    Event on{};on.type=Event::kNoteOnEvent;on.busIndex=0;on.sampleOffset=128;on.noteOn.channel=0;on.noteOn.pitch=43;on.noteOn.velocity=.8f;on.noteOn.noteId=1;events.addEvent(on);
    sum=0;count=0;pump(.5);double rms=std::sqrt(sum/std::max<size_t>(count,1));printf("MIDI playback RMS %.6f\n",rms);CHECK(rms>.05,"VST3 note-on plays sampled tone on stereo output");
    Event off{};off.type=Event::kNoteOffEvent;off.busIndex=0;off.sampleOffset=32;off.noteOff.channel=0;off.noteOff.pitch=43;off.noteOff.velocity=.5f;off.noteOff.noteId=1;events.addEvent(off);pump(2);
    int32 qi=0;auto *q=changes.addParameterData(bend,qi);int32 pi=0;q->addPoint(10,.75,pi);q->addPoint(200,.5,pi);block();CHECK(p->engine.stats().midi_dropped==0,"MIDI and pitch bend queues do not drop input");
    if(!boot_floppy.empty()) {
        MemoryStream floppy_state;CHECK(component->getState(&floppy_state)==kResultOk,"save active floppy-only VST3 project state");
        processor->setProcessing(false);component->setActive(false);floppy_state.seek(0,IBStream::kIBSeekSet,nullptr);
        p->config.floppy_path.clear();CHECK(component->setState(&floppy_state)==kResultOk && p->config.floppy_path==boot_floppy && p->config.hd_path.empty(),"restore floppy-only project with no HD");
        component->terminate();mapping->release();controller->release();processor->release();component->release();factory->release();
        printf("%s floppy VST3: %d failures\n",failures?"FAIL":"OK",failures);return failures?1:0;
    }
    press("save_bank");CHECK(wait("SAVE BANK into",5),"OS Save Bank prompt");printf("Save prompt: %s\n",lcd().c_str());press("enter");printf("After save Enter: %s\n",lcd().c_str());
    CHECK(wait("Saving Bank",5),"OS writes bank sectors");
    CHECK(wait("P00 ",25),"OS saves sampled bank to D1");pump(2);printf("Saved screen: %s\n",lcd().c_str());
    for(int i=0;i<2000 && p->engine.hd_changes(1).empty();++i)block();
    CHECK(!p->engine.hd_changes(1).empty(),"disk change set contains the saved sample bank");
    p->editor_width=1104;
    MemoryStream saved;CHECK(component->getState(&saved)==kResultOk && saved.getSize()>8,"save VST3 media/display state");
    processor->setProcessing(false);component->setActive(false);saved.seek(0,IBStream::kIBSeekSet,nullptr);
    p->editor_width=1472;
    CHECK(component->setState(&saved)==kResultOk,"restore VST3 state while inactive");
    CHECK(p->editor_width==1104,"editor size round-trips in project state");
    p->config.rom_dir.clear();MemoryStream empty_rom;component->getState(&empty_rom);empty_rom.seek(0,IBStream::kIBSeekSet,nullptr);
    CHECK(component->setState(&empty_rom)==kResultOk && p->config.rom_dir==rom,"empty legacy ROM state falls back to remembered ROM folder");
    char bad[]{'B','A','D'};MemoryStream invalid(bad,3);CHECK(component->setState(&invalid)!=kResultOk,"reject truncated state");
    auto config=p->config;CHECK(config.rom_dir==env("EIII_ROM_DIR") && config.hd_path==env("EIII_HD"),"restored media paths match");
    component->setActive(true);processor->setProcessing(true);pump(10);
    press("load_bank");wait("LOAD BANK from",5);pump(.8);
    if(lcd().find("Select a Bank")!=std::string::npos && lcd().find("D1 ")==std::string::npos)press("cursor_up");
    if(lcd().find("Select a Bank")==std::string::npos){
        if(lcd().find("Select a Drive")==std::string::npos){press("enter");wait("Select a Drive",5);}
        press("keypad_1");wait("D1 ",5);press("enter");wait("Select a Bank",10);
    }
    press("keypad_0");press("keypad_0");printf("Restored bank list: %s\n",lcd().c_str());
    CHECK(wait("B00 Untitled Bank",5),"restored project state retains D1 bank B00");
    press("enter");CHECK(wait("P00 ",30),"saved sample bank loads after state restore");pump(.5);
    on.noteOn.channel=13;events.addEvent(on);sum=0;count=0;pump(.4);rms=std::sqrt(sum/std::max<size_t>(count,1));
    printf("Restored bank RMS %.6f\n",rms);CHECK(rms>.02,"restored sample bank plays via VST3 on MIDI channel 14");
    off.noteOff.channel=13;events.addEvent(off);pump(1);
    std::vector<uint8> oversized(20000,0);oversized.front()=0xf0;oversized.back()=0xf7;
    Event bulk{};bulk.type=Event::kDataEvent;bulk.busIndex=0;bulk.sampleOffset=0;bulk.data.type=DataEvent::kMidiSysEx;bulk.data.bytes=oversized.data();bulk.data.size=oversized.size();events.addEvent(bulk);
    uint8 small_sysex[]{0xf0,0x7d,0x01,0xf7};Event sx=bulk;sx.sampleOffset=20;sx.data.bytes=small_sysex;sx.data.size=4;events.addEvent(sx);
    ParamID sustain=0;mapping->getMidiControllerAssignment(0,13,64,sustain);qi=0;q=changes.addParameterData(sustain,qi);q->addPoint(5,0,pi);
    events.addEvent(on);sum=0;count=0;pump(.4);rms=std::sqrt(sum/std::max<size_t>(count,1));
    CHECK(rms>.02 && p->engine.stats().midi_dropped==0,"oversized SysEx is skipped whole; short SysEx/CC do not prevent the following note");
    events.addEvent(off);pump(1);
    processor->setProcessing(false);component->setActive(false);
    setup.symbolicSampleSize=kSample64;setup.sampleRate=44100;setup.maxSamplesPerBlock=512;CHECK(processor->setupProcessing(setup)==kResultOk,"64-bit / 44.1 kHz setup");
    CHECK(processor->getLatencySamples()==1536,"latency follows host block size");
    component->setActive(true);processor->setProcessing(true);
    double dl[512]{},dr[512]{};double *dp[]{dl,dr};AudioBusBuffers out64;out64.numChannels=2;out64.channelBuffers64=dp;
    ProcessData d64{};d64.processMode=kOffline;d64.symbolicSampleSize=kSample64;d64.numSamples=512;d64.numOutputs=1;d64.outputs=&out64;
    bool finite=true;for(int b=0;b<20;++b){finite=finite && processor->process(d64)==kResultOk;for(double v:dl)finite=finite && std::isfinite(v);}
    CHECK(finite,"64-bit audio process after deactivate/reactivate");
    if(auto seconds=env("EIII_SOAK_SECONDS");!seconds.empty()) {
        double duration=std::strtod(seconds.c_str(),nullptr);
        size_t blocks=size_t(std::ceil(std::clamp(duration,0.0,86400.0)*44100.0/512.0));
        for(size_t b=0;b<blocks;++b) {
            if(processor->process(d64)!=kResultOk){++failures;break;}
            if(b%10000==0){printf("SOAK %.1f / %.1f seconds; emulated %.1f\n",double(b)*512/44100,duration,p->engine.stats().emu_seconds);fflush(stdout);}
        }
        CHECK(p->engine.stats().emu_seconds >= duration,"idle VST3 processor advances past long-run clock boundaries");
    }
    processor->setProcessing(false);component->setActive(false);
    component->terminate();mapping->release();controller->release();processor->release();component->release();factory->release();
    printf("%s: %d failures\n",failures?"FAIL":"OK",failures);return failures?1:0;
}
