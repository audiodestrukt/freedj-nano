# Shared by hw/*/cad/Makefile: the Python the CAD scripts run in, and where Blender is.
#
# By default the scripts run in hw/.venv, which is created on first use from
# cad-requirements.txt and rebuilt when that file changes. build123d needs Python 3.10+,
# so the venv is made with uv when it is installed (it fetches a suitable Python);
# otherwise with VENV_PYTHON, which must itself be 3.10+.
# Pass PY=/some/python to use your own interpreter and skip the venv entirely.
# Included above the including Makefile's own targets, so keep `all` the default.
.DEFAULT_GOAL := all

HW := $(dir $(lastword $(MAKEFILE_LIST)))
VENV ?= $(HW).venv
VENV_PYTHON ?= python3
CAD_REQS := $(HW)cad-requirements.txt

ifndef PY
PY := $(VENV)/bin/python
CAD_DEPS := $(VENV)/.stamp
endif

$(VENV)/.stamp: $(CAD_REQS)
	@if command -v uv >/dev/null 2>&1; then \
		set -ex; uv venv --python 3.12 --allow-existing $(VENV); \
		uv pip install --python $(VENV)/bin/python -r $(CAD_REQS); \
	else \
		set -ex; $(VENV_PYTHON) -m venv $(VENV); \
		$(VENV)/bin/python -m pip install -r $(CAD_REQS); \
	fi
	@touch $@

deps: $(CAD_DEPS)

# Blender, for `make renders` only: on PATH, else the usual macOS / Linux install places.
BLENDER ?= $(firstword $(shell command -v blender 2>/dev/null) $(wildcard \
	/Applications/Blender.app/Contents/MacOS/Blender /opt/blender-5.0.1-linux-x64/blender) blender)

.PHONY: deps
