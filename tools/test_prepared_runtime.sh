#!/usr/bin/env bash
# Run inside the supplied build image with the project bound at /work.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/prepared-tests
export LD_LIBRARY_PATH=${DEPS:-build/linux/x86_64/deps}/lib
common=(-O2 -ffunction-sections -fdata-sections -Iruntime -I${DEPS:-build/linux/x86_64/deps}/include)
link=(-Wl,--gc-sections -pthread -lm)
clang "${common[@]}" tools/test_texture_identity.c "${link[@]}" -o build/prepared-tests/identity
build/prepared-tests/identity
for test in program_reupload program_cache_identity command_argument_snapshot command_lifetime; do
    clang "${common[@]}" tools/test_${test}.c "${link[@]}" -o build/prepared-tests/${test}
    build/prepared-tests/${test}
done
clang "${common[@]}" tools/test_texture_view_memo.c "${link[@]}" -o build/prepared-tests/view-memo
build/prepared-tests/view-memo
clang "${common[@]}" tools/test_complete_frames.c -L${DEPS:-build/linux/x86_64/deps}/lib -lSDL3 "${link[@]}" -o build/prepared-tests/complete
build/prepared-tests/complete success
expect_failure() {
    local code=0
    "$@" || code=$?
    test "$code" = 99
}
expect_failure build/prepared-tests/complete fail
expect_failure build/prepared-tests/complete timeout
clang "${common[@]}" tools/test_engage_timing.c runtime/engage_timing.c "${link[@]}" -o build/prepared-tests/timing
ENGAGE_FPS=30 build/prepared-tests/timing 30
ENGAGE_FPS=60 build/prepared-tests/timing 60
expect_failure env ENGAGE_FPS=144 build/prepared-tests/timing 144
clang "${common[@]}" tools/test_pipeline_cache.c -o build/prepared-tests/cache
build/prepared-tests/cache
clang "${common[@]}" -Iruntime/nvn/maxwell tools/test_bindless_texel_reads.c \
    runtime/nvn/maxwell/{translate,scf_build,scf_cfg,scf_dom,scf_sim}.c \
    -L${DEPS:-build/linux/x86_64/deps}/lib -lshaderc_shared -lm -o build/prepared-tests/bindless
build/prepared-tests/bindless
printf 'PASS: prepared runtime focused suite, including the real 30-second timeout\n'
