#include "predictors/predictor_one.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#ifndef PIPESIM_SC_THRESHOLD
#define PIPESIM_SC_THRESHOLD 4
#endif
#ifndef PIPESIM_SC_BIAS
#define PIPESIM_SC_BIAS 12
#endif

namespace pipesim
{

  void StatisticalCorrector::Reset()
  {
    for (int i = 0; i < kNumTables; ++i)
      tables_[i].assign(kTableSizes[i], ScEntry{});
    lookups_.fill(Lookup{});
    stats_ = ScStats{};
  }

  uint32_t StatisticalCorrector::FoldHistory(const std::bitset<359>& history,
                                              int length, int width)
  {
    if (length == 0)
      return 0;
    uint32_t folded = 0;
    for (int bit = 0; bit < length; ++bit)
      if (history.test(bit))
        folded ^= 1U << (bit % width);
    return folded & ((1U << width) - 1U);
  }

  uint32_t StatisticalCorrector::Index(uint32_t pc, bool dir,
                                       const std::bitset<359>& history, int table)
  {
    int width = 0;
    while ((1U << width) < static_cast<unsigned>(kTableSizes[table])) ++width;
    uint32_t pc_hash = (pc >> 2) ^ (pc >> (2 + width));
    uint32_t history_hash = FoldHistory(history, kHistoryLengths[table], width);
    uint32_t direction_hash = dir ? (0x2DU + 7U * table) : 0U;
    return (pc_hash ^ history_hash ^ direction_hash) & (kTableSizes[table] - 1);
  }

  uint8_t StatisticalCorrector::Tag(uint32_t pc, bool dir,
                                    const std::bitset<359>& history, int table)
  {
    uint32_t folded = FoldHistory(history, kHistoryLengths[table], kTagBits - 1);
    uint32_t raw = ((pc >> 8) ^ (pc >> 15) ^ folded ^
                    (dir ? (0x53U + 11U * table) : 0U)) & ((1U << kTagBits) - 1U);
    return raw == 0 ? 1 : static_cast<uint8_t>(raw);
  }

  void StatisticalCorrector::Prepare(uint32_t pc, bool tage_prediction,
                                     const std::bitset<359>& history,
                                     uint8_t checkpoint_id)
  {
    Lookup& lookup = lookups_[checkpoint_id];
    lookup = Lookup{};
    lookup.tage_prediction = tage_prediction;
    lookup.valid = true;
    int counter_sum = 0;
    for (int table = 0; table < kNumTables; ++table)
    {
      lookup.indices[table] = static_cast<uint16_t>(Index(pc, tage_prediction, history, table));
      lookup.tags[table] = Tag(pc, tage_prediction, history, table);
      const ScEntry& entry = tables_[table][lookup.indices[table]];
      if (entry.tag == lookup.tags[table])
        counter_sum += entry.ctr;
    }
    lookup.score = counter_sum + (tage_prediction ? 1 : -1) * PIPESIM_SC_BIAS;
  }

  bool StatisticalCorrector::ShouldInvert(uint32_t pc, bool tage_prediction,
                                          bool tage_high_confidence,
                                          uint8_t checkpoint_id)
  {
    (void)pc;
    Lookup& lookup = lookups_[checkpoint_id];
    if (!lookup.valid)
      return false;
    stats_.lookups++;
    for (int table = 0; table < kNumTables; ++table)
    {
      if (table < 2) ++stats_.bias_reads;
      else ++stats_.gehl_reads;
      if (tables_[table][lookup.indices[table]].tag == lookup.tags[table])
        ++stats_.hits;
    }
    bool sc_prediction = lookup.score >= 0;
    bool invert = (sc_prediction != tage_prediction) && !tage_high_confidence &&
                  std::abs(lookup.score) >= PIPESIM_SC_THRESHOLD;
    if (invert) ++stats_.inverts;
    return invert;
  }

  void StatisticalCorrector::UpdateCounter(int8_t& counter, bool taken)
  {
    if (taken && counter < kCtrMax) ++counter;
    else if (!taken && counter > kCtrMin) --counter;
  }

  void StatisticalCorrector::Update(uint32_t pc, bool tage_prediction, bool taken,
                                    bool was_inverted, uint8_t checkpoint_id)
  {
    (void)pc;
    Lookup& lookup = lookups_[checkpoint_id];
    if (!lookup.valid)
      return;
    const bool tage_wrong = (tage_prediction != taken);
    if (was_inverted)
    {
      ++stats_.resolved_inverts;
      if (tage_wrong) ++stats_.inverts_correct;
      else ++stats_.inverts_wrong;
    }

    bool sc_correct = ((lookup.score >= 0) == taken);
    bool train = !sc_correct || std::abs(lookup.score) < kTrainThreshold;
    if (train)
    {
      for (int table = 0; table < kNumTables; ++table)
      {
        if (table < 2) ++stats_.bias_reads;
        else ++stats_.gehl_reads;
        ScEntry& entry = tables_[table][lookup.indices[table]];
        if (entry.tag == lookup.tags[table])
        {
          UpdateCounter(entry.ctr, taken);
        }
        else
        {
          entry.tag = lookup.tags[table];
          entry.ctr = taken ? 1 : -1;
          ++stats_.allocations;
        }
        if (table < 2) ++stats_.bias_writes;
        else ++stats_.gehl_writes;
      }
    }
    lookup.valid = false;
  }

  uint32_t PredictorOne::BtbTag(uint32_t pc)
  {
    if (kBtbTagBits >= 32)
      return pc;
    int index_bits = 0;
    while ((std::size_t{1} << index_bits) < kBtbSize)
      ++index_bits;
    return (pc >> (2 + index_bits)) & static_cast<uint32_t>((std::uint64_t{1} << kBtbTagBits) - 1U);
  }

