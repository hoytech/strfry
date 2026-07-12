BIN  ?= strfry
APPS ?= dbutils relay mesh
OPT  ?= -O3 -g

include golpe/rules.mk

LDLIBS += -lsecp256k1 -lzstd
ifeq ($(shell uname -s),Darwin)
LDLIBS += -luv
BREW_PREFIX    := $(shell brew --prefix 2>/dev/null)
OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
ifneq ($(BREW_PREFIX),)
BREW_INCS += -I$(BREW_PREFIX)/include
BREW_LIBS += -L$(BREW_PREFIX)/lib
endif
ifneq ($(OPENSSL_PREFIX),)
BREW_INCS += -I$(OPENSSL_PREFIX)/include
BREW_LIBS += -L$(OPENSSL_PREFIX)/lib
endif
INCS      += $(BREW_INCS)
LDFLAGS   += $(BREW_LIBS)
# Propagate to sub-makes (e.g. uWebSockets) that honor XCXXFLAGS / XLDFLAGS
export XCXXFLAGS += $(BREW_INCS)
export XLDFLAGS  += $(BREW_LIBS)
endif
INCS += -Iexternal/negentropy/cpp

build/StrfryTemplates.h: $(wildcard src/tmpls/*.tmpl)
	perl -Igolpe/vendor golpe/external/templar/templar.pl src/tmpls/ strfrytmpl $@

src/apps/relay/RelayWebsocket.o: build/StrfryTemplates.h

ifeq ($(OS),Windows_NT)
    TEST_BIN = build/subid_tests.exe
else
    TEST_BIN = build/subid_tests
endif

.PHONY: test-subid
test-subid: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): test/SubIdTests.cpp build/golpe.h
	$(CXX) $(CXXFLAGS) $(INCS) $< -o $@
