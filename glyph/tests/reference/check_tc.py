#!/usr/bin/env python3
"""Check Glyph's Turing-completeness demonstrations against independent
Python implementations.

  python3 tests/reference/check_tc.py build/glyph

1. examples/turing.glyph runs the 2-, 3- and 4-state busy beavers. Steps,
   ones, final head position and the whole tape must equal a direct Python
   simulation of the same machines.
2. examples/rule110.glyph draws 24 generations of Rule 110. Every frame row
   must equal a Python Rule 110 evolution.
3. Recursion and array results printed by tests/corpus/tc_*.glyph programs
   must equal Python computations of the same functions.

Exit status 0 when everything matches.
"""
import subprocess
import sys

GLYPH = sys.argv[1] if len(sys.argv) > 1 else "build/glyph"

BUSY_BEAVERS = {  # name: (load action, {(state, symbol): (write, move, next)})
    "BB2": ("load2", {("A", 0): (1, 1, "B"), ("A", 1): (1, -1, "B"),
                      ("B", 0): (1, -1, "A"), ("B", 1): (1, 1, "H")}),
    "BB3": ("load3", {("A", 0): (1, 1, "B"), ("A", 1): (1, 1, "H"),
                      ("B", 0): (0, 1, "C"), ("B", 1): (1, 1, "B"),
                      ("C", 0): (1, -1, "C"), ("C", 1): (1, -1, "A")}),
    "BB4": ("load4", {("A", 0): (1, 1, "B"), ("A", 1): (1, -1, "B"),
                      ("B", 0): (1, -1, "A"), ("B", 1): (0, -1, "C"),
                      ("C", 0): (1, 1, "H"), ("C", 1): (1, -1, "D"),
                      ("D", 0): (1, 1, "D"), ("D", 1): (0, 1, "A")}),
}


def simulate(machine, size=80, head=40):
    tape = [0] * (size + 1)          # 1-based, like the Glyph ARRAY
    state, steps = "A", 0
    while state != "H":
        write, move, state = machine[(state, tape[head])]
        tape[head] = write
        head += move
        steps += 1
    return {"STEPS": steps, "ONES": sum(tape), "HEAD": head, "TAPE": tape[1:]}


def rule110(width=80, rows=24):
    row = [0] * width
    row[-1] = 1
    out = []
    for _ in range(rows):
        out.append("".join("#" if c else " " for c in row).rstrip())
        row = [(110 >> (4 * row[(i - 1) % width] + 2 * row[i] + row[(i + 1) % width])) & 1
               for i in range(width)]
    return out


def replay(program, script):
    res = subprocess.run([GLYPH, "replay", program, "-"], input=script.encode(),
                         capture_output=True, check=False)
    if res.returncode != 0:
        sys.exit(f"glyph replay {program} failed: {res.stderr.decode()}")
    return res.stdout.decode()


def state_blocks(transcript):
    """Each '@ state' block as a dict of name -> value string."""
    blocks, cur = [], None
    for line in transcript.splitlines():
        if line == "@ state":
            cur = {}
            blocks.append(cur)
        elif cur is not None and line.startswith("  ") and " = " in line:
            k, v = line.strip().split(" = ", 1)
            cur[k.split("[")[0]] = v
        else:
            cur = None
    return blocks


def frame_rows(transcript):
    rows, inside = [], False
    for line in transcript.splitlines():
        if line == "@ frame":
            inside = True
            continue
        if inside and len(line) > 2 and line[2] == "|":
            rows.append(line[3:])
        elif inside:
            break
    return rows


failures = 0


def check(label, got, want):
    global failures
    if got == want:
        print(f"PASS {label}")
    else:
        failures += 1
        print(f"FAIL {label}\n  glyph:  {got}\n  python: {want}")


# 1. busy beavers
script = "".join(f"action {act}\naction dorun\nstate\n" for act, _ in BUSY_BEAVERS.values())
blocks = state_blocks(replay("examples/turing.glyph", script))
for (name, (_, machine)), got in zip(BUSY_BEAVERS.items(), blocks):
    want = simulate(machine)
    for key in ("STEPS", "ONES", "HEAD"):
        check(f"{name} {key.lower()}", int(got[key]), want[key])
    check(f"{name} tape", [int(x) for x in got["TAPE"].split()], want["TAPE"])

# 2. Rule 110
check("rule110 24 generations x 80 cells", frame_rows(replay("examples/rule110.glyph", "frame\n")), rule110())


# 3. recursion and arrays (tests/corpus/tc_procedures.glyph, tc_sieve.glyph)
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def fact(n):
    return 1 if n == 0 else n * fact(n - 1)


def hanoi(n):
    return 0 if n == 0 else 2 * hanoi(n - 1) + 1


def ack(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ack(m - 1, 1)
    return ack(m - 1, ack(m, n - 1))


def gcd(a, b):
    while b:
        a, b = b, a % b
    return a


got = state_blocks(replay("tests/corpus/tc_procedures.glyph", "state\n"))[0]
check("fib 18", int(got["FIB18"]), fib(18))
check("fact 12", int(got["FACT12"]), fact(12))
check("hanoi 9 moves", int(got["HANOI9"]), hanoi(9))
check("ackermann 2 3", int(got["ACK23"]), ack(2, 3))
check("gcd 1071 462", int(got["GCD"]), gcd(1071, 462))

primes = [p for p in range(2, 201) if all(p % q for q in range(2, p))]
got = state_blocks(replay("tests/corpus/tc_sieve.glyph", "state\n"))[0]
check("sieve primes below 201: count", int(got["COUNT"]), len(primes))
check("sieve primes below 201: list", [int(x) for x in got["PRIMES"].split()][:len(primes)], primes)

print(f"{failures} failed")
sys.exit(1 if failures else 0)
