#!/usr/bin/env bash
#
# Build the project in every optimisation mode and run the whole unit suite in
# each of them.
#
# A build that passes in one mode says little about the others: ReleaseFast
# drops the assertions Debug relies on, ReleaseSmall folds code Debug keeps
# apart, and the caches of the two never meet.  Each mode therefore builds and
# runs on its own, reusing whatever is already there.
#
# Pass --clean when the answer itself is in doubt.  A warm cache has reported
# "fine" here for a configuration that broke when it was asked from cold, so a
# build claim worth standing behind is one made after wiping.
#
# Usage:
#   ./test_all.sh                    every mode, over the existing cache
#   ./test_all.sh fast small         only these modes
#   ./test_all.sh --clean            wipe .zig-cache and zig-out before each mode
#   ./test_all.sh --list             show the modes and their flags
#
# Exits 0 only when every configured mode built and every test passed.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

# The fixtures are addressed from the project root and the target is named
# explicitly — a bare `zig build` reaches into the host's kernel headers.
TARGET="${GEOINDEX_TARGET:-x86_64-linux-gnu}"

# The four modes, in the order they are run.  "default" is what a bare
# `zig build` chooses; Zig resolves that to Debug, so it stands here as the
# invocation a developer actually types rather than as a fifth kind of binary.
MODES=(default debug fast small)
FLAGS_default=""
FLAGS_debug="-Doptimize=Debug"
FLAGS_fast="--release=fast"
FLAGS_small="--release=small"

if [ -t 1 ]; then
  BOLD=$'\033[1m'; RED=$'\033[31m'; GREEN=$'\033[32m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=""; RED=""; GREEN=""; DIM=""; OFF=""
fi

clean_cache=0
selected=()

for argument in "$@"; do
  case "$argument" in
    --clean|clean) clean_cache=1 ;;
    --list)
      for mode in "${MODES[@]}"; do
        flags="FLAGS_${mode}"
        printf '  %-8s zig build %s\n' "$mode" "${!flags:-(no flag)}"
      done
      exit 0
      ;;
    -h|--help)
      # the header of this file is the help text; it ends where the comments do
      awk 'NR>2 && /^#/ { sub(/^# ?/, ""); print; next } NR>2 { exit }' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    -*)
      echo "unknown option: $argument" >&2
      exit 2
      ;;
    *)
      flags="FLAGS_${argument}"
      if [ -z "${!flags+set}" ]; then
        echo "unknown mode: $argument (known: ${MODES[*]})" >&2
        exit 2
      fi
      selected+=("$argument")
      ;;
  esac
done

[ ${#selected[@]} -eq 0 ] && selected=("${MODES[@]}")

# --- one line per mode, collected while the modes run and printed at the end ---
summary=()
failed=0

for mode in "${selected[@]}"; do
  flags="FLAGS_${mode}"
  build_flags="${!flags}"
  started=$SECONDS

  printf '\n%s══ %s ══%s  zig build %s -Dtarget=%s -Dtests=true\n' \
    "$BOLD" "$mode" "$OFF" "${build_flags:-(default)}" "$TARGET"

  if [ "$clean_cache" -eq 1 ]; then
    printf '%s   clearing .zig-cache and zig-out%s\n' "$DIM" "$OFF"
    rm -rf .zig-cache zig-out
  fi

  # shellcheck disable=SC2086 -- build_flags is a word list, not one argument
  if ! zig build $build_flags -Dtarget="$TARGET" -Dtests=true; then
    printf '%s   build failed%s\n' "$RED" "$OFF"
    summary+=("$mode|build failed|-|-")
    failed=1
    continue
  fi

  binaries=(zig-out/bin/test_*)
  if [ ! -e "${binaries[0]}" ]; then
    printf '%s   no test binaries were installed%s\n' "$RED" "$OFF"
    summary+=("$mode|no tests built|-|-")
    failed=1
    continue
  fi

  mode_tests=0
  mode_failed=0
  for binary in "${binaries[@]}"; do
    name="$(basename "$binary")"
    # from the project root: the fixtures under tests/data are named relative to it
    output="$("$binary" 2>&1)"
    status=$?
    count="$(printf '%s' "$output" | grep -oP '\d+(?= tests? from \d+ test suites? ran)' | tail -1)"
    count="${count:-0}"
    mode_tests=$((mode_tests + count))

    if [ $status -eq 0 ]; then
      printf '   %s%-28s%s %4s tests %sok%s\n' "$DIM" "$name" "$OFF" "$count" "$GREEN" "$OFF"
    else
      mode_failed=$((mode_failed + 1))
      failed=1
      printf '   %-28s %4s tests %sFAILED%s\n' "$name" "$count" "$RED" "$OFF"
      printf '%s' "$output" | grep -E '^\[  FAILED  \]|Failure$' | sed 's/^/      /'
    fi
  done

  took=$((SECONDS - started))
  if [ "$mode_failed" -eq 0 ]; then
    summary+=("$mode|ok|$mode_tests|${took}s")
  else
    summary+=("$mode|$mode_failed suites failed|$mode_tests|${took}s")
  fi
done

printf '\n%s── summary ──%s\n' "$BOLD" "$OFF"
for line in "${summary[@]}"; do
  IFS='|' read -r mode verdict tests took <<< "$line"
  colour="$GREEN"
  [ "$verdict" = "ok" ] || colour="$RED"
  printf '  %-8s %s%-20s%s %6s tests  %6s\n' "$mode" "$colour" "$verdict" "$OFF" "$tests" "$took"
done

if [ "$failed" -ne 0 ]; then
  printf '\n%ssomething did not pass%s\n' "$RED" "$OFF"
  exit 1
fi
printf '\n%severy mode built and every test passed%s\n' "$GREEN" "$OFF"