  PredictorOne::PredictorOne()
  {
    Reset();
  }

  PredictorOne::~PredictorOne() = default;

  std::string PredictorOne::Name() const
  {
    return "PAPER_ONE";
  }

  void PredictorOne::Reset()
  {
    metrics_ = PredictorMetrics();
    btb_stats_ = BtbStats{};
    btb_.assign(kBtbSize, BTBEntry{});
    tage_.Reset();
    sc_.Reset();
    loop_.Reset();
  }

  BranchPrediction PredictorOne::Predict(std::uint32_t pc)
  {
    metrics_.lookups++;
    BranchPrediction pred;
    pred.branch_found = false;
    pred.taken = false;
    pred.target = pc + 4;
    pred.confidence = 0;

    TagePrediction tage_pred = tage_.Predict(pc);
    sc_.Prepare(pc, tage_pred.taken, tage_pred.history, tage_pred.checkpoint_id);

    pred.information = tage_pred.information;
    pred.confidence = tage_pred.high_confidence ? 1 : 0;

    uint32_t btb_idx = (pc >> 2) % btb_.size();
    ++btb_stats_.reads;
    if (btb_[btb_idx].valid && btb_[btb_idx].tag == BtbTag(pc))
    {
      pred.branch_found = true;
      metrics_.branches_found++;

      if (!btb_[btb_idx].is_conditional)
      {
        pred.taken = true;
        pred.target = btb_[btb_idx].target;
        metrics_.predicted_taken++;
        return pred;
      }
    }
    else
    {
      metrics_.branches_not_found++;
      metrics_.predicted_not_taken++;
      return pred;
    }

    bool sc_invert = sc_.ShouldInvert(pc, tage_pred.taken, tage_pred.high_confidence,
                                      tage_pred.checkpoint_id);
    bool final_direction = tage_pred.taken ^ sc_invert;
    pred.information |= (sc_invert ? 1ULL : 0ULL) << 43;

    LoopPrediction loop_prediction = loop_.Predict(pc);
    if (loop_prediction.confident)
    {
      final_direction = loop_prediction.taken;
      pred.information |= 1ULL << 44;
      pred.information |= static_cast<uint64_t>(loop_prediction.taken) << 45;
    }
    pred.confidence = (tage_pred.high_confidence && !sc_invert &&
                       !loop_prediction.confident) ? 1 : 0;

    if (final_direction && !btb_[btb_idx].target_valid)
      final_direction = false;

    pred.taken = final_direction;
    pred.target = final_direction ? btb_[btb_idx].target : pc + 4;

    if (pred.taken)
      metrics_.predicted_taken++;
    else
      metrics_.predicted_not_taken++;

    return pred;
  }

  void PredictorOne::ObserveInstruction(const Instruction &instruction,
                                        const BranchPrediction &prediction)
  {
    bool actual_branch = (instruction.opcode == Opcode::kBeq ||
                          instruction.opcode == Opcode::kBne ||
                          instruction.opcode == Opcode::kJ);
    if (actual_branch && !prediction.branch_found)
    {
      metrics_.missed_branches++;
    }
    else if (!actual_branch && prediction.branch_found)
    {
      metrics_.false_branch_predictions++;
    }
  }

  void PredictorOne::Update(const BranchResult &result,
                            const BranchPrediction &prediction)
  {
    metrics_.updates++;
    if (result.type == BranchType::kNotBranch)
      return;

    bool is_cond = (result.type == BranchType::kConditional);

    uint32_t btb_idx = (result.pc >> 2) % btb_.size();
    ++btb_stats_.reads;
    ++btb_stats_.writes;
    if (!btb_[btb_idx].valid || btb_[btb_idx].tag != BtbTag(result.pc))
    {
      btb_[btb_idx] = BTBEntry{};
      btb_[btb_idx].valid = true;
      btb_[btb_idx].tag = BtbTag(result.pc);
    }
    if (result.taken)
    {
      btb_[btb_idx].target = result.target;
      btb_[btb_idx].target_valid = true;
    }
    btb_[btb_idx].is_conditional = is_cond;

    bool predicted_taken = prediction.branch_found && prediction.taken;
    bool direction_wrong = (predicted_taken != result.taken);
    bool target_wrong = result.taken && predicted_taken && (prediction.target != result.target);

    if (direction_wrong)
    {
      metrics_.direction_mispredictions++;
    }
    else if (target_wrong)
    {
      metrics_.target_mispredictions++;
    }
    else
    {
      metrics_.correct_predictions++;
    }

    if (is_cond)
    {
      bool tage_dir = (prediction.information >> 42) & 1ULL;
      bool sc_inverted = (prediction.information >> 43) & 1ULL;
      sc_.Update(result.pc, tage_dir, result.taken, sc_inverted,
                 static_cast<uint8_t>(prediction.information & 0xFFULL));
    }
    bool loop_selected = (prediction.information >> 44) & 1ULL;
    bool loop_taken = (prediction.information >> 45) & 1ULL;
    loop_.Update(result, loop_selected, loop_taken);
    tage_.Update(result, prediction.information);
  }

  std::uint64_t PredictorOne::StorageBits() const
  {
    uint64_t btb_bits = btb_.size() * (std::min(kBtbTagBits, 32) + 32 + 1 + 1 + 1);
    return tage_.StorageBits() + sc_.StorageBits() + loop_.StorageBits() + btb_bits;
  }

  const PredictorMetrics &PredictorOne::Metrics() const
  {
    return metrics_;
  }
}
