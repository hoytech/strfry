BIN = strfry
OPT = -O3 -g

include golpe/rules.mk

LDLIBS += -lsecp256k1 -lb2 -lzstd

test/xor: OPT=-O0 -g
test/xor: test/xor.cpp
	$(CXX) $(CXXFLAGS) $(INCS) $(LDFLAGS) $(LDLIBS) $< -o $@
