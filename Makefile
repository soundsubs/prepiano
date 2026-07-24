# PrePiano — convenience targets. See scripts/ for the full build/deploy flow.
CC      ?= cc
CFLAGS  ?= -O3 -Wall -Wextra -Isrc
LDLIBS  ?= -lm

.PHONY: all demo plugin clean tools showcase
all: demo plugin

# Analysis / verification harnesses
tools: build/measure build/bench
build/measure: test/measure_partials.c src/prepiano_dsp.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test/measure_partials.c src/prepiano_dsp.c $(LDLIBS)
build/bench: test/bench.c src/prepiano_dsp.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test/bench.c src/prepiano_dsp.c $(LDLIBS)

# Focused showcase of inharmonicity + detuned unisons
showcase: build/showcase
	./build/showcase build/prepiano_showcase.wav
build/showcase: test/render_showcase.c src/prepiano_dsp.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test/render_showcase.c src/prepiano_dsp.c $(LDLIBS)

# Desktop renderer + demo WAV
demo: build/prepiano_demo
	./build/prepiano_demo build/prepiano_demo.wav

build/prepiano_demo: test/render_demo.c src/prepiano_dsp.c src/prepiano_dsp.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ test/render_demo.c src/prepiano_dsp.c $(LDLIBS)

# Host-native shared library (sanity build; use scripts/build_move.sh for ARM64)
plugin: build/modules/prepiano/dsp.so
build/modules/prepiano/dsp.so: src/plugin/prepiano_plugin.c src/prepiano_dsp.c
	@mkdir -p build/modules/prepiano
	$(CC) $(CFLAGS) -shared -fPIC -o $@ \
		src/plugin/prepiano_plugin.c src/prepiano_dsp.c $(LDLIBS)
	@cp src/modules/prepiano/module.json src/modules/prepiano/ui.js build/modules/prepiano/

clean:
	rm -rf build
