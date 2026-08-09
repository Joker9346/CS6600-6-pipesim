#include "pipesim.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pipesim {
namespace {

constexpr int kRegisterCount = 8;

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(text[begin])) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin && std::isspace(text[end - 1])) {
    --end;
  }

  return text.substr(begin, end - begin);
}

bool ParseUnsigned(const std::string& text, std::uint32_t* value) {
  try {
    std::size_t used = 0;
    unsigned long parsed = std::stoul(text, &used, 0);
    if (used != text.size()) {
      return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseSigned(const std::string& text, std::int32_t* value) {
  try {
    std::size_t used = 0;
    long parsed = std::stol(text, &used, 0);
    if (used != text.size()) {
      return false;
    }
    *value = static_cast<std::int32_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseOpcode(const std::string& text, Opcode* opcode) {
  static const std::map<std::string, Opcode> kOpcodes = {
      {"NOP", Opcode::kNop},   {"ADD", Opcode::kAdd},
      {"SUB", Opcode::kSub},   {"AND", Opcode::kAnd},
      {"OR", Opcode::kOr},     {"XOR", Opcode::kXor},
      {"ADDI", Opcode::kAddi}, {"LW", Opcode::kLw},
      {"SW", Opcode::kSw},     {"BEQ", Opcode::kBeq},
      {"BNE", Opcode::kBne},   {"J", Opcode::kJ},
      {"HALT", Opcode::kHalt},
  };

  auto it = kOpcodes.find(text);
  if (it == kOpcodes.end()) {
    return false;
  }
  *opcode = it->second;
  return true;
}

const char* StageRoleName(StageRole role) {
  switch (role) {
    case StageRole::kFetch:
      return "IF";
    case StageRole::kDecode:
      return "ID";
    case StageRole::kExecute:
      return "EX";
    case StageRole::kMemory:
      return "MEM";
    case StageRole::kWriteback:
      return "WB";
  }
  return "UNKNOWN";
}

const char* RegisterRoleName(RegisterRole role) {
  switch (role) {
    case RegisterRole::kProgramCounter:
      return "PC";
    case RegisterRole::kIfId:
      return "IF_ID";
    case RegisterRole::kIdEx:
      return "ID_EX";
    case RegisterRole::kExMem:
      return "EX_MEM";
    case RegisterRole::kMemWb:
      return "MEM_WB";
    case RegisterRole::kCustom:
      return "CUSTOM";
  }
  return "UNKNOWN";
}

const char* UnitRoleName(UnitRole role) {
  switch (role) {
    case UnitRole::kAlu:
      return "ALU";
    case UnitRole::kMainControl:
      return "MAIN_CONTROL";
    case UnitRole::kHazardControl:
      return "HAZARD_CONTROL";
    case UnitRole::kCustom:
      return "CUSTOM";
  }
  return "UNKNOWN";
}

const char* ConnectionTypeName(ConnectionType type) {
  switch (type) {
    case ConnectionType::kData:
      return "DATA";
    case ConnectionType::kClockedData:
      return "CLOCKED_DATA";
    case ConnectionType::kControl:
      return "CONTROL";
    case ConnectionType::kStall:
      return "STALL";
    case ConnectionType::kBubble:
      return "BUBBLE";
    case ConnectionType::kRedirect:
      return "REDIRECT";
  }
  return "UNKNOWN";
}

}  // namespace

const char* OpcodeName(Opcode opcode) {
  switch (opcode) {
    case Opcode::kNop:
      return "NOP";
    case Opcode::kAdd:
      return "ADD";
    case Opcode::kSub:
      return "SUB";
    case Opcode::kAnd:
      return "AND";
    case Opcode::kOr:
      return "OR";
    case Opcode::kXor:
      return "XOR";
    case Opcode::kAddi:
      return "ADDI";
    case Opcode::kLw:
      return "LW";
    case Opcode::kSw:
      return "SW";
    case Opcode::kBeq:
      return "BEQ";
    case Opcode::kBne:
      return "BNE";
    case Opcode::kJ:
      return "J";
    case Opcode::kHalt:
      return "HALT";
  }
  return "UNKNOWN";
}

class Machine::Impl {
 public:
  std::map<std::uint32_t, Instruction> program;
  std::array<std::uint32_t, kRegisterCount> registers = {};
  std::map<std::uint32_t, std::uint32_t> memory;

  std::array<std::uint32_t, kRegisterCount> initial_registers = {};
  std::map<std::uint32_t, std::uint32_t> initial_memory;

  std::uint32_t entry_point = 0;
  bool halted = false;
  std::uint64_t retired = 0;
  std::uint64_t data_stalls = 0;
  std::uint64_t control_stalls = 0;
};

Machine::Machine() : impl_(new Impl) {}

bool Machine::LoadProgram(const std::string& file_name, std::string* error) {
  std::ifstream input(file_name);
  if (!input) {
    *error = "cannot open program file";
    return false;
  }

  impl_->program.clear();
  impl_->initial_registers.fill(0);
  impl_->initial_memory.clear();
  impl_->entry_point = 0;

  bool saw_header = false;
  bool saw_end = false;
  std::string line;
  int line_number = 0;

  while (std::getline(input, line)) {
    ++line_number;
    std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }

    std::istringstream fields(line);
    std::string kind;
    fields >> kind;

    if (kind == "PIPEISA") {
      int version = 0;
      fields >> version;
      if (version != 1) {
        *error = "line " + std::to_string(line_number) +
                 ": expected PIPEISA 1";
        return false;
      }
      saw_header = true;
      continue;
    }

    if (!saw_header) {
      *error = "line " + std::to_string(line_number) +
               ": missing PIPEISA 1 header";
      return false;
    }

    if (kind == "ENTRY") {
      std::string value;
      fields >> value;
      if (!ParseUnsigned(value, &impl_->entry_point)) {
        *error = "line " + std::to_string(line_number) +
                 ": invalid entry point";
        return false;
      }
      continue;
    }

    if (kind == "REG") {
      int index = 0;
      std::string value_text;
      std::uint32_t value = 0;
      fields >> index >> value_text;
      if (index < 0 || index >= kRegisterCount ||
          !ParseUnsigned(value_text, &value)) {
        *error = "line " + std::to_string(line_number) +
                 ": invalid REG record";
        return false;
      }
      if (index != 0) {
        impl_->initial_registers[index] = value;
      }
      continue;
    }

    if (kind == "MEM") {
      std::string address_text;
      std::string value_text;
      std::uint32_t address = 0;
      std::uint32_t value = 0;
      fields >> address_text >> value_text;
      if (!ParseUnsigned(address_text, &address) ||
          !ParseUnsigned(value_text, &value) || address % 4 != 0) {
        *error = "line " + std::to_string(line_number) +
                 ": invalid MEM record";
        return false;
      }
      impl_->initial_memory[address] = value;
      continue;
    }

    if (kind == "INST") {
      std::string pc_text;
      std::string opcode_text;
      std::string immediate_text;
      Instruction instruction;
      fields >> pc_text >> opcode_text >> instruction.rd >> instruction.rs1 >>
          instruction.rs2 >> immediate_text;

      if (!ParseUnsigned(pc_text, &instruction.pc) ||
          !ParseOpcode(opcode_text, &instruction.opcode) ||
          !ParseSigned(immediate_text, &instruction.immediate) ||
          instruction.pc % 4 != 0 || instruction.rd < 0 ||
          instruction.rd >= kRegisterCount || instruction.rs1 < 0 ||
          instruction.rs1 >= kRegisterCount || instruction.rs2 < 0 ||
          instruction.rs2 >= kRegisterCount) {
        *error = "line " + std::to_string(line_number) +
                 ": invalid INST record";
        return false;
      }
      impl_->program[instruction.pc] = instruction;
      continue;
    }

    if (kind == "END") {
      saw_end = true;
      break;
    }

    *error = "line " + std::to_string(line_number) +
             ": unknown record type";
    return false;
  }

  if (!saw_header || !saw_end) {
    *error = "program must contain PIPEISA 1 and END";
    return false;
  }

  Reset();
  return true;
}

void Machine::Reset() {
  impl_->registers = impl_->initial_registers;
  impl_->registers[0] = 0;
  impl_->memory = impl_->initial_memory;
  impl_->halted = false;
  impl_->retired = 0;
  impl_->data_stalls = 0;
  impl_->control_stalls = 0;
}

bool Machine::Fetch(std::uint32_t pc, Instruction* instruction) const {
  auto it = impl_->program.find(pc);
  if (it == impl_->program.end()) {
    return false;
  }
  *instruction = it->second;
  return true;
}

std::uint32_t Machine::ReadRegister(int index) const {
  if (index < 0 || index >= kRegisterCount || index == 0) {
    return 0;
  }
  return impl_->registers[index];
}

void Machine::WriteRegister(int index, std::uint32_t value) {
  if (index > 0 && index < kRegisterCount) {
    impl_->registers[index] = value;
  }
  impl_->registers[0] = 0;
}

std::uint32_t Machine::LoadWord(std::uint32_t address) const {
  if (address % 4 != 0) {
    throw std::runtime_error("unaligned load");
  }
  auto it = impl_->memory.find(address);
  return it == impl_->memory.end() ? 0 : it->second;
}

void Machine::StoreWord(std::uint32_t address, std::uint32_t value) {
  if (address % 4 != 0) {
    throw std::runtime_error("unaligned store");
  }
  impl_->memory[address] = value;
}

std::uint32_t Machine::EntryPoint() const { return impl_->entry_point; }

bool Machine::IsHalted() const { return impl_->halted; }

void Machine::SetHalted() { impl_->halted = true; }

void Machine::RetireInstruction() { ++impl_->retired; }

void Machine::AddDataStall() { ++impl_->data_stalls; }

void Machine::AddControlStall() { ++impl_->control_stalls; }

std::uint64_t Machine::RetiredInstructions() const { return impl_->retired; }

std::uint64_t Machine::DataStalls() const { return impl_->data_stalls; }

std::uint64_t Machine::ControlStalls() const {
  return impl_->control_stalls;
}

void Machine::PrintState(std::ostream& output) const {
  output << "BEGIN STATE\n";
  for (int i = 0; i < kRegisterCount; ++i) {
    output << "x" << i << " " << impl_->registers[i] << "\n";
  }
  for (const auto& item : impl_->memory) {
    if (item.second != 0) {
      output << "mem 0x" << std::hex << std::setw(8) << std::setfill('0')
             << item.first << std::dec << std::setfill(' ') << " "
             << item.second << "\n";
    }
  }
  output << "status " << (impl_->halted ? "HALTED" : "RUNNING") << "\n";
  output << "END STATE\n";
}

std::uint64_t TrackedUnit::Uses() const { return uses_; }

void TrackedUnit::Touch() { ++uses_; }

struct NodeRecord {
  enum class Kind { kStage, kRegister, kUnit };

  int id = -1;
  Kind kind = Kind::kStage;
  std::string name;
  StageRole stage_role = StageRole::kFetch;
  RegisterRole register_role = RegisterRole::kCustom;
  UnitRole unit_role = UnitRole::kCustom;
};

struct EdgeRecord {
  int source = -1;
  int destination = -1;
  ConnectionType type = ConnectionType::kData;
};

struct StageEntry {
  StageRole role;
  std::string name;
  std::unique_ptr<StageComponent> component;
};

struct RegisterEntry {
  RegisterRole role;
  std::string name;
  std::unique_ptr<ClockedState> state;
};

struct UnitEntry {
  UnitRole role;
  std::string name;
  std::unique_ptr<TrackedUnit> unit;
};

class ProcessorBuilder::Impl {
 public:
  int next_id = 0;
  std::vector<NodeRecord> nodes;
  std::vector<EdgeRecord> edges;
  std::vector<StageEntry> stages;
  std::vector<RegisterEntry> registers;
  std::vector<UnitEntry> units;
};

class Processor::Impl {
 public:
  Machine* machine = nullptr;
  std::vector<NodeRecord> nodes;
  std::vector<EdgeRecord> edges;
  std::vector<StageEntry> stages;
  std::vector<RegisterEntry> registers;
  std::vector<UnitEntry> units;
  std::uint64_t cycles = 0;

  StageEntry* FindStage(StageRole role) {
    for (auto& stage : stages) {
      if (stage.role == role) {
        return &stage;
      }
    }
    return nullptr;
  }

  const StageEntry* FindStage(StageRole role) const {
    for (const auto& stage : stages) {
      if (stage.role == role) {
        return &stage;
      }
    }
    return nullptr;
  }
};

ProcessorBuilder::ProcessorBuilder() : impl_(new Impl) {}
ProcessorBuilder::~ProcessorBuilder() = default;

NodeId ProcessorBuilder::AddStage(StageRole role, const std::string& name,
                                  StageComponent* component) {
  if (component == nullptr) {
    throw std::runtime_error("stage component is null");
  }
  NodeId id{impl_->next_id++};
  impl_->nodes.push_back(
      {id.value, NodeRecord::Kind::kStage, name, role,
       RegisterRole::kCustom, UnitRole::kCustom});
  impl_->stages.push_back({role, name, std::unique_ptr<StageComponent>(component)});
  return id;
}

NodeId ProcessorBuilder::AddRegister(RegisterRole role,
                                     const std::string& name,
                                     ClockedState* state) {
  if (state == nullptr) {
    throw std::runtime_error("clocked state is null");
  }
  NodeId id{impl_->next_id++};
  impl_->nodes.push_back(
      {id.value, NodeRecord::Kind::kRegister, name, StageRole::kFetch,
       role, UnitRole::kCustom});
  impl_->registers.push_back({role, name, std::unique_ptr<ClockedState>(state)});
  return id;
}

NodeId ProcessorBuilder::AddUnit(UnitRole role, const std::string& name,
                                 TrackedUnit* unit) {
  if (unit == nullptr) {
    throw std::runtime_error("unit is null");
  }
  NodeId id{impl_->next_id++};
  impl_->nodes.push_back(
      {id.value, NodeRecord::Kind::kUnit, name, StageRole::kFetch,
       RegisterRole::kCustom, role});
  impl_->units.push_back({role, name, std::unique_ptr<TrackedUnit>(unit)});
  return id;
}

void ProcessorBuilder::Connect(NodeId source, NodeId destination,
                               ConnectionType type) {
  impl_->edges.push_back({source.value, destination.value, type});
}

namespace {

int FindStageNode(const std::vector<NodeRecord>& nodes, StageRole role) {
  for (const auto& node : nodes) {
    if (node.kind == NodeRecord::Kind::kStage && node.stage_role == role) {
      return node.id;
    }
  }
  return -1;
}

int FindRegisterNode(const std::vector<NodeRecord>& nodes,
                     RegisterRole role) {
  for (const auto& node : nodes) {
    if (node.kind == NodeRecord::Kind::kRegister &&
        node.register_role == role) {
      return node.id;
    }
  }
  return -1;
}

int FindUnitNode(const std::vector<NodeRecord>& nodes, UnitRole role) {
  for (const auto& node : nodes) {
    if (node.kind == NodeRecord::Kind::kUnit && node.unit_role == role) {
      return node.id;
    }
  }
  return -1;
}

bool HasEdge(const std::vector<EdgeRecord>& edges, int source,
             int destination, ConnectionType type) {
  for (const auto& edge : edges) {
    if (edge.source == source && edge.destination == destination &&
        edge.type == type) {
      return true;
    }
  }
  return false;
}

bool HasEitherEdge(const std::vector<EdgeRecord>& edges, int source,
                   int first_destination, int second_destination,
                   ConnectionType type) {
  return HasEdge(edges, source, first_destination, type) ||
         HasEdge(edges, source, second_destination, type);
}

void Require(bool condition, const std::string& message,
             std::vector<std::string>* errors) {
  if (!condition) {
    errors->push_back(message);
  }
}

std::vector<std::string> ValidateBuilder(const ProcessorBuilder::Impl& builder) {
  std::vector<std::string> errors;

  int fetch = FindStageNode(builder.nodes, StageRole::kFetch);
  int decode = FindStageNode(builder.nodes, StageRole::kDecode);
  int execute = FindStageNode(builder.nodes, StageRole::kExecute);
  int memory = FindStageNode(builder.nodes, StageRole::kMemory);
  int writeback = FindStageNode(builder.nodes, StageRole::kWriteback);

  int pc = FindRegisterNode(builder.nodes, RegisterRole::kProgramCounter);
  int if_id = FindRegisterNode(builder.nodes, RegisterRole::kIfId);
  int id_ex = FindRegisterNode(builder.nodes, RegisterRole::kIdEx);
  int ex_mem = FindRegisterNode(builder.nodes, RegisterRole::kExMem);
  int mem_wb = FindRegisterNode(builder.nodes, RegisterRole::kMemWb);

  int alu = FindUnitNode(builder.nodes, UnitRole::kAlu);
  int main_control = FindUnitNode(builder.nodes, UnitRole::kMainControl);
  int hazard = FindUnitNode(builder.nodes, UnitRole::kHazardControl);

  Require(fetch >= 0, "missing IF stage", &errors);
  Require(decode >= 0, "missing ID stage", &errors);
  Require(execute >= 0, "missing EX stage", &errors);
  Require(memory >= 0, "missing MEM stage", &errors);
  Require(writeback >= 0, "missing WB stage", &errors);

  Require(pc >= 0, "missing PC state", &errors);
  Require(if_id >= 0, "missing IF/ID register", &errors);
  Require(id_ex >= 0, "missing ID/EX register", &errors);
  Require(ex_mem >= 0, "missing EX/MEM register", &errors);
  Require(mem_wb >= 0, "missing MEM/WB register", &errors);

  Require(alu >= 0, "missing ALU", &errors);
  Require(main_control >= 0, "missing main control unit", &errors);
  Require(hazard >= 0, "missing hazard control unit", &errors);

  if (errors.empty()) {
    Require(HasEdge(builder.edges, pc, fetch, ConnectionType::kData),
            "missing PC -> IF data connection", &errors);
    Require(HasEdge(builder.edges, fetch, if_id,
                    ConnectionType::kClockedData),
            "missing IF -> IF/ID connection", &errors);
    Require(HasEdge(builder.edges, if_id, decode, ConnectionType::kData),
            "missing IF/ID -> ID connection", &errors);
    Require(HasEdge(builder.edges, decode, id_ex,
                    ConnectionType::kClockedData),
            "missing ID -> ID/EX connection", &errors);
    Require(HasEdge(builder.edges, id_ex, execute, ConnectionType::kData),
            "missing ID/EX -> EX connection", &errors);
    Require(HasEdge(builder.edges, execute, ex_mem,
                    ConnectionType::kClockedData),
            "missing EX -> EX/MEM connection", &errors);
    Require(HasEdge(builder.edges, ex_mem, memory, ConnectionType::kData),
            "missing EX/MEM -> MEM connection", &errors);
    Require(HasEdge(builder.edges, memory, mem_wb,
                    ConnectionType::kClockedData),
            "missing MEM -> MEM/WB connection", &errors);
    Require(HasEdge(builder.edges, mem_wb, writeback, ConnectionType::kData),
            "missing MEM/WB -> WB connection", &errors);

    Require(HasEitherEdge(builder.edges, alu, execute, ex_mem,
                          ConnectionType::kData),
            "ALU must feed EX or EX/MEM", &errors);
    Require(HasEitherEdge(builder.edges, main_control, decode, id_ex,
                          ConnectionType::kControl),
            "main control must feed ID or ID/EX", &errors);
    Require(HasEdge(builder.edges, hazard, pc, ConnectionType::kStall),
            "hazard control must be able to stall PC", &errors);
    Require(HasEdge(builder.edges, hazard, if_id, ConnectionType::kStall),
            "hazard control must be able to stall IF/ID", &errors);
    Require(HasEdge(builder.edges, hazard, id_ex, ConnectionType::kBubble),
            "hazard control must be able to bubble ID/EX", &errors);
    Require(HasEdge(builder.edges, execute, pc, ConnectionType::kRedirect),
            "EX must be able to redirect PC", &errors);
  }

  return errors;
}

}  // namespace

std::unique_ptr<Processor> ProcessorBuilder::Build(Machine* machine) {
  std::vector<std::string> errors = ValidateBuilder(*impl_);
  if (!errors.empty()) {
    std::ostringstream message;
    message << "invalid processor structure:";
    for (const std::string& error : errors) {
      message << "\n  - " << error;
    }
    throw std::runtime_error(message.str());
  }

  std::unique_ptr<Processor::Impl> processor_impl(new Processor::Impl);
  processor_impl->machine = machine;
  processor_impl->nodes = std::move(impl_->nodes);
  processor_impl->edges = std::move(impl_->edges);
  processor_impl->stages = std::move(impl_->stages);
  processor_impl->registers = std::move(impl_->registers);
  processor_impl->units = std::move(impl_->units);

  return std::unique_ptr<Processor>(new Processor(std::move(processor_impl)));
}

Processor::Processor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Processor::~Processor() = default;

void Processor::Reset() {
  impl_->machine->Reset();
  impl_->cycles = 0;

  for (auto& unit : impl_->units) {
    unit.unit->uses_ = 0;
    unit.unit->Reset();
  }
  for (auto& stage : impl_->stages) {
    stage.component->Reset();
  }
  for (auto& state : impl_->registers) {
    state.state->Reset();
  }
}

void Processor::Clock() {
  if (impl_->machine->IsHalted()) {
    return;
  }

  const StageRole kOrder[] = {
      StageRole::kWriteback,
      StageRole::kMemory,
      StageRole::kExecute,
      StageRole::kDecode,
      StageRole::kFetch,
  };

  for (StageRole role : kOrder) {
    StageEntry* stage = impl_->FindStage(role);
    stage->component->Evaluate();
  }

  for (auto& state : impl_->registers) {
    state.state->Commit();
  }
  ++impl_->cycles;
}

bool Processor::IsHalted() const { return impl_->machine->IsHalted(); }

std::uint64_t Processor::Cycles() const { return impl_->cycles; }

void Processor::PrintPipeline(std::ostream& output) const {
  output << "BEGIN PIPELINE\n";
  output << "cycle " << impl_->cycles << "\n";

  const StageRole kOrder[] = {
      StageRole::kFetch,
      StageRole::kDecode,
      StageRole::kExecute,
      StageRole::kMemory,
      StageRole::kWriteback,
  };

  for (StageRole role : kOrder) {
    const StageEntry* stage = impl_->FindStage(role);
    StageSnapshot snapshot = stage->component->Snapshot();
    output << StageRoleName(role) << " valid " << (snapshot.valid ? 1 : 0)
           << " pc 0x" << std::hex << std::setw(8) << std::setfill('0')
           << snapshot.pc << std::dec << std::setfill(' ')
           << " opcode " << snapshot.opcode << " action " << snapshot.action
           << "\n";
  }
  output << "END PIPELINE\n";
}

void Processor::PrintStructure(std::ostream& output) const {
  output << "BEGIN STRUCTURE\n";
  output << "status OK\n";

  for (const NodeRecord& node : impl_->nodes) {
    output << "node " << node.id << " " << node.name << " ";
    if (node.kind == NodeRecord::Kind::kStage) {
      output << "STAGE " << StageRoleName(node.stage_role);
    } else if (node.kind == NodeRecord::Kind::kRegister) {
      output << "REGISTER " << RegisterRoleName(node.register_role);
    } else {
      output << "UNIT " << UnitRoleName(node.unit_role);
    }
    output << "\n";
  }

  for (const EdgeRecord& edge : impl_->edges) {
    output << "edge " << edge.source << " " << edge.destination << " "
           << ConnectionTypeName(edge.type) << "\n";
  }

  for (const UnitEntry& unit : impl_->units) {
    if (unit.role != UnitRole::kCustom) {
      output << "uses " << unit.name << " " << unit.unit->Uses() << "\n";
    }
  }
  output << "END STRUCTURE\n";
}

void Processor::PrintStatistics(std::ostream& output) const {
  output << "BEGIN STATISTICS\n";
  output << "cycles " << impl_->cycles << "\n";
  output << "retired " << impl_->machine->RetiredInstructions() << "\n";
  output << "data_stalls " << impl_->machine->DataStalls() << "\n";
  output << "control_stalls " << impl_->machine->ControlStalls() << "\n";
  output << "END STATISTICS\n";
}

}  // namespace pipesim

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: ./pipesim program.pisa\n";
    return 1;
  }

  pipesim::Machine machine;
  std::string error;
  if (!machine.LoadProgram(argv[1], &error)) {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  std::unique_ptr<pipesim::Processor> processor;
  try {
    processor = pipesim::BuildDesign(&machine);
    processor->Reset();
  } catch (const std::exception& exception) {
    std::cerr << "error: " << exception.what() << "\n";
    return 1;
  }

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream command(line);
    std::string name;
    command >> name;

    if (name.empty()) {
      continue;
    }
    if (name == "q") {
      break;
    }
    if (name == "n") {
      int count = 1;
      command >> count;
      if (count < 0) {
        count = 0;
      }
      int executed = 0;
      try {
        while (executed < count && !processor->IsHalted()) {
          processor->Clock();
          ++executed;
        }
      } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << "\n";
        return 1;
      }
      std::cout << "CYCLES " << executed << "\n";
      std::cout << "STATUS "
                << (processor->IsHalted() ? "HALTED" : "RUNNING") << "\n";
      continue;
    }
    if (name == "p") {
      machine.PrintState(std::cout);
      continue;
    }
    if (name == "pipe") {
      processor->PrintPipeline(std::cout);
      continue;
    }
    if (name == "graph") {
      processor->PrintStructure(std::cout);
      continue;
    }
    if (name == "s") {
      processor->PrintStatistics(std::cout);
      continue;
    }
    if (name == "reset") {
      processor->Reset();
      std::cout << "RESET OK\n";
      continue;
    }

    std::cout << "ERROR unknown command\n";
  }

  return 0;
}
