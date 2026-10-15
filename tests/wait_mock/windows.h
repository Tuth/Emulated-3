#pragma once
#include <cstdint>
using DWORD=uint32_t;using LONG=int32_t;using LONGLONG=int64_t;using BOOL=int;
struct mock_timer;using HANDLE=mock_timer*;
struct LARGE_INTEGER {LONGLONG QuadPart;};
constexpr BOOL FALSE=0;
constexpr DWORD TIMER_ALL_ACCESS=0x1f0003,INFINITE=0xffffffff,WAIT_OBJECT_0=0,WAIT_FAILED=0xffffffff;
HANDLE CreateWaitableTimerExW(void*,wchar_t const*,DWORD,DWORD);
BOOL CloseHandle(HANDLE);
BOOL SetWaitableTimer(HANDLE,LARGE_INTEGER const*,LONG,void*,void*,BOOL);
DWORD WaitForSingleObject(HANDLE,DWORD);
void Sleep(DWORD);
