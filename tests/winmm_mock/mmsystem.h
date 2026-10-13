#pragma once
#include "windows.h"
using HMIDIIN=void*;
struct mock_wave;using HWAVEOUT=mock_wave*;
struct WAVEHDR {char *lpData=nullptr;DWORD dwBufferLength=0,dwFlags=0;};
struct MIDIHDR {char *lpData=nullptr;DWORD dwBufferLength=0,dwBytesRecorded=0,dwFlags=0;};
struct WAVEFORMATEX {WORD wFormatTag=0,nChannels=0;DWORD nSamplesPerSec=0,nAvgBytesPerSec=0;WORD nBlockAlign=0,wBitsPerSample=0;};
struct WAVEOUTCAPSA {char szPname[32]{};};
struct MIDIINCAPSA {char szPname[32]{};};
constexpr UINT WAVE_MAPPER=UINT(-1),WAVE_FORMAT_PCM=1;
constexpr DWORD WHDR_DONE=1,WHDR_PREPARED=2,WHDR_INQUEUE=16,MHDR_PREPARED=2;
constexpr DWORD CALLBACK_EVENT=0x50000,CALLBACK_FUNCTION=0x30000;
constexpr UINT MIM_DATA=0x3c3,MIM_LONGDATA=0x3c4;
constexpr UINT MMSYSERR_NOERROR=0,TIMERR_NOERROR=0;
UINT waveOutGetNumDevs();UINT midiInGetNumDevs();
UINT waveOutGetDevCapsA(UINT,WAVEOUTCAPSA*,UINT);UINT midiInGetDevCapsA(UINT,MIDIINCAPSA*,UINT);
UINT waveOutOpen(HWAVEOUT*,UINT,WAVEFORMATEX const*,DWORD_PTR,DWORD_PTR,DWORD);
UINT waveOutPause(HWAVEOUT);UINT waveOutRestart(HWAVEOUT);
UINT waveOutPrepareHeader(HWAVEOUT,WAVEHDR*,UINT);UINT waveOutUnprepareHeader(HWAVEOUT,WAVEHDR*,UINT);
UINT waveOutWrite(HWAVEOUT,WAVEHDR*,UINT);UINT waveOutReset(HWAVEOUT);UINT waveOutClose(HWAVEOUT);
UINT timeBeginPeriod(UINT);UINT timeEndPeriod(UINT);
UINT midiInOpen(HMIDIIN*,UINT,DWORD_PTR,DWORD_PTR,DWORD);
UINT midiInStart(HMIDIIN);UINT midiInStop(HMIDIIN);UINT midiInReset(HMIDIIN);UINT midiInClose(HMIDIIN);
UINT midiInPrepareHeader(HMIDIIN,MIDIHDR*,UINT);UINT midiInUnprepareHeader(HMIDIIN,MIDIHDR*,UINT);UINT midiInAddBuffer(HMIDIIN,MIDIHDR*,UINT);
