#!/bin/sh
# Renders every simulator scenario and compares the frame hashes with golden.sha1.
# Pass --update after an intended visual change to rewrite the golden file.
set -eu
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

c++ -std=gnu++17 -Wall -Wextra \
  -Ilib/DisplayTask -Ilib/Messaging -Ilib/DosingModel \
  lib/DisplayTask/DisplayTask.cpp -o "$out/display_sim"
"$out/display_sim" "$out" > /dev/null
(cd "$out" && shasum boot.dsim dose.dsim edge.dsim ota.dsim) > "$out/actual.sha1"

if [ "${1:-}" = "--update" ]; then
  cp "$out/actual.sha1" tools/display_sim/golden.sha1
  echo "golden.sha1 updated"
elif diff -u tools/display_sim/golden.sha1 "$out/actual.sha1"; then
  echo "display_sim: all scenarios match the golden hashes"
else
  echo "display_sim: output changed; inspect the frames, then rerun with --update if intended" >&2
  exit 1
fi
