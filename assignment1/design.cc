#include "pipesim.h"

#include <cstdint>
#include <memory>
#include <string>

namespace pipesim
{
  namespace
  {

    enum class AluOp
    {
      kAdd,
      kSub,
      kAnd,
      kOr,
      kXor,
    };

    struct Control
    {
      bool reg_write = false;
      bool mem_read = false;
      bool mem_write = false;
      bool use_immediate = false;
      bool halt = false;
      AluOp alu_op = AluOp::kAdd;
    };

    struct Latch
    {
      bool valid = false;
      Instruction instruction;
      std::uint32_t first = 0;
      std::uint32_t second = 0;
      std::uint32_t result = 0;
      std::uint32_t store_value = 0;
      Control control;
    };

    bool IsControl(Opcode opcode)
    {
      switch (opcode)
      {
      case Opcode::kBeq:
      case Opcode::kBne:
      case Opcode::kJ:
      case Opcode::kHalt:
        // Worked example -- this is why Execute() redirects on BEQ: Decode()
        // needs to know to stop fetching behind it while EX resolves it.
        return true;
      case Opcode::kAdd:
      case Opcode::kSub:
      case Opcode::kAnd:
      case Opcode::kOr:
      case Opcode::kXor:
      case Opcode::kAddi:
      case Opcode::kLw:
      case Opcode::kSw:
      case Opcode::kNop:
        break; // TODO
      }
      return false;
    }

    bool ReadsRs1(Opcode opcode)
    {
      switch (opcode)
      {
      case Opcode::kAdd:
      // Worked example: ADD reads both source registers.
      case Opcode::kSub:
      case Opcode::kAnd:
      case Opcode::kOr:
      case Opcode::kXor:
      case Opcode::kAddi:
      case Opcode::kLw:
      case Opcode::kSw:
      case Opcode::kBeq:
      case Opcode::kBne:
        return true;
      case Opcode::kJ:
      case Opcode::kHalt:
      case Opcode::kNop:
        break; // TODO
      }
      return false;
    }

    bool ReadsRs2(Opcode opcode)
    {
      switch (opcode)
      {
      case Opcode::kAdd:
      case Opcode::kSub:
      case Opcode::kAnd:
      case Opcode::kOr:
      case Opcode::kXor:
      case Opcode::kSw:
      case Opcode::kBeq:
      case Opcode::kBne:
        return true;
      case Opcode::kAddi:
      case Opcode::kLw:
      case Opcode::kJ:
      case Opcode::kHalt:
      case Opcode::kNop:
        break; // TODO
      }
      return false;
    }

    class Alu : public TrackedUnit
    {
    public:
      std::uint32_t Run(AluOp operation, std::uint32_t left, std::uint32_t right)
      {
        Touch();

        switch (operation)
        {
        case AluOp::kAdd:
          return left + right;
        case AluOp::kSub:
          return left - right; // TODO
        case AluOp::kAnd:
          return left & right; // TODO
        case AluOp::kOr:
          return left | right; // TODO
        case AluOp::kXor:
          return left ^ right; // TODO
        }
        return 0;
      }
    };

    class MainControl : public TrackedUnit
    {
    public:
      Control Decode(Opcode opcode)
      {
        Touch();

        Control control;
        switch (opcode)
        {
        case Opcode::kAddi:
          control.reg_write = true;
          control.use_immediate = true;
          control.alu_op = AluOp::kAdd;
          break;

        case Opcode::kAdd:
          control.reg_write = true;
          control.alu_op = AluOp::kAdd;
          break;
        case Opcode::kSub:
          control.alu_op = AluOp::kSub;
          control.reg_write = true;
          break;
        case Opcode::kAnd:
          control.alu_op = AluOp::kAnd;
          control.reg_write = true;
          break;
        case Opcode::kOr:
          control.alu_op = AluOp::kOr;
          control.reg_write = true;
          break;
        case Opcode::kXor:
          control.reg_write = true;
          control.alu_op = AluOp::kXor;
          break; // TODO

        case Opcode::kLw:
          control.mem_read = true;
          control.reg_write = true;
          control.use_immediate = true;
          control.alu_op = AluOp::kAdd;
          break; // TODO

        case Opcode::kSw:
          control.mem_write = true;
          control.use_immediate = true;
          control.alu_op = AluOp::kAdd;
          break; // TODO

        case Opcode::kBeq:
        case Opcode::kBne:
          control.alu_op = AluOp::kSub;
          break; // TODO

        case Opcode::kHalt:
          control.halt = true;
          break; // TODO

        case Opcode::kJ:
        case Opcode::kNop:
          break;
        }

        return control;
      }
    };

