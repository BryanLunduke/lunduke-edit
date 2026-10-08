#!/bin/sh
# Fail if a production lunduke-edit binary contains test-hook environment
# names or a /workspace build path.
#
# Scans a stripped copy. Strip drops debug sections; it does not drop string
# literals, so LUNDUKE_EDIT_TEST compiled into the binary still fails this.
set -eu

if [ "$#" -ne 1 ] || [ -z "${1}" ]; then
  echo "usage: check-production-binary.sh BINARY" >&2
  exit 2
fi

bin=$1
if [ ! -f "$bin" ]; then
  echo "production binary not found: $bin" >&2
  exit 1
fi

stripped=$(mktemp)
trap 'rm -f "$stripped"' EXIT INT TERM
cp "$bin" "$stripped"
strip --strip-unneeded "$stripped"

hits=$(strings -a "$stripped" | grep -F -e 'LUNDUKE_EDIT_TEST' -e '/workspace' || true)
if [ -n "$hits" ]; then
  echo "FAIL: $bin contains LUNDUKE_EDIT_TEST or /workspace" >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi

echo "ok: $bin has no LUNDUKE_EDIT_TEST or /workspace"
