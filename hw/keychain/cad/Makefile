# Keychain deck CAD. `make` builds the printable STLs and the assembly GLBs (with the fit
# check) into build/<screen>/; `make renders` re-renders the hero and section views there
# (not the log). SCREEN=tft20 (default, 2" ST7789) or SCREEN=oled095 (0.95" OLED).
SCREEN ?= tft20
export KC_SCREEN = $(SCREEN)
B = build/$(SCREEN)
include ../../cad.mk
RENDER = LC_ALL=C LANG=C $(BLENDER) -b -P render.py --

all: parts glb

parts: $(CAD_DEPS)
	$(PY) shell.py
	$(PY) base.py
	$(PY) jog.py
	$(PY) controls.py

glb: $(CAD_DEPS)
	$(PY) machine.py

renders: glb
	$(RENDER) $(B)/deck.glb $(B)/hero.png --az 35 --el 34 --dist 2.7 --res 1400x1100 --samples 128 --exposure -0.6
	$(RENDER) $(B)/deck.glb $(B)/top.png --az 0 --el 89 --dist 2.4 --res 1100x1400 --samples 128 --exposure -0.6 --fstop 90
	$(RENDER) $(B)/deck_cut.glb $(B)/cut.png --az 90 --el 8 --dist 1.9 --res 1500x900 --samples 128 --exposure -0.4 --fstop 90

.PHONY: all parts glb renders