    class HazardControl : public TrackedUnit
    {
    public:
      bool MustStall(const Latch &consumer, const Latch &id_ex,
                     const Latch &ex_mem)
      {
        Touch();
        const Instruction &instruction = consumer.instruction;
        bool uses_rs1 = ReadsRs1(instruction.opcode);
        bool uses_rs2 = ReadsRs2(instruction.opcode);

        return (HasDependency(instruction.rs2, uses_rs2, id_ex) ||
                HasDependency(instruction.rs1, uses_rs1, id_ex) ||
                HasDependency(instruction.rs2, uses_rs2, ex_mem) ||
                HasDependency(instruction.rs1, uses_rs1, ex_mem));
        // TODO: also check rs1 against id_ex, and both rs1/rs2 against ex_mem --
        // no forwarding means ex_mem is a real hazard source too, not just id_ex.
      }

    private:
      bool HasDependency(int source, bool source_is_used,
                         const Latch &producer) const
      {
        if (!source_is_used || source == 0)
          return false;
        return (producer.valid &&
                producer.control.reg_write &&
                (producer.instruction.rd == source)); // TODO: does `producer` actually write `source`?
      }
    };

    enum class FrontAction
    {
      kNormal,
      kHold,
      kStop,
      kRedirect,
    };

    enum class RegisterKind
    {
      kPc,
      kIfId,
      kIdEx,
      kExMem,
      kMemWb,
    };

    class Core
    {
    public:
      Core(Machine *machine, Alu *alu, MainControl *main_control,
           HazardControl *hazard_control)
          : machine_(machine),
            alu_(alu),
            main_control_(main_control),
            hazard_control_(hazard_control) {}

      void ResetRegister(RegisterKind kind)
      {
        switch (kind)
        {
        case RegisterKind::kPc:
          pc_ = machine_->EntryPoint();
          next_pc_ = pc_;
          halt_pending_ = false;
          next_halt_pending_ = false;
          break;
        case RegisterKind::kIfId:
          if_id_ = Latch();
          next_if_id_ = Latch();
          break;
        case RegisterKind::kIdEx:
          id_ex_ = Latch();
          next_id_ex_ = Latch();
          break;
        case RegisterKind::kExMem:
          ex_mem_ = Latch();
          next_ex_mem_ = Latch();
          break;
        case RegisterKind::kMemWb:
          mem_wb_ = Latch();
          next_mem_wb_ = Latch();
          break;
        }
      }

      void CommitRegister(RegisterKind kind)
      {
        switch (kind)
        {
        case RegisterKind::kPc:
          pc_ = next_pc_;
          halt_pending_ = next_halt_pending_;
          break;
        case RegisterKind::kIfId:
          if_id_ = next_if_id_;
          break;
        case RegisterKind::kIdEx:
          id_ex_ = next_id_ex_;
          break;
        case RegisterKind::kExMem:
          ex_mem_ = next_ex_mem_;
          break;
        case RegisterKind::kMemWb:
          mem_wb_ = next_mem_wb_;
          break;
        }
      }

      std::string RegisterSnapshot(RegisterKind kind) const
      {
        if (kind == RegisterKind::kPc)
        {
          return std::to_string(pc_);
        }
        const Latch *latch = nullptr;
        switch (kind)
        {
        case RegisterKind::kIfId:
          latch = &if_id_;
          break;
        case RegisterKind::kIdEx:
          latch = &id_ex_;
          break;
        case RegisterKind::kExMem:
          latch = &ex_mem_;
          break;
        case RegisterKind::kMemWb:
          latch = &mem_wb_;
          break;
        case RegisterKind::kPc:
          break;
        }
        if (latch == nullptr || !latch->valid)
          return "EMPTY";
        return OpcodeName(latch->instruction.opcode);
      }

      void ResetStage(StageRole role) { snapshots_[StageIndex(role)] = StageSnapshot(); }
      StageSnapshot Snapshot(StageRole role) const { return snapshots_[StageIndex(role)]; }

