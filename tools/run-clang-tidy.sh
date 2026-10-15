#!/usr/bin/env bash
# Run clang-tidy over the engine sources, identically on macOS and Linux.
#
# The point of this script is that "it passed locally" and "it passed in CI"
# mean the same thing. Use the pinned clang-tidy from requirements-lint.txt.
set -euo pipefail

BUILD_DIR=${1:-build/debug}
CLANG_TIDY=${CLANG_TIDY:-clang-tidy}

if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
  echo "error: no compile_commands.json in $BUILD_DIR" >&2
  echo "hint: run 'cmake --preset debug' first" >&2
  exit 1
fi

extra_args=()
if [[ "$(uname -s)" == "Darwin" ]]; then
  # pip's clang-tidy is not Apple's toolchain, so it does not know where the
  # macOS SDK keeps the standard library headers. Without this, every
  # '#include <string>' is reported as a hard error rather than a real finding.
  extra_args+=("--extra-arg=-isysroot$(xcrun --show-sdk-path)")
fi

# Only .cc files are passed: a header has no entry in the compilation database,
# and HeaderFilterRegex in .clang-tidy covers headers through their includers.
find src -name '*.cc' -print0 \
  | xargs -0 "$CLANG_TIDY" -p "$BUILD_DIR" ${extra_args[@]+"${extra_args[@]}"}
