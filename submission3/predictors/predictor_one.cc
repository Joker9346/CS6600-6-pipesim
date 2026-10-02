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
  TageCore::TageCore()
  {
    Reset();
  }

  void TageCore::Reset()
  {
    stats_ = TageStats{};
    base_predictor_.assign(2048, 1);
    use_alt_on_na_.assign(32, 8);

    tagged_tables_.resize(kNumTables);
    for (int i = 0; i < kNumTables; ++i)
    {
      tagged_tables_[i].assign(table_sizes_[i], TageEntry());
    }

    tick_counter_ = 0;
    ghr_.reset();
    phr_.reset();
    history_ring_.fill(HistoryCheckpoint{});
    ring_head_ = 0;
  }

  uint32_t TageCore::FoldHistory(const std::bitset<359> &history, int hist_len, int out_len) const
  {
    if (out_len <= 0)
      return 0;
    uint32_t folded = 0;
    uint32_t mask = (1U << out_len) - 1U;

    for (int i = 0; i < hist_len; i += out_len)
    {
      uint32_t chunk = 0;
      int chunk_bits = std::min(out_len, hist_len - i);
      for (int b = 0; b < chunk_bits; ++b)
      {
        if (history.test(i + b))
        {
          chunk |= (1U << b);
        }
      }
      folded ^= chunk;
    }
    return folded & mask;
  }

  uint32_t TageCore::GetIndex(uint32_t pc, const std::bitset<359> &hist, const std::bitset<27> &phr, int table) const
  {
    int idx_bits = index_bits_[table];
    uint32_t pc_hash = pc >> 2;

    uint32_t hist_fold = FoldHistory(hist, history_lengths_[table], idx_bits);

    uint32_t mask = (1U << idx_bits) - 1U;
    uint32_t phr_val = static_cast<uint32_t>(phr.to_ulong()) & mask;

    int rot = table % idx_bits;
    uint32_t phr_rot = phr_val;
    if (rot != 0)
    {
      uint32_t low_part = phr_val & ((1U << rot) - 1U);
      uint32_t high_part = phr_val >> rot;
      phr_rot = ((low_part << (idx_bits - rot)) | high_part) & mask;
    }

    return (pc_hash ^ hist_fold ^ phr_rot) % table_sizes_[table];
  }

  uint32_t TageCore::GetTag(uint32_t pc, const std::bitset<359> &hist, const std::bitset<27> &phr, int table) const
  {
    (void)phr;

    int tag_width = tag_widths_[table];
    uint32_t pc_hash = pc >> 2;

    uint32_t tag_fold1 = FoldHistory(hist, history_lengths_[table], tag_width);
    uint32_t tag_fold2 = FoldHistory(hist, history_lengths_[table], tag_width - 1);

    uint32_t tag = pc_hash ^ tag_fold1 ^ (tag_fold2 << 1);

    return tag & ((1U << tag_width) - 1U);
  }

  void TageCore::UpdateCtr(int8_t &ctr, bool taken, int max_val)
  {
    if (taken && ctr < max_val)
    {
      ctr++;
    }
    else if (!taken && ctr > 0)
    {
      ctr--;
    }
  }
  TagePrediction TageCore::Predict(std::uint32_t pc)
  {
    TagePrediction pred;

    uint8_t checkpoint_id = ring_head_++;
    history_ring_[checkpoint_id] = {ghr_, phr_};
    pred.checkpoint_id = checkpoint_id;
    pred.history = ghr_;

    bool provider_found = false, alt_found = false;
    int provider_table = -1, alt_table = -1;
    uint32_t provider_idx = 0, alt_idx = 0;
    bool provider_pred = false, alt_pred = false;

    uint32_t base_idx = (pc >> 2) % base_predictor_.size();
    bool base_pred = (base_predictor_[base_idx] >= 2);
    alt_pred = base_pred;
    ++stats_.base_reads;

    bool alt_used = false;
    for (int i = kNumTables - 1; i >= 0; --i)
    {
      ++stats_.tagged_reads;
      uint32_t idx = GetIndex(pc, ghr_, phr_, i);
      uint32_t tag = GetTag(pc, ghr_, phr_, i);

      if (tagged_tables_[i][idx].valid && tagged_tables_[i][idx].tag == tag)
      {
        if (!provider_found)
        {
          provider_found = true;
          provider_table = i;
          provider_idx = idx;
          provider_pred = (tagged_tables_[i][idx].ctr >= 4);
        }
        else if (!alt_found)
        {
          alt_found = true;
          alt_table = i;
          alt_idx = idx;
          alt_pred = (tagged_tables_[i][idx].ctr >= 4);
          break;
        }
      }
    }

    bool is_weak = false;
    uint32_t use_alt_idx = (pc >> 2) % use_alt_on_na_.size();

    if (provider_found)
    {
      int8_t ctr = tagged_tables_[provider_table][provider_idx].ctr;
      is_weak = (ctr == 3 || ctr == 4);
      ++stats_.aux_reads;

      if (is_weak && use_alt_on_na_[use_alt_idx] >= 8)
      {
        pred.taken = alt_pred;
        alt_used = true;
      }
      else
      {
        pred.taken = provider_pred;
      }
      pred.high_confidence = !is_weak;
      pred.provider_table = provider_table;
    }
    else
    {
      pred.taken = base_pred;
      pred.high_confidence = (base_predictor_[base_idx] == 0 || base_predictor_[base_idx] == 3);
      pred.provider_table = -1;
    }

    uint64_t info = 0;
    info |= (static_cast<uint64_t>(checkpoint_id) & 0xFFULL);
    info |= (static_cast<uint64_t>(provider_found ? 1 : 0) & 0x1ULL) << 8;
    info |= (static_cast<uint64_t>(provider_table & 0xFULL)) << 9;
    info |= (static_cast<uint64_t>(provider_idx & 0x3FFULL)) << 13;
    info |= (static_cast<uint64_t>(provider_pred ? 1 : 0) & 0x1ULL) << 23;
    info |= (static_cast<uint64_t>(alt_found ? 1 : 0) & 0x1ULL) << 24;
    info |= (static_cast<uint64_t>(alt_table & 0xFULL)) << 25;
    info |= (static_cast<uint64_t>(alt_idx & 0x3FFULL)) << 29;
    info |= (static_cast<uint64_t>(alt_pred ? 1 : 0) & 0x1ULL) << 39;
    info |= (static_cast<uint64_t>(base_pred ? 1 : 0) & 0x1ULL) << 40;
    info |= (static_cast<uint64_t>(is_weak ? 1 : 0) & 0x1ULL) << 41;
    info |= (static_cast<uint64_t>(pred.taken ? 1 : 0) & 0x1ULL) << 42;
    info |= (static_cast<uint64_t>(alt_used ? 1 : 0) & 0x1ULL) << 46;
    pred.information = info;

    return pred;
  }

  void TageCore::Update(const BranchResult &result, uint64_t info)
  {
    bool is_cond = (result.type == BranchType::kConditional);

    if (!is_cond)
    {
      uint32_t target_bits = (result.target >> 2) & 0xFU;
      for (int b = 0; b < 4; ++b)
      {
        ghr_ <<= 1;
        ghr_.set(0, (target_bits >> b) & 1U);
      }
      phr_ <<= 1;
      phr_.set(0, (result.pc >> 2) & 1U);
      return;
    }

    uint8_t checkpoint_id = info & 0xFFULL;
    bool provider_found = (info >> 8) & 1ULL;
    int provider_table = static_cast<int8_t>((info >> 9) & 0xFULL);
    if (provider_table > 9)
      provider_table = -1;
    uint32_t provider_idx = (info >> 13) & 0x3FFULL;
    bool provider_pred = (info >> 23) & 1ULL;
    bool alt_found = (info >> 24) & 1ULL;
    int alt_table = static_cast<int8_t>((info >> 25) & 0xFULL);
    if (alt_table > 9)
      alt_table = -1;
    uint32_t alt_idx = (info >> 29) & 0x3FFULL;
    bool alt_pred = (info >> 39) & 1ULL;
    bool is_weak = (info >> 41) & 1ULL;
    bool tage_pred = (info >> 42) & 1ULL;
    bool alt_used = (info >> 46) & 1ULL;

    {
      int source = alt_used ? TageStats::kAltSource
                            : (provider_found ? provider_table : TageStats::kBaseSource);
      ++stats_.resolved_conditional;
      ++stats_.provider[source];
      if (tage_pred == result.taken)
        ++stats_.provider_correct[source];
    }

    uint32_t base_idx = (result.pc >> 2) % base_predictor_.size();
    uint32_t use_alt_idx = (result.pc >> 2) % use_alt_on_na_.size();

    bool pred_correct = (tage_pred == result.taken);
    bool provider_correct = (provider_pred == result.taken);
    bool alt_correct = (alt_pred == result.taken);

    if (provider_found && is_weak)
    {
      if (alt_correct && !provider_correct)
      {
        UpdateCtr(use_alt_on_na_[use_alt_idx], true, 15);
        ++stats_.aux_writes;
      }
      else if (provider_correct && !alt_correct)
      {
        UpdateCtr(use_alt_on_na_[use_alt_idx], false, 15);
        ++stats_.aux_writes;
      }
    }

    if (provider_found)
    {
      UpdateCtr(tagged_tables_[provider_table][provider_idx].ctr, result.taken, 7);
      ++stats_.tagged_writes;

      if (provider_correct && !alt_correct)
      {
        if (tagged_tables_[provider_table][provider_idx].u < 3)
        {
          tagged_tables_[provider_table][provider_idx].u++;
        }
      }

      if (is_weak)
      {
        if (alt_found)
        {
          UpdateCtr(tagged_tables_[alt_table][alt_idx].ctr, result.taken, 7);
          ++stats_.tagged_writes;
        }
        else
        {
          UpdateCtr(base_predictor_[base_idx], result.taken, 3);
          ++stats_.base_writes;
        }
      }
    }
    else
    {
      UpdateCtr(base_predictor_[base_idx], result.taken, 3);
      ++stats_.base_writes;
    }

    if (!pred_correct)
    {
      const auto &saved_ghr = history_ring_[checkpoint_id].ghr;
      const auto &saved_phr = history_ring_[checkpoint_id].phr;

      int start_table = provider_found ? (provider_table + 1) : 0;
      int alloc_count = 0;

      if (start_table < kNumTables)
      {
        for (int i = start_table; i < kNumTables; ++i)
        {
          uint32_t idx = GetIndex(result.pc, saved_ghr, saved_phr, i);
          uint32_t tag = GetTag(result.pc, saved_ghr, saved_phr, i);

          ++stats_.tagged_reads;
          if (tagged_tables_[i][idx].u == 0)
          {
            ++stats_.tagged_writes;
            ++stats_.allocations;
            tagged_tables_[i][idx].valid = true;
            tagged_tables_[i][idx].tag = tag;
            tagged_tables_[i][idx].ctr = result.taken ? 4 : 3;
            tagged_tables_[i][idx].u = 0;
            alloc_count++;

            ++i;
            if (alloc_count == 2)
              break;
          }
        }

        if (alloc_count == 0)
        {
          ++stats_.allocation_failures;
          for (int i = start_table; i < kNumTables; ++i)
          {
            uint32_t idx = GetIndex(result.pc, saved_ghr, saved_phr, i);
            ++stats_.tagged_reads;
            if (tagged_tables_[i][idx].u > 0)
            {
              tagged_tables_[i][idx].u--;
              ++stats_.tagged_writes;
            }
          }

          tick_counter_++;
          if (tick_counter_ == 0)
          {
            for (int t = 0; t < kNumTables; ++t)
            {
              for (auto &entry : tagged_tables_[t])
              {
                if (entry.u > 0)
                {
                  entry.u--;
                  ++stats_.tagged_writes;
                }
              }
            }
          }
        }
      }
    }

    ghr_ <<= 1;
    ghr_.set(0, result.taken);
    phr_ <<= 1;
    phr_.set(0, (result.pc >> 2) & 1U);
  }

  std::uint64_t TageCore::StorageBits() const
  {
    uint64_t bits = 0;
    bits += base_predictor_.size() * 2;
    for (int i = 0; i < kNumTables; ++i)
    {
      bits += table_sizes_[i] * (3 + 2 + tag_widths_[i] + 1);
    }
    bits += use_alt_on_na_.size() * 4;
    bits += 8;
    bits += 359;
    bits += 27;
    return bits;
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