      void Evaluate(StageRole role)
      {
        switch (role)
        {
        case StageRole::kWriteback:
          Writeback();
          break;
        case StageRole::kMemory:
          Memory();
          break;
        case StageRole::kExecute:
          Execute();
          break;
        case StageRole::kDecode:
          Decode();
          break;
        case StageRole::kFetch:
          Fetch();
          break;
        }
      }

    private:
      int StageIndex(StageRole role) const
      {
        switch (role)
        {
        case StageRole::kFetch:
          return 0;
        case StageRole::kDecode:
          return 1;
        case StageRole::kExecute:
          return 2;
        case StageRole::kMemory:
          return 3;
        case StageRole::kWriteback:
          return 4;
        }
        return 0;
      }

      void SetSnapshot(StageRole role, const Latch &latch, const std::string &action)
      {
        StageSnapshot &snapshot = snapshots_[StageIndex(role)];
        snapshot = StageSnapshot();
        snapshot.valid = latch.valid;
        snapshot.action = action;
        if (latch.valid)
        {
          snapshot.pc = latch.instruction.pc;
          snapshot.opcode = OpcodeName(latch.instruction.opcode);
        }
      }

      void Writeback()
      {
        SetSnapshot(StageRole::kWriteback, mem_wb_, mem_wb_.valid ? "RETIRE" : "EMPTY");
        if (!mem_wb_.valid)
          return;

        if (mem_wb_.control.reg_write)
        {
          machine_->WriteRegister(mem_wb_.instruction.rd, mem_wb_.result);
        }
        machine_->RetireInstruction();
        if (mem_wb_.control.halt)
        {
          machine_->SetHalted();
        }
      }

      void Memory()
      {
        SetSnapshot(StageRole::kMemory, ex_mem_, ex_mem_.valid ? "RUN" : "EMPTY");
        next_mem_wb_ = Latch();
        if (!ex_mem_.valid)
          return;

        next_mem_wb_ = ex_mem_;
        if (ex_mem_.control.mem_write)
        {
          machine_->StoreWord(ex_mem_.result, ex_mem_.store_value);
        }
        // TODO: on ex_mem_.control.mem_read, machine_->LoadWord(...) and
        // overwrite next_mem_wb_.result with it.
        if (ex_mem_.control.mem_read)
        {
          next_mem_wb_.result = machine_->LoadWord(ex_mem_.result);
        }
      }

      void Execute()
      {
        front_action_ = FrontAction::kNormal;
        redirect_pc_ = 0;

        SetSnapshot(StageRole::kExecute, id_ex_, id_ex_.valid ? "RUN" : "EMPTY");
        next_ex_mem_ = Latch();
        if (!id_ex_.valid)
          return;

        next_ex_mem_ = id_ex_;
        next_ex_mem_.store_value = id_ex_.second;

        const Instruction &instruction = id_ex_.instruction;

        std::uint32_t right = id_ex_.control.use_immediate
                                  ? static_cast<std::uint32_t>(instruction.immediate)
                                  : id_ex_.second;
        next_ex_mem_.result = alu_->Run(id_ex_.control.alu_op, id_ex_.first, right);

        if (instruction.opcode == Opcode::kBeq)
        {
          Redirect(id_ex_.first == id_ex_.second
                       ? static_cast<std::uint32_t>(instruction.immediate)
                       : instruction.pc + 4);
        }
        else if (instruction.opcode == Opcode::kBne)
        {
          Redirect(id_ex_.first != id_ex_.second
                       ? static_cast<std::uint32_t>(instruction.immediate)
                       : instruction.pc + 4);
        }
        else if (instruction.opcode == Opcode::kJ)
        {
          Redirect(static_cast<std::uint32_t>(instruction.immediate));
        }
        // else if (instruction.opcode == Opcode::kHalt)
        // {
        //   return;
        // }
        // TODO: BNE, J, HALT
      }

      void Redirect(std::uint32_t new_pc)
      {
        front_action_ = FrontAction::kRedirect;
        redirect_pc_ = new_pc;
        snapshots_[StageIndex(StageRole::kExecute)].action = "RESOLVE";
      }

