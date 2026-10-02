// Direct tests of PredictorOne (TAGE + Statistical Corrector), no pipeline involved.
// Build/run through tests/pa3/run_predictor_tests.sh
#include "predictors/predictor_one.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace pipesim;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("  FAIL: ");                            \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

// ---- helpers --------------------------------------------------------------
struct Ev { uint32_t pc; bool taken; };

// plain 2-bit bimodal reference: what a predictor WITHOUT history would do
struct Bimodal {
  std::vector<int> t = std::vector<int>(4096, 1);
  bool Predict(uint32_t pc) { return t[(pc >> 2) % t.size()] >= 2; }
  void Update(uint32_t pc, bool tk) {
    int &c = t[(pc >> 2) % t.size()];
    if (tk && c < 3) ++c; else if (!tk && c > 0) --c;
  }
};

struct Score { long n = 0, miss = 0; double rate() const { return n ? 100.0 * miss / n : 0; } };

// Runs a stream; scores only events for 'watch_pc' (or all if 0) in the 2nd half.
static Score RunPredictor(const std::vector<Ev> &s, uint32_t watch_pc, PredictorOne **keep = nullptr) {
  static PredictorOne *p = nullptr;
  delete p;
  p = new PredictorOne();
  if (keep) *keep = p;
  Score sc;
  for (size_t i = 0; i < s.size(); ++i) {
    BranchPrediction pr = p->Predict(s[i].pc);
    bool ptaken = pr.branch_found && pr.taken;
    if (i >= s.size() / 2 && (watch_pc == 0 || s[i].pc == watch_pc)) {
      ++sc.n;
      if (ptaken != s[i].taken) ++sc.miss;
    }
    BranchResult r;
    r.sequence = i; r.pc = s[i].pc; r.type = BranchType::kConditional;
    r.taken = s[i].taken; r.target = s[i].taken ? s[i].pc + 0x40 : s[i].pc + 4;
    p->Update(r, pr);
  }
  return sc;
}
static Score RunBimodal(const std::vector<Ev> &s, uint32_t watch_pc) {
  Bimodal b; Score sc;
  for (size_t i = 0; i < s.size(); ++i) {
    bool pt = b.Predict(s[i].pc);
    if (i >= s.size() / 2 && (watch_pc == 0 || s[i].pc == watch_pc)) { ++sc.n; if (pt != s[i].taken) ++sc.miss; }
    b.Update(s[i].pc, s[i].taken);
  }
  return sc;
}

static void Report(const char *name, const Score &tage, const Score &bim) {
  std::printf("  %-34s TAGE-SC miss %6.2f%%   bimodal miss %6.2f%%\n", name, tage.rate(), bim.rate());
}

