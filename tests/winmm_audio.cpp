// Compile and exercise the actual Windows backend against a deterministic
// device clock. No Windows device, private ROM or timing-sensitive sleep test.
#include "mmsystem.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <vector>
#include <cstdio>
#include "../standalone/host_io_win.cpp"

struct mock_event {bool signaled=false;mock_wave *wave=nullptr;};
struct mock_wave {
	mock_event *event=nullptr;bool paused=false;unsigned rate=0;
	std::deque<WAVEHDR*> queue;uint64_t completion=0;
};
static uint64_t clock_ns=0;
static unsigned waits=0,sleeps=0,restarts=0,writes=0,timer_refs=0,event_refs=0,wave_refs=0,failures=0;
static unsigned smallest_start_queue=1000;
static bool stalled=false,race_completion=false,fail_open=false;
static mock_wave *active=nullptr;
static std::vector<int16_t> last_written;
static uint64_t duration(mock_wave *w,WAVEHDR *h) {return uint64_t(h->dwBufferLength/4)*1000000000/w->rate;}
static void advance(uint64_t to) {
	if(active && !active->paused && !stalled) {
		while(!active->queue.empty() && active->completion<=to) {
			auto h=active->queue.front();active->queue.pop_front();h->dwFlags=(h->dwFlags&~WHDR_INQUEUE)|WHDR_DONE;active->event->signaled=true;
			if(!active->queue.empty())active->completion+=duration(active,active->queue.front());
		}
	}
	clock_ns=to;
}
HANDLE CreateEventA(void*,int manual,int,char const*) {if(!manual)++failures;++event_refs;return new mock_event;}
int CloseHandle(HANDLE e) {--event_refs;delete e;return 1;}
int ResetEvent(HANDLE e) {
	e->signaled=false;
	if(race_completion){race_completion=false;advance(e->wave->completion);}
	return 1;
}
DWORD WaitForSingleObject(HANDLE e,DWORD ms) {
	++waits;if(e->signaled)return WAIT_OBJECT_0;
	auto end=clock_ns+uint64_t(ms)*1000000;
	if(!stalled && e->wave && !e->wave->paused && !e->wave->queue.empty() && e->wave->completion<=end)advance(e->wave->completion);
	else advance(end);
	return e->signaled?WAIT_OBJECT_0:WAIT_TIMEOUT;
}
ULONGLONG GetTickCount64() {return clock_ns/1000000;}
void Sleep(DWORD) {++sleeps;advance(clock_ns+16000000);}
UINT waveOutGetNumDevs(){return 1;}UINT midiInGetNumDevs(){return 0;}
UINT waveOutGetDevCapsA(UINT,WAVEOUTCAPSA*,UINT){return 0;}UINT midiInGetDevCapsA(UINT,MIDIINCAPSA*,UINT){return 0;}
UINT waveOutOpen(HWAVEOUT *out,UINT,WAVEFORMATEX const *f,DWORD_PTR event,DWORD_PTR,DWORD flags) {
	if(fail_open)return 1;
	if(flags!=CALLBACK_EVENT || f->nSamplesPerSec!=48000 || f->nChannels!=2 || f->wBitsPerSample!=16)++failures;
	active=new mock_wave;active->event=reinterpret_cast<mock_event*>(event);active->event->wave=active;active->event->signaled=true;active->rate=f->nSamplesPerSec;*out=active;++wave_refs;return 0;
}
UINT waveOutPause(HWAVEOUT w){w->paused=true;return 0;}
UINT waveOutRestart(HWAVEOUT w){++restarts;smallest_start_queue=std::min<unsigned>(smallest_start_queue,w->queue.size());w->paused=false;w->completion=clock_ns+duration(w,w->queue.front());return 0;}
UINT waveOutPrepareHeader(HWAVEOUT,WAVEHDR *h,UINT){h->dwFlags=WHDR_PREPARED;return 0;}
UINT waveOutUnprepareHeader(HWAVEOUT,WAVEHDR *h,UINT){if(h->dwFlags&WHDR_INQUEUE)++failures;h->dwFlags=0;return 0;}
UINT waveOutWrite(HWAVEOUT w,WAVEHDR *h,UINT){
	if((h->dwFlags&WHDR_INQUEUE) || !(h->dwFlags&WHDR_PREPARED))++failures;
	++writes;h->dwFlags=(h->dwFlags&~WHDR_DONE)|WHDR_INQUEUE;bool empty=w->queue.empty();w->queue.push_back(h);
	if(empty && !w->paused)w->completion=clock_ns+duration(w,h);
	auto p=reinterpret_cast<int16_t*>(h->lpData);last_written.assign(p,p+h->dwBufferLength/2);return 0;
}
UINT waveOutReset(HWAVEOUT w){for(auto h:w->queue)h->dwFlags=(h->dwFlags&~WHDR_INQUEUE)|WHDR_DONE;w->queue.clear();return 0;}
UINT waveOutClose(HWAVEOUT w){if(active==w)active=nullptr;--wave_refs;delete w;return 0;}
UINT timeBeginPeriod(UINT n){if(n!=1)++failures;++timer_refs;return 0;}UINT timeEndPeriod(UINT n){if(n!=1 || !timer_refs)++failures;--timer_refs;return 0;}
UINT midiInOpen(HMIDIIN*,UINT,DWORD_PTR,DWORD_PTR,DWORD){return 1;}
UINT midiInStart(HMIDIIN){return 0;}UINT midiInStop(HMIDIIN){return 0;}UINT midiInReset(HMIDIIN){return 0;}UINT midiInClose(HMIDIIN){return 0;}
UINT midiInPrepareHeader(HMIDIIN,MIDIHDR*,UINT){return 0;}UINT midiInUnprepareHeader(HMIDIIN,MIDIHDR*,UINT){return 0;}UINT midiInAddBuffer(HMIDIIN,MIDIHDR*,UINT){return 0;}
static void check(bool ok,char const *s){printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)++failures;}
int main() {
	std::vector<float> block(512,.5f);unsigned reports=0;
	{
		audio_output out;out.on_xrun=[&](unsigned){++reports;};check(out.open("default",48000,256,4),"open event-driven WinMM output");
		for(int i=0;i<3;++i){check(out.wait_writable(),"startup slot available before engine pull");out.write(block.data(),256);}
		check(restarts==0 && active->paused && active->queue.size()==3,"hardware stays paused until queue is full");
		out.write(block.data(),256);check(restarts==1 && active->queue.size()==4,"playback starts with all four buffers queued");
		for(int i=0;i<200;++i){if(!out.wait_writable()){++failures;break;}advance(clock_ns+1000000);out.write(block.data(),256);}
		check(waits>=200 && sleeps==0 && out.xruns()==0,"device-clock pacing avoids coarse Sleep polling and remains continuous");
		check(clock_ns>=1000000000 && clock_ns<1200000000,"200 blocks follow the actual 48 kHz device clock");
		unsigned before=waits;race_completion=true;check(out.wait_writable() && waits==before,"completion during ResetEvent is caught by header recheck");out.write(block.data(),256);
		stalled=true;before=writes;auto start=clock_ns;
		check(!out.wait_writable(250) && clock_ns-start<=251000000 && writes==before,"stalled driver wait is bounded and does not consume/write another block");
		stalled=false;advance(clock_ns+100000000);check(out.wait_writable() && active->paused,"empty hardware queue is detected and paused for recovery");
		for(int i=0;i<4;++i)out.write(block.data(),256);
		check(restarts==2 && smallest_start_queue==4 && reports==2,"one starvation report and full-queue restart after the timeout");
		check(out.wait_writable(),"conversion buffer available");block[0]=std::numeric_limits<float>::quiet_NaN();block[1]=2;block[2]=-2;out.write(block.data(),256);
		check(last_written[0]==0 && last_written[1]==32767 && last_written[2]==-32767,"stereo PCM conversion clips safely and preserves sample order");
	}
	check(timer_refs==0 && event_refs==0 && wave_refs==0,"device close releases event and matches timeBeginPeriod/timeEndPeriod");
	for(unsigned count:{2u,8u}) {
		audio_output out;check(out.open("default",48000,256,count),"open selectable buffer count");
		for(unsigned i=0;i<count;++i)out.write(block.data(),256);
		for(int i=0;i<100;++i){if(!out.wait_writable()){++failures;break;}advance(clock_ns+1000000);out.write(block.data(),256);}
		check(out.xruns()==0 && active->queue.size()==count,"low-latency and larger queues retain device-clock pacing");
	}
	check(timer_refs==0 && event_refs==0 && wave_refs==0,"buffer-choice sessions release all resources");
	fail_open=true;{audio_output out;check(!out.open("default",48000,256,4),"device-open failure refused");}
	check(timer_refs==0 && event_refs==0 && wave_refs==0,"failed open releases timer-resolution and event resources");
	printf("%s WinMM backend: %u failures\n",failures?"FAIL":"OK",failures);return failures?1:0;
}
