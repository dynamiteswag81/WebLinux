#!/bin/sh
# Run every boot test (the default one and test/*.json) in headless
# Chromium; logs and screenshots go to $OUT (default /tmp/wltests).
#   sh test/run-all.sh [name...]        e.g. sh test/run-all.sh jobs net
cd "$(dirname "$0")/.."
OUT=${OUT:-/tmp/wltests}
mkdir -p $OUT
export NODE_PATH=${NODE_PATH:-/opt/wltools/node_modules} STEP_TIMEOUT=${STEP_TIMEOUT:-30}
fail=0
names=${*:-default $(ls test/*.json | sed 's|test/||; s|\.json$||')}
for n in $names; do
	f=test/$n.json
	[ "$n" = default ] && f=
	printf '%-12s ' "$n"
	if timeout 400 node test/boot-test.mjs $f --log $OUT/$n.log --shot $OUT/$n.png; then :; else
		fail=$((fail + 1))
	fi
done
echo "$fail failed; logs and screenshots in $OUT"
[ $fail = 0 ]