// ---- TAGE tests -----------------------------------------------------------
static void TestTage() {
  std::printf("[TAGE] history-based prediction must beat a history-less bimodal\n");
  const uint32_t A = 0x100, B = 0x140;

  { // 1. alternating T,N,T,N
    std::vector<Ev> s;
    for (int i = 0; i < 4000; ++i) s.push_back({A, (i & 1) == 0});
    Score t = RunPredictor(s, A), b = RunBimodal(s, A);
    Report("alternating T/N", t, b);
    CHECK(t.rate() < 2.0, "alternating: TAGE miss %.2f%% should be < 2%%", t.rate());
    CHECK(b.rate() > 40.0, "alternating: bimodal sanity %.2f%%", b.rate());
  }
  { // 2. period-3 pattern T,T,N
    std::vector<Ev> s;
    for (int i = 0; i < 6000; ++i) s.push_back({A, (i % 3) != 2});
    Score t = RunPredictor(s, A), b = RunBimodal(s, A);
    Report("period-3 (T,T,N)", t, b);
    CHECK(t.rate() < 2.0, "period-3: TAGE miss %.2f%% should be < 2%%", t.rate());
    CHECK(b.rate() > 25.0, "period-3: bimodal sanity %.2f%%", b.rate());
  }
  { // 3. loop exit: taken 7 times, then not taken (trip count 8)
    std::vector<Ev> s;
    for (int i = 0; i < 8000; ++i) s.push_back({A, (i % 8) != 7});
    Score t = RunPredictor(s, A), b = RunBimodal(s, A);
    Report("loop exit, trip 8", t, b);
    CHECK(t.rate() < 2.0, "loop trip 8: TAGE miss %.2f%% should be < 2%%", t.rate());
    CHECK(b.rate() > 10.0, "loop trip 8: bimodal sanity %.2f%%", b.rate());
  }
  { // 4. correlation: B repeats the random outcome of the branch A just before it
    std::mt19937 g(1);
    std::vector<Ev> s;
    for (int i = 0; i < 20000; ++i) { bool r = g() & 1; s.push_back({A, r}); s.push_back({B, r}); }
    Score t = RunPredictor(s, B), b = RunBimodal(s, B);
    Report("B copies random A (short hist)", t, b);
    CHECK(t.rate() < 5.0, "correlation: TAGE miss on B %.2f%% should be < 5%%", t.rate());
    CHECK(b.rate() > 40.0, "correlation: bimodal sanity %.2f%%", b.rate());
  }
  { // 5. long-history correlation: 30 always-taken filler branches between A and B
    std::mt19937 g(2);
    std::vector<Ev> s;
    for (int i = 0; i < 12000; ++i) {
      bool r = g() & 1;
      s.push_back({A, r});
      for (int k = 0; k < 30; ++k) s.push_back({0x200u + 4u * k, true});
      s.push_back({B, r});
    }
    Score t = RunPredictor(s, B), b = RunBimodal(s, B);
    Report("B copies A, 30 branches apart", t, b);
    CHECK(t.rate() < 10.0, "long-history: TAGE miss on B %.2f%% should be < 10%%", t.rate());
    CHECK(b.rate() > 40.0, "long-history: bimodal sanity %.2f%%", b.rate());
  }
  { // 6. biased random branch: TAGE must not do much worse than the best static guess
    std::mt19937 g(3);
    std::vector<Ev> s;
    for (int i = 0; i < 20000; ++i) s.push_back({A, (g() % 100) < 90});
    Score t = RunPredictor(s, A), b = RunBimodal(s, A);
    Report("90% taken random", t, b);
    CHECK(t.rate() < 16.0, "biased: TAGE miss %.2f%% should be < 16%%", t.rate());
  }
  { // 7. two branches with opposite behaviour must not interfere
    std::vector<Ev> s;
    for (int i = 0; i < 4000; ++i) { s.push_back({A, true}); s.push_back({B, false}); }
    Score t = RunPredictor(s, 0);
    std::printf("  %-34s TAGE-SC miss %6.2f%%\n", "always-T and always-N branches", t.rate());
    CHECK(t.rate() < 1.0, "static branches: miss %.2f%% should be < 1%%", t.rate());
  }
}

// ---- Statistical corrector unit tests (the class on its own) ---------------
static void TestScUnit() {
  std::printf("[SC] reduced Multi-GEHL table behavior\n");
  const uint32_t pc = 0x1234 & ~3u;
  const std::bitset<359> history{};
  { StatisticalCorrector sc;
    CHECK(sc.StorageBits() == 640 * (7 + 6), "storage bits %llu != 8320",
          (unsigned long long)sc.StorageBits());
    sc.Prepare(pc, false, history, 0);
    CHECK(!sc.ShouldInvert(pc, false, false, 0), "cold tables must not revert");
  }
  { // The component tables learn a consistently missed TAGE direction.
    StatisticalCorrector sc;
    int first_revert = -1;
    for (int i = 0; i < 40; ++i) {
      uint8_t id = static_cast<uint8_t>(i);
      sc.Prepare(pc, false, history, id);
      bool inv = sc.ShouldInvert(pc, false, false, id);
      if (inv && first_revert < 0) first_revert = i;
      sc.Update(pc, false, true, inv, id);
    }
    CHECK(first_revert > 0, "SC never started reverting a consistently wrong TAGE");
    sc.Prepare(pc, false, history, 40);
    CHECK(sc.ShouldInvert(pc, false, false, 40), "SC should revert after repeated TAGE misses");
    CHECK(!sc.ShouldInvert(pc, false, true, 40), "high-confidence TAGE must be retained");
    std::printf("  starts reverting after %d TAGE misses\n", first_revert);
  }
  { // Every component table is direct-indexed and verifies its tag.
    StatisticalCorrector sc;
    for (int i = 0; i < 40; ++i) {
      uint8_t id = static_cast<uint8_t>(i);
      sc.Prepare(pc, false, history, id);
      bool inv = sc.ShouldInvert(pc, false, false, id);
      sc.Update(pc, false, true, inv, id);
    }
    std::bitset<359> different_history;
    different_history.set(0);
    sc.Prepare(pc, true, different_history, 41);
    (void)sc.ShouldInvert(pc, true, false, 41);
    CHECK(sc.Stats().bias_reads >= 4 && sc.Stats().gehl_reads >= 8,
          "SC did not read both bias and GEHL tables");
    sc.Reset();
    CHECK(sc.Stats().lookups == 0 && sc.StorageBits() == 8320,
          "Reset must clear counters and preserve table configuration");
  }
}

