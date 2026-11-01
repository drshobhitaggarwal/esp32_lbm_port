#!/bin/sh
# Usage: tools/host_check/run.sh [config-name ...]   (default: all configs in configs/)
# Needs: python3 with esp-idf-kconfig (pip install esp-idf-kconfig), cmake, a C compiler.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/lbm_host_check}
CONFIGS=${*:-$(cd "$HERE/configs" && ls *.cfg | sed 's/\.cfg$//')}
for cfg in $CONFIGS; do
  for timer in ON OFF; do
    [ "$cfg" != default ] && [ "$timer" = OFF ] && continue
    D="$OUT/$cfg-timer$timer"
    rm -rf "$D"; mkdir -p "$D"
    echo "=== config: $cfg  (esp_timer ISR dispatch: $timer)"
    (cd "$HERE" && python3 -m kconfgen --kconfig Kconfig.top --defaults "configs/$cfg.cfg" \
        --output header "$D/sdkconfig.h" --output cmake "$D/sdkconfig.cmake" --config "$D/sdkconfig" \
        --env IDF_TARGET=esp32s3 --env IDF_ENV_FPGA= --env IDF_INIT_VERSION=5.4.2 > "$D/kconfgen.log" 2>&1)
    cmake -S "$HERE" -B "$D/build" -DSDKCONFIG_DIR="$D" -DTIMER_ISR=$timer > "$D/cmake.log" 2>&1 || { cat "$D/cmake.log"; exit 1; }
    make -C "$D/build" -j8 > "$D/make.log" 2>&1 || { grep -E "error|Error|undefined" "$D/make.log" | head -30; exit 1; }
    echo "build ok; warnings in upstream+port: $(grep -c 'warning:' "$D/make.log")"
    "$D/build/host_test"
  done
done

# otaa_class_a example: generate its sdkconfig and compile main.c with strict warnings.
COMP=$HERE/../../components/lora_basics_modem
for cfg in default variant; do
  D="$OUT/otaa-$cfg"
  rm -rf "$D"; mkdir -p "$D"
  echo "=== otaa_class_a example: $cfg"
  (cd "$HERE" && python3 -m kconfgen --kconfig Kconfig.otaa.top --defaults "configs_otaa/$cfg.cfg" \
      --output header "$D/sdkconfig.h" --config "$D/sdkconfig" \
      --env IDF_TARGET=esp32s3 --env IDF_ENV_FPGA= --env IDF_INIT_VERSION=5.4.2 > "$D/kconfgen.log" 2>&1) || { cat "$D/kconfgen.log"; exit 1; }
  gcc -fsyntax-only -Wall -Wextra -Werror -Wno-unused-parameter -DSX126X -DNUMBER_OF_STACKS=1 \
      -I"$HERE/mock" -I"$D" -I"$COMP/include" -I"$COMP/third_party/SWL2001/lbm_lib/smtc_modem_api" \
      "$HERE/../../examples/otaa_class_a/main/main.c" && echo "main.c compiles clean"
done
