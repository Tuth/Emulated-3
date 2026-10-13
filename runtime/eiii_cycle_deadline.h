#pragma once
#include <algorithm>
#include <cstdint>

namespace eiii {
// Integer hardware/host deadlines must not round-trip through absolute seconds.
inline int cycle_deadline_budget(uint64_t deadline, uint64_t current)
{
	return deadline > current ? int(std::min<uint64_t>(deadline - current, 1'000'000'000)) : 0;
}
}
