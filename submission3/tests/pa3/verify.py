#!/usr/bin/env python3
"""PA3 verification: runs every tests/pa3/*.pisa on pipesim and compares the result
against an independent, non-speculative reference interpreter.

Usage (from the starter root):   python3 tests/pa3/verify.py
Checks per program x config: halt status, x1..x7, data memory, retired count,
resolved/taken/not-taken/conditional/unconditional branch counts, and the README
statistic identities (via tests/check_branch_metrics.py).
"""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "tests", "pa3")
CONFIGS = ["default_config.json", "multicycle_config.json"]
M32 = 0xFFFFFFFF

def load(path):
    prog = {}
    for line in open(path):
        f = line.split()
        if len(f) >= 7 and f[0] == "INST":
            prog[int(f[1], 0)] = (f[2], int(f[3]), int(f[4]), int(f[5]), int(f[6], 0))
    return prog

def reference(prog):
    r = [0] * 8
    mem = {}
    pc, retired = 0, 0
    st = dict(resolved=0, taken=0, nottaken=0, cond=0, uncond=0)
    def rd(i): return 0 if i == 0 else r[i]
    def wr(i, v):
        if i != 0: r[i] = v & M32
    for _ in range(5_000_000):
        op, a, b, c, imm = prog[pc]
        retired += 1
        nxt = pc + 4
        if op == "HALT":
            break
        elif op == "ADD":  wr(a, rd(b) + rd(c))
        elif op == "SUB":  wr(a, rd(b) - rd(c))
        elif op == "AND":  wr(a, rd(b) & rd(c))
        elif op == "OR":   wr(a, rd(b) | rd(c))
        elif op == "XOR":  wr(a, rd(b) ^ rd(c))
        elif op == "ADDI": wr(a, rd(b) + imm)
        elif op == "LW":   wr(a, mem.get((rd(b) + imm) & M32, 0))
        elif op == "SW":   mem[(rd(b) + imm) & M32] = rd(c)
        elif op in ("BEQ", "BNE"):
            taken = (rd(b) == rd(c)) if op == "BEQ" else (rd(b) != rd(c))
            st["resolved"] += 1; st["cond"] += 1
            st["taken" if taken else "nottaken"] += 1
            if taken: nxt = imm
        elif op == "J":
            st["resolved"] += 1; st["uncond"] += 1; st["taken"] += 1
            nxt = imm
        else:
            raise RuntimeError("unknown op " + op)
        pc = nxt
    else:
        raise RuntimeError("reference did not halt")
    return r, {k: v for k, v in mem.items() if v != 0}, retired, st

def run(prog_path, config):
    cmd = [os.path.join(ROOT, "pipesim"), prog_path, os.path.join(ROOT, config), "0"]
    out = subprocess.run(cmd, input="n 3000000\np\ns\nq\n", capture_output=True,
                         text=True, cwd=ROOT, timeout=120).stdout
    regs, mem, stats, status = {}, {}, {}, None
    in_state = False
    for line in out.splitlines():
        f = line.split()
        if line == "BEGIN STATE": in_state = True; continue
        if line == "END STATE": in_state = False; continue
        if in_state and f and f[0].startswith("x") and len(f) == 2:
            regs[int(f[0][1:])] = int(f[1])
        elif in_state and f and f[0] == "mem":
            if int(f[2]) != 0: mem[int(f[1], 0)] = int(f[2])
        elif in_state and f and f[0] == "status":
            status = f[1]
        elif len(f) == 2:
            try: stats[f[0]] = int(f[1])
            except ValueError: pass
    return out, regs, mem, stats, status

def main():
    failures, total = 0, 0
    for path in sorted(glob.glob(os.path.join(HERE, "*.pisa"))):
        name = os.path.basename(path)
        r, mem, retired, st = reference(load(path))
        for cfg in CONFIGS:
            total += 1
            out, regs, pmem, stats, status = run(path, cfg)
            errs = []
            if status != "HALTED": errs.append("status %s" % status)
            for i in range(1, 8):
                if regs.get(i) != r[i]: errs.append("x%d got %s exp %d" % (i, regs.get(i), r[i]))
            if pmem != mem: errs.append("mem got %s exp %s" % (pmem, mem))
            if stats.get("retired") != retired: errs.append("retired got %s exp %d" % (stats.get("retired"), retired))
            for key, exp in (("branch.branches_resolved", st["resolved"]), ("branch.taken_branches", st["taken"]),
                             ("branch.not_taken_branches", st["nottaken"]), ("branch.conditional_branches", st["cond"]),
                             ("branch.unconditional_branches", st["uncond"])):
                if stats.get(key) != exp: errs.append("%s got %s exp %d" % (key, stats.get(key), exp))
            tmp = "/tmp/pa3_stats.txt"
            open(tmp, "w").write(out)
            chk = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "check_branch_metrics.py"), tmp],
                                 capture_output=True, text=True)
            if chk.returncode != 0: errs.append("metric identity: " + (chk.stderr.strip().splitlines() or ["?"])[-1])
            cyc, mp = stats.get("cycles"), stats.get("predictor0.direction_mispredictions")
            ipc = retired / cyc if cyc else 0
            mpki = 1000.0 * (stats.get("predictor0.direction_mispredictions", 0) + stats.get("predictor0.target_mispredictions", 0)) / retired
            tag = "PASS" if not errs else "FAIL"
            print("%-4s %-22s %-22s cycles=%-7s retired=%-6d IPC=%.3f MPKI=%.1f dirMiss=%s" %
                  (tag, name, cfg, cyc, retired, ipc, mpki, mp))
            for e in errs: print("       -", e)
            failures += bool(errs)
    print("\n%d/%d passed" % (total - failures, total))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
