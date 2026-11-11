# Puck deck CAD. `make` builds the printable STLs and the assembly GLBs (with the fit check)
# into build/; `make renders` re-renders the hero, top, inside and section views there.
include ../../cad.mk
RENDER = LC_ALL=C LANG=C $(BLENDER) -b -P render.py --

all: parts glb

parts: $(CAD_DEPS)
	$(PY) shell.py
	$(PY) jog.py

glb: $(CAD_DEPS)
	$(PY) machine.py

renders: glb
	$(RENDER) build/deck.glb build/hero.png --az 30 --el 32 --dist 2.5 --res 1400x1100 --samples 128 --exposure -0.6
	$(RENDER) build/deck.glb build/top.png --az 0 --el 89 --dist 3.0 --res 1200x1200 --samples 128 --exposure -0.6 --fstop 90
	$(RENDER) build/deck_inside.glb build/inside.png --az 25 --el 40 --dist 3.0 --res 1400x1100 --samples 128 --exposure -0.6 --fstop 90
	$(RENDER) build/deck_cut.glb build/cut.png --az 0 --el 6 --dist 3.2 --res 1600x900 --samples 128 --exposure -0.4 --fstop 90

.PHONY: all parts glb renders
