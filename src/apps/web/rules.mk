build/WebTemplates.h: $(shell find src/apps/web/tmpls/ -type f -name '*.tmpl')
	perl golpe/external/templar/templar.pl src/apps/web/tmpls/ tmpl $@

src/apps/web/WebReader.o: build/WebTemplates.h

LDLIBS += -lre2
