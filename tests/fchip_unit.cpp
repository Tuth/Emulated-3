// Deterministic loop-mode tests for eiii::fchip_bank (ramp sample RAM: word i = i, rate N = 1, one request per clock).
#include "../fchip/eiii_fchip.h"
#include <cstdio>
#include <vector>

static constexpr uint32_t DEC = 1u << 23, LOOP = 1u << 24, FORBK = 1u << 22;
static int fails = 0;

static std::vector<int> run(eiii::fchip_bank &fc, unsigned n)
{
	std::vector<int> out;
	for (unsigned i = 0; i < n; ++i) { out.push_back(fc.pcm(0)); fc.advance(1); }
	return out;
}

static void check(char const *name, std::vector<int> const &got, std::vector<int> const &exp)
{
	bool ok = got == exp;
	printf("%s %s:", ok ? "PASS" : "FAIL", name);
	for (int v : got) printf(" %d", v);
	printf("\n");
	if (!ok) { ++fails; printf("     expected:"); for (int v : exp) printf(" %d", v); printf("\n"); }
}

static void setup(eiii::fchip_bank &fc, uint32_t cs, uint32_t is, uint32_t ca, uint32_t ia)
{
	fc.reset();
	fc.ctrl_write(0, 0x00); fc.ctrl_write(0, 0x01);        // N = 1
	fc.rf_write(0, 2, ca); fc.rf_write(0, 0, cs); fc.rf_write(0, 3, ia); fc.rf_write(0, 1, is);
}

int main()
{
	std::vector<uint16_t> ram(0x1000);
	for (size_t i = 0; i < ram.size(); ++i) ram[i] = uint16_t(i);
	eiii::fchip_bank fc(ram.data(), ram.size());

	// mode 0 forward one-shot: size 4 -> 5 samples, then silence
	setup(fc, 4 | DEC, 4 | DEC, 100, 100);
	check("forward one-shot", run(fc, 8), {100, 101, 102, 103, 104, 0, 0, 0});

	// mode 1 forward loop: first pass 100..104, loop 102..104
	setup(fc, 4 | DEC | LOOP, 2 | DEC | LOOP, 100, 102);
	check("forward loop", run(fc, 11), {100, 101, 102, 103, 104, 102, 103, 104, 102, 103, 104});

	// mode 4 reverse one-shot: address DECFLG
	setup(fc, 3 | DEC, 3 | DEC, 200 | DEC, 200 | DEC);
	check("reverse one-shot", run(fc, 6), {200, 199, 198, 197, 0, 0});

	// mode 5 reverse loop: 210 down to 205, loop 207..205
	setup(fc, 5 | DEC | LOOP, 2 | DEC | LOOP, 210 | DEC, 207 | DEC);
	check("reverse loop", run(fc, 12), {210, 209, 208, 207, 206, 205, 207, 206, 205, 207, 206, 205});

	// mode 2 forward-backward: start 100, loop start 110, loop end 115 (L = 5);
	// firmware sizes: first pass loopend - start - 2 = 13, loop L - 2 = 3 (STRONG turnaround rule)
	setup(fc, 13 | DEC | LOOP | FORBK, 3 | DEC | LOOP | FORBK, 100, 110);
	check("forward-backward", run(fc, 30),
		{100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
		 114, 113, 112, 111, 110, 111, 112, 113, 114, 113, 112, 111, 110, 111, 112, 113});

	// rate: N = 0x1C5 gives one request per 453 clocks
	setup(fc, 100 | DEC | LOOP, 100 | DEC | LOOP, 300, 300);
	fc.ctrl_write(0, 0x01); fc.ctrl_write(0, 0xC5);
	fc.advance(453 * 10 - 1);
	check("rate 0x1C5 (9 requests in 4529 clocks)", {fc.pcm(0)}, {309});

	// stop as the firmware does it (size 0, no LOOPFLG): one more sample, then silence
	setup(fc, 50 | DEC | LOOP, 50 | DEC | LOOP, 400, 400);
	fc.advance(3);
	fc.rf_write(0, 0, 0 | DEC);
	check("firmware stop (size 0x800000)", run(fc, 3), {403, 0, 0});

	printf("%s\n", fails ? "SOME TESTS FAILED" : "all tests passed");
	return fails ? 1 : 0;
}
