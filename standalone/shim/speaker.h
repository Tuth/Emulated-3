#pragma once
#include "emu.h"
struct speaker_stub { speaker_stub &front() { return *this; } template <typename... A> speaker_stub &add_route(A &&...) { return *this; } };
template <typename... A> speaker_stub &SPEAKER(machine_config &, A &&...) { static speaker_stub s; return s; }