      void Decode()
      {
        SetSnapshot(StageRole::kDecode, if_id_, if_id_.valid ? "RUN" : "EMPTY");
        next_id_ex_ = Latch();

        if (front_action_ == FrontAction::kRedirect || !if_id_.valid)
          return;

        const Instruction &instruction = if_id_.instruction;

        if (hazard_control_->MustStall(if_id_, id_ex_, ex_mem_))
        {
          front_action_ = FrontAction::kHold;
          snapshots_[StageIndex(StageRole::kDecode)].action = "STALL";
          machine_->AddDataStall();
          return;
        }
        next_id_ex_.valid = true;
        next_id_ex_.instruction = instruction;
        next_id_ex_.first = machine_->ReadRegister(instruction.rs1);
        next_id_ex_.second = machine_->ReadRegister(instruction.rs2);
        next_id_ex_.control = main_control_->Decode(instruction.opcode);

        // TODO: on a hazard (hazard_control_->MustStall), hold the front end
        // and call machine_->AddDataStall().

        // TODO: on a control instruction (IsControl), stop fetching and call
        // machine_->AddControlStall(). HALT needs one more bit set somewhere
        // before it can actually drain the pipeline.
        if (IsControl(instruction.opcode))
        {
          front_action_ = FrontAction::kStop;
          machine_->AddControlStall();
          if (next_id_ex_.control.halt)
          {
            next_halt_pending_ = true;
          }
        }
      }

      void Fetch()
      {
        StageSnapshot &snapshot = snapshots_[StageIndex(StageRole::kFetch)];
        snapshot = StageSnapshot();
        snapshot.pc = pc_;

        if (front_action_ == FrontAction::kRedirect)
        {
          next_pc_ = redirect_pc_;
          next_if_id_ = Latch();
          snapshot.action = "REDIRECT";
          return;
        }

        if (front_action_ == FrontAction::kHold)
        {
          next_pc_ = pc_;
          next_if_id_ = if_id_;
          snapshot.action = "HOLD";
          return;
        }

        if (front_action_ == FrontAction::kStop || halt_pending_ || next_halt_pending_)
        {
          next_pc_ = pc_;
          next_if_id_ = Latch();
          snapshot.action = "BUBBLE";
          return;
        }

        Instruction instruction;
        if (!machine_->Fetch(pc_, &instruction))
        {
          next_pc_ = pc_;
          next_if_id_ = Latch();
          snapshot.action = "EMPTY";
          return;
        }

        next_if_id_ = Latch();
        next_if_id_.valid = true;
        next_if_id_.instruction = instruction;
        next_pc_ = pc_ + 4;

        snapshot.valid = true;
        snapshot.pc = instruction.pc;
        snapshot.opcode = OpcodeName(instruction.opcode);
        snapshot.action = "FETCH";
      }

      Machine *machine_;
      Alu *alu_;
      MainControl *main_control_;
      HazardControl *hazard_control_;

      std::uint32_t pc_ = 0;
      std::uint32_t next_pc_ = 0;

      Latch if_id_, next_if_id_;
      Latch id_ex_, next_id_ex_;
      Latch ex_mem_, next_ex_mem_;
      Latch mem_wb_, next_mem_wb_;

      bool halt_pending_ = false;
      bool next_halt_pending_ = false;

      FrontAction front_action_ = FrontAction::kNormal;
      std::uint32_t redirect_pc_ = 0;
      StageSnapshot snapshots_[5];
    };

    class Stage : public StageComponent
    {
    public:
      Stage(StageRole role, Core *core, bool owns_core = false)
          : role_(role), core_(core)
      {
        if (owns_core)
          owned_core_.reset(core);
      }
      void Reset() override { core_->ResetStage(role_); }
      void Evaluate() override { core_->Evaluate(role_); }
      StageSnapshot Snapshot() const override { return core_->Snapshot(role_); }

    private:
      StageRole role_;
      Core *core_;
      std::unique_ptr<Core> owned_core_;
    };

    class Register : public ClockedState
    {
    public:
      Register(RegisterKind kind, Core *core) : kind_(kind), core_(core) {}
      void Reset() override { core_->ResetRegister(kind_); }
      void Commit() override { core_->CommitRegister(kind_); }
      std::string Snapshot() const override { return core_->RegisterSnapshot(kind_); }

    private:
      RegisterKind kind_;
      Core *core_;
    };

  } // namespace

