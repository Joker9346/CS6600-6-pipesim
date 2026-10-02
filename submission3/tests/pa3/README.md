# PA3 predictor tests

Run these from the assignment starter root:

```bash
sh tests/pa3/run_predictor_tests.sh
python3 tests/pa3/verify.py
```

`run_predictor_tests.sh` tests TAGE history patterns, direct-indexed loop trip-count learning in both branch polarities, Multi-GEHL SC table training and reset, and SC enabled/disabled behavior. `verify.py` runs every `.pisa` workload under both processor configurations and compares final architectural state and branch statistics against its non-speculative reference model. The program workloads include loop prediction, SC-biased outcomes, wrong-path recovery, BTB aliasing, and memory operations.
