# Windows x64 0.2.3-preview

Fix a reproducible long-run scheduler rounding defect that can cause an idle watchdog stop: integer host/PIT cycle deadlines no longer round-trip their budgets through absolute seconds. The watchdog remains enabled.

Future VST3 watchdog stops append clock/PC/deadline diagnostics to `%APPDATA%/eiii/eiii-vst3-watchdog.log`.

Includes the Windows standalone EXE, VST3 bundle, matching source, pinned MIT VST3 SDK subset and licenses. No E-mu firmware, OS or sample media is bundled.

The numerical regression reproduces zero budgets in the old algorithm and checks exact progress at long-run timestamps. Native VST3 sampling/MIDI/bank-state and 64-bit regressions pass. Windows host validation of this hotfix remains pending.
