#!/usr/bin/env bash
# Cross-compile the Windows x64 DLL and CLI on Linux with clang-cl + xwin.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="cloudredirect-windows-builder"

runtime=""
for c in podman docker; do
	if command -v "$c" >/dev/null 2>&1; then runtime="$c"; break; fi
done
[ -n "$runtime" ] || { echo "need podman or docker" >&2; exit 1; }

if ! "$runtime" image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "==> building builder image ($IMAGE)"
	"$runtime" build -f "$HERE/docker/windows/Dockerfile" -t "$IMAGE" "$HERE/docker/windows"
fi

GIT_SHA="$(git -C "$HERE" rev-parse --short=7 HEAD)"
if [ -n "$(git -C "$HERE" status --porcelain --untracked-files=normal)" ]; then
	GIT_SHA="$GIT_SHA-dirty"
fi

echo "==> building Windows x64 CloudRedirect DLL + CLI (sha $GIT_SHA)"
"$runtime" run --rm -v "$HERE":/src:Z -w /src "$IMAGE" bash -euc '
  cmake -S . -B build/win -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/src/cmake/clang-cl-msvc.cmake \
        -DCMAKE_BUILD_TYPE=Release -DCR_GIT_SHA="$1"
  cmake --build build/win --target cloud_redirect cloud_redirect_cli --parallel "$(nproc)"
' bash "$GIT_SHA"

sha256sum "$HERE/build/win/cloud_redirect.dll" "$HERE/build/win/cloud_redirect_cli.exe"
