#include "execution_unit/internal.h"
#include "execution_unit/helpers.h"

#include <limits>
#include <stdexcept>

namespace pipesim
{

  const char *ExecutionUnitKindName(ExecutionUnitKind kind)
  {
    switch (kind)
    {
    case ExecutionUnitKind::kAdd:
      return "ADD";
    case ExecutionUnitKind::kSub:
      return "SUB";
    case ExecutionUnitKind::kAnd:
      return "AND";
    case ExecutionUnitKind::kOr:
      return "OR";
    case ExecutionUnitKind::kXor:
      return "XOR";
    case ExecutionUnitKind::kControl:
      return "CONTROL";
    case ExecutionUnitKind::kAgu:
      return "AGU";
    }

    return "UNKNOWN";
  }

  ExecutionPipeline::ExecutionPipeline(ExecutionUnitKind kind, const ExecutionUnitConfig &config) : impl_(new Impl(kind, config)) {}

  ExecutionPipeline::~ExecutionPipeline() = default;

 

  void ExecutionPipeline::Reset()
  {

    for (auto &stage : impl_->current)
    {
      stage.reset();
    }

    for (auto &stage : impl_->next)
    {
      stage.reset();
    }

    impl_->next_legal_current = 0;
    impl_->next_legal_next = 0;

    impl_->begun = false;
    impl_->operated = false;
  }

  void ExecutionPipeline::BeginCycle()
  {
    if (impl_->begun)
    {
      throw std::runtime_error(
          "execution pipeline BeginCycle called twice in the same cycle");
    }

    impl_->next = impl_->current;
    impl_->next_legal_next = impl_->next_legal_current;

    impl_->begun = true;
    impl_->operated = false;
  }

  ExecutionUnitKind ExecutionPipeline::kind() const
  {
    return impl_->kind;
  }

  const ExecutionUnitConfig &ExecutionPipeline::config() const
  {
    return impl_->config;
  }



  std::optional<ExecutionResult> ExecutionPipeline::OutputIntent() const
  {
    if (impl_->current.empty())
    {
      return std::nullopt;
    }

    const auto &output_stage = impl_->current.back();

    if (!output_stage.has_value())
    {
      return std::nullopt;
    }

    return impl_->ResultFor(*output_stage);
  }



  bool ExecutionPipeline::CanAccept(
      std::uint64_t dispatch_boundary,
      bool output_will_be_consumed) const
  {
    // Initiation interval has not yet expired.
    if (dispatch_boundary < impl_->next_legal_current)
    {
      return false;
    }

    // Work only with occupancy information.
    std::vector<bool> occupied(impl_->current.size(), false);

    for (std::size_t i = 0; i < impl_->current.size(); ++i)
    {
      occupied[i] = impl_->current[i].has_value();
    }

    // The final stage may leave this cycle.
    if (output_will_be_consumed && !occupied.empty())
    {
      occupied.back() = false;
    }

    for (std::size_t i = occupied.size() - 1; i > 0; --i)
    {
      if (!occupied[i])
      {
        occupied[i] = occupied[i - 1];
        occupied[i - 1] = false;
      }
    }
    // New dispatch can be inserted only when stage 0 will be free.
    return !occupied[0];
  }


  void ExecutionPipeline::Operate(
      std::uint64_t current_boundary,
      const std::optional<ExecutionOperation> &dispatch,
      bool consume_output)
  {
    if (!impl_->begun || impl_->operated)
    {
      throw std::runtime_error(
          "execution pipeline Operate lifecycle violation");
    }

    // A consume grant is only meaningful if a current output exists.
    if (consume_output && !impl_->current.back().has_value())
    {
      throw std::runtime_error(
          "execution pipeline consume requested with no output");
    }

    if (dispatch.has_value())
    {
      if (dispatch->dispatch_boundary != current_boundary + 1U)
      {
        throw std::runtime_error(
            "execution dispatch boundary mismatch");
      }

      if (!execution_internal::OpcodeBelongsTo(
              dispatch->instruction.opcode,
              impl_->kind))
      {
        throw std::runtime_error(
            "opcode dispatched to wrong execution unit");
      }

      if (!CanAccept(dispatch->dispatch_boundary, consume_output))
      {
        throw std::runtime_error(
            "dispatch supplied to execution unit that cannot accept");
      }
    }

    if (consume_output)
    {
      impl_->next.back().reset();
    }

    for (std::size_t i = impl_->next.size(); i-- > 1;)
    {
      if (!impl_->next[i].has_value() &&
          impl_->next[i - 1].has_value())
      {
        impl_->next[i] = impl_->next[i - 1];
        impl_->next[i - 1].reset();
      }
    }

    if (dispatch.has_value())
    {
      if (impl_->next[0].has_value())
      {
        throw std::runtime_error(
            "execution stage zero unexpectedly occupied");
      }

      impl_->next[0] = dispatch;

      const std::uint64_t interval =
          static_cast<std::uint64_t>(
              impl_->config.initiation_interval);

      if (current_boundary >
          std::numeric_limits<std::uint64_t>::max() - interval)
      {
        throw std::runtime_error(
            "execution initiation interval boundary overflow");
      }

      impl_->next_legal_next =
          dispatch->dispatch_boundary + interval;

      Touch();
    }

    impl_->operated = true;
  }



  void ExecutionPipeline::Commit()
  {
    if (!impl_->begun || !impl_->operated)
    {
      throw std::runtime_error(
          "execution pipeline Commit lifecycle violation");
    }

    impl_->current = impl_->next;
    impl_->next_legal_current = impl_->next_legal_next;

    impl_->begun = false;
    impl_->operated = false;
  }

  ExecutionSnapshot ExecutionPipeline::Snapshot() const
  {
    ExecutionSnapshot snapshot;

    snapshot.kind = impl_->kind;
    snapshot.next_legal_dispatch_boundary =
        impl_->next_legal_current;

    snapshot.stages.reserve(impl_->current.size());

    for (std::size_t i = 0; i < impl_->current.size(); ++i)
    {
      ExecutionStageSnapshot stage{};

      stage.stage = static_cast<std::uint32_t>(i);
      stage.unit = ExecutionUnitKindName(impl_->kind);

      if (impl_->current[i].has_value())
      {
        const ExecutionOperation &operation =
            *impl_->current[i];

        const Instruction &instruction =
            operation.instruction;

        stage.valid = true;
        stage.sequence = operation.sequence;
        stage.pc = instruction.pc;
        stage.opcode = OpcodeName(instruction.opcode);

        stage.dispatch_boundary =
            operation.dispatch_boundary;

        stage.nominal_result_boundary =
            execution_internal::CheckedResultBoundary(
                operation.dispatch_boundary,
                impl_->config.latency);

        stage.lhs = operation.lhs;
        stage.rhs = operation.rhs;
        stage.store_value = operation.store_value;

        stage.destination = instruction.rd;

        stage.writes_register =
            execution_internal::WritesRegister(
                instruction.opcode,
                instruction.rd);
      }

      snapshot.stages.push_back(stage);
    }

    return snapshot;
  }

  bool ExecutionPipeline::HasWork() const
  {
    for (const auto &stage : impl_->current)
    {
      if (stage.has_value())
      {
        return true;
      }
    }

    return false;
  }
}