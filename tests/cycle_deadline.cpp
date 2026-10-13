// Regression: long-running absolute-time subtraction can lose a CPU cycle.
#include "runtime/eiii_cycle_deadline.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main()
{
	int fails = 0, old_stalls = 0;
	for (uint64_t base : {40'000'000'000ULL, 360'000'000'000ULL, 864'000'000'000ULL})
		for (uint64_t c = base; c < base + 100'000; ++c)
		{
			long double now = (long double)c / 10'000'000.L;
			long double target = (long double)(c + 1) / 10'000'000.L;
			if (std::floor((target - now) * 10'000'000.L + 1e-9L) == 0) ++old_stalls;
			if (eiii::cycle_deadline_budget(c + 1, c) != 1) ++fails;
		}
	if (eiii::cycle_deadline_budget(10, 10) || eiii::cycle_deadline_budget(9, 10)) ++fails;
	if (eiii::cycle_deadline_budget(std::numeric_limits<uint64_t>::max(), 0) != 1'000'000'000) ++fails;
	printf("old floating-point stalls reproduced: %d; integer-deadline failures: %d\n", old_stalls, fails);
	return fails ? 1 : 0;
}
