# Windows x64 0.2.5-preview

Add real D0 floppy reading and OS boot through the unmodified MAME WD1772 (8 MHz). The EIII drive adapter supplies rotating MFM flux, index, head stepping and active-low motor/side signals. Accept EIII 819200-byte raw IMG and 80-track double-sided 250 kbit/s MFM HFE v1 images.

Standalone and VST3 have D0 insert/eject menus; the physical front-panel drive/eject shortcut now operates D0. D1 can be disconnected for a floppy-only cold boot. Mount/eject preserves the running OS. Standalone D0 defaults remain independent of VST3; VST3 stores the floppy path per project in v2 state and still reads old v1 state.

Floppy media are write protected: reading/boot is supported, Save Bank/format to floppy is not. E3O is an updater package, not a raw floppy image. HD changes and the watchdog fix are retained. OS 2.42 boot tested from both IMG and HFE with no HD, including loading the MIDI Globals menu. Synthetic WD1772 sector bytes/CRC, track/head boundaries, IRQ acknowledgement, write protection and rejected replacement pass. Native VST3 sampling/MIDI/bank/state regression passes. Windows x64 EXE/VST3 cross-build succeeds; host-side Windows GUI confirmation remains necessary.

No E-mu ROM, OS or sample media is bundled.

# Windows x64 0.2.4-preview

Bring the Windows standalone GUI up to date with Settings, boot-ROM selection, D1 OS image selection, D3 CD insert/eject, zoom and analog model controls. Add live WinMM audio and MIDI input device menus. Double-click starts the GUI without command-line media.

Standalone defaults, display appearance, watchdog diagnostics and persistent HD change sidecars have dedicated filenames independent of VST3. The original OS MIDI Globals were verified to survive a fresh engine via its standalone disk sidecar; the base image remains read-only. VST3 continues to store disk changes per instance/project.

The 0.2.3 watchdog fix is retained. Native regression tests and Windows builds pass; Windows GUI/device confirmation is pending. Floppy access remains unimplemented (WD1772 no-disk stub); it requires a separate controller/media implementation and an original EIII floppy image for validation.

No E-mu ROM, OS or sample media is bundled.
