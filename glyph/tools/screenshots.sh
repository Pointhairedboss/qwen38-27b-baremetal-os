#!/bin/bash
# screenshots.sh — regenerate docs/screenshots/*.png.
#
#   make screenshots          (or: tools/screenshots.sh [OUTDIR])
#
# Runs `glyph run` in a real xterm (80x25, VGA 16-colour palette) on a
# virtual X display, sends real key events with xdotool, and captures the
# window with ImageMagick. Linux host only; this is not the bare-metal target.
# Needs: Xvfb, xterm, xdotool, ImageMagick (import), DejaVu Sans Mono.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-docs/screenshots}
GLYPH=$PWD/build/glyph
DISP=:97
mkdir -p "$OUT"
[ -x "$GLYPH" ] || { echo "build/glyph missing: run make first" >&2; exit 1; }
for t in Xvfb xterm xdotool import; do
    command -v $t >/dev/null || { echo "missing tool: $t" >&2; exit 1; }
done

export DISPLAY=$DISP
Xvfb $DISP -screen 0 2000x1000x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
sleep 1

# VGA palette in ANSI order: black red green brown blue magenta cyan grey, then bright
PAL=(000000 AA0000 00AA00 AA5500 0000AA AA00AA 00AAAA AAAAAA 555555 FF5555 55FF55 FFFF55 5555FF FF55FF 55FFFF FFFFFF)
XRM=()
for i in "${!PAL[@]}"; do XRM+=(-xrm "XTerm*color$i: #${PAL[$i]}"); done
XTERM=(xterm -fa 'DejaVu Sans Mono' -fs 15 -bg black -fg '#AAAAAA' -bw 0 "${XRM[@]}")

# shot PROGRAM NAME:KEYS...   KEYS are xdotool key names; type=TEXT types text
shot() {
    local prog=$1; shift
    "${XTERM[@]}" -geometry 80x25+0+0 -b 0 -title glyphshot -e "$GLYPH" run "$prog" &
    local xt=$! step name keys k wid
    sleep 1.5
    wid=$(xdotool search --name glyphshot | head -1)
    xdotool mousemove 200 200
    for step in "$@"; do
        name=${step%%:*}; keys=${step#*:}
        for k in $keys; do
            if [[ $k == type=* ]]; then xdotool type --delay 60 "${k#type=}"; else xdotool key --delay 60 "$k"; fi
            sleep 0.15
        done
        sleep 0.6
        import -window "$wid" "$OUT/$name.png"
        echo "captured $OUT/$name.png"
    done
    xdotool key Escape
    sleep 0.5
    kill $xt 2>/dev/null || true
    wait $xt 2>/dev/null || true
}

shot examples/demo.glyph \
    "01-demo-start:" \
    "02-demo-interacting:Tab Return Return Return Tab Tab type=Ada Return Tab Return Return Return" \
    "03-demo-locked:Tab Return"
shot examples/turing.glyph \
    "04-turing-start:" \
    "05-turing-stepping:Tab Tab Tab Return Return Return Return Return Return Return Return Return Return Return Return" \
    "06-turing-halted:Tab Return"
shot examples/rule110.glyph "07-rule110:"
shot examples/tree.glyph "08-tree:"

# Diagnostics: glyph check on programs with typical generated-code mistakes.
D=$(mktemp -d)
cp examples/turing.glyph "$D/ok_turing.glyph"
printf 'REPAET 4 [FD 10 RT 90]\n' > "$D/typo.glyph"
printf 'TO square :n\n  REPEAT 4 [FD :n RT 90]\nEND\nsquare\n' > "$D/inputs.glyph"
printf 'SCREEN 40 10\nPANEL box 0 0 20 5 "Main"\n' > "$D/reserved.glyph"
printf 'REPEAT 4 [FD 10 RT 90\nPU\n' > "$D/unclosed.glyph"
printf 'ARRAY v 5\nSETITEM v 5 50\nSET total ITEM v 1 + ITEM v 5   ; means ITEM v (1 + ITEM v 5)\n' > "$D/precedence.glyph"
"${XTERM[@]}" -geometry 118x13+0+0 -b 4 -title glyphdiag -e bash -c \
    "cd '$D'; printf '\$ glyph check *.glyph\n'; '$GLYPH' check *.glyph;
     printf '\n\$ glyph replay precedence.glyph   # loads fine, fails at run time\n';
     '$GLYPH' replay precedence.glyph | grep -E 'main|!'; sleep 20" &
XT=$!
sleep 2
import -window "$(xdotool search --name glyphdiag | head -1)" "$OUT/09-diagnostics.png"
echo "captured $OUT/09-diagnostics.png"
kill $XT 2>/dev/null || true
wait $XT 2>/dev/null || true
case "$D" in /tmp/*) rm -r "$D" ;; esac