// ---- SC inside the full predictor: data TAGE cannot learn -----------------
static void TestLoopPredictor() {
  std::printf("[Loop] learn trip count and both continuation directions\n");
  CHECK(LoopPredictor{}.StorageBits() == 16 * 62,
        "loop table storage should be 992 bits");

  auto train_and_check = [](uint32_t pc, bool continue_taken) {
    LoopPredictor loop;
    uint64_t seq = 0;
    for (int repetition = 0; repetition < 8; ++repetition) {
      for (int iteration = 0; iteration < 5; ++iteration) {
        bool outcome = (iteration < 4) ? continue_taken : !continue_taken;
        BranchResult result;
        result.sequence = seq++;
        result.pc = pc;
        result.type = BranchType::kConditional;
        result.taken = outcome;
        loop.Update(result, false, false);
      }
    }

    int selected = 0;
    int correct = 0;
    for (int iteration = 0; iteration < 5; ++iteration) {
      bool actual = (iteration < 4) ? continue_taken : !continue_taken;
      LoopPrediction prediction = loop.Predict(pc);
      if (prediction.confident) {
        ++selected;
        if (prediction.taken == actual) ++correct;
      }
      BranchResult result;
      result.sequence = seq++;
      result.pc = pc;
      result.type = BranchType::kConditional;
      result.taken = actual;
      loop.Update(result, prediction.confident, prediction.taken);
    }
    CHECK(selected >= 4, "loop predictor selected only %d of 5 predictions", selected);
    CHECK(correct == selected, "loop predictor got %d of %d selected predictions correct", correct, selected);
    CHECK(loop.Metrics().selected == static_cast<uint64_t>(selected),
          "loop selected-prediction metric does not match test observations");
    CHECK(loop.Metrics().correct == static_cast<uint64_t>(correct),
          "loop correct-prediction metric does not match test observations");
  };

  train_and_check(0x240, true);   // taken loop body, not-taken exit
  train_and_check(0x244, false);  // not-taken loop body, taken exit
}

static void TestScIntegrated(bool expect_help) {
  std::printf("[SC] inside PredictorOne on a 75%%-taken random branch\n");
  std::mt19937 g(7);
  std::vector<Ev> s;
  const uint32_t A = 0x100;
  for (int i = 0; i < 60000; ++i) s.push_back({A, (g() % 100) < 75});
  PredictorOne *p = nullptr;
  Score t = RunPredictor(s, A, &p);
  const ScStats &st = p->ScStatistics();
  std::printf("  miss %.2f%% | SC lookups=%llu hits=%llu reverts(resolved)=%llu correct=%llu wrong=%llu allocations=%llu\n",
              t.rate(), (unsigned long long)st.lookups, (unsigned long long)st.hits,
              (unsigned long long)st.resolved_inverts, (unsigned long long)st.inverts_correct,
              (unsigned long long)st.inverts_wrong, (unsigned long long)st.allocations);
  std::printf("  SC_MISS_RATE %.4f\n", t.rate());          // consumed by run_predictor_tests.sh
  CHECK(st.lookups > 0 && st.bias_reads > 0 && st.gehl_reads > 0, "SC was never consulted");
  CHECK(st.resolved_inverts == st.inverts_correct + st.inverts_wrong, "SC revert accounting inconsistent");
  if (expect_help) {
    CHECK(st.resolved_inverts > 0, "SC never reverted TAGE on a branch TAGE cannot learn");
    CHECK(st.inverts_correct > st.inverts_wrong, "SC overrides were not net-positive (%llu good vs %llu bad)",
          (unsigned long long)st.inverts_correct, (unsigned long long)st.inverts_wrong);
    CHECK(t.rate() < 30.0, "SC+TAGE miss %.2f%% too high for a 75%% biased branch", t.rate());
  } else {
    CHECK(st.inverts == 0, "SC is switched off but still reverted predictions");
  }
}

int main(int argc, char **argv) {
  bool sc_off = (argc > 1 && std::atoi(argv[1]) == 0);
  TestLoopPredictor();
  if (!sc_off) { TestTage(); TestScUnit(); }
  TestScIntegrated(!sc_off);
  std::printf(g_fail ? "RESULT: %d check(s) FAILED\n" : "RESULT: all checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
