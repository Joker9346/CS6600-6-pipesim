#include "design/core.h"
#include "design/instruction_helpers.h"
#include <limits>
#include <stdexcept>

namespace pipesim::design_internal
{

    Core::Core(Machine *machine, const std::array<ExecutionPipeline *, 7> &pipelines, MainControl *main_control, HazardControl *hazard_control) : machine_(machine), pipelines_(pipelines), main_control_(main_control), hazard_control_(hazard_control)
    {
        if (machine_ == nullptr || main_control_ == nullptr || hazard_control_ == nullptr)
            throw std::runtime_error("CPU core received a null component");
        for (std::size_t i = 0; i < pipelines_.size(); ++i)
            if (pipelines_[i] == nullptr || pipelines_[i]->kind() != static_cast<ExecutionUnitKind>(i))
                throw std::runtime_error("CPU core execution-unit registration mismatch");
    }

    void Core::ResetRegister(RegisterKind kind)
    {
        if (kind != RegisterKind::kPc)
            return;
        pc_ = machine_->EntryPoint();
        next_pc_ = pc_;
        fetch_stopped_ = false;
        next_fetch_stopped_ = false;
        if_id_ = FrontendLatch();
        next_if_id_ = FrontendLatch();
        current_boundary_ = 0;
        cycle_begun_ = false;
        for (auto &s : snapshots_)
            s = StageSnapshot();
    }

