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
INCS    += -I$(BREW_PREFIX)/include
LDFLAGS += -L$(BREW_PREFIX)/lib
endif
ifneq ($(OPENSSL_PREFIX),)
INCS    += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif
endif
INCS += -Iexternal/negentropy/cpp

build/StrfryTemplates.h: $(shell find src/tmpls/ -type f -name '*.tmpl')
	PERL5LIB=golpe/vendor/ perl golpe/external/templar/templar.pl src/tmpls/ strfrytmpl $@

src/apps/relay/RelayWebsocket.o: build/StrfryTemplates.h

.PHONY: test-subid
test-subid: build/subid_tests
	build/subid_tests

build/subid_tests: test/SubIdTests.cpp build/golpe.h
	$(CXX) $(CXXFLAGS) $(INCS) $< -o $@
