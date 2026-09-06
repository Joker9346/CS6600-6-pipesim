# CS6600 · Assignment 2 — pipesim (student starter)

Multi-cycle execution + a pipelined, non-blocking Harvard cache hierarchy on top
of the Assignment 1 pipesim. This README tells you **how to build, run, and
test** the starter. For the full architectural spec and the timing / ownership
rules, read `main.pdf` (the assignment PDF you received on Moodle).

> **Please read `ACADEMIC_INTEGRITY.md` before you touch anything.** Uploading
> this starter (or your in-progress solution) to any AI assistant is treated
> as plagiarism under the CS6600 policy. See that file for details.

---

## 1. What you build

Seven in-order-issue / out-of-order-complete execution pipelines (ADD, SUB,
AND, OR, XOR, CONTROL, AGU), a completion / retirement split, and split
L1I/L2I + L1D/L2D caches with MSHRs, writeback merging, and Tree-PLRU /
SRRIP replacement.

The framework code is done. You fill in the `TODO(A2-…)` markers.

## 2. Requirements

- A Linux/macOS shell (WSL works)
- `g++` with C++17 support (GCC 9+ / Clang 10+)
- `make`, `bash`, `sed`, `grep` (standard on any Linux)

No external libraries. The whole thing builds from source in one command.

## 3. Build

```sh
make            # produces ./pipesim
make clean      # removes ./pipesim and ./tests/unit_tests
```

If `make` fails with template/impl errors, that just means one of your TODOs
still throws — the build itself succeeds even before you write any code, as
long as your changes compile.

## 4. Run a program

```
./pipesim <program.pisa> <cache_config.json>
```

Then use the interactive REPL commands (one per line, then Enter):

| Cmd | What it does |
|---|---|
| `n [k]`   | run up to `k` cycles (default 1); stops early on HALT |
| `p`       | print machine state (registers + memory + status) |
| `pipe`    | print the pipeline snapshot |
| `cache`   | print all cache state (lines, MSHRs, writebacks) |
| `graph`   | print the processor's structural graph |
| `s`       | print statistics / counters |
| `reset`   | reset the processor to boundary 0 |
| `q`       | quit |

Example — run `example.pisa` for up to 1000 cycles then print state:

```sh
printf 'n 1000\np\nq\n' | ./pipesim example.pisa default_config.json
```

## 5. Provided programs and configs

Programs (`.pisa`):

- `example.pisa` — 10-instruction sanity program (used by two tests)
- `tests/eviction.pisa` — forces dirty L1D evictions
- `tests/execution_hazards.pisa` — exercises WAW / unit-busy / completion-slot stalls
- `tests/one_hundred.pisa` — 100-instruction retirement check

Configs (`.json`):

- `default_config.json` — latency-0 units, always-hit-style bring-up
- `multicycle_config.json` — real multi-cycle latencies (see PDF §1.1 bring-up sequence)
- `tests/unequal_config.json` — L1/L2 line sizes differ
- `tests/execution_config.json` — for the execution-hazards test
- `tests/invalid_unknown_key.json` — negative test (must be **rejected**)

## 6. Test the full assignment

The one-shot integration harness runs the 8 unit tests **and** the six
end-to-end scenarios and prints `integration tests: PASS` on success:

```sh
make test
# equivalent to:
#   make && ./tests/run_tests.sh
```

Individual scenarios you can eyeball while debugging:

```sh
# 1. Sanity: example program on the bring-up config, expect CYCLES 71
printf 'n 1000\np\nq\n' | ./pipesim example.pisa default_config.json

# 2. Dirty evictions: expect L1D.dirty_evictions >= 1 and 4 mem lines written
printf 'n 2000\np\ns\nq\n' | ./pipesim tests/eviction.pisa default_config.json

# 3. Unequal L1/L2 line sizes on the same program
printf 'n 1000\np\nq\n' | ./pipesim example.pisa tests/unequal_config.json

# 4. Execution hazards: expect x1=7, x5=0, WAW / busy / slot stalls > 0
printf 'n 1000\np\ns\nq\n' | \
  ./pipesim tests/execution_hazards.pisa tests/execution_config.json

# 5. Real multi-cycle timing: 100 instructions must all retire, x1=33
printf 'n 10000\np\ns\nq\n' | \
  ./pipesim tests/one_hundred.pisa multicycle_config.json

# 6. Reset determinism: two identical runs after a reset must match
printf 'n 1000\ns\nreset\nn 1000\ns\nq\n' | \
  ./pipesim example.pisa multicycle_config.json

# 7. Invalid config must be REJECTED
./pipesim example.pisa tests/invalid_unknown_key.json      # must exit non-zero
```

## 7. TODO tracking
A helper script counts
what's left:

```sh
./check_todos.sh          # summary + remaining IDs + exit code 0 iff done
./check_todos.sh --list   # also lists every remaining marker with file:line
```

## 8. Submission

Follow the PDF exactly: place this entire folder inside your 
`pipesim` repository under a new top-level directory literally named
**`asignment 2`** (all lowercase, `asignment` with one `s` — the PDF
confirms this spelling is intentional). Do not touch `assignment 1`.

Include your `report.pdf` at the top of `asignment 2/`.

---

*This README was produced to help you run the starter. The **architectural
spec** — timing, ownership, arbitration, replacement details — is still the
assignment PDF. Read it.*