  std::unique_ptr<Processor> BuildDesign(Machine *machine)
  {
    ProcessorBuilder builder;

    Alu *alu = new Alu;
    MainControl *main_control = new MainControl;
    HazardControl *hazard_control = new HazardControl;
    Core *core = new Core(machine, alu, main_control, hazard_control);

    Register *pc = new Register(RegisterKind::kPc, core);
    Register *if_id = new Register(RegisterKind::kIfId, core);
    Register *id_ex = new Register(RegisterKind::kIdEx, core);
    Register *ex_mem = new Register(RegisterKind::kExMem, core);
    Register *mem_wb = new Register(RegisterKind::kMemWb, core);

    Stage *fetch = new Stage(StageRole::kFetch, core, /*owns_core=*/true);
    Stage *decode = new Stage(StageRole::kDecode, core);
    Stage *execute = new Stage(StageRole::kExecute, core);
    Stage *memory = new Stage(StageRole::kMemory, core);
    Stage *writeback = new Stage(StageRole::kWriteback, core);

    // Worked example: register the PC and the fetch stage, then connect
    // them. Every other register/stage/unit follows the same AddRegister /
    // AddStage / AddUnit shape -- each returns a NodeId, keep it, pass it
    // to Connect(). Fill in the rest against SPEC.md's connection table.
    NodeId pc_node = builder.AddRegister(RegisterRole::kProgramCounter, "pc", pc);
    NodeId fetch_node = builder.AddStage(StageRole::kFetch, "fetch", fetch);
    builder.Connect(pc_node, fetch_node, ConnectionType::kData);

    NodeId if_id_node = builder.AddRegister(RegisterRole::kIfId, "if/id", if_id);
    NodeId id_node = builder.AddStage(StageRole::kDecode, "decode", decode);
    NodeId id_ex_node = builder.AddRegister(RegisterRole::kIdEx, "id/ex", id_ex);
    NodeId ex_node = builder.AddStage(StageRole::kExecute, "execute", execute);
    NodeId ex_mem_node = builder.AddRegister(RegisterRole::kExMem, "ex/mem", ex_mem);
    NodeId mem_node = builder.AddStage(StageRole::kMemory, "mem", memory);
    NodeId mem_wb_node = builder.AddRegister(RegisterRole::kMemWb, "mem/wb", mem_wb);
    NodeId wb_node = builder.AddStage(StageRole::kWriteback, "wb", writeback);
    // StageRole::
    builder.Connect(fetch_node, if_id_node, ConnectionType::kClockedData);
    builder.Connect(if_id_node, id_node, ConnectionType::kData);
    builder.Connect(id_node, id_ex_node, ConnectionType::kClockedData);
    builder.Connect(id_ex_node, ex_node, ConnectionType::kData);
    builder.Connect(ex_node, ex_mem_node, ConnectionType::kClockedData);
    builder.Connect(ex_mem_node, mem_node, ConnectionType::kData);
    builder.Connect(mem_node, mem_wb_node, ConnectionType::kClockedData);
    builder.Connect(mem_wb_node, wb_node, ConnectionType::kData);

    NodeId alu_node = builder.AddUnit(UnitRole::kAlu, "alu", alu);
    NodeId maincontrol_node = builder.AddUnit(UnitRole::kMainControl, "main_control", main_control);
    NodeId hazardcontrol_node = builder.AddUnit(UnitRole::kHazardControl, "hazard_control", hazard_control);

    builder.Connect(alu_node, ex_node, ConnectionType::kData);
    builder.Connect(maincontrol_node, id_node, ConnectionType::kControl);
    builder.Connect(hazardcontrol_node, pc_node, ConnectionType::kStall);
    builder.Connect(hazardcontrol_node, if_id_node, ConnectionType::kStall);
    builder.Connect(hazardcontrol_node, id_ex_node, ConnectionType::kBubble);
    builder.Connect(ex_node, pc_node, ConnectionType::kRedirect);
    // TODO: register if_id, id_ex, ex_mem, mem_wb (AddRegister)
    // TODO: register decode, execute, memory, writeback (AddStage)
    // TODO: register alu, main_control, hazard_control (AddUnit)
    // TODO: connect every edge in SPEC.md's table. In the worked example
    //       above, a register feeding a stage is kData -- a stage writing
    //       into a register is kClockedData instead. kControl, kStall,
    //       kBubble, kRedirect are each used for exactly the one
    //       connection SPEC.md names them for.

    return builder.Build(machine);
  }

} // namespace pipesim