// Compiles MAME's unmodified src/emu/diserial.cpp against the shim: diserial.cpp sits next to MAME's real emu.h,
// whose include guard (__EMU_H__) is already defined by shim/emu.h, so the real header is skipped.
#include "emu.h"
#include "src/emu/diserial.cpp"   // via -I$(MAME): vendored third_party/mame or a MAME tree
