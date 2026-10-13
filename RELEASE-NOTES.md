# Windows x64 0.2.2-preview

Working Windows port of parguinsys/Emulated-3, retaining the original firmware/OS execution and emulation core.

- Windows standalone with WinMM audio/MIDI and resizable rack panel.
- VST3 instrument with stereo output, Sample In, 16 MIDI channels and project disk change state.
- CD Insert native accented-path crash fix.
- Remembered ROM directory; editor grip and zoom controls.
- All source build dependencies included, including pinned MIT VST3 SDK subset.

Windows tester confirmed firmware and OS boot, CD bank load, MIDI notes and audible output; Cubase resizing works. Native processor tests cover sampling, MIDI, save-bank/state restore and 64-bit audio. Windows sampling and bulk SysEx have not been validated. VSTHost may forget editor size. No MIDI OUT. Arbitrary Unicode paths remain limited by the native Windows code page.

No E-mu firmware, OS or sample media is included. Supply your own ROMs and OS 2.42 disk image. If a loaded bank is silent outside Supermode, check Master / 8 Special / 7 MIDI Globals and match the host channel or choose Omni.

Release asset: Emulated-3-WIN64-VST3-0.2.2.zip, containing standalone EXE, full VST3 bundle, corresponding source, SDK dependencies, licenses and setup instructions.
