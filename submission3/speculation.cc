#include "speculation/internal.h"
#include <algorithm>
#include <stdexcept>

namespace pipesim
{
  SpeculationController::SpeculationController(std::size_t maximum_checkpoints)
      : impl_(new Impl(maximum_checkpoints))
  {
    if (maximum_checkpoints == 0)
    {
      throw std::runtime_error("maximum checkpoints must be positive");
    }
  }

  SpeculationController::~SpeculationController() = default;

  void SpeculationController::Reset()
  {
    impl_->checkpoints.clear();
  }

  bool SpeculationController::CanFetch() const
  {
    return impl_->checkpoints.size() < impl_->maximum_checkpoints;
  }

  bool SpeculationController::CanDispatch(
      std::uint64_t sequence,
      const Instruction &instruction) const
  {
    (void)sequence;
    if (impl_->checkpoints.empty())
    {
      return true;
    }
    switch (instruction.opcode)
    {
    case Opcode::kAdd:
    case Opcode::kAddi:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
      return true;

    case Opcode::kLw:
    case Opcode::kSw:
    case Opcode::kBeq:
    case Opcode::kBne:
    case Opcode::kJ:
    case Opcode::kHalt:
    case Opcode::kNop:
      return false;
    }
    return false;
  }

  void SpeculationController::AddCheckpoint(
      const BranchCheckpoint &checkpoint)
  {
    if (impl_->checkpoints.size() >= impl_->maximum_checkpoints)
    {
      throw std::runtime_error("maximum checkpoints exceeded");
    }
    impl_->checkpoints.push_back(checkpoint);
  }

  bool SpeculationController::IsSpeculative(std::uint64_t sequence) const
  {
    for (const auto &cp : impl_->checkpoints)
    {
      if (cp.sequence < sequence)
        return true;
    }
    return false;
  }

  std::uint64_t SpeculationController::SpeculationDepth(
      std::uint64_t sequence) const
  {
    std::uint64_t depth = 0;
    for (const auto &cp : impl_->checkpoints)
    {
      if (cp.sequence < sequence)
        ++depth;
    }
    return depth;
  }

  RecoveryDecision SpeculationController::Resolve(const BranchResult &result)
  {
    RecoveryDecision decision;
    decision.branch_sequence = result.sequence;

    uint32_t actual_next_pc = result.taken ? result.target : result.pc + 4;
    decision.correct_next_pc = actual_next_pc;

    auto it = std::find_if(impl_->checkpoints.begin(), impl_->checkpoints.end(),
                           [&](const BranchCheckpoint &cp)
                           {
                             return cp.sequence == result.sequence;
                           });

    if (it != impl_->checkpoints.end())
    {
      decision.mispredicted = (it->predicted_next_pc != actual_next_pc);
      impl_->checkpoints.erase(it);
    }
    else
    {
      decision.mispredicted = false;
    }
    return decision;
  }

  void SpeculationController::RemoveYoungerCheckpoints(std::uint64_t sequence)
  {
    impl_->checkpoints.erase(
        std::remove_if(impl_->checkpoints.begin(), impl_->checkpoints.end(),
                       [sequence](const BranchCheckpoint &cp)
                       {
                         return cp.sequence > sequence;
                       }),
        impl_->checkpoints.end());
  }

  std::size_t SpeculationController::CheckpointCount() const
  {
    return impl_->checkpoints.size();
  }
}
