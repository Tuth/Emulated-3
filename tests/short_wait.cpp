#define _WIN32
#include "../runtime/eiii_short_wait.h"
#include <cstdio>
struct mock_timer {int64_t due=0;};
static uint64_t now_ns=0;
static unsigned handles=0,waits=0,sleeps=0,high_attempts=0,normal_attempts=0,failures=0;
static bool fail_high=false,fail_all=false,fail_set=false,fail_wait=false;
HANDLE CreateWaitableTimerExW(void*,wchar_t const*,DWORD flags,DWORD access){
	if(flags==2)++high_attempts;else if(flags==0)++normal_attempts;else ++failures;
	if(access!=TIMER_ALL_ACCESS)++failures;
	if(fail_all||(flags==2&&fail_high))return nullptr;
	++handles;return new mock_timer;
}
BOOL CloseHandle(HANDLE h){--handles;delete h;return 1;}
BOOL SetWaitableTimer(HANDLE h,LARGE_INTEGER const*d,LONG period,void*c,void*a,BOOL wake){
	if(d->QuadPart>=0||period||c||a||wake)++failures;
	h->due=d->QuadPart;return !fail_set;
}
DWORD WaitForSingleObject(HANDLE h,DWORD timeout){
	++waits;if(timeout!=INFINITE)++failures;
	if(fail_wait)return WAIT_FAILED;
	now_ns+=uint64_t(-h->due)*100;return WAIT_OBJECT_0;
}
void Sleep(DWORD ms){++sleeps;if(!ms)++failures;now_ns+=uint64_t(ms)*1000000;}
static void check(bool ok,char const*name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;}
int main(){
	{
		eiii::short_wait w;w.wait_us(500);check(now_ns==500000,"500 us arms a genuine sub-ms blocking wait");
		w.wait_us(100);check(now_ns==600000,"100 us is preserved without truncation");
		for(int i=0;i<1000;++i)w.wait_us(500);
		check(now_ns==500600000&&handles==1&&sleeps==0&&waits==1002,"repeated waits reuse one timer without Sleep or polling");
		auto n=waits;w.wait_us(0);check(n==waits,"zero wait does not enter the kernel");
	}check(handles==0,"normal timer closed");
	fail_high=true;{eiii::short_wait w;auto n=now_ns;w.wait_us(500);check(now_ns==n+500000&&normal_attempts==1,"older-OS fallback uses a normal waitable timer");}
	fail_all=true;{eiii::short_wait w;auto n=now_ns;w.wait_us(100);check(now_ns==n+1000000,"creation failure rounds Sleep upward to one ms");}fail_all=false;fail_high=false;
	{eiii::short_wait w;fail_set=true;auto n=now_ns;w.wait_us(500);check(now_ns==n+1000000,"arming failure does not spin");fail_set=false;
		fail_wait=true;n=now_ns;w.wait_us(500);check(now_ns==n+1000000,"wait failure does not spin");fail_wait=false;}
	check(handles==0,"fallback/failure paths release every handle");
	std::printf("%s short Windows wait: %u failures\n",failures?"FAIL":"OK",failures);return failures?1:0;
}
