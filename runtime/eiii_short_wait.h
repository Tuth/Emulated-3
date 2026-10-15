#pragma once
#include <chrono>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eiii {
// MinGW's nanosleep in the original build truncates sub-ms durations to zero.
// Use a reusable kernel timer on Windows, never a deadline-polling loop. This
// is host pacing only: no emulated clock, audio queue or voice state changes.
class short_wait
{
public:
#ifdef _WIN32
	short_wait() {
		m_timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
		if(!m_timer)m_timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
	}
	~short_wait(){if(m_timer)CloseHandle(m_timer);}
#endif
	short_wait(short_wait const&)=delete;
	short_wait& operator=(short_wait const&)=delete;
#ifndef _WIN32
	short_wait()=default;
#endif
	void wait_us(unsigned microseconds) {
		if(!microseconds)return;
#ifdef _WIN32
		LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(uint64_t(microseconds)*10);
		if(m_timer && SetWaitableTimer(m_timer,&due,0,nullptr,nullptr,FALSE)
			&& WaitForSingleObject(m_timer,INFINITE)==WAIT_OBJECT_0)return;
		// A failure/older OS must not reintroduce a zero-duration busy loop.
		Sleep(static_cast<DWORD>((uint64_t(microseconds)+999)/1000));
#else
		std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
	}
private:
#ifdef _WIN32
	HANDLE m_timer=nullptr;
#endif
};

inline void short_pause_us(unsigned microseconds) {
	if(!microseconds)return;
	thread_local short_wait waiter;
	waiter.wait_us(microseconds);
}
}
