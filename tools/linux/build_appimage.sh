#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
podman build -t localhost/engage-public-build:0.2.0 -f tools/linux/Containerfile tools/linux
podman run --rm --userns=keep-id -v "$PWD:/src:rw" -w /src --entrypoint bash localhost/engage-public-build:0.2.0 -euc '
bash tools/linux/build_deps.sh x86_64
make -j3 B=build/linux/x86_64/game DEPS=build/linux/x86_64/deps SHADERC=-lshaderc_shared GAMELIB_ID=engage-200-bringup EXE=build/linux/x86_64/game/engage-recomp build/linux/x86_64/game/engage-recomp
bash tools/linux/generator_helper.sh x86_64
ZIG_TARGET=x86_64-linux bash tools/fetch_zig.sh
bash tools/linux/package_engage.sh
'
