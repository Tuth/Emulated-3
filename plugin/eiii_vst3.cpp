// Windows VST3 instrument. One engine per combined processor/controller instance.
// The sampler runs its original ROM/OS; GUI and MIDI only supply hardware inputs.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/common/pluginview.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "runtime/eiii_engine.h"
#include "panel/eiii_panel_adapter.h"
#include "ui/panel_window.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <thread>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "standalone/native_path.h"
#include <fstream>
#include <memory>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace {
constexpr ParamID midi_base = 0x1000;
constexpr int controllers_per_channel = 131; // CC, pressure, bend, program change
std::string env(char const *key) { auto p=std::getenv(key); return p?p:""; }
std::string config_file() {
#ifdef _WIN32
    auto dir=env("APPDATA");
#else
    auto dir=env("XDG_CONFIG_HOME"); if(dir.empty())dir=env("HOME")+"/.config";
#endif
    return dir.empty()?"":dir+"/eiii/eiii-vst3.conf";
}
int view_width(int w) { return std::clamp(w,736,2944); }
ViewRect view_rect(int width,int x=0,int y=0) {int w=view_width(width);return {x,y,x+w,y+int(std::lround(w*564.0/1472.0))};}
int read_view_width() {
    std::ifstream in(config_file());
    for(std::string line;std::getline(in,line);)if(line.rfind("view_width=",0)==0){int w=1472;auto r=std::from_chars(line.data()+11,line.data()+line.size(),w);if(r.ec==std::errc{})return view_width(w);}
    return 1472;
}
bool remember_preference(std::string const &key,std::string const &value) {
    auto file=config_file();if(file.empty() || !eiii::create_native_directory(eiii::path_parent(file)))return false;
    std::string contents;std::ifstream in(file);
    for(std::string line;std::getline(in,line);)if(line.rfind(key+"=",0)!=0)contents+=line+"\n";
    in.close();contents+=key+"="+value+"\n";
    std::ofstream out(file+".tmp",std::ios::binary);out<<contents;out.close();if(!out)return false;
#ifdef _WIN32
    return MoveFileExA((file+".tmp").c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    return std::rename((file+".tmp").c_str(),file.c_str())==0;
#endif
}
eiii::runtime_config defaults() {
    eiii::runtime_config c;
    std::ifstream in(config_file());
    for(std::string line;std::getline(in,line);) {
        auto pos=line.find('=');if(pos==std::string::npos)continue;
        auto k=line.substr(0,pos),v=line.substr(pos+1);
        if(!v.empty() && v.back()=='\r')v.pop_back();
        if(k=="rom_dir")c.rom_dir=v;else if(k=="hd")c.hd_path=v;else if(k=="cd")c.cd_path=v;else if(k=="cem_model")c.cem_model=v;
    }
    for(auto [key,out]:{std::pair{"EIII_ROM_DIR",&c.rom_dir},{"EIII_HD",&c.hd_path},{"EIII_CD",&c.cd_path}}) {
        auto v=env(key);if(!v.empty())*out=v;
    }
    c.rom_only=c.hd_path.empty();return c;
}
// Length-prefixed state with explicit bounds. No ROM, OS or original image bytes in projects.
bool transfer(IBStream *s,void *p,uint32 n,bool write) {
    while(n) {
        int32 done=0;auto chunk=int32(std::min<uint32>(n,1u<<20));
        auto r=write?s->write(p,chunk,&done):s->read(p,chunk,&done);
        if(r!=kResultOk || done<=0 || done>chunk)return false;
        p=static_cast<char *>(p)+done;n-=uint32(done);
    }
    return true;
}
bool string_state(IBStream *s,std::string &v,bool write,uint32 limit) {
    if(write && v.size()>limit)return false;
    uint32 n=write?uint32(v.size()):0;uint8 bytes[4];
    if(write)for(int i=0;i<4;++i)bytes[i]=uint8(n>>(8*i));
    if(!transfer(s,bytes,4,write))return false;
    if(!write){n=0;for(int i=0;i<4;++i)n|=uint32(bytes[i])<<(8*i);}
    if(n>limit)return false;
    if(!write)v.resize(n);
    return !n || transfer(s,v.data(),n,write);
}
class RackPlugin;
#ifdef _WIN32
class RackView : public CPluginView {
public:
    explicit RackView(RackPlugin &p);
    ~RackView() override;
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {return type && !std::strcmp(type,kPlatformTypeHWND)?kResultTrue:kResultFalse;}
    tresult PLUGIN_API attached(void *parent,FIDString type) override;
    tresult PLUGIN_API removed() override;
    tresult PLUGIN_API onSize(ViewRect *r) override;
    tresult PLUGIN_API canResize() override {return kResultTrue;}
    tresult PLUGIN_API checkSizeConstraint(ViewRect *r) override;
private:
    RackPlugin &plugin;
    eiii::ui::panel_window win;
    std::string pick(char const *filter,char const *title,std::string const &current);
    void menus();
    void request_width(int width);
};
#endif
class RackPlugin : public SingleComponentEffect, public IMidiMapping {
public:
    RackPlugin():config(defaults()),panel(engine),editor_width(read_view_width()) {}
    ~RackPlugin() override {engine.shutdown();}
    static FUnknown *create(void *) {return static_cast<IComponent *>(new RackPlugin);}
    tresult PLUGIN_API queryInterface(TUID const iid,void **out) override {
        if(!out)return kInvalidArgument;
        if(FUnknownPrivate::iidEqual(iid,IMidiMapping::iid)) {*out=static_cast<IMidiMapping *>(this);addRef();return kResultOk;}
        return SingleComponentEffect::queryInterface(iid,out);
    }
    uint32 PLUGIN_API addRef() override {return SingleComponentEffect::addRef();}
    uint32 PLUGIN_API release() override {return SingleComponentEffect::release();}
    tresult PLUGIN_API initialize(FUnknown *context) override {
        auto r=SingleComponentEffect::initialize(context);if(r!=kResultOk)return r;
        addAudioOutput(STR16("Main Out"),SpeakerArr::kStereo);
        addAudioInput(STR16("Sample In"),SpeakerArr::kStereo,kAux,0);
        addEventInput(STR16("MIDI In"),16);
        for(int ch=0;ch<16;++ch)for(int cc=0;cc<controllers_per_channel;++cc) {
            String128 title{};char text[64];std::snprintf(text,sizeof text,"MIDI %d Controller %d",ch+1,cc);
            for(size_t i=0;i<std::strlen(text);++i)title[i]=char16(text[i]);
            parameters.addParameter(title,nullptr,cc==kPitchBend?16383:127,cc==kPitchBend?0.5:0.0,
                ParameterInfo::kIsHidden|ParameterInfo::kCanAutomate,midi_base+ch*controllers_per_channel+cc);
        }
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {stop();return SingleComponentEffect::terminate();}
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus,int16 channel,CtrlNumber cc,ParamID &id) override {
        if(bus!=0 || channel<0 || channel>=16 || cc<0 || cc>=controllers_per_channel)return kResultFalse;
        id=midi_base+channel*controllers_per_channel+cc;return kResultTrue;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {return size==kSample32 || size==kSample64?kResultTrue:kResultFalse;}
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in,int32 ni,SpeakerArrangement *out,int32 no) override {
        if(ni<0 || ni>1 || no!=1 || !out || out[0]!=SpeakerArr::kStereo || (ni && (!in || in[0]!=SpeakerArr::kStereo)))return kResultFalse;
        return kResultTrue;
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup &s) override {
        if(s.maxSamplesPerBlock<=0 || s.maxSamplesPerBlock>8192 || !std::isfinite(s.sampleRate) || s.sampleRate<8000 || s.sampleRate>192000 || canProcessSampleSize(s.symbolicSampleSize)!=kResultTrue)return kInvalidArgument;
        auto r=SingleComponentEffect::setupProcessing(s);if(r!=kResultOk)return r;
        left.resize(s.maxSamplesPerBlock);right.resize(s.maxSamplesPerBlock);
        input_l.resize(s.maxSamplesPerBlock);input_r.resize(s.maxSamplesPerBlock);
        config.sample_rate=s.sampleRate;return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if(state){if(left.empty()){ProcessSetup s=processSetup;auto r=setupProcessing(s);if(r!=kResultOk)return r;}start();}
        else stop();
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool state) override {
        if(!state && active)for(uint8 ch=0;ch<16;++ch){uint8 b[]{uint8(0xb0|ch),123,0};engine.midi(0,b,3);}
        return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() override {
        uint32 n=uint32(std::lround(1024.0*processSetup.sampleRate/48000.0));
        return std::min<uint32>(16384,std::max(n,uint32(2*processSetup.maxSamplesPerBlock+512)));
    }
    tresult PLUGIN_API process(ProcessData &d) override {
        if(d.numSamples<0 || size_t(d.numSamples)>left.size() || canProcessSampleSize(d.symbolicSampleSize)!=kResultTrue)return kInvalidArgument;
        size_t midi_count=0;
        auto queue=[&](uint32 offset,uint8 const *b,uint8 n){
            if(midi_count>=pending.size())return;
            auto &e=pending[midi_count];e.offset=offset;e.order=uint32(midi_count);e.n=n;
            std::copy_n(b,n,e.bytes);++midi_count;
        };
        if(active) {
            // Merge note events and controller queues before posting to the engine's chronological FIFO.
            if(d.inputEvents)for(int32 i=0;i<d.inputEvents->getEventCount();++i) {
                Event e{};if(d.inputEvents->getEvent(i,e)!=kResultOk || e.busIndex!=0)continue;
                uint32 offset=uint32(std::clamp(e.sampleOffset,0,std::max(0,d.numSamples-1)));uint8 b[3]{};uint8 n=0;
                switch(e.type) {
                case Event::kNoteOnEvent:if(e.noteOn.channel<0 || e.noteOn.channel>15)break;
                    b[0]=0x90|e.noteOn.channel;b[1]=uint8(std::clamp<int>(e.noteOn.pitch,0,127));b[2]=uint8(std::clamp<int>(int(e.noteOn.velocity*127.f+0.5f),1,127));n=3;break;
                case Event::kNoteOffEvent:if(e.noteOff.channel<0 || e.noteOff.channel>15)break;
                    b[0]=0x80|e.noteOff.channel;b[1]=uint8(std::clamp<int>(e.noteOff.pitch,0,127));b[2]=uint8(std::clamp<int>(int(e.noteOff.velocity*127.f+0.5f),0,127));n=3;break;
                case Event::kPolyPressureEvent:if(e.polyPressure.channel<0 || e.polyPressure.channel>15)break;
                    b[0]=0xa0|e.polyPressure.channel;b[1]=uint8(std::clamp<int>(e.polyPressure.pitch,0,127));b[2]=uint8(std::clamp<int>(int(e.polyPressure.pressure*127.f+0.5f),0,127));n=3;break;
                case Event::kDataEvent:
                    if(e.data.type==DataEvent::kMidiSysEx && e.data.bytes && e.data.size<=3*(pending.size()-midi_count))
                        for(uint32 j=0;j<e.data.size;j+=3)queue(offset,e.data.bytes+j,uint8(std::min<uint32>(3,e.data.size-j)));
                    break;
                default:break;
                }
                if(n)queue(offset,b,n);
            }
            if(d.inputParameterChanges)for(int32 i=0;i<d.inputParameterChanges->getParameterCount();++i) {
                auto q=d.inputParameterChanges->getParameterData(i);if(!q)continue;
                auto id=q->getParameterId();if(id<midi_base || id>=midi_base+16*controllers_per_channel)continue;
                auto channel=int((id-midi_base)/controllers_per_channel),cc=int((id-midi_base)%controllers_per_channel);
                for(int32 j=0;j<q->getPointCount();++j){int32 off=0;ParamValue value=0;if(q->getPoint(j,off,value)!=kResultOk || !std::isfinite(value))continue;
                    value=std::clamp(value,0.0,1.0);int v=int(value*(cc==kPitchBend?16383:127)+0.5);uint8 b[3]{};uint8 n=3;
                    if(cc<128){b[0]=0xb0|channel;b[1]=cc;b[2]=v;}
                    else if(cc==kAfterTouch){b[0]=0xd0|channel;b[1]=v;n=2;}
                    else if(cc==kPitchBend){b[0]=0xe0|channel;b[1]=v&127;b[2]=(v>>7)&127;}
                    else {b[0]=0xc0|channel;b[1]=v;n=2;}
                    queue(uint32(std::clamp(off,0,std::max(0,d.numSamples-1))),b,n);
                }
            }
        }
        std::sort(pending.begin(),pending.begin()+midi_count,[](auto const &a,auto const &b){return a.offset<b.offset || (a.offset==b.offset && a.order<b.order);});
        for(size_t i=0;i<midi_count;++i)engine.midi(pending[i].offset,pending[i].bytes,pending[i].n);
        if(!d.numSamples)return kResultOk;
        bool has_input=d.numInputs>0 && d.inputs && d.inputs[0].numChannels>=2 && (d.inputs[0].silenceFlags & 3)!=3;
        if(active){
            engine.set_realtime(processSetup.processMode!=kOffline);
            if(has_input && d.symbolicSampleSize==kSample32 && d.inputs[0].channelBuffers32 && d.inputs[0].channelBuffers32[0] && d.inputs[0].channelBuffers32[1])
                engine.input((d.inputs[0].silenceFlags & 1)?nullptr:d.inputs[0].channelBuffers32[0],(d.inputs[0].silenceFlags & 2)?nullptr:d.inputs[0].channelBuffers32[1],d.numSamples);
            else if(has_input && d.symbolicSampleSize==kSample64 && d.inputs[0].channelBuffers64 && d.inputs[0].channelBuffers64[0] && d.inputs[0].channelBuffers64[1]){
                for(int32 i=0;i<d.numSamples;++i){input_l[i]=(d.inputs[0].silenceFlags & 1)?0.f:float(d.inputs[0].channelBuffers64[0][i]);input_r[i]=(d.inputs[0].silenceFlags & 2)?0.f:float(d.inputs[0].channelBuffers64[1][i]);}engine.input(input_l.data(),input_r.data(),d.numSamples);
            }else engine.input(nullptr,nullptr,d.numSamples);
            engine.pull(left.data(),right.data(),d.numSamples);
        }else{std::fill_n(left.data(),d.numSamples,0.f);std::fill_n(right.data(),d.numSamples,0.f);}
        if(d.numOutputs && d.outputs && d.outputs[0].numChannels>=2){auto &o=d.outputs[0];
            if(d.symbolicSampleSize==kSample32 && o.channelBuffers32){for(int ch=0;ch<2;++ch)if(o.channelBuffers32[ch])std::copy_n(ch?right.data():left.data(),d.numSamples,o.channelBuffers32[ch]);}
            else if(d.symbolicSampleSize==kSample64 && o.channelBuffers64){for(int ch=0;ch<2;++ch)if(o.channelBuffers64[ch])std::copy_n(ch?right.data():left.data(),d.numSamples,o.channelBuffers64[ch]);}
            o.silenceFlags=0;
        }
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream *s) override {
        if(!s)return kInvalidArgument;
        poll_media();
        auto c=config;std::string display=panel.display_state()+"view_width="+std::to_string(editor_width)+"\n";
        if(active) {
            // A host save must include even disk writes from the last 300 ms publication window.
            // This runs on the state/GUI thread; process() continues without waiting or allocating.
            auto snapshot=std::make_shared<std::array<std::string,8>>();
            if(!engine.inspect([snapshot](eiii::runtime &rt){for(unsigned id=1;id<8;++id)(*snapshot)[id]=rt.hd_changes(id);}))return kResultFalse;
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);bool done=false;
            while(std::chrono::steady_clock::now()<deadline && !done) {
                eiii::media_result result;
                while(engine.poll_media_result(result))if(result.slot==eiii::media_slot::inspect){
                    for(unsigned id=1;id<8;++id)c.hd_changes[id]=result.ok?(*snapshot)[id]:engine.hd_changes(id);
                    done=true;
                }
                if(!done)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if(!done)return kResultFalse;
        }
        char magic[8]={'E','I','I','I','V','S','T',1};if(!transfer(s,magic,8,true))return kResultFalse;
        for(auto p:{&c.rom_dir,&c.hd_path,&c.cd_path,&c.cem_model,&display})if(!string_state(s,*p,true,1u<<20))return kResultFalse;
        for(unsigned id=1;id<8;++id)if(!string_state(s,c.hd_changes[id],true,256u<<20))return kResultFalse;
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream *s) override {
        if(!s)return kInvalidArgument;
        char magic[8];char expected[8]={'E','I','I','I','V','S','T',1};
        if(!transfer(s,magic,8,false) || std::memcmp(magic,expected,8))return kResultFalse;
        auto c=config;std::string display;
        for(auto p:{&c.rom_dir,&c.hd_path,&c.cd_path,&c.cem_model,&display})if(!string_state(s,*p,false,1u<<20))return kResultFalse;
        uint64 total=0;for(unsigned id=1;id<8;++id){if(!string_state(s,c.hd_changes[id],false,uint32((256u<<20)-total)))return kResultFalse;total+=c.hd_changes[id].size();if(total>256u<<20)return kResultFalse;}
        if(c.cem_model!="v1" && c.cem_model!="v2")return kResultFalse;
        if(c.rom_dir.empty())c.rom_dir=defaults().rom_dir;
        config=std::move(c);config.rom_only=config.hd_path.empty();
        for(size_t pos=0;pos<display.size();){auto end=display.find('\n',pos);panel.apply_display_state_line(display.substr(pos,end-pos).c_str());if(end==std::string::npos)break;pos=end+1;}
        for(size_t pos=display.find("view_width=");pos!=std::string::npos;pos=std::string::npos){int w=editor_width;auto r=std::from_chars(display.data()+pos+11,display.data()+display.size(),w);if(r.ec==std::errc{})editor_width=view_width(w);}
        if(active){poll_media();engine.set_media(eiii::media_slot::rom,config.rom_dir);engine.set_media(eiii::media_slot::hd,config.hd_path);engine.set_media(eiii::media_slot::cd,config.cd_path);engine.set_media(eiii::media_slot::cem_model,config.cem_model);engine.set_hd_changes(1,config.hd_changes[1]);}
        return kResultOk;
    }
    IPlugView *PLUGIN_API createView(FIDString name) override {
#ifdef _WIN32
        if(name && !std::strcmp(name,ViewType::kEditor))return new RackView(*this);
#endif
        return nullptr;
    }
    void media(eiii::media_slot slot,std::string const &path) {
        poll_media();
        std::string error;bool ok=true;
        if(slot==eiii::media_slot::rom)ok=eiii::runtime::check_rom_dir(path,error);
        if(slot==eiii::media_slot::hd)ok=eiii::runtime::check_hd_image(path,error);
        if(slot==eiii::media_slot::cd && !path.empty())ok=eiii::runtime::check_cd_image(path,error);
        if(!ok){
#ifdef _WIN32
            MessageBoxA(nullptr,error.c_str(),"EIII Rack",MB_OK|MB_ICONERROR);
#endif
            return;
        }
        if(slot==eiii::media_slot::rom){config.rom_dir=path;remember_preference("rom_dir",path);}
        else if(slot==eiii::media_slot::hd){config.hd_path=path;config.hd_changes[1].clear();config.rom_only=false;}
        else if(slot==eiii::media_slot::cd)config.cd_path=path;
        else if(slot==eiii::media_slot::cem_model)config.cem_model=path;
        if(active)engine.set_media(slot,path);
    }
    std::string poll_media() {
        eiii::media_result result;std::string error;
        while(engine.poll_media_result(result))if(!result.ok)error=result.error;
        return error;
    }

    void save_defaults() {
        auto file=config_file();if(file.empty())return;
        if(!eiii::create_native_directory(eiii::path_parent(file)))return;
        std::ofstream out(file);out<<"rom_dir="<<config.rom_dir<<"\nhd="<<config.hd_path<<"\ncd="<<config.cd_path<<"\ncem_model="<<config.cem_model<<"\nview_width="<<editor_width<<"\n";
    }
    eiii::runtime_config config;
    eiii::engine engine;
    eiii::panel_adapter panel;
    int editor_width=1472;
private:
    struct queued_midi {uint32 offset,order;uint8 n,bytes[3];};
    std::array<queued_midi,4096> pending{};
    bool active=false;
    std::vector<float> left,right,input_l,input_r;
    void start(){if(active)return;eiii::engine_config c;c.rt=config;c.max_block=processSetup.maxSamplesPerBlock;c.realtime=processSetup.processMode!=kOffline;engine.start(c);active=true;}
    void stop(){if(!active)return;engine.shutdown();for(unsigned id=1;id<8;++id)config.hd_changes[id]=engine.hd_changes(id);active=false;}
};
#ifdef _WIN32
RackView::RackView(RackPlugin &p):plugin(p),win(p.panel){plugin.addRef();rect=view_rect(plugin.editor_width);win.keyboard_shortcuts=false;
    win.on_resize_request=[this](int w,int){request_width(w);};
    win.on_resize_finish=[this]{remember_preference("view_width",std::to_string(plugin.editor_width));};
    win.on_tick=[this]{auto error=plugin.poll_media();if(!error.empty())win.view().set_menu_status(error);};
    menus();}
RackView::~RackView(){win.close();remember_preference("view_width",std::to_string(plugin.editor_width));plugin.release();}
tresult RackView::attached(void *parent,FIDString type){if(!parent || isPlatformTypeSupported(type)!=kResultTrue)return kResultFalse;std::string error;if(!win.open(reinterpret_cast<uintptr_t>(parent),rect.getWidth(),rect.getHeight(),error))return kResultFalse;CPluginView::attached(parent,type);win.tick();return kResultOk;}
tresult RackView::removed(){win.close();return CPluginView::removed();}
tresult RackView::onSize(ViewRect *r){if(!r)return kInvalidArgument;CPluginView::onSize(r);plugin.editor_width=view_width(r->getWidth());if(win.is_open())win.set_size(r->getWidth(),r->getHeight());return kResultOk;}
tresult RackView::checkSizeConstraint(ViewRect *r){if(!r)return kInvalidArgument;*r=view_rect(r->getWidth(),r->left,r->top);return kResultOk;}
void RackView::request_width(int width){
    auto requested=view_rect(width,rect.left,rect.top);
    if(!plugFrame || plugFrame->resizeView(this,&requested)!=kResultOk){win.view().set_menu_status("Host rejected editor resize");return;}
}

std::string RackView::pick(char const *filter,char const *title,std::string const &current){
    char path[32768]{};if(current.size()<sizeof path)std::memcpy(path,current.c_str(),current.size());
    OPENFILENAMEA of{};of.lStructSize=sizeof of;of.hwndOwner=reinterpret_cast<HWND>(win.native_handle());of.lpstrFile=path;of.nMaxFile=sizeof path;of.lpstrFilter=filter;of.lpstrTitle=title;of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&of)?path:"";
}
void RackView::menus(){
    using namespace eiii::ui;using eiii::media_slot;
    std::vector<menu> m;
    auto choose=[this](media_slot slot){auto path=pick("All files\0*.*\0\0",slot==media_slot::rom?"Choose either boot ROM file":slot==media_slot::hd?"Choose OS hard disk image":"Choose EIII CD image","");if(!path.empty())plugin.media(slot,slot==media_slot::rom?eiii::path_parent(path):path);};
    m.push_back({"settings",[]{return "Settings";},[this,choose]{std::vector<menu_item> v;
        v.push_back({"Boot ROM folder...",false,true,false,false,[choose]{choose(media_slot::rom);}});
        v.push_back({"Save paths as defaults",false,true,false,false,[this]{plugin.save_defaults();}});
        v.push_back({"Restart (cold boot)",false,true,false,false,[this]{plugin.engine.set_media(media_slot::restart,"");}});
        v.push_back(menu_item::sep());
        for(int percent:{50,75,100,125,150,200})v.push_back({"GUI "+std::to_string(percent)+"%",plugin.editor_width==1472*percent/100,true,false,false,[this,percent]{request_width(1472*percent/100);remember_preference("view_width",std::to_string(plugin.editor_width));}});
        v.push_back(menu_item::sep());
        for(auto model:{"v1","v2"})v.push_back({std::string("Analog model ")+model,plugin.config.cem_model==model,true,false,false,[this,model]{plugin.media(media_slot::cem_model,model);}});
        return v;}});
    m.push_back({"d1",[this]{return std::string("D1 HD ")+(plugin.config.hd_path.empty()?"(choose OS image)":eiii::native_display(eiii::path_leaf(plugin.config.hd_path)));},[this,choose]{return std::vector<menu_item>{menu_item::info(eiii::native_display(plugin.config.hd_path)),{"Choose OS hard disk image...",false,true,false,false,[choose]{choose(media_slot::hd);}}};}});
    m.push_back({"d3",[this]{return std::string("D3 CD ")+(plugin.config.cd_path.empty()?"(empty)":eiii::native_display(eiii::path_leaf(plugin.config.cd_path)));},[this,choose]{return std::vector<menu_item>{menu_item::info(eiii::native_display(plugin.config.cd_path)),{"Insert CD image...",false,true,false,false,[choose]{choose(media_slot::cd);}},{"Eject",false,true,false,false,[this]{plugin.media(media_slot::cd,"");}}};}});
    win.view().set_menus(std::move(m));win.view().on_drive_click=[this](bool eject,float x,float y){if(eject)plugin.media(media_slot::cd,"");else win.view().open_menu_at(win.view().find_menu("d3"),x,y);};
}
#endif
}

bool InitModule(){return true;}
bool DeinitModule(){return true;}
BEGIN_FACTORY("eiii-core (unofficial)","https://github.com/parguinsys/Emulated-3","",PFactoryInfo::kNoFlags)
DEF_CLASS2(INLINE_UID(0xEF98301A,0xDBF84D09,0xB6BAE344,0x04F37C11),PClassInfo::kManyInstances,kVstAudioEffectClass,"EIII Rack",0,"Instrument|Sampler","0.2.2-preview",kVstVersionString,RackPlugin::create)
END_FACTORY