    void Core::CommitRegister(RegisterKind kind)
    {
        if (kind != RegisterKind::kPc)
            return;
        if (!cycle_begun_ || !pipelines_operated_)
            throw std::runtime_error("CPU core commit without a complete operate pass");
        pc_ = next_pc_;
        fetch_stopped_ = next_fetch_stopped_;
        if_id_ = next_if_id_;
        if (current_boundary_ == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("CPU boundary counter overflow");
        ++current_boundary_;
        cycle_begun_ = false;
    }

    std::string Core::RegisterSnapshot(RegisterKind kind) const
    {
        std::ostringstream output;
        switch (kind)
        {
        case RegisterKind::kPc:
            output << "pc=0x" << std::hex << pc_ << std::dec << " stopped=" << (fetch_stopped_ ? 1 : 0);
            return output.str();
        case RegisterKind::kIfId:
            if (!if_id_.valid)
                return "EMPTY";
            output << OpcodeName(if_id_.fetched.instruction.opcode) << "@0x" << std::hex << if_id_.fetched.instruction.pc << std::dec << "#" << if_id_.fetched.sequence;
            return output.str();
        case RegisterKind::kIdEx:
            return "EXECUTION_PIPELINES";
        case RegisterKind::kExMem:
            return "AGU_HANDOFF";
        case RegisterKind::kMemWb:
            return "COMPLETION_QUEUE";
        }
        return "EMPTY";
    }

    void Core::ResetStage(StageRole role)
    {
        snapshots_[static_cast<std::size_t>(role)] = StageSnapshot();
    }

    StageSnapshot Core::Snapshot(StageRole role) const { return snapshots_[static_cast<std::size_t>(role)]; }

    void Core::Writeback()
    {
        BeginCycle();
        StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kWriteback)];
        snapshot = StageSnapshot();
        snapshot.action = "EMPTY";
        Retirement retirement;
        if (machine_->PeekRetirement(&retirement))
        {
            machine_->ApplyRetirement(retirement);
            snapshot = SnapshotFor(retirement.instruction);
            snapshot.action = "RETIRE";
        }
    }

    void Core::Memory()
    {
        StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kMemory)];
        snapshot = StageSnapshot();
        snapshot.action = "EMPTY";

        // Job 1: fixed units (ADD, SUB, AND, OR, XOR)
        for (std::size_t i = 0; i < 5; ++i)
        {
            ExecutionPipeline *fixed_unit = pipelines_[i];
            std::optional<ExecutionResult> output = fixed_unit->OutputIntent();
            if (output.has_value())
            {
                const ExecutionResult &result = *output;
                if (current_boundary_ + 1U == result.nominal_result_boundary)
                {
                    machine_->CompleteNonMemory(result.sequence, result.value, result.halt);
                    consume_output_[i] = true;
                    snapshot = SnapshotFor(result.instruction);
                    snapshot.action = "COMPLETE";
                }
            }
        }

        // Job 2: CONTROL resolution
        ExecutionPipeline *control_unit = pipelines_[static_cast<std::size_t>(ExecutionUnitKind::kControl)];
        std::optional<ExecutionResult> control_output = control_unit->OutputIntent();
        if (control_output.has_value())
        {
            const ExecutionResult &result = *control_output;
            if (current_boundary_ + 1U == result.nominal_result_boundary)
            {
                machine_->CompleteNonMemory(result.sequence, result.value, result.halt);
                consume_output_[static_cast<std::size_t>(ExecutionUnitKind::kControl)] = true;
                if (result.redirect)
                {
                    next_pc_ = result.next_pc;
                    machine_->InvalidateFetchesAfter(result.sequence);
                }
                // FIX: Only restart fetch if it's not a HALT
                if (!result.halt)
                {
                    next_fetch_stopped_ = false;
                }
                snapshot = SnapshotFor(result.instruction);
                snapshot.action = "CONTROL";
            }
        }

        // Job 3: AGU handoff
        ExecutionPipeline *agu_unit = pipelines_[static_cast<std::size_t>(ExecutionUnitKind::kAgu)];
        std::optional<ExecutionResult> agu_output = agu_unit->OutputIntent();
        if (agu_output.has_value())
        {
            const ExecutionResult &result = *agu_output;
            if (current_boundary_ + 1U == result.nominal_result_boundary)
            {
                if (machine_->TryIssueData(result.sequence, result.address, result.memory_write, result.store_value))
                {
                    consume_output_[static_cast<std::size_t>(ExecutionUnitKind::kAgu)] = true;
                    snapshot = SnapshotFor(result.instruction);
                    snapshot.action = "MEMORY";
                }
                else
                {
                    machine_->RecordCpuStall(CpuStallReason::kDcacheInput);
                }
            }
        }
    }

    void Core::Execute()
    {
        StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kExecute)];
        snapshot = StageSnapshot();
        snapshot.action = "EMPTY";
    }

    void Core::Decode()
    {
        StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kDecode)];
        snapshot = StageSnapshot();
        snapshot.action = "EMPTY";

        std::optional<ExecutionOperation> dispatch_op = std::nullopt;
        int dispatched_unit = -1;

        if (if_id_.valid)
        {
            const Instruction &instr = if_id_.fetched.instruction;
            const std::uint64_t sequence = if_id_.fetched.sequence;

            if (!(machine_->CompletionQueueHasCapacity()))
            {
                machine_->RecordCpuStall(CpuStallReason::kCompletionQueueFull);
            }
            else
            {
                DecodeResult decoded = main_control_->Decode(instr);
                HazardResult hazard = hazard_control_->Check(machine_, sequence, instr, decoded.writes_register);

                if (hazard == HazardResult::kRaw)
                {
                    machine_->RecordCpuStall(CpuStallReason::kOutstandingRaw);
                }
                else if (hazard == HazardResult::kWaw)
                {
                    machine_->RecordCpuStall(CpuStallReason::kOutstandingWaw);
                }
                else
                {
                    ExecutionPipeline *unit_ = pipelines_[static_cast<std::size_t>(decoded.unit)];
                    if (!(unit_->CanAccept(current_boundary_ + 1U, consume_output_[static_cast<std::size_t>(decoded.unit)])))
                    {
                        machine_->RecordCpuStall(CpuStallReason::kExecutionUnitBusy);
                    }
                    else
                    {
                        std::optional<std::uint64_t> result_boundary;

                        if (decoded.unit == ExecutionUnitKind::kAgu)
                        {
                            result_boundary = std::nullopt;
                        }
                        else
                        {
                            result_boundary = ResultBoundary(current_boundary_ + 1U, unit_->config().latency);
                        }

                        if (!(machine_->TryAllocateCompletion(sequence, instr, decoded.writes_register, result_boundary)))
                        {
                            machine_->RecordCpuStall(CpuStallReason::kExecutionCompletionSlot);
                        }
                        else
                        {
                            ExecutionOperation operation;
                            operation.instruction = instr;
                            operation.sequence = sequence;
                            operation.dispatch_boundary = current_boundary_ + 1U;
                            operation.lhs = machine_->ReadRegister(instr.rs1);

                            // FIX: Correctly populate SW address calculation operands
                            if (instr.opcode == Opcode::kSw)
                            {
                                operation.rhs = static_cast<std::uint32_t>(instr.immediate);
                                operation.store_value = machine_->ReadRegister(instr.rs2);
                            }
                            else
                            {
                                if (ReadsRs2(instr.opcode))
                                {
                                    operation.rhs = machine_->ReadRegister(instr.rs2);
                                }
                                else
                                {
                                    operation.rhs = static_cast<std::uint32_t>(instr.immediate);
                                }
                                operation.store_value = 0;
                            }

                            dispatch_op = operation;
                            dispatched_unit = static_cast<int>(decoded.unit);

                            if (decoded.unit == ExecutionUnitKind::kControl)
                            {
                                next_fetch_stopped_ = true;
                                control_recognized_this_cycle_ = true;
                            }
                            snapshot = SnapshotFor(instr);
                            snapshot.action = "DISPATCH";
                            if_id_consumed_ = true;
                        }
                    }
                }
            }
        }

        for (std::size_t i = 0; i < 7; ++i)
        {
            if (static_cast<int>(i) == dispatched_unit)
            {
                pipelines_[i]->Operate(current_boundary_, dispatch_op, consume_output_[i]);
            }
            else
            {
                pipelines_[i]->Operate(current_boundary_, std::nullopt, consume_output_[i]);
            }
        }
        pipelines_operated_ = true;
    }

    void Core::Fetch()
    {
        StageSnapshot &snapshot = snapshots_[static_cast<std::size_t>(StageRole::kFetch)];
        snapshot = StageSnapshot();
        snapshot.pc = pc_;

        bool can_deliver = !fetch_stopped_ && !control_recognized_this_cycle_;

        if (can_deliver)
        {
            if (!if_id_.valid || if_id_consumed_)
            {
                FetchedInstruction fetched;
                if (machine_->PeekFetched(&fetched))
                {
                    next_if_id_.valid = true;
                    next_if_id_.fetched = fetched;
                    machine_->ConsumeFetched();
                }
                else
                {
                    next_if_id_.valid = false;
                }
            }
        }
        else
        {
            if (!if_id_.valid || if_id_consumed_)
            {
                next_if_id_.valid = false;
            }
        }

        if (fetch_stopped_ || control_recognized_this_cycle_)
        {
            machine_->RecordCpuStall(CpuStallReason::kControlDisabled);
        }
        else
        {
            if (machine_->FetchQueueHasCapacity() && machine_->FetchRequestLinkCanAccept())
            {
                if (machine_->TryIssueFetch(pc_))
                {
                    next_pc_ = pc_ + 4;
                }
            }
            else
            {
                machine_->RecordCpuStall(CpuStallReason::kIcacheInput);
            }
        }
    }

    void Core::BeginCycle()
    {
        if (cycle_begun_)
            throw std::runtime_error("CPU logic cycle initialized twice");
        next_pc_ = pc_;
        next_fetch_stopped_ = fetch_stopped_;
        next_if_id_ = if_id_;
        consume_output_.fill(false);
        if_id_consumed_ = false;
        control_recognized_this_cycle_ = false;
        pipelines_operated_ = false;
        cycle_begun_ = true;
    }

    StageSnapshot Core::SnapshotFor(const Instruction &instruction)
    {
        StageSnapshot snapshot;
        snapshot.valid = true;
        snapshot.pc = instruction.pc;
        snapshot.opcode = OpcodeName(instruction.opcode);
        return snapshot;
    }

} // namespace pipesim::design_internal