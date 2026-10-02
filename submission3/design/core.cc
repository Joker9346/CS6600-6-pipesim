#include "design/core.h"
#include "design/instruction_helpers.h"
#include <limits>
#include <stdexcept>

namespace pipesim::design_internal
{

    Core::Core(Machine *machine, const std::array<ExecutionPipeline *, 7> &pipelines, MainControl *main_control, HazardControl *hazard_control, BranchPredictionSystem *bps, SpeculationController *spec)
        : machine_(machine), pipelines_(pipelines), main_control_(main_control), hazard_control_(hazard_control), branch_prediction_(bps), speculation_control_(spec)
    {
        if (machine_ == nullptr || main_control_ == nullptr || hazard_control_ == nullptr || branch_prediction_ == nullptr || speculation_control_ == nullptr)
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

        ExecutionPipeline *control_unit = pipelines_[static_cast<std::size_t>(ExecutionUnitKind::kControl)];
        std::optional<ExecutionResult> control_output = control_unit->OutputIntent();
        if (control_output.has_value())
        {
            const ExecutionResult &result = *control_output;
            if (current_boundary_ + 1U == result.nominal_result_boundary)
            {
                machine_->CompleteNonMemory(result.sequence, result.value, result.halt);
                consume_output_[static_cast<std::size_t>(ExecutionUnitKind::kControl)] = true;

                if (result.instruction.opcode != Opcode::kHalt)
                {
                    BranchResult bres;
                    bres.sequence = result.sequence;
                    bres.fetch_cycle = result.fetch_cycle;
                    bres.resolve_cycle = current_boundary_ + 1U;
                    bres.pc = result.instruction.pc;

                    if (result.instruction.opcode == Opcode::kJ)
                    {
                        bres.type = BranchType::kUnconditional;
                        bres.taken = true;
                        bres.target = result.next_pc;
                    }
                    else
                    {
                        bres.type = BranchType::kConditional;
                        bres.taken = result.redirect;
                        bres.target = result.redirect ? result.next_pc : result.instruction.pc + 4;
                    }

                    branch_prediction_->Resolve(bres, result.predictions);
                    RecoveryDecision decision = speculation_control_->Resolve(bres);

                    if (decision.mispredicted)
                    {
                        next_pc_ = decision.correct_next_pc;

                        SquashReport rep = machine_->SquashAfter(result.sequence);
                        uint64_t squashed_ops = 0;
                        for (auto *pipeline : pipelines_)
                        {
                            squashed_ops += pipeline->SquashAfter(result.sequence);
                        }
                        speculation_control_->RemoveYoungerCheckpoints(result.sequence);

                        squash_sequence_ = result.sequence;
                        if (next_if_id_.valid && next_if_id_.fetched.sequence > result.sequence)
                        {
                            next_if_id_.valid = false;
                        }
                        uint64_t squashed_if_id = 0;
                        if (if_id_.valid && if_id_.fetched.sequence > result.sequence)
                        {
                            if_id_.valid = false;
                            next_if_id_.valid = false;
                            squashed_if_id = 1;
                        }
                        branch_prediction_->RecordSquash(rep.fetches + squashed_if_id, squashed_ops, rep.completions, rep.data_requests);
                        branch_prediction_->RecordRecoveryCycle();
                    }
                }
                else
                {
                    next_fetch_stopped_ = true;
                }

                snapshot = SnapshotFor(result.instruction);
                snapshot.action = "CONTROL";
            }
        }

        ExecutionPipeline *agu_unit = pipelines_[static_cast<std::size_t>(ExecutionUnitKind::kAgu)];
        std::optional<ExecutionResult> agu_output = agu_unit->OutputIntent();
        if (agu_output.has_value())
        {
            const ExecutionResult &result = *agu_output;
            if (current_boundary_ + 1U >= result.nominal_result_boundary)
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

        if (squash_sequence_ != std::numeric_limits<std::uint64_t>::max())
        {
            if (if_id_.valid && if_id_.fetched.sequence > squash_sequence_)
            {
                next_if_id_.valid = false;
                if_id_consumed_ = true;
            }
        }

        if (if_id_.valid && !if_id_consumed_)
        {
            const Instruction &instr = if_id_.fetched.instruction;
            const std::uint64_t sequence = if_id_.fetched.sequence;

            bool false_branch = false;
            if (!if_id_.decoded)
            {
                if (next_if_id_.valid && next_if_id_.fetched.sequence == sequence)
                {
                    next_if_id_.decoded = true;
                }
                false_branch = branch_prediction_->ObserveInstruction(instr, if_id_.fetched.predictions);
            }
            if (false_branch)
            {
                next_pc_ = instr.pc + 4;
                SquashReport false_rep = machine_->SquashAfter(sequence);
                squash_sequence_ = sequence;
                if (next_if_id_.valid && next_if_id_.fetched.sequence > sequence)
                {
                    next_if_id_.valid = false;
                }
                branch_prediction_->RecordSquash(false_rep.fetches, 0, false_rep.completions, false_rep.data_requests);
                branch_prediction_->RecordRecoveryCycle();
            }

            if (speculation_control_->CanDispatch(sequence, instr))
            {
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
                                result_boundary = std::nullopt;
                            else
                                result_boundary = ResultBoundary(current_boundary_ + 1U, unit_->config().latency);

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
                                if (instr.opcode == Opcode::kSw)
                                {
                                    operation.rhs = static_cast<std::uint32_t>(instr.immediate);
                                    operation.store_value = machine_->ReadRegister(instr.rs2);
                                }
                                else
                                {
                                    if (ReadsRs2(instr.opcode))
                                        operation.rhs = machine_->ReadRegister(instr.rs2);
                                    else
                                        operation.rhs = static_cast<std::uint32_t>(instr.immediate);
                                    operation.store_value = 0;
                                }

                                operation.fetch_cycle = if_id_.fetched.fetch_cycle;
                                operation.predictions = if_id_.fetched.predictions;
                                dispatch_op = operation;
                                if (speculation_control_->CheckpointCount() > 0)
                                {
                                    branch_prediction_->RecordSpeculativeDispatch();
                                }
                                dispatched_unit = static_cast<int>(decoded.unit);

                                if (decoded.unit == ExecutionUnitKind::kControl)
                                {
                                    if (instr.opcode == Opcode::kHalt)
                                    {
                                        next_fetch_stopped_ = true;
                                        control_recognized_this_cycle_ = true;
                                    }
                                    else
                                    {
                                        BranchCheckpoint checkpoint;
                                        checkpoint.sequence = sequence;
                                        checkpoint.predicted_next_pc = if_id_.fetched.predictions.next_pc;
                                        checkpoint.predictions = if_id_.fetched.predictions;
                                        speculation_control_->AddCheckpoint(checkpoint);
                                    }
                                }

                                snapshot = SnapshotFor(instr);
                                snapshot.action = "DISPATCH";
                                if_id_consumed_ = true;
                            }
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

        if (squash_sequence_ != std::numeric_limits<std::uint64_t>::max())
        {
            if (next_if_id_.valid && next_if_id_.fetched.sequence > squash_sequence_)
            {
                next_if_id_.valid = false;
            }
            else if (if_id_consumed_)
            {
                next_if_id_.valid = false;
            }
            return;
        }

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
                    next_if_id_.decoded = false;
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
                next_if_id_.valid = false;
        }

        if (fetch_stopped_ || control_recognized_this_cycle_)
        {
            machine_->RecordCpuStall(CpuStallReason::kControlDisabled);
        }
        else if (!speculation_control_->CanFetch())
        {
            machine_->RecordCpuStall(CpuStallReason::kControlDisabled);
        }
        else
        {
            if (!machine_->FetchQueueHasCapacity())
            {
                machine_->RecordCpuStall(CpuStallReason::kFetchQueueFull);
            }
            else if (!machine_->FetchRequestLinkCanAccept())
            {
                machine_->RecordCpuStall(CpuStallReason::kIcacheInput);
            }
            else
            {
                BranchPredictionSet preds = branch_prediction_->Predict(pc_, current_boundary_);
                if (machine_->TryIssuePredictedFetch(pc_, preds, current_boundary_))
                {
                    next_pc_ = preds.next_pc;
                    branch_prediction_->RecordSpeculativeFetch(speculation_control_->CheckpointCount());
                }
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
        squash_sequence_ = std::numeric_limits<std::uint64_t>::max();
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

}
