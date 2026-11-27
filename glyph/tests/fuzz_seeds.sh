#!/bin/sh
# Build fuzz seeds from the conformance corpus and examples:
# each seed is PROGRAM, then "\n%%\n", then its event script (if any).
out=${1:?usage: fuzz_seeds.sh OUTDIR}
mkdir -p "$out"
for f in tests/corpus/*.glyph examples/*.glyph; do
    [ -f "$f" ] || continue
    base=$(basename "$f" .glyph)
    ev="${f%.glyph}.events"
    if [ -f "$ev" ]; then
        { cat "$f"; printf '\n%%%%\n'; cat "$ev"; } > "$out/seed-$base"
    else
        cat "$f" > "$out/seed-$base"
    fi
done
