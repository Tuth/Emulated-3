# Windows x64 0.2.4-preview

Bring the Windows standalone GUI up to date with Settings, boot-ROM selection, D1 OS image selection, D3 CD insert/eject, zoom and analog model controls. Add live WinMM audio and MIDI input device menus. Double-click starts the GUI without command-line media.

Standalone defaults, display appearance, watchdog diagnostics and persistent HD change sidecars have dedicated filenames independent of VST3. The original OS MIDI Globals were verified to survive a fresh engine via its standalone disk sidecar; the base image remains read-only. VST3 continues to store disk changes per instance/project.

The 0.2.3 watchdog fix is retained. Native regression tests and Windows builds pass; Windows GUI/device confirmation is pending. Floppy access remains unimplemented (WD1772 no-disk stub); it requires a separate controller/media implementation and an original EIII floppy image for validation.

No E-mu ROM, OS or sample media is bundled.
