BIN = strfry
OPT = -O3 -g

include golpe/rules.mk

LDLIBS += -lsecp256k1 -lb2 -lzstd
