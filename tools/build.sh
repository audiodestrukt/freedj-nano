#!/bin/sh
# Build every firmware config in Docker (ESP-IDF release-v5.5), then the host tests.
#   tools/build.sh                 all configs
#   tools/build.sh devkit_tft      one (xiao_oled is the plain sdkconfig.defaults)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIGS=${*:-"xiao_oled xiao_tft devkit_tft devkit_tft4"}
docker run --rm -v "$ROOT":/repo -w /repo/firmware espressif/idf:release-v5.5 bash -c "
  set -e
  for c in $CONFIGS; do
    defs=sdkconfig.defaults; [ \$c = xiao_oled ] || defs=\"sdkconfig.defaults;sdkconfig.defaults.\$c\"
    echo \"== \$c\"
    idf.py -B build_\$c -D SDKCONFIG=build_\$c/sdkconfig -D SDKCONFIG_DEFAULTS=\"\$defs\" reconfigure build 2>&1 \
      | grep -E '^/repo/(firmware/main|components).*: (error|warning)|binary size|Project build complete|FAILED' || true
    test -f build_\$c/freedj_nano.bin
  done
  chown -R $(id -u):$(id -g) /repo/firmware"
cmake -S "$ROOT/tests" -B "$ROOT/build/tests" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$ROOT/build/tests" -j4 >/dev/null
cd "$ROOT/build/tests" && ./nano_tests | tail -1
