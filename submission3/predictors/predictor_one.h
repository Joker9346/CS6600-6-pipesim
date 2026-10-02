#pragma once

#include "branch_prediction.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace pipesim
{

  struct BTBEntry
  {
    uint32_t tag = 0;
    uint32_t target = 0;
    bool valid = false;
    bool is_conditional = false;
    bool target_valid = false;
  };

  struct BtbStats
  {
    uint64_t reads = 0, writes = 0;
  };

  struct TageStats
  {
    static constexpr int kNumSources = 12;
    static constexpr int kBaseSource = 10;
    static constexpr int kAltSource = 11;
    uint64_t resolved_conditional = 0;
    uint64_t provider[kNumSources] = {};
    uint64_t provider_correct[kNumSources] = {};
    uint64_t base_reads = 0, base_writes = 0;
    uint64_t tagged_reads = 0, tagged_writes = 0;
    uint64_t aux_reads = 0, aux_writes = 0;
    uint64_t allocations = 0, allocation_failures = 0;
  };

  struct TageEntry
  {
    int8_t ctr = 3;
    uint8_t u = 0;
    uint32_t tag = 0;
    bool valid = false;
  };

  struct TagePrediction
  {
    bool taken = false;
    bool high_confidence = false;
    int provider_table = -1;
    uint64_t information = 0;
    uint8_t checkpoint_id = 0;
    std::bitset<359> history;
  };

  class TageCore
  {
  public:
    TageCore();
    void Reset();
    TagePrediction Predict(uint32_t pc);
    void Update(const BranchResult &result, uint64_t info);
    uint64_t StorageBits() const;
    const TageStats &Stats() const { return stats_; }

  private:
    TageStats stats_;
    std::vector<int8_t> base_predictor_;
    std::vector<std::vector<TageEntry>> tagged_tables_;
    std::vector<int8_t> use_alt_on_na_;

    uint8_t tick_counter_ = 0;

    std::bitset<359> ghr_;
    std::bitset<27> phr_;

    struct HistoryCheckpoint
    {
      std::bitset<359> ghr;
      std::bitset<27> phr;
    };
    static constexpr std::size_t kHistoryRingSize = 256;
    std::array<HistoryCheckpoint, kHistoryRingSize> history_ring_;
    uint8_t ring_head_ = 0;

    static constexpr int kNumTables = 10;
    const int history_lengths_[kNumTables] = {4, 9, 13, 24, 37, 53, 91, 145, 226, 359};
    const int tag_widths_[kNumTables] = {7, 7, 7, 8, 9, 10, 10, 11, 13, 13};
    const int table_sizes_[kNumTables] = {256, 256, 256, 256, 128, 256, 128, 64, 64, 64};
    const int index_bits_[kNumTables] = {8, 8, 8, 8, 7, 8, 7, 6, 6, 6};

    uint32_t FoldHistory(const std::bitset<359> &history, int hist_len, int out_len) const;
    uint32_t GetIndex(uint32_t pc, const std::bitset<359> &hist, const std::bitset<27> &phr, int table) const;
    uint32_t GetTag(uint32_t pc, const std::bitset<359> &hist, const std::bitset<27> &phr, int table) const;
    void UpdateCtr(int8_t &ctr, bool taken, int max_val);
  };

  struct ScEntry
  {
    int8_t ctr = 0;
    uint8_t tag = 0;
  };

  struct ScStats
  {
    uint64_t lookups = 0, hits = 0, inverts = 0;
    uint64_t resolved_inverts = 0, inverts_correct = 0, inverts_wrong = 0;
    uint64_t allocations = 0;
    uint64_t bias_reads = 0, bias_writes = 0;
    uint64_t gehl_reads = 0, gehl_writes = 0;
  };

  class StatisticalCorrector
  {
  public:
    static constexpr int kNumTables = 6;
    static constexpr int kTagBits = 7;
    static constexpr int kCtrBits = 6;
    static constexpr int kCtrMax = 31, kCtrMin = -32;
    static constexpr int kTrainThreshold = 8;

    StatisticalCorrector() { Reset(); }
    void Reset();
    void Prepare(uint32_t pc, bool tage_prediction,
                 const std::bitset<359> &history, uint8_t checkpoint_id);
    bool ShouldInvert(uint32_t pc, bool tage_prediction, bool tage_high_confidence,
                      uint8_t checkpoint_id);
    void Update(uint32_t pc, bool tage_prediction, bool taken, bool was_inverted,
                uint8_t checkpoint_id);
    uint64_t StorageBits() const
    {
      uint64_t entries = 0;
      for (int size : kTableSizes)
        entries += size;
      return entries * (kTagBits + kCtrBits);
    }
    const ScStats &Stats() const { return stats_; }

  private:
    static constexpr int kTableSizes[kNumTables] = {128, 128, 64, 64, 128, 128};
    static constexpr int kHistoryLengths[kNumTables] = {0, 0, 4, 16, 64, 256};
    struct Lookup
    {
      std::array<uint16_t, kNumTables> indices{};
      std::array<uint8_t, kNumTables> tags{};
      bool tage_prediction = false;
      bool valid = false;
      int score = 0;
    };
    std::array<std::vector<ScEntry>, kNumTables> tables_;
    std::array<Lookup, 256> lookups_{};
    ScStats stats_;
    static uint32_t FoldHistory(const std::bitset<359> &history, int length, int width);
    static uint32_t Index(uint32_t pc, bool dir, const std::bitset<359> &history, int table);
    static uint8_t Tag(uint32_t pc, bool dir, const std::bitset<359> &history, int table);
    static void UpdateCounter(int8_t &counter, bool taken);
  };

  class PredictorOne final : public BranchPredictor
  {
  public:
    PredictorOne();
    ~PredictorOne() override;
    const ScStats &ScStatistics() const { return sc_.Stats(); }
    const LoopPredictorMetrics &LoopMetrics() const { return loop_.Metrics(); }
    const TageStats &TageStatistics() const { return tage_.Stats(); }
    const BtbStats &BtbStatistics() const { return btb_stats_; }
    std::string Name() const override;
    void Reset() override;
    BranchPrediction Predict(std::uint32_t pc) override;
    void ObserveInstruction(const Instruction &instruction,
                            const BranchPrediction &prediction) override;
    void Update(const BranchResult &result,
                const BranchPrediction &prediction) override;
    std::uint64_t StorageBits() const override;
    const PredictorMetrics &Metrics() const override;

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
