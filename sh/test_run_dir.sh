#!/usr/bin/env bash
# 创建 build/test-results/<ts>_<build>_<target>/ 并写 meta.txt。
# 用法：sh/test_run_dir.sh [debug|release] [debug_ut|release_ut]
# 约定见 docs/design/testing_guide.md §8.7。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-debug}
TARGET=${2:-debug_ut}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$ROOT/build/test-results/${STAMP}_${BUILD}_${TARGET}"
ELF="$ROOT/build/firmware/${BUILD}/bin/${TARGET}.elf"

mkdir -p "$OUT"

COMMIT=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)

# 镜像横幅时间戳 = 烧录镜像的判据（CLAUDE.md §3.3）；取不到时退回文件名 + mtime。
IMAGE=unknown
if [ -f "$ELF" ]; then
    if command -v strings >/dev/null 2>&1; then
        IMAGE=$(strings "$ELF" | grep -m1 -oE 'built [A-Z][a-z]{2} [ 0-9]{2} [0-9]{4} [0-9:]{8}' || true)
    fi
    if [ -z "$IMAGE" ]; then
        MTIME=$(date -r "$ELF" -Iseconds 2>/dev/null || echo unknown)
        IMAGE="$(basename "$ELF") mtime=${MTIME}"
    fi
fi

{
    printf 'commit=%s\n' "$COMMIT"
    printf 'image=%s\n' "$IMAGE"
    printf 'build=%s\n' "$BUILD"
    printf 'target=%s\n' "$TARGET"
    printf 'created=%s\n' "$(date -Iseconds)"
    printf 'command=\n'
    printf 'result=\n'
    printf 'notes=\n'
} > "$OUT/meta.txt"

printf '%s\n' "$OUT"
