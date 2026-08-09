#ifndef PIPESIM_H_
#define PIPESIM_H_

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

namespace pipesim {

enum class Opcode {
  kNop,
  kAdd,
  kSub,
  kAnd,
  kOr,
  kXor,
  kAddi,
  kLw,
  kSw,
  kBeq,
  kBne,
  kJ,
  kHalt,
};

struct Instruction {
  Opcode opcode = Opcode::kNop;
  std::uint32_t pc = 0;
  int rd = 0;
  int rs1 = 0;
  int rs2 = 0;
  std::int32_t immediate = 0;
};

const char* OpcodeName(Opcode opcode);

class Machine {
 public:
  Machine();

  bool LoadProgram(const std::string& file_name, std::string* error);
  void Reset();

  bool Fetch(std::uint32_t pc, Instruction* instruction) const;

  std::uint32_t ReadRegister(int index) const;
  void WriteRegister(int index, std::uint32_t value);

  std::uint32_t LoadWord(std::uint32_t address) const;
  void StoreWord(std::uint32_t address, std::uint32_t value);

  std::uint32_t EntryPoint() const;
  bool IsHalted() const;
  void SetHalted();

  void RetireInstruction();
  void AddDataStall();
  void AddControlStall();

  std::uint64_t RetiredInstructions() const;
  std::uint64_t DataStalls() const;
  std::uint64_t ControlStalls() const;

  void PrintState(std::ostream& output) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

enum class StageRole {
  kFetch,
  kDecode,
  kExecute,
  kMemory,
  kWriteback,
};

enum class RegisterRole {
  kProgramCounter,
  kIfId,
  kIdEx,
  kExMem,
  kMemWb,
  kCustom,
};

enum class UnitRole {
  kAlu,
  kMainControl,
  kHazardControl,
  kCustom,
};

enum class ConnectionType {
  kData,
  kClockedData,
  kControl,
  kStall,
  kBubble,
  kRedirect,
};

struct StageSnapshot {
  bool valid = false;
  std::uint32_t pc = 0;
  std::string opcode = "NOP";
  std::string action = "EMPTY";
};

class StageComponent {
 public:
  virtual ~StageComponent() = default;
  virtual void Reset() = 0;
  virtual void Evaluate() = 0;
  virtual StageSnapshot Snapshot() const = 0;
};

class ClockedState {
 public:
  virtual ~ClockedState() = default;
  virtual void Reset() = 0;
  virtual void Commit() = 0;
  virtual std::string Snapshot() const = 0;
};

class TrackedUnit {
 public:
  virtual ~TrackedUnit() = default;
  virtual void Reset() {}
  std::uint64_t Uses() const;

 protected:
  void Touch();

 private:
  std::uint64_t uses_ = 0;

  friend class Processor;
};

struct NodeId {
  int value = -1;
};

class Processor {
 public:
  ~Processor();

  void Reset();
  void Clock();
  bool IsHalted() const;
  std::uint64_t Cycles() const;

  void PrintPipeline(std::ostream& output) const;
  void PrintStructure(std::ostream& output) const;
  void PrintStatistics(std::ostream& output) const;

 private:
  class Impl;
  explicit Processor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend class ProcessorBuilder;
};

class ProcessorBuilder {
 public:
  class Impl;

  ProcessorBuilder();
  ~ProcessorBuilder();

  NodeId AddStage(StageRole role, const std::string& name,
                  StageComponent* component);
  NodeId AddRegister(RegisterRole role, const std::string& name,
                     ClockedState* state);
  NodeId AddUnit(UnitRole role, const std::string& name, TrackedUnit* unit);

  void Connect(NodeId source, NodeId destination, ConnectionType type);

  std::unique_ptr<Processor> Build(Machine* machine);

 private:
  std::unique_ptr<Impl> impl_;
};

// Students implement this function in design.cc.
std::unique_ptr<Processor> BuildDesign(Machine* machine);

}  // namespace pipesim

#endif  // PIPESIM_H_
