# Keychain deck CAD. `make` builds the printable STLs and the assembly GLBs (with the fit
# check); `make renders` re-renders the hero and section views into build/ (not the log).
PY ?= python3
BLENDER ?= /opt/blender-5.0.1-linux-x64/blender
RENDER = LC_ALL=C LANG=C $(BLENDER) -b -P render.py --

all: parts glb

parts:
	$(PY) shell.py
	$(PY) base.py
	$(PY) jog.py
	$(PY) controls.py

glb:
	$(PY) machine.py

renders: glb
	$(RENDER) build/deck.glb build/hero.png --az 35 --el 34 --dist 2.7 --res 1400x1100 --samples 128 --exposure -0.6
	$(RENDER) build/deck.glb build/top.png --az 0 --el 89 --dist 2.4 --res 1100x1400 --samples 128 --exposure -0.6 --fstop 90
	$(RENDER) build/deck_cut.glb build/cut.png --az 90 --el 8 --dist 1.9 --res 1500x900 --samples 128 --exposure -0.4 --fstop 90

.PHONY: all parts glb renders
