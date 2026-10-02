#pragma once

#include "branch_prediction.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace pipesim {

struct BTBEntry {
  uint32_t tag = 0;
  uint32_t target = 0;
  bool valid = false;
  bool is_conditional = false;
  bool target_valid = false;
};

struct BtbStats {
  uint64_t reads = 0, writes = 0;
};


struct ScEntry {
  int8_t ctr = 0;
  uint8_t tag = 0;
};

struct ScStats {
  uint64_t lookups = 0, hits = 0, inverts = 0;
  uint64_t resolved_inverts = 0, inverts_correct = 0, inverts_wrong = 0;
  uint64_t allocations = 0;
  uint64_t bias_reads = 0, bias_writes = 0;
  uint64_t gehl_reads = 0, gehl_writes = 0;
};

class StatisticalCorrector {
 public:
  static constexpr int kNumTables = 6;
  static constexpr int kTagBits = 7;
  static constexpr int kCtrBits = 6;
  static constexpr int kCtrMax = 31, kCtrMin = -32;
  static constexpr int kTrainThreshold = 8;

  StatisticalCorrector() { Reset(); }
  void Reset();
  void Prepare(uint32_t pc, bool tage_prediction,
               const std::bitset<359>& history, uint8_t checkpoint_id);
  bool ShouldInvert(uint32_t pc, bool tage_prediction, bool tage_high_confidence,
                    uint8_t checkpoint_id);
  void Update(uint32_t pc, bool tage_prediction, bool taken, bool was_inverted,
              uint8_t checkpoint_id);
  uint64_t StorageBits() const {
    uint64_t entries = 0;
    for (int size : kTableSizes) entries += size;
    return entries * (kTagBits + kCtrBits);
  }
  const ScStats& Stats() const { return stats_; }

 private:
  static constexpr int kTableSizes[kNumTables] = {128, 128, 64, 64, 128, 128};
  static constexpr int kHistoryLengths[kNumTables] = {0, 0, 4, 16, 64, 256};
  struct Lookup {
    std::array<uint16_t, kNumTables> indices{};
    std::array<uint8_t, kNumTables> tags{};
    bool tage_prediction = false;
    bool valid = false;
    int score = 0;
  };
  std::array<std::vector<ScEntry>, kNumTables> tables_;
  std::array<Lookup, 256> lookups_{};
  ScStats stats_;
  static uint32_t FoldHistory(const std::bitset<359>& history, int length, int width);
  static uint32_t Index(uint32_t pc, bool dir, const std::bitset<359>& history, int table);
  static uint8_t Tag(uint32_t pc, bool dir, const std::bitset<359>& history, int table);
  static void UpdateCounter(int8_t& counter, bool taken);
};

class PredictorOne final : public BranchPredictor {
 public:
  PredictorOne();
  ~PredictorOne() override;
  const ScStats& ScStatistics() const { return sc_.Stats(); }
  const LoopPredictorMetrics& LoopMetrics() const { return loop_.Metrics(); }
  const TageStats& TageStatistics() const { return tage_.Stats(); }
  const BtbStats& BtbStatistics() const { return btb_stats_; }
  std::string Name() const override;
  void Reset() override;
  BranchPrediction Predict(std::uint32_t pc) override;
  void ObserveInstruction(const Instruction& instruction,
                          const BranchPrediction& prediction) override;
  void Update(const BranchResult& result,
              const BranchPrediction& prediction) override;
  std::uint64_t StorageBits() const override;
  const PredictorMetrics& Metrics() const override;

 private:
  PredictorMetrics metrics_;

#ifndef PIPESIM_BTB_ENTRIES
#define PIPESIM_BTB_ENTRIES 512
#endif
#ifndef PIPESIM_BTB_TAG_BITS
#define PIPESIM_BTB_TAG_BITS 32
#endif
  static constexpr std::size_t kBtbSize = PIPESIM_BTB_ENTRIES;
  static constexpr int kBtbTagBits = PIPESIM_BTB_TAG_BITS;
  std::vector<BTBEntry> btb_;
  BtbStats btb_stats_;
  static uint32_t BtbTag(uint32_t pc);

  TageCore tage_;
  StatisticalCorrector sc_;
  LoopPredictor loop_;
};

}
