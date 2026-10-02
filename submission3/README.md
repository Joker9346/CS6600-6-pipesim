# CS6600 Programming Assignment 3

This assignment extends your correct PA2 processor with speculative branch prediction.

Copy your PA2 implementations into this tree before starting PA3. Do not change the instruction set, cache behavior, execution-unit timing, current/next-state convention, or in-order retirement rule.

## Team work

The team must implement the common speculative framework and one predictor, reproducing TAGE-SC-L, built jointly by all three students. A natural split of the predictor across three people is the TAGE core (bimodal base, tagged tables, folded history, allocation/aging), the statistical corrector, and the loop predictor plus final integration — but the team may split it differently as long as the whole design is implemented.

Only slot 0 is graded. Slots 1 and 2 are fixed, ungraded fillers (`predictors/predictor_two.cc`, `predictors/predictor_three.cc`) that exist only because `BranchPredictionSystem` calls every slot as a shadow predictor each branch (see Fetch behavior, below). Do not modify slots 1 or 2.

```text
./pipesim program.pisa default_config.json 0
```

## Required PA3 work

| Identifier | Required implementation |
|---|---|
| PA3-FRAMEWORK-01 | Reset and fetch-capacity handling |
| PA3-FRAMEWORK-02 | Safe speculative dispatch |
| PA3-FRAMEWORK-03 | Branch checkpoints and speculation depth |
| PA3-FRAMEWORK-04 | Resolution and correct-PC recovery decision |
| PA3-FRAMEWORK-05 | Removal of younger checkpoints and pipeline work |
| PA3-PREDICTOR-ONE | TAGE-SC-L, slot 0 |

The PA3 work must also be connected inside `Core::Fetch`, `Core::Decode`, `Core::Execute`, and `BuildDesign`.

## Statistical corrector variant in this submission

PredictorOne uses a reduced Multi-GEHL statistical corrector based on the 256 Kbit design in Seznec (2014, Sec. 5.2). It has two bias tables (128 entries each) and four global-history GEHL tables (64, 64, 128, and 128 entries), with history lengths 4, 16, 64, and 256. Each table is direct-indexed, stores a 7-bit partial tag, and uses a signed 6-bit counter. The six counter outputs are summed with a TAGE-direction bias term; the SC may override TAGE only when TAGE is not high-confidence.

This is a reduced adaptation, not the paper's full 46,809-bit SC. It omits return-stack and local-history components to keep the implementation and storage smaller, and uses a fixed training threshold of 8 instead of the paper's dynamic threshold policy to keep training behavior simple and reproducible. These reductions can lower accuracy and change convergence compared with the full design. The component table storage is 640 entries × 13 bits = 8,320 bits. SC table reads and writes are reported separately for the bias and GEHL groups. Per-branch lookup snapshots retain 40 index bits, 42 tag bits, a 9-bit score, and two flags per checkpoint slot: 23,808 logical bits across 256 slots. Report this metadata separately from the 8,320 component-table bits.

## Direct-indexed loop predictor

PredictorOne includes a 16-entry direct-mapped loop table. Bits PC[5:2] select one entry, and PC[31:6] are checked as its tag. A tag conflict replaces that entry; lookup never searches another entry. Each entry stores a 16-bit current iteration count, a 16-bit learned trip count, 2 confidence bits, a continuation-direction bit, and a valid bit in addition to the 26-bit tag, for 992 logical table bits.

The predictor learns the continuing direction and trip count from completed loops. It predicts the continuing direction until the learned trip-count boundary, then predicts the exit direction. It supports either branch polarity and overrides the TAGE-SC result only when the entry matches and confidence is at least 2. `loop.lookups` and `loop.table_reads` count conditional BTB-hit predictions. `loop.selected` and `loop.correct` count resolved branches for which a saved loop prediction was used and whether it matched. `loop.table_writes` counts conditional branch updates; `loop.replacements` counts direct-map tag conflicts. These metrics are tracked inside the predictor.

## Fetch behavior

At a fetch PC, call all three predictors through `BranchPredictionSystem::Predict`. Pass the returned `BranchPredictionSet` to `Machine::TryIssuePredictedFetch`. Set the next PC from `BranchPredictionSet::next_pc`.

The prediction set travels with the fetched instruction and its execution operation. It must not be recomputed when the branch resolves.

Decode must call `BranchPredictionSystem::ObserveInstruction`. If it returns true, the selected predictor incorrectly identified a normal instruction as a branch. Redirect to `pc + 4` and squash younger work immediately.

## Dispatch behavior

Every decoded control instruction creates a checkpoint before it is dispatched. Younger arithmetic instructions may execute speculatively. A younger load, store, control instruction, or halt instruction must not dispatch while an older branch remains unresolved.

## Resolution behavior

The control execution unit computes the actual branch direction and actual target. The core creates a `BranchResult` and gives it to both `SpeculationController::Resolve` and `BranchPredictionSystem::Resolve`.

On a misprediction:

1. Set the next PC to the actual target when taken, otherwise `pc + 4`.
2. Call `Machine::SquashAfter`.
3. Call `ExecutionPipeline::SquashAfter` for all seven execution pipelines.
4. Remove younger checkpoints.
5. Clear a younger IF/ID instruction.
6. Record every removed item with `BranchPredictionSystem::RecordSquash`.

No instruction younger than a mispredicted branch may retire. No wrong-path data request may reach L1D.

## Predictor contract

This contract applies to the graded predictor (slot 0, `predictors/predictor_one.cc`). Slots 1 and 2 already satisfy it with fixed, ungraded filler implementations.

Each predictor must implement:

```cpp
std::string Name() const;
void Reset();
BranchPrediction Predict(std::uint32_t pc);
void ObserveInstruction(const Instruction& instruction, const BranchPrediction& prediction);
void Update(const BranchResult& result, const BranchPrediction& prediction);
std::uint64_t StorageBits() const;
const PredictorMetrics& Metrics() const;
```

`Predict` must update lookup and predicted-direction metrics. `ObserveInstruction` handles false branch identification after instruction fetch. `Update` must update result metrics exactly once. `StorageBits` must report all state used to make predictions, including tags, targets, counters, history, validity, useful bits, weights, and chooser state.

## Required statistics

The `s` command prints framework, cache, speculation, and per-predictor statistics. The following identities must hold:

```text
branch.branches_resolved = branch.correct_predictions + branch.direction_mispredictions + branch.target_mispredictions
branch.branches_resolved = branch.conditional_branches + branch.unconditional_branches
branch.branches_resolved = branch.taken_branches + branch.not_taken_branches
predictorN.lookups = predictorN.branches_found + predictorN.branches_not_found
predictorN.updates = branch.branches_resolved
predictorN.updates = predictorN.correct_predictions + predictorN.direction_mispredictions + predictorN.target_mispredictions
```

False branch predictions and missed branches are classifications of prediction failures. They are not added again when checking the total number of updates.

## Correctness

The final registers, data memory, PC, halt state, and retired instruction behavior must match the non-speculative reference processor. Runtime checks reject a speculative data request that reaches the cache before its controlling branch resolves.

## Submission

Submit the complete source tree and a document mapping each student to their component of the TAGE-SC-L implementation (for example: TAGE core, statistical corrector, loop predictor plus integration) and any shared-framework responsibility they took on.
