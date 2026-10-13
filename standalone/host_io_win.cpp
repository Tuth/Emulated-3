// Initial Windows standalone backend: WinMM MIDI input and buffered waveOut playback.
#include "host_io.h"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <memory>

namespace {
bool device_id(std::string const &s, UINT &id, bool audio=false) {
    if(audio && s=="default") {id=WAVE_MAPPER;return true;}
    char *end=nullptr; unsigned long n=strtoul(s.c_str(),&end,10);
    if(s.empty() || !end || *end || n>= (audio?waveOutGetNumDevs():midiInGetNumDevs())) return false;
    id=UINT(n);return true;
}
struct midi_state {
    HMIDIIN handle=nullptr;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::vector<uint8_t> bytes;
    std::array<std::array<char,4096>,4> data{};
    std::array<MIDIHDR,4> headers{};
    ~midi_state() {
        if(!handle)return;
        stopping=true;midiInStop(handle);midiInReset(handle);
        for(auto &h:headers) if(h.dwFlags & MHDR_PREPARED) midiInUnprepareHeader(handle,&h,sizeof h);
        midiInClose(handle);
    }
    static void CALLBACK callback(HMIDIIN h,UINT msg,DWORD_PTR user,DWORD_PTR value,DWORD_PTR) {
        auto &s=*reinterpret_cast<midi_state *>(user);
        if(msg==MIM_DATA) {
            uint8_t b=value & 255; unsigned n=1;
            if(b<0xf0) n=((b&0xf0)==0xc0 || (b&0xf0)==0xd0)?2:3;
            else if(b==0xf1 || b==0xf3) n=2; else if(b==0xf2) n=3;
            std::lock_guard<std::mutex> lock(s.mutex);
            if(s.bytes.size()+n<=65536) for(unsigned i=0;i<n;++i) s.bytes.push_back(uint8_t(value>>(8*i)));
        } else if(msg==MIM_LONGDATA) {
            auto *hdr=reinterpret_cast<MIDIHDR *>(value);
            {std::lock_guard<std::mutex> lock(s.mutex); if(s.bytes.size()+hdr->dwBytesRecorded<=65536) s.bytes.insert(s.bytes.end(),hdr->lpData,hdr->lpData+hdr->dwBytesRecorded);}
            if(!s.stopping) midiInAddBuffer(h,hdr,sizeof(*hdr));
        }
    }
};
struct audio_state {
    HWAVEOUT handle=nullptr;
    HANDLE event=nullptr;
    bool timer_resolution=false,started=false;
    size_t primed=0;
    std::vector<WAVEHDR> headers;
    std::vector<std::vector<int16_t>> data;
    size_t next=0;
    static bool queued(WAVEHDR const &h) {
        return (h.dwFlags & WHDR_INQUEUE) && !(h.dwFlags & WHDR_DONE);
    }
    ~audio_state() {
        if(handle) {
            waveOutReset(handle);
            for(auto &h:headers) if(h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(handle,&h,sizeof h);
            waveOutClose(handle);
        }
        if(event)CloseHandle(event);
        if(timer_resolution)timeEndPeriod(1);
    }
};
}
int list_midi_inputs(){printf("Windows MIDI inputs (use --midi-in NUMBER):\n");for(UINT i=0;i<midiInGetNumDevs();++i){MIDIINCAPSA c{};if(!midiInGetDevCapsA(i,&c,sizeof c))printf("  %u  %s\n",i,c.szPname);}return 0;}
int list_audio_outputs(){printf("Windows waveOut outputs (use --audio-out default or NUMBER):\n  default  System default\n");for(UINT i=0;i<waveOutGetNumDevs();++i){WAVEOUTCAPSA c{};if(!waveOutGetDevCapsA(i,&c,sizeof c))printf("  %u  %s\n",i,c.szPname);}return 0;}
midi_input::~midi_input(){delete static_cast<midi_state *>(m_h);}
bool midi_input::open(std::string const &name){
    if(m_h)return false;
    UINT id;if(!device_id(name,id))return false;
    auto s=std::make_unique<midi_state>();
    if(midiInOpen(&s->handle,id,reinterpret_cast<DWORD_PTR>(midi_state::callback),reinterpret_cast<DWORD_PTR>(s.get()),CALLBACK_FUNCTION)){return false;}
    for(size_t i=0;i<s->headers.size();++i){auto &h=s->headers[i];h.lpData=s->data[i].data();h.dwBufferLength=s->data[i].size();if(midiInPrepareHeader(s->handle,&h,sizeof h) || midiInAddBuffer(s->handle,&h,sizeof h))return false;}
    if(midiInStart(s->handle)!=MMSYSERR_NOERROR)return false;
    m_h=s.release();return true;
}
size_t midi_input::read(std::vector<uint8_t> &out){auto *s=static_cast<midi_state *>(m_h);if(!s)return 0;std::lock_guard<std::mutex> lock(s->mutex);size_t n=s->bytes.size();out.insert(out.end(),s->bytes.begin(),s->bytes.end());s->bytes.clear();return n;}
audio_output::~audio_output(){delete static_cast<audio_state *>(m_h);}
bool audio_output::open(std::string const &name,unsigned rate,unsigned period_frames,unsigned periods){
    if(m_h)return false;
    UINT id;if(!device_id(name,id,true))return false;
    unsigned period=period_frames?period_frames:256,count=periods?periods:4;
    if(period>65536 || count>64 || count<2)return false;
    WAVEFORMATEX f{};f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=rate;f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=rate*4;
    auto s=std::make_unique<audio_state>();
    s->event=CreateEventA(nullptr,TRUE,FALSE,nullptr);
    if(!s->event)return false;
    s->timer_resolution=timeBeginPeriod(1)==TIMERR_NOERROR;
    if(waveOutOpen(&s->handle,id,&f,reinterpret_cast<DWORD_PTR>(s->event),0,CALLBACK_EVENT))return false;
    // Start with a full hardware queue, rather than playing the first block
    // immediately while the remaining blocks are still being produced.
    if(waveOutPause(s->handle))return false;
    s->headers.resize(count);s->data.resize(count);m_period=period;m_buffer=period*count;
    for(size_t i=0;i<count;++i){s->data[i].resize(period*2);auto &h=s->headers[i];h.lpData=reinterpret_cast<char *>(s->data[i].data());h.dwBufferLength=period*4;if(waveOutPrepareHeader(s->handle,&h,sizeof h))return false;}
    ResetEvent(s->event);
    m_h=s.release();return true;
}
bool audio_output::wait_writable(unsigned timeout_ms) {
    auto *s=static_cast<audio_state *>(m_h);if(!s)return false;
    auto &h=s->headers[s->next];
    ULONGLONG const deadline=GetTickCount64()+timeout_ms;
    while(audio_state::queued(h)) {
        // Reset then recheck: a completion between these two operations must
        // not be lost, nor may an old signalled event cause a busy spin.
        ResetEvent(s->event);
        if(!audio_state::queued(h))break;
        auto now=GetTickCount64();
        if(now>=deadline || WaitForSingleObject(s->event,DWORD(deadline-now))!=WAIT_OBJECT_0) {
            ++m_xruns;if(on_xrun)on_xrun(m_xruns);return false;
        }
    }
    if(s->started && std::none_of(s->headers.begin(),s->headers.end(),audio_state::queued)) {
        // The device consumed its whole queue: count the real starvation and
        // refill before restarting, instead of repeatedly starting tiny blocks.
        ++m_xruns;if(on_xrun)on_xrun(m_xruns);
        if(waveOutPause(s->handle))return false;
        s->started=false;s->primed=0;
    }
    return true;
}
void audio_output::write(float const *p,size_t frames){
    auto *s=static_cast<audio_state *>(m_h);if(!s)return;
    while(frames){
        if(!wait_writable())return;
        size_t n=std::min<size_t>(frames,m_period);auto &h=s->headers[s->next];
        auto &d=s->data[s->next];for(size_t i=0;i<n*2;++i){float v=std::isfinite(p[i])?p[i]:0;d[i]=int16_t(std::clamp(v,-1.f,1.f)*32767.f);}
        h.dwBufferLength=DWORD(n*4);if(waveOutWrite(s->handle,&h,sizeof h)){++m_xruns;if(on_xrun)on_xrun(m_xruns);return;}
        s->next=(s->next+1)%s->headers.size();frames-=n;p+=n*2;
        if(!s->started && ++s->primed==s->headers.size()) {
            if(waveOutRestart(s->handle)){++m_xruns;if(on_xrun)on_xrun(m_xruns);return;}
            s->started=true;
        }
    }
}
