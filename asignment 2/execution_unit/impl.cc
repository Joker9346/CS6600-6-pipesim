#include "execution_unit/internal.h"
#include "execution_unit/helpers.h"

#include <stdexcept>

namespace pipesim
{

  ExecutionPipeline::Impl::Impl( // constructor
      ExecutionUnitKind unit_kind,
      const ExecutionUnitConfig &unit_config)
      : kind(unit_kind),
        config(unit_config)
  {
    if (static_cast<std::size_t>(kind) >
        static_cast<std::size_t>(ExecutionUnitKind::kAgu))
    {
      throw std::runtime_error("invalid execution-unit kind");
    }

    if (config.initiation_interval == 0)
    {
      throw std::runtime_error("zero execution initiation interval");
    }
    const std::size_t stage_count = static_cast<std::size_t>(config.latency) + 1U;
    current.assign(stage_count, std::nullopt);
    next = current;
  }

  ExecutionResult ExecutionPipeline::Impl::ResultFor(
      const ExecutionOperation &operation) const
  {
    ExecutionResult result;

    result.sequence = operation.sequence;
    result.instruction = operation.instruction;
    result.nominal_result_boundary =
        execution_internal::CheckedResultBoundary(
            operation.dispatch_boundary,
            config.latency);

    const Instruction &instruction = operation.instruction;

    switch (kind)
    {
    case ExecutionUnitKind::kAdd:
      result.value = operation.lhs + operation.rhs;
      return result;

    case ExecutionUnitKind::kSub:
      result.value = operation.lhs - operation.rhs;
      return result;

    case ExecutionUnitKind::kAnd:
      result.value = operation.lhs & operation.rhs;
      return result;

    case ExecutionUnitKind::kOr:
      result.value = operation.lhs | operation.rhs;
      return result;

    case ExecutionUnitKind::kXor:
      result.value = operation.lhs ^ operation.rhs;
      return result;

    case ExecutionUnitKind::kControl:
      switch (instruction.opcode)
      {
      case Opcode::kBeq:
        if (operation.lhs == operation.rhs)
        {
          result.redirect = true;
          result.next_pc =
              static_cast<std::uint32_t>(instruction.immediate);
        }
        else
        {
          result.redirect = false;
          result.next_pc = instruction.pc + 4U;
        }
        return result;

      case Opcode::kBne:
        if (operation.lhs != operation.rhs)
        {
          result.redirect = true;
          result.next_pc =
              static_cast<std::uint32_t>(instruction.immediate);
        }
        else
        {
          result.redirect = false;
          result.next_pc = instruction.pc + 4U;
        }
        return result;
      case Opcode::kJ:
        result.redirect = true;
        result.next_pc =
            static_cast<std::uint32_t>(instruction.immediate);
        return result;

      case Opcode::kHalt:
        result.halt = true;
        return result;

      default:
        throw std::runtime_error(
            "invalid opcode for CONTROL execution unit");
      }

    case ExecutionUnitKind::kAgu:
      switch (instruction.opcode)
      {
      case Opcode::kLw:
        result.memory = true;
        result.memory_write = false;
        result.address = operation.lhs + operation.rhs;
        return result;

      case Opcode::kSw:
        result.memory = true;
        result.memory_write = true;
        result.address = operation.lhs + operation.rhs;
        result.store_value = operation.store_value;
        return result;

      default:
        throw std::runtime_error(
            "invalid opcode for AGU execution unit");
      }
    }

    throw std::runtime_error("invalid execution-unit kind");
  }

} // namespace pipesim