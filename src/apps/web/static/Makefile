CSS := reset.css oddbean.css
JS  := base.ts turbo.js oddbean.js alpine.js

.PHONY: all css js logo

all: css js logo

css: $(CSS)
	mkdir -p build/
	cat $(CSS) | sassc -s -t compressed > build/oddbean.css
	gzip -k9f build/oddbean.css

js: $(JS)
	mkdir -p build/
	cat $(JS) | esbuild --loader=ts --minify > build/oddbean.js
	gzip -k9f build/oddbean.js

logo:
	cp oddbean.svg build/
