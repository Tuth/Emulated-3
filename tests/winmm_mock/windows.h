// Deterministic WinMM driver model for the real Windows backend regression.
#pragma once
#include <cstdint>
#define CALLBACK
using UINT=unsigned;using DWORD=uint32_t;using DWORD_PTR=uintptr_t;
using ULONGLONG=uint64_t;using WORD=uint16_t;
struct mock_event;using HANDLE=mock_event*;
constexpr int TRUE=1,FALSE=0;
constexpr DWORD WAIT_OBJECT_0=0,WAIT_TIMEOUT=258;
HANDLE CreateEventA(void*,int,int,char const*);
int CloseHandle(HANDLE);
int ResetEvent(HANDLE);
DWORD WaitForSingleObject(HANDLE,DWORD);
ULONGLONG GetTickCount64();
void Sleep(DWORD);
