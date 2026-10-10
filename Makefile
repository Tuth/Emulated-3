# eiii-core: portable EIII voice hardware (no MAME dependencies)
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -ffp-contract=off -Wall -Wextra
LIB_SRC   = fchip/eiii_fchip.cpp cem/eiii_cem3387.cpp cem/eiii_cem_calibration.cpp cem/eiii_cem_cv_conditioning.cpp cem/eiii_cem3387_v2.cpp host/eiii_backend.cpp
LIB_OBJ   = $(LIB_SRC:.cpp=.o)
TRACES   ?= ../analysis/traces

all: libeiiicore.a tests/fchip_unit tests/fchip_equiv tests/cem_unit tests/cem_vs_reference tests/cem_golden tests/cem_v2_unit tools/render_trace tools/eiii_fakehost tools/cem3387_lab

libeiiicore.a: $(LIB_OBJ)
	ar rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# every object embeds these classes by value: any header change must rebuild all of them
LIB_HDR   = fchip/eiii_fchip.h cem/eiii_cem3387.h cem/eiii_cem_calibration.h cem/eiii_cem_cv_conditioning.h cem/eiii_cem3387_v2.h host/eiii_backend.h host/eiii_decimator.h host/eiii_core.h
$(LIB_OBJ): $(LIB_HDR)

tests/%: tests/%.cpp libeiiicore.a $(LIB_HDR)
	$(CXX) $(CXXFLAGS) $< libeiiicore.a -o $@

tools/%: tools/%.cpp libeiiicore.a $(LIB_HDR)
	$(CXX) $(CXXFLAGS) -Ihost $< libeiiicore.a -o $@

check: all
	./tests/fchip_unit
	./tests/cem_unit
	./tests/cem_golden 0dbb559bed479225 tests/cem_golden_ref.txt
	./tests/cem_golden --v2 4bd99acd3c15aca1 tests/cem_v2_golden_ref.txt
	./tests/cem_v2_unit
	./tests/cem_vs_reference ../analysis/tools/voice_hw_model.py
	for t in fwdbk oneshot_loop dpscrub; do ./tests/fchip_equiv $(TRACES)/fchip_equiv/$$t.trace $(TRACES)/fchip_equiv/$$t.snd || exit 1; done
	python3 tests/standalone_acceptance.py $(TRACES)/standalone_accept

clean:
	rm -f $(LIB_OBJ) libeiiicore.a tests/fchip_unit tests/fchip_equiv tests/cem_unit tests/cem_vs_reference tests/cem_golden tests/cem_v2_unit tools/render_trace tools/eiii_fakehost tools/cem3387_lab

.PHONY: all check clean
