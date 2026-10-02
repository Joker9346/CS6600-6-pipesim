#!/usr/bin/env python3
"""Generates the PA3 verification programs (*.pisa) in this directory."""
import os
HERE = os.path.dirname(os.path.abspath(__file__))

def emit(name, items):
    """items: list of (addr, text). Writes a .pisa file."""
    lines = ["PIPEISA 1", "ENTRY 0x00000000", ""]
    for a, t in items:
        lines.append("INST 0x%08x %s" % (a, t))
    lines += ["", "END"]
    open(os.path.join(HERE, name + ".pisa"), "w", newline="").write("\n".join(lines) + "\n")

def seq(code, base=0):
    return [(base + 4 * i, c) for i, c in enumerate(code)]

# 1. alternating T/NT branch: direction errors while TAGE warms up, then learned
emit("alt_pattern", seq([
    "ADDI 1 0 0 60",            # 00 trip count
    "ADDI 2 0 0 0",             # 04 toggle
    "ADDI 3 0 0 0",             # 08 count of 'taken'
    "ADDI 4 0 0 1",             # 0c const 1
    "XOR  2 2 4 0",             # 10 toggle ^= 1
    "BEQ  0 2 4 0x00000020",    # 14 taken when toggle==1
    "J    0 0 0 0x00000024",    # 18 not-taken path
    "ADDI 7 7 0 100",           # 1c (never executed) padding
    "ADDI 3 3 0 1",             # 20 taken path: count++
    "ADDI 1 1 0 -1",            # 24
    "BNE  0 1 0 0x00000010",    # 28 loop
    "SW   0 0 3 0",             # 2c mem[0] = count
    "HALT 0 0 0 0",             # 30
]))

# 2. nested loops, inner trip count changes every outer iteration (1,2,..,9)
emit("nested_changing", seq([
    "ADDI 1 0 0 0",             # 00 outer i
    "ADDI 5 0 0 9",             # 04 outer limit
    "ADDI 3 0 0 0",             # 08 total
    "ADDI 1 1 0 1",             # 0c i++
    "ADDI 2 0 0 0",             # 10 j = 0
    "ADDI 3 3 0 1",             # 14 total++
    "ADDI 2 2 0 1",             # 18 j++
    "BNE  0 2 1 0x00000014",    # 1c while j != i
    "BNE  0 1 5 0x0000000c",    # 20 while i != 9
    "SW   0 0 3 8",             # 24 mem[8] = total (=45)
    "HALT 0 0 0 0",             # 28
]))

# 3. wrong-path HALT / ALU / store behind a cold, taken branch
emit("wrongpath_halt", seq([
    "ADDI 1 0 0 1",             # 00
    "ADDI 2 0 0 1",             # 04
    "BEQ  0 1 2 0x00000024",    # 08 taken (cold BTB predicts not-taken)
    "ADDI 5 0 0 99",            # 0c wrong path ALU
    "ADDI 6 0 0 77",            # 10 wrong path ALU
    "SW   0 0 5 0",             # 14 wrong path store (must never be visible)
    "ADDI 7 0 0 55",            # 18
    "HALT 0 0 0 0",             # 1c wrong path HALT (must not terminate)
    "HALT 0 0 0 0",             # 20
    "ADDI 3 0 0 5",             # 24 correct path
    "SW   0 0 3 4",             # 28 mem[4] = 5
    "HALT 0 0 0 0",             # 2c
]))

# 4. branch resolves late (long dependent chain); younger ALU ops finish first.
#    Loops 24 times, branch taken only every 3rd iteration (mix of hits/misses).
emit("wrongpath_late", seq([
    "ADDI 1 0 0 24",            # 00 trip
    "ADDI 4 0 0 0",             # 04 phase
    "ADDI 3 0 0 0",             # 08 result
    "ADDI 4 4 0 1",             # 0c phase++
    "ADDI 5 0 0 3",             # 10
    "SUB  6 4 5 0",             # 14 phase-3
    "BNE  0 6 0 0x00000030",    # 18 skip reset unless phase==3
    "ADDI 4 0 0 0",             # 1c phase=0
    "ADDI 3 3 0 10",            # 20 result += 10
    "J    0 0 0 0x00000034",    # 24
    "ADDI 7 7 0 1",             # 28 (never executed)
    "ADDI 7 7 0 1",             # 2c (never executed)
    "ADDI 3 3 0 1",             # 30 result += 1 (BNE-taken path)
    "ADDI 1 1 0 -1",            # 34
    "BNE  0 1 0 0x0000000c",    # 38
    "SW   0 0 3 12",            # 3c
    "HALT 0 0 0 0",             # 40
]))

# 5. BTB conflict: branches at 0x10 and 0x810 share a BTB index (512 entries)
code = [(0x00, "ADDI 1 0 0 30"), (0x04, "ADDI 3 0 0 0"), (0x08, "ADDI 4 0 0 0"),
        (0x0c, "ADDI 3 3 0 1"),
        (0x10, "BNE  0 3 0 0x00000014"),      # always taken, target = next
        (0x14, "J    0 0 0 0x00000810"),
        (0x810, "BNE  0 3 0 0x00000814"),     # aliases with 0x10
        (0x814, "ADDI 4 4 0 2"),
        (0x818, "ADDI 1 1 0 -1"),
        (0x81c, "BNE  0 1 0 0x0000000c"),
        (0x820, "SW   0 0 4 16"),
        (0x824, "HALT 0 0 0 0")]
# gap 0x18..0x80c is only reachable on the wrong path; the simulator rejects
# fetched words that are not instructions, so fill it with harmless no-ops.
code += [(a, "ADDI 7 7 0 0") for a in range(0x18, 0x810, 4)]
emit("btb_alias", sorted(code))

# 6. memory + branches: fill array, then conditionally sum
emit("array_sum", seq([
    "ADDI 1 0 0 0",             # 00 addr
    "ADDI 2 0 0 0",             # 04 val
    "ADDI 5 0 0 64",            # 08 limit
    "ADDI 6 0 0 7",             # 0c mask
    "SW   0 1 2 0",             # 10 mem[addr]=val
    "ADDI 2 2 0 3",             # 14
    "AND  2 2 6 0",             # 18
    "ADDI 1 1 0 4",             # 1c
    "BNE  0 1 5 0x00000010",    # 20
    "ADDI 1 0 0 0",             # 24
    "ADDI 3 0 0 0",             # 28
    "LW   2 1 0 0",             # 2c
    "ADDI 4 0 0 4",             # 30
    "BEQ  0 2 4 0x0000003c",    # 34 skip when val==4
    "ADD  3 3 2 0",             # 38
    "ADDI 1 1 0 4",             # 3c
    "BNE  0 1 5 0x0000002c",    # 40
    "SW   0 0 3 100",           # 44
    "HALT 0 0 0 0",             # 48
]))

# 7. constant long loop (trip 50) repeated 6 times: loop-predictor style
emit("long_loop", seq([
    "ADDI 5 0 0 6",             # 00 outer
    "ADDI 3 0 0 0",             # 04
    "ADDI 1 0 0 50",            # 08 inner
    "ADDI 3 3 0 1",             # 0c
    "ADDI 1 1 0 -1",            # 10
    "BNE  0 1 0 0x0000000c",    # 14 inner loop branch
    "ADDI 5 5 0 -1",            # 18
    "BNE  0 5 0 0x00000008",    # 1c outer
    "SW   0 0 3 20",            # 20 mem[20] = 300
    "HALT 0 0 0 0",             # 24
]))

# 8. unconditional jumps only, forward and backward
emit("jump_chain", seq([
    "ADDI 1 0 0 3",             # 00
    "J    0 0 0 0x00000010",    # 04 -> 10
    "ADDI 7 0 0 9",             # 08 skipped
    "HALT 0 0 0 0",             # 0c skipped
    "ADDI 2 2 0 5",             # 10
    "ADDI 1 1 0 -1",            # 14
    "BEQ  0 1 0 0x00000020",    # 18 exit when 0
    "J    0 0 0 0x00000010",    # 1c backward
    "SW   0 0 2 24",            # 20 mem[24] = 15
    "HALT 0 0 0 0",             # 24
]))
print("generated")
