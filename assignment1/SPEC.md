# PipeSim Assignment Contract

You should  implement a single-issue, five-stage, in-order processor simulator.
The framework fixes only the toy ISA, program format, command/output format,
clock boundary, and a coarse structural builder.

## Files

The complete starter has six files:

- `pipesim.h`: the framework API and toy ISA types.
- `framework.cc`: parser, command loop, builder, validation, and clock engine.
- `design.cc`: the student processor design.
- `example.pisa`: one example program.
- `Makefile`: builds `./pipesim`.
- `SPEC.md`: this specification.

You may keep all your work in `design.cc`. You may add files if you 
want, but no extra files are required.

## Build and run

```bash
make
./pipesim example.pisa
```

The simulator reads commands from standard input. It does not print a prompt.

```text
graph
n 1
pipe
n 100
p
s
q
```

## Commands

- `n`: advance one cycle.
- `n N`: advance up to `N` cycles, stopping early after HALT retires.
- `p`: print registers, non-zero memory words, and halt status.
- `pipe`: print the five stage snapshots after the last cycle.
- `graph`: print the registered datapath/control graph and unit-use counts.
- `s`: print cycle, retirement, and stall statistics.
- `reset`: restore the initial program state.
- `q`: exit.

## Toy ISA

There are eight 32-bit registers, `x0` through `x7`.

- `x0` always reads zero.
- Writes to `x0` are ignored.
- Arithmetic wraps modulo 2^32.
- Instructions are four bytes.
- `LW` and `SW` access aligned 32-bit words.
- Branch and jump immediates are absolute byte addresses.

Supported opcodes:

```text
ADD SUB AND OR XOR ADDI LW SW BEQ BNE J HALT
```

Every instruction record has the same form:

```text
INST <pc> <opcode> <rd> <rs1> <rs2> <immediate>
```

Unused fields are zero. Examples:

```text
INST 0x00 ADD  3 1 2 0
INST 0x04 ADDI 3 1 0 10
INST 0x08 LW   3 1 0 8
INST 0x0c SW   0 1 3 8
INST 0x10 BEQ  0 1 2 0x20
INST 0x14 J    0 0 0 0x40
INST 0x18 HALT 0 0 0 0
```

## Pipeline timing

The required machine is:

- Five stages: IF, ID, EX, MEM, WB.
- Single issue and in order.
- No forwarding.
- WB writes before ID reads in the same cycle.
- Data hazards stall the instruction in ID.
- A RAW stall holds PC and IF/ID and inserts a bubble into ID/EX.
- Older instructions continue through EX, MEM, and WB.
- Control instructions are recognized in ID and resolve in EX.
- Fetch stops behind an unresolved control instruction.
- There is no branch prediction and no wrong-path execution.
- HALT stops fetch and halts only when it retires in WB.

All stages read current pipeline state. They write next pipeline state. The
framework calls every registered stage and then commits every registered
clocked state simultaneously.

The framework evaluates stages in this order so that WB-before-ID timing is
well defined:

```text
WB, MEM, EX, ID, IF, then commit all clocked state
```

## Required builder roles

Students write their own classes. The framework does not prescribe class names,
fields, helper methods, or file layout. `BuildDesign()` must register the actual
objects used by the simulator.

Required stages:

```text
IF ID EX MEM WB
```

Required clocked state:

```text
PC IF/ID ID/EX EX/MEM MEM/WB
```

Required functional roles:

```text
ALU MAIN_CONTROL HAZARD_CONTROL
```

Required coarse connections:

```text
PC -> IF -> IF/ID -> ID -> ID/EX -> EX -> EX/MEM
   -> MEM -> MEM/WB -> WB

ALU -> EX or EX/MEM
MAIN_CONTROL -> ID or ID/EX
HAZARD_CONTROL -> PC       (STALL)
HAZARD_CONTROL -> IF/ID    (STALL)
HAZARD_CONTROL -> ID/EX    (BUBBLE)
EX -> PC                   (REDIRECT)
```

You may add any number of custom units, registers, and connections.
Control may be centralized or distributed internally. The builder checks only
this coarse shape.

## What is verified automatically

The grader can verify:

1. Architectural results: registers, memory, and HALT.
2. Exact cycle behavior: stage occupancy, stalls, bubbles, and redirects.
3. Structural roles and connections registered with the builder.
4. Runtime activity counts for registered ALU and control units.

The structural builder is evidence, not a mathematical proof. A team could
register decorative objects and bypass them. Therefore, design quality should
also be checked using a short datapath/control diagram, code review, or viva.
Static checks for class names are intentionally avoided because they are easy
to game and unnecessarily restrictive.

## Worked two-component example

`tutorial.cc` is a complete, unrelated example of the design style expected in
`design.cc`. Build and run it with:

```bash
make tutorial
./tutorial
```

Its datapath is:

```text
Counter -> ValueRegister -> Accumulator
```

Its control path is:

```text
Controller -> Counter
Controller -> ValueRegister
Controller -> Accumulator
```

The example demonstrates four ideas:

1. Components communicate through explicit state.
2. Components read current state and produce next state.
3. Clocked state commits only after evaluation is complete.
4. A builder declares and checks the coarse datapath/control graph.

The example is illustrative, not a required class layout. Students may organize
their processor differently as long as the real processor objects are registered
with `ProcessorBuilder` and satisfy the specification.
