// Regression of the C++ CEM3387 laws against the Python reference model (analysis/tools/voice_hw_model.py).
// usage: cem_vs_reference path/to/voice_hw_model.py
// Compares, for all 4096 CV codes: volts, Fc1, Fc2, gain dB, pan L/R; and gain-network step times
// (Python explicit Euler at 10 us vs the C++ voice at 192 kHz) for rising and falling steps.
#include "../cem/eiii_cem3387.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace eiii;

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s voice_hw_model.py\n", argv[0]); return 2; }
	std::string path = argv[1];
	std::string dir = path.substr(0, path.find_last_of('/'));
	std::string cmd = "python3 -c \"import sys; sys.path.insert(0,'" + dir + "'); import voice_hw_model as m\n"
		"for c in range(4096):\n"
		"  w=c<<2; v=m.word_to_volts(w); l,r=m.pan_lr(v)\n"
		"  print('L',w,repr(v),repr(m.fc1(v)),repr(m.fc2(v)),repr(m.gain_db(v)),repr(l),repr(r))\n"
		"for a,b in ((0,0x3FFC),(0x3FFC,0),(0,0x2000),(0x2000,0x1000),(0x1000,0x1400)):\n"
		"  v0,v1=m.word_to_volts(a),m.word_to_volts(b); tr=m.gain_network_step(v0,v1,t_end=0.06); tg=v0+0.632*(v1-v0)\n"
		"  t=next((t for t,v in tr if (v1>v0 and v>=tg) or (v1<v0 and v<=tg)),-1)\n"
		"  print('S',a,b,repr(t))\n\"";
	FILE *p = popen(cmd.c_str(), "r");
	if (!p) { perror("popen"); return 2; }
	char line[512];
	unsigned n = 0, bad = 0; double worst[5] = {0, 0, 0, 0, 0};
	while (fgets(line, sizeof line, p))
	{
		if (line[0] == 'L')
		{
			unsigned w; double v, f1, f2, g, l, r;
			sscanf(line + 2, "%u %lf %lf %lf %lf %lf %lf", &w, &v, &f1, &f2, &g, &l, &r);
			double const cv = cem_law::word_to_volts(uint16_t(w));
			double cl, cr; cem_law::pan_lr(cv, cl, cr);
			double const e[5] = {std::fabs(cv - v), std::fabs(cem_law::fc1_hz(cv) / f1 - 1), std::fabs(cem_law::fc2_hz(cv) / f2 - 1),
				std::fabs(cem_law::gain_db(cv) - g), std::fabs(cl - l) + std::fabs(cr - r)};
			for (int i = 0; i < 5; ++i) worst[i] = std::fmax(worst[i], e[i]);
			bad += e[0] > 1e-9 || e[1] > 1e-9 || e[2] > 1e-9 || e[3] > 1e-9 || e[4] > 1e-9;
			++n;
		}
		else if (line[0] == 'S')
		{
			unsigned a, b; double tref;
			sscanf(line + 2, "%u %u %lf", &a, &b, &tref);
			cem3387_voice vc; vc.set_cv(3, uint16_t(a)); vc.prepare(192000.0); vc.set_cv(3, uint16_t(b));
			double const v0 = cem_law::word_to_volts(uint16_t(a)), v1 = cem_law::word_to_volts(uint16_t(b)), tg = v0 + 0.632 * (v1 - v0);
			double l, r; int i = 0;
			while (i < 192000 && !((v1 > v0 && vc.cv_volts(3) >= tg) || (v1 < v0 && vc.cv_volts(3) <= tg))) { vc.process(0, l, r); ++i; }
			double const t = i / 192000.0, err = std::fabs(t / tref - 1);
			bool const ok = err < 0.03;
			printf("%s gain-network step %04X -> %04X: 63 %% at %.3f ms (reference %.3f ms, %+.1f %%)\n",
				ok ? "PASS" : "FAIL", a, b, t * 1e3, tref * 1e3, (t / tref - 1) * 100);
			bad += !ok;
		}
	}
	pclose(p);
	printf("%s law sweep over %u codes: max |dV| %.2e V, Fc1 rel %.2e, Fc2 rel %.2e, gain %.2e dB, pan %.2e\n",
		n == 4096 && bad == 0 ? "PASS" : "FAIL", n, worst[0], worst[1], worst[2], worst[3], worst[4]);
	return (n == 4096 && bad == 0) ? 0 : 1;
}
