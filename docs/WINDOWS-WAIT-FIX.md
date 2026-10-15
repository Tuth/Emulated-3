# Windows sub-millisecond host-wait fix, 2026-10-15

## User-validated READY result

The matching Windows follow-up capture measured 3718.75 ms of process CPU
over 30.0043 seconds, versus 30687.5 ms over 30.0018 seconds before the fix.
One-logical-CPU-equivalent load fell from 102.285% to 12.3941%, an 87.9%
reduction in measured READY process CPU consumption. This is not an 8x
instrument-rendering speedup: it removes wasted host polling.

The emulation worker fell from 99.7856% to 10.6756% of one logical CPU.
The audio-output and GUI threads used about 0.73% and 0.68%, respectively.
No extra emulation worker was introduced. Windows can schedule one thread
on different logical processors over time; activity on two CPU graphs does
not establish parallel emulation.

The new report has valid CPU counters, a matching target fingerprint, 5604
snapshots per thread, and no suspend/context/resume/timer errors. CSV snapshot
counts match the text report. The user also observed the lower CPU load
immediately without profiling. Sustained-bank audio and low-buffer behavior
still require separate listening/driver tests; this capture validates READY
CPU use only. The optional Windows preset menu remains lower priority.

## Evidence from the original 0.2.6 READY capture

The user's 30.0018-second capture measured 30687.5 ms of target process CPU
time. The emulation thread alone used 29937.5 ms, or 99.7856% of one logical
processor. The sampler used 281.25 ms (about 0.94% of one logical processor).
There were 5602 snapshots per thread, with valid counters and no suspend,
context, timer or resume errors.

On the emulation thread, prominent instruction-pointer labels were
clock_gettime (939 snapshots), engine::drain_input (428), engine::thread_main
(323), steady_clock::now (292), and nanosleep (208). The CEM voice function
had 144 snapshots and NS32000 execute_run 34. DLL samples were largely in
ntdll.dll. These are wall-clock snapshots, not exact CPU-time attribution;
DLL RVAs have not been assigned unverified function names.

The exact released executable provides stronger, independent evidence: its
`nanosleep` at RVA 0x87e60 computes whole milliseconds by truncating the
timespec nanoseconds, then returns immediately if the result is zero. The
engine's buffer-full branch requests 500000 ns, so it makes no blocking wait.
Its offline consumer's 100000 ns wait has the same issue. Both requests are
below one millisecond. The 500 us branch can consequently poll a full audio
queue continuously, repeatedly draining input and reading the clock.

## Change

Only the two sub-ms pacing calls in `runtime/eiii_engine.cpp` are replaced by
`short_wait` / `short_pause_us`. The worker constructs one reusable waitable
timer before emulation starts; offline pulls use a thread-local one. Each wait
requests a negative relative due time in 100 ns units and blocks on the timer.
A high-resolution timer is preferred; an ordinary timer is the compatibility
fallback. API failure falls back to native Sleep rounded upward to at least
one millisecond, never Sleep(0). Handles are closed on normal thread exit.
Non-Windows behavior retains std::this_thread::sleep_for.

The worker waits only after the existing audio queue is already full. The
100 us call is only in non-realtime/offline pull. Realtime pull does not gain
a blocking wait. Emulated CPU frequency, voice processing, oversampling,
queue capacity, MIDI timestamps and all 16 continuously evolving voices are
unchanged. This is correction of a broken host wait, not silence detection or
voice suspension. OS scheduling can still wake late; actual Windows audio
and CPU behavior must be checked by the user.

## Validation and package scope

The Windows test was built from the exact original 0.2.6 source package with
only `eiii_engine.cpp` and the new `eiii_short_wait.h` affecting the application.
It retains the original scalar CEM implementation and O2 compiler flags,
without the earlier O3/LTO or SSE2 experiments. The standalone was user-tested. The VST3 is also rebuilt with the same shared
engine fix and advertises 0.2.7-preview; Windows VST host confirmation remains
pending. Neither binary uses the earlier SIMD/O3/LTO experiments.

- Deterministic native mock of the actual Windows wait helper: passed 100/500 us,
  1000 repeated waits with one handle, zero waits, normal-timer fallback,
  creation/arming/wait failure, upward rounding and handle cleanup.
- Existing WinMM regression: zero failures.
- Native VST3 host smoke with the engine change: zero failures, original
  firmware/OS, sampling, MIDI and state checks. Windows-specific API behavior
  is tested by the mock; actual Windows host execution is not claimed here.
- Windows x64 standalone cross-compiled successfully; imports are system DLLs.

The package contains a refreshed symbol map and matching executable hash for
repeat READY/play captures. Compare the same device, period and buffer count
first; do not change buffers while measuring the CPU fix. The native mock
does not establish Windows driver latency or a measured CPU reduction.

API references:
- https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
- https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer
