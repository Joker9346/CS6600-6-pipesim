#pragma once
#include "cache/cache_controller.h"
#include "cache/replacement.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace pipesim::cache
{
  namespace cache_controller_internal
  {
    enum class OwnerState
    {
      kUnsent,
      kInFlight,
      kResponding,
      kAckReady
    };
    struct AddressFields
    {
      std::uint32_t block = 0, set = 0, tag = 0, offset = 0;
    };
    struct PipelineEntry
    {
      bool valid = false;
      CacheRequest request;
      std::uint64_t arrival_order = 0;
      AddressFields address;
    };
    struct OutputEntry
    {
      bool valid = false;
      CacheResponse response;
      std::uint64_t arrival_order = 0;
    };
    struct MshrEntry
    {
      bool valid = false;
      std::uint32_t block_address = 0, set = 0, tag = 0, reserved_way = 0;
      std::uint64_t arrival_order = 0;
      OwnerState state = OwnerState::kUnsent;
      CacheRequest lower_request;
      std::vector<MshrWaiter> waiters;
    };
    struct WritebackEntry
    {
      bool valid = false;
      std::uint32_t address = 0, size = 0;
      std::vector<std::uint8_t> data;
      std::uint64_t arrival_order = 0;
      OwnerState state = OwnerState::kUnsent;
      CacheRequest lower_request;
      std::vector<WritebackAckWaiter> waiters;
      std::uint32_t demand_bypasses = 0;
    };
    enum class ResponseOwner
    {
      kNone,
      kOutput,
      kMshr,
      kWriteback
    };
    struct ResponseChoice
    {
      ResponseOwner owner = ResponseOwner::kNone;
      std::size_t entry = 0, waiter = 0;
      CacheResponse response;
      std::uint64_t arrival_order = 0;
    };
    enum class RequestOwner
    {
      kNone,
      kMshr,
      kWriteback
    };
    struct RequestChoice
    {
      RequestOwner owner = RequestOwner::kNone;
      std::size_t entry = 0;
      CacheRequest request;
      std::uint64_t arrival_order = 0;
      bool demand_blocked_by_writeback = false;
      std::optional<std::size_t> oldest_eligible_writeback;
    };
    enum class LookupKind
    {
      kEmpty,
      kHit,
      kMshrMerge,
      kMshrAllocate,
      kWritebackForward,
      kHold
    };
    enum class LookupStall
    {
      kNone,
      kOutput,
      kMshrWaiterFull,
      kMshrFull,
      kReservedWay,
      kWritebackOrder,
      kWritebackWaiterFull,
      kWritebackFull
    };
    struct LookupDecision
    {
      LookupKind kind = LookupKind::kEmpty;
      LookupStall stall = LookupStall::kNone;
      int hit_way = -1, mshr = -1, victim_way = -1;
    };
    struct WritebackPlan
    {
      bool allowed = false;
      std::vector<std::size_t> merge_entries;
      int survivor = -1;
      LookupStall stall = LookupStall::kNone;
    };
    inline bool RangesOverlap(std::uint32_t a, std::uint32_t as, std::uint32_t b, std::uint32_t bs)
    {
      const auto ae = static_cast<std::uint64_t>(a) + as, be = static_cast<std::uint64_t>(b) + bs;
      return a < be && b < ae;
    }
    inline bool ResponseLess(const ResponseChoice &a, const ResponseChoice &b) { return std::tie(a.arrival_order, a.response.id) < std::tie(b.arrival_order, b.response.id); }
    inline TransactionSource SourceForRole(CacheRole role)
    {
      switch (role)
      {
      case CacheRole::kL1I:
        return TransactionSource::kL1I;
      case CacheRole::kL1D:
        return TransactionSource::kL1D;
      case CacheRole::kL2I:
        return TransactionSource::kL2I;
      case CacheRole::kL2D:
        return TransactionSource::kL2D;
      }
      throw std::runtime_error("invalid cache role");
    }
    inline const char *OwnerStateName(OwnerState state)
    {
      switch (state)
      {
      case OwnerState::kUnsent:
        return "UNSENT";
      case OwnerState::kInFlight:
        return "IN_FLIGHT";
      case OwnerState::kResponding:
        return "RESPONDING";
      case OwnerState::kAckReady:
        return "ACK_READY";
      }
      return "UNKNOWN";
    }
  }
  using namespace cache_controller_internal;

  class CacheController::Impl
  {
  public:
    Impl(CacheRole role, const CacheConfig &config,
         TransactionIdService *transaction_ids)
        : role_(role),
          config_(config),
          transaction_ids_(transaction_ids),
          current_lines_(config.sets,
                         std::vector<CacheLine>(config.ways)),
          next_lines_(current_lines_),
          current_mshrs_(config.mshr_entries),
          next_mshrs_(current_mshrs_),
          current_writebacks_(config.writeback_entries),
          next_writebacks_(current_writebacks_),
          replacement_(MakeReplacementPolicy(config.replacement, config.sets,
                                             config.ways))
    {
      if (transaction_ids_ == nullptr)
      {
        throw std::runtime_error("cache controller transaction-id service is null");
      }
      Reset();
    }

    void Reset()
    {
      for (auto &set : current_lines_)
      {
        for (CacheLine &line : set)
        {
          line = CacheLine();
          line.data.assign(config_.line_size, 0);
        }
      }
      next_lines_ = current_lines_;
      current_index_ = PipelineEntry();
      current_lookup_ = PipelineEntry();
      current_output_ = OutputEntry();
      next_index_ = current_index_;
      next_lookup_ = current_lookup_;
      next_output_ = current_output_;
      current_mshrs_.assign(config_.mshr_entries, MshrEntry());
      next_mshrs_ = current_mshrs_;
      current_writebacks_.assign(config_.writeback_entries, WritebackEntry());
      next_writebacks_ = current_writebacks_;
      current_arrival_ = 0;
      next_arrival_ = 0;
      stats_ = CacheStats();
      next_stats_ = stats_;
      replacement_->Reset();
      begun_ = false;
      operated_ = false;
    }

    void BeginCycle()
    {
      if (begun_)
      {
        throw std::runtime_error("cache BeginCycle called twice");
      }
      next_lines_ = current_lines_;
      next_index_ = current_index_;
      next_lookup_ = current_lookup_;
      next_output_ = current_output_;
      next_mshrs_ = current_mshrs_;
      next_writebacks_ = current_writebacks_;
      next_arrival_ = current_arrival_;
      next_stats_ = stats_;
      replacement_->BeginCycle();
      begun_ = true;
      operated_ = false;
    }


    AddressFields MapAddress(std::uint32_t address) const
    {
      AddressFields field;
      field.offset = address % config_.line_size;
      field.block = address - field.offset;
      std::uint32_t block_index = field.block / config_.line_size;
      field.set = block_index % config_.sets;
      field.tag = block_index / config_.sets;
      return field;
     
    }

    void ValidateInbound(const CacheRequest &request) const
    {
      ValidateRequestPayload(request);
      if (!RangeFitsLine(request.address, request.size, config_.line_size))
      {
        throw std::runtime_error(std::string(CacheRoleName(role_)) +
                                 " request crosses a local cache line");
      }
      switch (role_)
      {
      case CacheRole::kL1I:
      case CacheRole::kL2I:
        if (request.type != RequestType::kRead)
        {
          throw std::runtime_error("instruction cache received a write request");
        }
        break;
      case CacheRole::kL1D:
        if (request.type != RequestType::kRead &&
            request.type != RequestType::kWrite)
        {
          throw std::runtime_error("L1D received an illegal request type");
        }
        break;
      case CacheRole::kL2D:
        if (request.type != RequestType::kRead &&
            request.type != RequestType::kWriteback)
        {
          throw std::runtime_error("L2D received an illegal request type");
        }
        break;
      }
      if ((role_ == CacheRole::kL1I || role_ == CacheRole::kL1D) &&
          request.origin_id != request.id)
      {
        throw std::runtime_error("L1 CPU request origin must equal link id");
      }
      if ((role_ == CacheRole::kL1I || role_ == CacheRole::kL1D) &&
          (request.size != 4 || request.address % 4 != 0))
      {
        throw std::runtime_error("CPU cache request must be one aligned word");
      }
      if ((role_ == CacheRole::kL1D || role_ == CacheRole::kL2D) &&
          request.type != RequestType::kWriteback && request.generation != 0)
      {
        throw std::runtime_error("data demand carries a fetch generation");
      }
    }

    int FindHit(const AddressFields &address) const
    {
      int hit = -1;
      for (std::uint32_t way = 0; way < config_.ways; ++way)
      {
        const CacheLine &line = current_lines_[address.set][way];
        if (line.valid && line.tag == address.tag)
        {
          if (hit >= 0)
            throw std::runtime_error("duplicate tags in one cache set");
          hit = static_cast<int>(way);
        }
      }
      return hit;
    }

    int FindMshrByBlock(std::uint32_t block) const
    {
      int found = -1;
      for (std::size_t i = 0; i < current_mshrs_.size(); ++i)
      {
        if (current_mshrs_[i].valid &&
            current_mshrs_[i].block_address == block)
        {
          if (found >= 0)
            throw std::runtime_error("duplicate MSHR block owner");
          found = static_cast<int>(i);
        }
      }
      return found;
    }

    int FindMshrByLowerId(std::uint64_t id) const
    {
      int found = -1;
      for (std::size_t i = 0; i < current_mshrs_.size(); ++i)
      {
        if (current_mshrs_[i].valid &&
            current_mshrs_[i].lower_request.id == id)
        {
          if (found >= 0)
            throw std::runtime_error("duplicate MSHR lower id");
          found = static_cast<int>(i);
        }
      }
      return found;
    }

    int FindWritebackByLowerId(std::uint64_t id) const
    {
      int found = -1;
      for (std::size_t i = 0; i < current_writebacks_.size(); ++i)
      {
        if (current_writebacks_[i].valid &&
            current_writebacks_[i].lower_request.id == id)
        {
          if (found >= 0)
            throw std::runtime_error("duplicate writeback lower id");
          found = static_cast<int>(i);
        }
      }
      return found;
    }

    int FindFreeMshr() const
    {
      for (std::size_t i = 0; i < current_mshrs_.size(); ++i)
      {
        if (!current_mshrs_[i].valid)
          return static_cast<int>(i);
      }
      return -1;
    }

    int FindFreeWriteback() const
    {
      for (std::size_t i = 0; i < current_writebacks_.size(); ++i)
      {
        if (!current_writebacks_[i].valid)
          return static_cast<int>(i);
      }
      return -1;
    }

    std::vector<bool> EligibleWays(std::uint32_t set) const
    {
      std::vector<bool> eligible(config_.ways, false);
      for (std::uint32_t way = 0; way < config_.ways; ++way)
      {
        eligible[way] = !current_lines_[set][way].reserved;
      }
      return eligible;
    }

    int PeekVictim(std::uint32_t set) const
    {
      const std::vector<bool> eligible = EligibleWays(set);
      for (std::uint32_t way = 0; way < config_.ways; ++way)
      {
        if (eligible[way] && !current_lines_[set][way].valid)
          return way;
      }
      return replacement_->PeekVictim(set, eligible);
    }

   
    int SelectVictim(std::uint32_t set)
    {
      const std::vector<bool> eligible = EligibleWays(set);
      for (std::uint32_t way = 0; way < config_.ways; ++way)
      {
        if (eligible[way] && !current_lines_[set][way].valid)
        {
          next_lines_[set][way].reserved = true;
          return static_cast<int>(way);
        }
      }
      const int chosen = replacement_->SelectVictim(set, eligible);
      if (chosen < 0)
      {
        throw std::runtime_error("SelectVictim() victim not found");
      }
      next_lines_[set][static_cast<std::uint32_t>(chosen)].reserved = true;
      return chosen;
  
    }

    WritebackPlan PlanWriteback(std::uint32_t address, std::uint32_t size,
                                std::size_t new_waiters,
                                int granted_writeback = -1) const
    {
      WritebackPlan plan;

      CheckedRangeEnd(address, size);

      std::uint32_t range_begin = address;
      std::uint64_t range_end = static_cast<std::uint64_t>(address) + size;
      std::vector<bool> included(current_writebacks_.size(), false);

      bool changed = true;
      while (changed)
      {
        changed = false;

        for (std::size_t i = 0; i < current_writebacks_.size(); ++i)
        {
          const WritebackEntry &entry = current_writebacks_[i];

          if (!entry.valid || entry.state == OwnerState::kAckReady)
          {
            continue;
          }

          const std::uint64_t entry_end =
              static_cast<std::uint64_t>(entry.address) + entry.size;
          const bool overlap =
              static_cast<std::uint64_t>(range_begin) < entry_end &&
              static_cast<std::uint64_t>(entry.address) < range_end;

          if (!overlap)
          {
            continue;
          }

          if (entry.state == OwnerState::kInFlight ||
              static_cast<int>(i) == granted_writeback)
          {
            plan.stall = LookupStall::kWritebackOrder;
            return plan;
          }

          if (entry.state != OwnerState::kUnsent)
          {
            throw std::runtime_error("invalid writeback state for merge");
          }

          if (!included[i])
          {
            included[i] = true;
            plan.merge_entries.push_back(i);
            range_begin = std::min(range_begin, entry.address);
            range_end = std::max(range_end, entry_end);
            changed = true;
          }
        }
      }

      std::size_t waiter_count = new_waiters;
      for (std::size_t i : plan.merge_entries)
      {
        waiter_count += current_writebacks_[i].waiters.size();
      }

      if (waiter_count > config_.writeback_waiters_per_entry)
      {
        plan.stall = LookupStall::kWritebackWaiterFull;
        return plan;
      }

      if (plan.merge_entries.empty())
      {
        if (FindFreeWriteback() < 0)
        {
          plan.stall = LookupStall::kWritebackFull;
          return plan;
        }

        plan.allowed = true;
        return plan;
      }

      std::size_t survivor = plan.merge_entries.front();
      for (std::size_t i : plan.merge_entries)
      {
        const WritebackEntry &a = current_writebacks_[i];
        const WritebackEntry &b = current_writebacks_[survivor];
        if (std::tie(a.arrival_order, a.lower_request.id) <
            std::tie(b.arrival_order, b.lower_request.id))
        {
          survivor = i;
        }
      }

      plan.survivor = static_cast<int>(survivor);
      plan.allowed = true;
      return plan;
     
    }


    bool OutputWillDrain(bool response_granted) const
    {
      if (!current_output_.valid || !response_granted)
      {
        return false;
      }
      return SelectResponse().owner == ResponseOwner::kOutput;
     
    }


    LookupDecision DecideLookup(bool output_space,
                                int granted_writeback = -1) const
    {
      LookupDecision decision;
      if (!current_lookup_.valid)
      {
        decision.kind = LookupKind::kEmpty;
        return decision;
      }

      const CacheRequest &request = current_lookup_.request;
      const AddressFields &address = current_lookup_.address;

      if (request.type == RequestType::kWriteback)
      {
        const int hit_way = FindHit(address);
        if (hit_way >= 0)
        {
          if (!output_space)
          {
            decision.kind = LookupKind::kHold;
            decision.stall = LookupStall::kOutput;
            return decision;
          }
          decision.kind = LookupKind::kHit;
          decision.hit_way = hit_way;
          return decision;
        }

        const WritebackPlan plan = PlanWriteback(
            request.address, request.size, 1, granted_writeback);
        if (!plan.allowed)
        {
          decision.kind = LookupKind::kHold;
          decision.stall = plan.stall;
          return decision;
        }

        decision.kind = LookupKind::kWritebackForward;
        return decision;
      }

      // Demand Read or Write
      const int hit_way = FindHit(address);
      if (hit_way >= 0)
      {
        if (!output_space)
        {
          decision.kind = LookupKind::kHold;
          decision.stall = LookupStall::kOutput;
          return decision;
        }
        decision.kind = LookupKind::kHit;
        decision.hit_way = hit_way;
        return decision;
      }

      const int existing_mshr = FindMshrByBlock(address.block);
      if (existing_mshr >= 0)
      {
        if (current_mshrs_[static_cast<std::size_t>(existing_mshr)].waiters.size() >=
            config_.mshr_waiters_per_entry)
        {
          decision.kind = LookupKind::kHold;
          decision.stall = LookupStall::kMshrWaiterFull;
          return decision;
        }
        decision.kind = LookupKind::kMshrMerge;
        decision.mshr = existing_mshr;
        return decision;
      }

      const int free_mshr = FindFreeMshr();
      if (free_mshr < 0)
      {
        decision.kind = LookupKind::kHold;
        decision.stall = LookupStall::kMshrFull;
        return decision;
      }

      const int victim_way = PeekVictim(address.set);
      if (victim_way < 0)
      {
        decision.kind = LookupKind::kHold;
        decision.stall = LookupStall::kReservedWay;
        return decision;
      }

      const CacheLine &victim_line = current_lines_[address.set][victim_way];
      if (victim_line.valid && victim_line.dirty)
      {
        const std::uint32_t v_block =
            (static_cast<std::uint64_t>(victim_line.tag) * config_.sets + address.set) * config_.line_size;
        const WritebackPlan plan =
            PlanWriteback(v_block, config_.line_size, 0, granted_writeback);
        if (!plan.allowed)
        {
          decision.kind = LookupKind::kHold;
          decision.stall = plan.stall;
          return decision;
        }
      }

      decision.kind = LookupKind::kMshrAllocate;
      decision.mshr = free_mshr;
      decision.victim_way = victim_way;
      return decision;
      

    }


    CacheStageIntent StageIntent(const CacheRequest *upper_request,
                                 const CacheResponse *lower_response,
                                 bool response_granted,
                                 bool lower_request_granted) const
    {
      (void)upper_request;
      CacheStageIntent intent;
      bool fill_owns_lookup = false;

      if (lower_response != nullptr)
      {
        int mshr_idx = FindMshrByLowerId(lower_response->id);
        if (mshr_idx >= 0 && current_mshrs_[mshr_idx].state == OwnerState::kInFlight &&
            lower_response->type == ResponseType::kReadData)
        {
          ValidateEcho(*lower_response, current_mshrs_[mshr_idx].lower_request, ResponseType::kReadData);
          intent.accept_lower_response = true;
          fill_owns_lookup = true;
        }
        else
        {
          int wb_idx = FindWritebackByLowerId(lower_response->id);
          if (wb_idx >= 0 && current_writebacks_[wb_idx].state == OwnerState::kInFlight &&
              lower_response->type == ResponseType::kWritebackAck)
          {
            ValidateEcho(*lower_response, current_writebacks_[wb_idx].lower_request, ResponseType::kWritebackAck);
            intent.accept_lower_response = true;
          }
        }
      }

      int granted_wb = -1;
      if (lower_request_granted)
      {
        RequestChoice req_choice = SelectLowerRequest();
        if (req_choice.owner == RequestOwner::kWriteback)
        {
          granted_wb = static_cast<int>(req_choice.entry);
        }
      }

      bool lookup_will_vacate = false;
      if (fill_owns_lookup)
      {
        lookup_will_vacate = false;
      }
      else if (!current_lookup_.valid)
      {
        lookup_will_vacate = true;
      }
      else
      {
        bool output_space = !current_output_.valid || OutputWillDrain(response_granted);
        LookupDecision decision = DecideLookup(output_space, granted_wb);
        lookup_will_vacate = (decision.kind != LookupKind::kHold && decision.kind != LookupKind::kEmpty);
      }

      intent.accept_upper_request = !current_index_.valid || lookup_will_vacate;
      return intent;

    }


    MshrWaiter MakeMshrWaiter(const PipelineEntry &pipeline) const
    {
      MshrWaiter waiter;
      waiter.request_id = pipeline.request.id;
      waiter.origin_id = pipeline.request.origin_id;
      waiter.sequence = pipeline.request.sequence;
      waiter.arrival_order = pipeline.arrival_order;
      waiter.generation = pipeline.request.generation;
      waiter.type = pipeline.request.type;
      waiter.address = pipeline.request.address;
      waiter.size = pipeline.request.size;
      waiter.data = pipeline.request.data;
      waiter.response_ready = false;
      waiter.response_sent = false;
      waiter.response_data.clear();
      return waiter;
  
    }

    WritebackAckWaiter MakeWritebackWaiter(
        const PipelineEntry &pipeline) const
    {
      if (!pipeline.valid || pipeline.request.type != RequestType::kWriteback)
      {
        throw std::runtime_error("invalid writeback waiter source");
      }
      WritebackAckWaiter waiter;
      waiter.request_id = pipeline.request.id;
      waiter.origin_id = pipeline.request.origin_id;
      waiter.sequence = pipeline.request.sequence;
      waiter.arrival_order = pipeline.arrival_order;
      waiter.generation = pipeline.request.generation;
      waiter.address = pipeline.request.address;
      waiter.size = pipeline.request.size;
      waiter.response_sent = false;
      return waiter;
   
    }

    std::uint64_t AllocateArrival()
    {
      if (next_arrival_ == std::numeric_limits<std::uint64_t>::max())
      {
        throw std::runtime_error("cache arrival-order counter overflow");
      }
      return next_arrival_++;
    }


    CacheResponse ResponseForHit(const PipelineEntry &pipeline,
                                 const CacheLine &line) const
    {
      CacheResponse response;
      response.id = pipeline.request.id;
      response.origin_id = pipeline.request.origin_id;
      response.sequence = pipeline.request.sequence;
      response.generation = pipeline.request.generation;
      response.address = pipeline.request.address;
      response.size = pipeline.request.size;

      switch (pipeline.request.type)
      {
      case RequestType::kRead:
        response.type = ResponseType::kReadData;
        response.data = ReadBytes(line.data, pipeline.address.block,
                                  pipeline.request.address,
                                  pipeline.request.size);
        break;
      case RequestType::kWrite:
        response.type = ResponseType::kWriteAck;
        break;
      case RequestType::kWriteback:
        response.type = ResponseType::kWritebackAck;
        break;
      }
      return response;
 
    }


    ResponseChoice SelectResponse() const
    {
      ResponseChoice best; // starts as owner = kNone by default (nothing selected yet)

      if (current_output_.valid)
      {
        ResponseChoice candidate;
        candidate.owner = ResponseOwner::kOutput;
        candidate.response = current_output_.response;
        candidate.arrival_order = current_output_.arrival_order;
        best = candidate;
      }

      for (std::size_t i = 0; i < current_mshrs_.size(); ++i)
      {
        const MshrEntry &mshr = current_mshrs_[i];
        if (!mshr.valid)
          continue;
        ResponseChoice mshr_best;
        for (std::size_t w = 0; w < mshr.waiters.size(); ++w)
        {
          const MshrWaiter &waiter = mshr.waiters[w];
          if (waiter.response_ready && !waiter.response_sent)
          {
            CacheResponse response;
            response.id = waiter.request_id;
            response.origin_id = waiter.origin_id;
            response.sequence = waiter.sequence;
            response.generation = waiter.generation;
            response.address = waiter.address;
            response.size = waiter.size;
            if (waiter.type == RequestType::kRead)
            {
              response.type = ResponseType::kReadData;
              response.data = waiter.response_data;
            }
            else
            {
              response.type = ResponseType::kWriteAck;
            }
            ResponseChoice candidate;
            candidate.owner = ResponseOwner::kMshr;
            candidate.entry = i;
            candidate.waiter = w;
            candidate.response = response;
            candidate.arrival_order = waiter.arrival_order;
            if (mshr_best.owner == ResponseOwner::kNone || ResponseLess(candidate, mshr_best))
            {
              mshr_best = candidate;
            }
          }
        }
        if (mshr_best.owner != ResponseOwner::kNone &&
            (best.owner == ResponseOwner::kNone || ResponseLess(mshr_best, best)))
        {
          best = mshr_best;
        }
      }

      for (std::size_t i = 0; i < current_writebacks_.size(); ++i)
      {
        const WritebackEntry &wb = current_writebacks_[i];
        if (!wb.valid || wb.state != OwnerState::kAckReady)
          continue;
        ResponseChoice wb_best;
        for (std::size_t w = 0; w < wb.waiters.size(); ++w)
        {
          const WritebackAckWaiter &waiter = wb.waiters[w];
          if (!waiter.response_sent)
          {
            CacheResponse response;
            response.id = waiter.request_id;
            response.origin_id = waiter.origin_id;
            response.sequence = waiter.sequence;
            response.generation = waiter.generation;
            response.address = waiter.address;
            response.size = waiter.size;
            response.type = ResponseType::kWritebackAck;
            ResponseChoice candidate;
            candidate.owner = ResponseOwner::kWriteback;
            candidate.entry = i;
            candidate.waiter = w;
            candidate.response = response;
            candidate.arrival_order = waiter.arrival_order;
            if (wb_best.owner == ResponseOwner::kNone || ResponseLess(candidate, wb_best))
            {
              wb_best = candidate;
            }
          }
        }
        if (wb_best.owner != ResponseOwner::kNone &&
            (best.owner == ResponseOwner::kNone || ResponseLess(wb_best, best)))
        {
          best = wb_best;
        }
      }

      return best;

      
    }

   
    bool ConflictsWithLowerIncompleteWriteback(const CacheRequest &request) const
    {
      for (const WritebackEntry &entry : current_writebacks_)
      {
        if (!entry.valid)
          continue;
        if (entry.state == OwnerState::kAckReady)
          continue;
        if (RangesOverlap(entry.address, entry.size, request.address, request.size))
        {
          return true;
        }
      }
      return false;

    }

   
    RequestChoice SelectLowerRequest() const
    {
      RequestChoice choice;

      int best_mshr = -1;
      for (std::size_t i = 0; i < current_mshrs_.size(); ++i)
      {
        const MshrEntry &entry = current_mshrs_[i];
        if (!entry.valid || entry.state != OwnerState::kUnsent)
          continue;
        if (ConflictsWithLowerIncompleteWriteback(entry.lower_request))
        {
          choice.demand_blocked_by_writeback = true;
          continue;
        }
        if (best_mshr < 0)
        {
          best_mshr = static_cast<int>(i);
          continue;
        }
        const MshrEntry &best = current_mshrs_[static_cast<std::size_t>(best_mshr)];
        if (std::tie(entry.arrival_order, entry.lower_request.id) < std::tie(best.arrival_order, best.lower_request.id))
        {
          best_mshr = static_cast<int>(i);
        }
      }

      int best_writeback = -1;
      for (std::size_t i = 0; i < current_writebacks_.size(); ++i)
      {
        const WritebackEntry &entry = current_writebacks_[i];
        if (!entry.valid || entry.state != OwnerState::kUnsent)
          continue;

        if (best_writeback < 0)
        {
          best_writeback = static_cast<int>(i);
          continue;
        }
        const WritebackEntry &best =
            current_writebacks_[static_cast<std::size_t>(best_writeback)];
        if (std::tie(entry.arrival_order, entry.lower_request.id) < std::tie(best.arrival_order, best.lower_request.id))
        {
          best_writeback = static_cast<int>(i);
        }
      }

      if (best_writeback >= 0)
      {
        choice.oldest_eligible_writeback = static_cast<std::size_t>(best_writeback);
      }
      if (best_mshr >= 0)
      {
        choice.demand_blocked_by_writeback = false;
      }

      const bool writeback_buffer_full = (FindFreeWriteback() < 0);
      const bool writeback_starved =
          best_writeback >= 0 &&
          current_writebacks_[static_cast<std::size_t>(best_writeback)].demand_bypasses >= 8;

      const bool writeback_must_win =
          best_writeback >= 0 && (writeback_buffer_full || writeback_starved);

      if (best_mshr >= 0 && !writeback_must_win)
      {
        choice.owner = RequestOwner::kMshr;
        choice.entry = static_cast<std::size_t>(best_mshr);
        choice.request = current_mshrs_[static_cast<std::size_t>(best_mshr)].lower_request;
        choice.arrival_order = current_mshrs_[static_cast<std::size_t>(best_mshr)].arrival_order;
        return choice;
      }

      if (best_writeback >= 0)
      {
        choice.owner = RequestOwner::kWriteback;
        choice.entry = static_cast<std::size_t>(best_writeback);
        choice.request = current_writebacks_[static_cast<std::size_t>(best_writeback)].lower_request;
        choice.arrival_order = current_writebacks_[static_cast<std::size_t>(best_writeback)].arrival_order;
        return choice;
      }

      return choice; // owner stays RequestOwner::kNone — nothing eligible

     
    }

    std::optional<CacheResponse> ResponseIntent() const
    {
      ResponseChoice choice = SelectResponse();
      if (choice.owner == ResponseOwner::kNone)
      {
        return std::nullopt;
      }
      return choice.response;
  
    }



    std::optional<CacheRequest> LowerRequestIntent() const
    {
      const RequestChoice choice = SelectLowerRequest();
      if (choice.owner == RequestOwner::kNone)
      {
        return std::nullopt;
      }
      return choice.request;
   
    }

  
    void ApplyResponseGrant()
    {
      const ResponseChoice choice = SelectResponse();

      switch (choice.owner)
      {
      case ResponseOwner::kNone:
        throw std::runtime_error(
            "upper response grant without response candidate");

      case ResponseOwner::kOutput:
        if (!current_output_.valid)
        {
          throw std::runtime_error("invalid OUTPUT response grant");
        }
        next_output_ = OutputEntry();
        return;

      case ResponseOwner::kMshr:
      {
        if (choice.entry >= current_mshrs_.size() ||
            !current_mshrs_[choice.entry].valid ||
            choice.waiter >= current_mshrs_[choice.entry].waiters.size())
        {
          throw std::runtime_error("invalid MSHR response grant");
        }

        next_mshrs_[choice.entry].waiters[choice.waiter].response_sent = true;

        const bool all_sent = std::all_of(
            next_mshrs_[choice.entry].waiters.begin(),
            next_mshrs_[choice.entry].waiters.end(),
            [](const MshrWaiter &waiter)
            {
              return waiter.response_sent;
            });

        if (all_sent)
        {
          const MshrEntry &entry = next_mshrs_[choice.entry];
          if (entry.set >= next_lines_.size() ||
              entry.reserved_way >= next_lines_[entry.set].size())
          {
            throw std::runtime_error("invalid MSHR reservation");
          }
          next_lines_[entry.set][entry.reserved_way].reserved = false;
          next_mshrs_[choice.entry] = MshrEntry();
        }
        return;
      }

      case ResponseOwner::kWriteback:
      {
        if (choice.entry >= current_writebacks_.size() ||
            !current_writebacks_[choice.entry].valid ||
            choice.waiter >=
                current_writebacks_[choice.entry].waiters.size())
        {
          throw std::runtime_error("invalid writeback response grant");
        }

        next_writebacks_[choice.entry]
            .waiters[choice.waiter]
            .response_sent = true;

        const bool all_sent = std::all_of(
            next_writebacks_[choice.entry].waiters.begin(),
            next_writebacks_[choice.entry].waiters.end(),
            [](const WritebackAckWaiter &waiter)
            {
              return waiter.response_sent;
            });

        if (all_sent)
        {
          next_writebacks_[choice.entry] = WritebackEntry();
        }
        return;
      }
      }

      throw std::runtime_error("invalid response owner");

    
    }

    void ValidateEcho(const CacheResponse &response,
                      const CacheRequest &request,
                      ResponseType expected_type) const
    {
      if (response.id != request.id ||
          response.origin_id != request.origin_id ||
          response.sequence != request.sequence ||
          response.generation != request.generation ||
          response.address != request.address || response.size != request.size ||
          response.type != expected_type)
      {
        throw std::runtime_error("lower response metadata does not echo request");
      }
    }

 
    void ApplyLowerResponse(const CacheResponse &response)
    {
      const int mshr_index = FindMshrByLowerId(response.id);
      if (mshr_index >= 0)
      {
        MshrEntry &mshr = next_mshrs_[static_cast<std::size_t>(mshr_index)];
        ValidateEcho(response, mshr.lower_request, ResponseType::kReadData);

        CacheLine &line = next_lines_[mshr.set][mshr.reserved_way];
        line.valid = true;
        line.tag = mshr.tag;
        line.data = response.data;

        const std::uint32_t block_address = mshr.block_address;
        bool any_write = false;

        for (MshrWaiter &waiter : mshr.waiters)
        {
          if (waiter.type == RequestType::kWrite)
          {
            WriteBytes(&line.data, block_address, waiter.address, waiter.data);
            any_write = true;
            waiter.response_data.clear();
          }
          else
          {
            waiter.response_data =
                ReadBytes(line.data, block_address, waiter.address, waiter.size);
          }
          waiter.response_ready = true;
        }

        line.dirty = any_write;
        replacement_->OnInsert(mshr.set, mshr.reserved_way);
        mshr.state = OwnerState::kResponding;
        CheckedIncrement(&next_stats_.fills, "fills");
        return;
      }

      const int writeback_index = FindWritebackByLowerId(response.id);
      if (writeback_index >= 0)
      {
        WritebackEntry &entry =
            next_writebacks_[static_cast<std::size_t>(writeback_index)];
        ValidateEcho(response, entry.lower_request, ResponseType::kWritebackAck);

        if (entry.waiters.empty())
        {
          entry = WritebackEntry();
        }
        else
        {
          entry.state = OwnerState::kAckReady;
        }
        return;
      }

      throw std::runtime_error("lower response matches no owned MSHR or writeback");
 
    }

    void InsertWriteback(std::uint32_t address,
                         const std::vector<std::uint8_t> &data,
                         std::uint64_t arrival_order,
                         const std::optional<WritebackAckWaiter> &waiter,
                         int granted_writeback = -1,
                         std::optional<std::uint64_t> preallocated_id =
                             std::nullopt)
    {
      if (data.empty())
      {
        throw std::runtime_error("cannot insert empty writeback");
      }

      const std::uint32_t size = static_cast<std::uint32_t>(data.size());
      const WritebackPlan plan =
          PlanWriteback(address, size, waiter ? 1U : 0U, granted_writeback);

      if (!plan.allowed)
      {
        throw std::runtime_error("writeback insertion attempted while blocked");
      }

      if (plan.merge_entries.empty())
      {
        const int free_entry = FindFreeWriteback();
        if (free_entry < 0)
        {
          throw std::runtime_error("writeback insertion without free entry");
        }

        WritebackEntry entry;
        entry.valid = true;
        entry.address = address;
        entry.size = size;
        entry.data = data;
        entry.arrival_order = arrival_order;
        entry.state = OwnerState::kUnsent;
        entry.demand_bypasses = 0;

        entry.lower_request.id =
            preallocated_id.has_value()
                ? *preallocated_id
                : transaction_ids_->Reserve(SourceForRole(role_));
        entry.lower_request.origin_id = kNoOriginId;
        entry.lower_request.sequence = kNoSequence;
        entry.lower_request.generation = 0;
        entry.lower_request.address = address;
        entry.lower_request.size = size;
        entry.lower_request.type = RequestType::kWriteback;
        entry.lower_request.data = data;

        if (waiter.has_value())
        {
          entry.waiters.push_back(*waiter);
        }

        next_writebacks_[static_cast<std::size_t>(free_entry)] =
            std::move(entry);

        if (!waiter.has_value())
        {
          CheckedIncrement(&next_stats_.writebacks_generated,
                           "writebacks_generated");
        }
        return;
      }

      if (plan.survivor < 0)
      {
        throw std::runtime_error("writeback merge without survivor");
      }

      std::vector<std::size_t> ordered = plan.merge_entries;
      std::sort(
          ordered.begin(), ordered.end(),
          [&](std::size_t a, std::size_t b)
          {
            return std::tie(current_writebacks_[a].arrival_order,
                            current_writebacks_[a].lower_request.id) <
                   std::tie(current_writebacks_[b].arrival_order,
                            current_writebacks_[b].lower_request.id);
          });

      std::uint32_t merged_begin = address;
      std::uint64_t merged_end =
          static_cast<std::uint64_t>(address) + size;

      for (std::size_t i : ordered)
      {
        const WritebackEntry &entry = current_writebacks_[i];
        merged_begin = std::min(merged_begin, entry.address);
        merged_end =
            std::max(merged_end,
                     static_cast<std::uint64_t>(entry.address) + entry.size);
      }

      const std::uint32_t merged_size =
          static_cast<std::uint32_t>(merged_end - merged_begin);
      std::vector<std::uint8_t> merged_data(merged_size, 0);

      for (std::size_t i : ordered)
      {
        const WritebackEntry &entry = current_writebacks_[i];
        const std::size_t offset =
            static_cast<std::size_t>(entry.address - merged_begin);
        for (std::size_t j = 0; j < entry.data.size(); ++j)
        {
          merged_data[offset + j] = entry.data[j];
        }
      }

      const std::size_t incoming_offset =
          static_cast<std::size_t>(address - merged_begin);
      for (std::size_t i = 0; i < data.size(); ++i)
      {
        merged_data[incoming_offset + i] = data[i];
      }

      WritebackEntry merged =
          current_writebacks_[static_cast<std::size_t>(plan.survivor)];
      merged.address = merged_begin;
      merged.size = merged_size;
      merged.data = merged_data;
      merged.lower_request.address = merged_begin;
      merged.lower_request.size = merged_size;
      merged.lower_request.data = merged_data;

      std::vector<WritebackAckWaiter> waiters;
      for (std::size_t i : ordered)
      {
        waiters.insert(waiters.end(),
                       current_writebacks_[i].waiters.begin(),
                       current_writebacks_[i].waiters.end());
      }
      if (waiter.has_value())
      {
        waiters.push_back(*waiter);
      }

      std::sort(
          waiters.begin(), waiters.end(),
          [](const WritebackAckWaiter &a,
             const WritebackAckWaiter &b)
          {
            return std::tie(a.arrival_order, a.request_id) <
                   std::tie(b.arrival_order, b.request_id);
          });
      merged.waiters = std::move(waiters);

      for (std::size_t i : plan.merge_entries)
      {
        if (static_cast<int>(i) != plan.survivor)
        {
          next_writebacks_[i] = WritebackEntry();
        }
      }

      next_writebacks_[static_cast<std::size_t>(plan.survivor)] =
          std::move(merged);

      CheckedIncrement(&next_stats_.writeback_merges,
                       "writeback_merges");

      if (!waiter.has_value())
      {
        CheckedIncrement(&next_stats_.writebacks_generated,
                         "writebacks_generated");
      }


    }


    void ApplyHit(const LookupDecision &decision)
    {
      const AddressFields &address = current_lookup_.address;
      CacheLine &line = next_lines_[address.set][static_cast<std::size_t>(decision.hit_way)];
      const CacheRequest &request = current_lookup_.request;

      if (request.type == RequestType::kWrite || request.type == RequestType::kWriteback)
      {
        WriteBytes(&line.data, address.block, request.address, request.data);
        line.dirty = true;
      }

      const CacheResponse response = ResponseForHit(current_lookup_, line);

      next_output_.valid = true;
      next_output_.response = response;
      next_output_.arrival_order = current_lookup_.arrival_order;

      replacement_->OnHit(address.set, static_cast<std::uint32_t>(decision.hit_way));

      if (request.type == RequestType::kWriteback)
      {
        next_stats_.writeback_hits += 1;
      }
      else
      {
        next_stats_.hits += 1;
      }

      next_lookup_ = PipelineEntry();
 
    }


    void ApplyMshrMerge(int mshr_index)
    {
      MshrEntry &mshr = next_mshrs_[static_cast<std::size_t>(mshr_index)];
      mshr.waiters.push_back(MakeMshrWaiter(current_lookup_));
      next_stats_.mshr_merges += 1;
      next_lookup_ = PipelineEntry();
 
    }

 
    void ApplyMshrAllocation(const LookupDecision &decision,
                             int granted_writeback)
    {
      const AddressFields &address = current_lookup_.address;
      const int reserved_way = SelectVictim(address.set);
      CacheLine &victim_line = next_lines_[address.set][static_cast<std::size_t>(reserved_way)];

      const bool was_valid = victim_line.valid;
      const bool was_dirty = victim_line.dirty;

      if (was_valid)
      {
        CheckedIncrement(&next_stats_.evictions, "evictions");
        if (was_dirty)
        {
          CheckedIncrement(&next_stats_.dirty_evictions, "dirty_evictions");
        }
      }

      const std::uint32_t victim_block_address =
          (static_cast<std::uint64_t>(victim_line.tag) * config_.sets + address.set) * config_.line_size;

      const std::vector<std::uint8_t> victim_data = victim_line.data;

      // Invalidate and clear victim way immediately upon reservation
      victim_line.valid = false;
      victim_line.dirty = false;
      victim_line.tag = 0;
      victim_line.data.assign(config_.line_size, 0);

      std::uint64_t mshr_id = 0;
      std::optional<std::uint64_t> wb_id = std::nullopt;

      if (was_valid && was_dirty)
      {
        WritebackPlan wb_plan =
            PlanWriteback(victim_block_address, config_.line_size, 0, granted_writeback);
        if (wb_plan.survivor < 0)
        {
          auto batch = transaction_ids_->ReserveBatch(SourceForRole(role_), 2);
          mshr_id = batch[0];
          wb_id = batch[1];
        }
        else
        {
          mshr_id = transaction_ids_->Reserve(SourceForRole(role_));
        }
        InsertWriteback(victim_block_address, victim_data, // <--- Pass saved victim_data!
                        current_lookup_.arrival_order, std::nullopt,
                        granted_writeback, wb_id);
      }
      else
      {
        mshr_id = transaction_ids_->Reserve(SourceForRole(role_));
      }

      MshrEntry &mshr = next_mshrs_[static_cast<std::size_t>(decision.mshr)];
      mshr.valid = true;
      mshr.block_address = address.block;
      mshr.set = address.set;
      mshr.tag = address.tag;
      mshr.reserved_way = static_cast<std::uint32_t>(reserved_way);
      mshr.arrival_order = current_lookup_.arrival_order;
      mshr.state = OwnerState::kUnsent;
      mshr.waiters.clear();
      mshr.waiters.push_back(MakeMshrWaiter(current_lookup_));

      CacheRequest lower_read;
      lower_read.id = mshr_id;
      lower_read.origin_id = current_lookup_.request.origin_id;
      lower_read.sequence = current_lookup_.request.sequence;
      lower_read.generation = current_lookup_.request.generation;
      lower_read.address = address.block;
      lower_read.size = config_.line_size;
      lower_read.type = RequestType::kRead;
      mshr.lower_request = lower_read;

      CheckedIncrement(&next_stats_.primary_misses, "primary_misses");
      next_lookup_ = PipelineEntry();

    }



    void ApplyWritebackForward(int granted_writeback)
    {
      if (!current_lookup_.valid || current_lookup_.request.type != RequestType::kWriteback)
      {
        throw std::runtime_error(
            "writeback forward without writeback in LOOKUP");
      }
      const WritebackAckWaiter waiter = MakeWritebackWaiter(current_lookup_);
      InsertWriteback(current_lookup_.request.address,
                      current_lookup_.request.data,
                      current_lookup_.arrival_order,
                      waiter,
                      granted_writeback);
      next_lookup_ = PipelineEntry();
 
    }


    void CountLookupStall(LookupStall stall)
    {
      switch (stall)
      {
      case LookupStall::kNone:
      case LookupStall::kOutput:
        return;

      case LookupStall::kMshrWaiterFull:
        CheckedIncrement(&next_stats_.mshr_waiter_full_stalls,
                         "mshr_waiter_full_stalls");
        return;

      case LookupStall::kMshrFull:
        CheckedIncrement(&next_stats_.mshr_full_stalls,
                         "mshr_full_stalls");
        return;

      case LookupStall::kReservedWay:
        CheckedIncrement(&next_stats_.reserved_way_stalls,
                         "reserved_way_stalls");
        return;

      case LookupStall::kWritebackOrder:
        CheckedIncrement(&next_stats_.writeback_order_stalls,
                         "writeback_order_stalls");
        return;

      case LookupStall::kWritebackWaiterFull:
        CheckedIncrement(&next_stats_.writeback_waiter_full_stalls,
                         "writeback_waiter_full_stalls");
        return;

      case LookupStall::kWritebackFull:
        CheckedIncrement(&next_stats_.writeback_full_stalls,
                         "writeback_full_stalls");
        return;
      }

    }


    void ApplyLowerRequestGrant()
    {
      const RequestChoice choice = SelectLowerRequest();

      switch (choice.owner)
      {
      case RequestOwner::kNone:
        throw std::runtime_error(
            "lower request grant without request candidate");

      case RequestOwner::kMshr:
      {
        if (choice.entry >= current_mshrs_.size() ||
            !current_mshrs_[choice.entry].valid ||
            current_mshrs_[choice.entry].state != OwnerState::kUnsent)
        {
          throw std::runtime_error("invalid MSHR lower-request grant");
        }

        next_mshrs_[choice.entry].state = OwnerState::kInFlight;

        if (choice.oldest_eligible_writeback.has_value())
        {
          const std::size_t index = *choice.oldest_eligible_writeback;

          if (index >= next_writebacks_.size() ||
              !next_writebacks_[index].valid)
          {
            throw std::runtime_error("invalid bypassed writeback");
          }

          next_writebacks_[index].demand_bypasses =
              std::min<std::uint32_t>(
                  8U,
                  next_writebacks_[index].demand_bypasses + 1U);
        }
        return;
      }

      case RequestOwner::kWriteback:
        if (choice.entry >= current_writebacks_.size() ||
            !current_writebacks_[choice.entry].valid ||
            current_writebacks_[choice.entry].state != OwnerState::kUnsent)
        {
          throw std::runtime_error("invalid writeback lower-request grant");
        }

        next_writebacks_[choice.entry].state = OwnerState::kInFlight;
        next_writebacks_[choice.entry].demand_bypasses = 0;
        CheckedIncrement(&next_stats_.writebacks_sent,
                         "writebacks_sent");
        return;
      }

      throw std::runtime_error("invalid lower request owner");


    }


    void Operate(std::uint64_t current_cycle,
                 const CacheRequest *upper_request,
                 const CacheResponse *lower_response,
                 const CacheOperateGrants &grants)
    {
      (void)current_cycle;
      if (!begun_ || operated_)
      {
        throw std::runtime_error("cache Operate lifecycle violation");
      }

      const std::optional<CacheResponse> response_intent = ResponseIntent();
      const std::optional<CacheRequest> lower_intent = LowerRequestIntent();
      const CacheStageIntent stage =
          StageIntent(upper_request, lower_response,
                      grants.upper_response, grants.lower_request);

      if (grants.upper_response && !response_intent.has_value())
      {
        throw std::runtime_error(
            "upper response grant without response intent");
      }

      if (grants.lower_request && !lower_intent.has_value())
      {
        throw std::runtime_error(
            "lower request grant without request intent");
      }

      if (grants.upper_request &&
          (upper_request == nullptr || !stage.accept_upper_request))
      {
        throw std::runtime_error(
            "upper request grant without accepted request");
      }

      if (grants.lower_response &&
          (lower_response == nullptr || !stage.accept_lower_response))
      {
        throw std::runtime_error(
            "lower response grant without accepted response");
      }

      int granted_writeback = -1;
      if (grants.lower_request)
      {
        const RequestChoice choice = SelectLowerRequest();
        if (choice.owner == RequestOwner::kWriteback)
        {
          granted_writeback = static_cast<int>(choice.entry);
        }
      }

      if (grants.upper_response)
      {
        ApplyResponseGrant();
      }

      bool fill_uses_lookup = false;
      if (grants.lower_response)
      {
        fill_uses_lookup = (lower_response->type == ResponseType::kReadData) &&
                           (FindMshrByLowerId(lower_response->id) >= 0);
        ApplyLowerResponse(*lower_response);
      }

      bool lookup_vacates = !current_lookup_.valid;

      if (!fill_uses_lookup && current_lookup_.valid)
      {
        const bool output_space =
            !current_output_.valid ||
            OutputWillDrain(grants.upper_response);
        const LookupDecision decision =
            DecideLookup(output_space, granted_writeback);

        switch (decision.kind)
        {
        case LookupKind::kEmpty:
          lookup_vacates = true;
          next_lookup_ = PipelineEntry();
          break;

        case LookupKind::kHit:
          ApplyHit(decision);
          lookup_vacates = true;
          break;

        case LookupKind::kMshrMerge:
          ApplyMshrMerge(decision.mshr);
          lookup_vacates = true;
          break;

        case LookupKind::kMshrAllocate:
          ApplyMshrAllocation(decision, granted_writeback);
          lookup_vacates = true;
          break;

        case LookupKind::kWritebackForward:
          ApplyWritebackForward(granted_writeback);
          lookup_vacates = true;
          break;

        case LookupKind::kHold:
          CountLookupStall(decision.stall);
          lookup_vacates = false;
          break;
        }
      }

      if (current_index_.valid && lookup_vacates)
      {
        next_lookup_ = current_index_;
        next_index_ = PipelineEntry();
      }

      if (grants.upper_request)
      {
        ValidateInbound(*upper_request);

        PipelineEntry entry;
        entry.valid = true;
        entry.request = *upper_request;
        entry.arrival_order = AllocateArrival();
        entry.address = MapAddress(upper_request->address);
        next_index_ = std::move(entry);

        if (upper_request->type == RequestType::kWriteback)
        {
          CheckedIncrement(&next_stats_.writeback_requests_accepted,
                           "writeback_requests_accepted");
        }
        else
        {
          CheckedIncrement(&next_stats_.demand_accesses_accepted,
                           "demand_accesses_accepted");
        }
      }

      if (grants.lower_request)
      {
        ApplyLowerRequestGrant();
      }

      operated_ = true;

 
    }

    void Commit()
    {
      if (!begun_ || !operated_)
      {
        throw std::runtime_error("cache Commit lifecycle violation");
      }
      replacement_->Commit();
      current_lines_ = std::move(next_lines_);
      current_index_ = std::move(next_index_);
      current_lookup_ = std::move(next_lookup_);
      current_output_ = std::move(next_output_);
      current_mshrs_ = std::move(next_mshrs_);
      current_writebacks_ = std::move(next_writebacks_);
      current_arrival_ = next_arrival_;
      stats_ = next_stats_;
      begun_ = false;
      operated_ = false;
    }

    CacheSnapshot Snapshot() const
    {
      CacheSnapshot snapshot;
      const PipelineEntry entries[2] = {current_index_, current_lookup_};
      for (int i = 0; i < 2; ++i)
      {
        snapshot.pipeline[i].valid = entries[i].valid;
        if (entries[i].valid)
        {
          snapshot.pipeline[i].request_id = entries[i].request.id;
          snapshot.pipeline[i].address = entries[i].request.address;
          snapshot.pipeline[i].state = i == 0 ? "INDEX" : "LOOKUP";
        }
      }
      snapshot.pipeline[2].valid = current_output_.valid;
      if (current_output_.valid)
      {
        snapshot.pipeline[2].request_id = current_output_.response.id;
        snapshot.pipeline[2].address = current_output_.response.address;
        snapshot.pipeline[2].state = "OUTPUT";
      }
      for (const MshrEntry &entry : current_mshrs_)
      {
        MshrSnapshot row;
        row.waiter_capacity = config_.mshr_waiters_per_entry;
        if (entry.valid)
        {
          row.valid = true;
          row.block_address = entry.block_address;
          row.set = entry.set;
          row.tag = entry.tag;
          row.reserved_way = static_cast<int>(entry.reserved_way);
          row.lower_id = entry.lower_request.id;
          row.arrival_order = entry.arrival_order;
          row.state = OwnerStateName(entry.state);
          row.waiters = entry.waiters;
        }
        snapshot.mshrs.push_back(std::move(row));
      }
      for (const WritebackEntry &entry : current_writebacks_)
      {
        WritebackSnapshot row;
        row.waiter_capacity = config_.writeback_waiters_per_entry;
        if (entry.valid)
        {
          row.valid = true;
          row.address = entry.address;
          row.size = entry.size;
          row.lower_id = entry.lower_request.id;
          row.arrival_order = entry.arrival_order;
          row.demand_bypasses = entry.demand_bypasses;
          row.state = OwnerStateName(entry.state);
          row.data = entry.data;
          row.waiters = entry.waiters;
        }
        snapshot.writebacks.push_back(std::move(row));
      }
      for (std::uint32_t set = 0; set < config_.sets; ++set)
      {
        const std::vector<std::uint32_t> replacement =
            replacement_->DebugState(set);
        snapshot.replacement.push_back({set, replacement});
        for (std::uint32_t way = 0; way < config_.ways; ++way)
        {
          std::uint64_t owner = kNoOriginId;
          for (const MshrEntry &mshr : current_mshrs_)
          {
            if (mshr.valid && mshr.set == set && mshr.reserved_way == way)
            {
              owner = mshr.lower_request.id;
            }
          }
          snapshot.lines.push_back(
              {set, way, current_lines_[set][way], owner});
        }
      }
      return snapshot;
    }

    bool HasWork() const
    {
      if (current_index_.valid || current_lookup_.valid || current_output_.valid)
      {
        return true;
      }
      return std::any_of(current_mshrs_.begin(), current_mshrs_.end(),
                         [](const MshrEntry &entry)
                         { return entry.valid; }) ||
             std::any_of(current_writebacks_.begin(), current_writebacks_.end(),
                         [](const WritebackEntry &entry)
                         { return entry.valid; });
    }

    CacheRole role_;
    CacheConfig config_;
    TransactionIdService *transaction_ids_;
    std::vector<std::vector<CacheLine>> current_lines_;
    std::vector<std::vector<CacheLine>> next_lines_;
    PipelineEntry current_index_;
    PipelineEntry current_lookup_;
    OutputEntry current_output_;
    PipelineEntry next_index_;
    PipelineEntry next_lookup_;
    OutputEntry next_output_;
    std::vector<MshrEntry> current_mshrs_;
    std::vector<MshrEntry> next_mshrs_;
    std::vector<WritebackEntry> current_writebacks_;
    std::vector<WritebackEntry> next_writebacks_;
    std::uint64_t current_arrival_ = 0;
    std::uint64_t next_arrival_ = 0;
    CacheStats stats_;
    CacheStats next_stats_;
    std::unique_ptr<ReplacementPolicy> replacement_;
    bool begun_ = false;
    bool operated_ = false;
  };
} // namespace pipesim::cache
