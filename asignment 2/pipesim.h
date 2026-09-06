#ifndef PIPESIM_H_
#define PIPESIM_H_

#include "execution_config.h"
#include "cache/cache_config.h"
#include "cache/cache_types.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pipesim
{

  namespace cache
  {
    class CacheController;
  }

  enum class Opcode
  {
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

  struct Instruction
  {
    Opcode opcode = Opcode::kNop;
    std::uint32_t pc = 0;
    int rd = 0;
    int rs1 = 0;
    int rs2 = 0;
    std::int32_t immediate = 0;
  };

  const char *OpcodeName(Opcode opcode);

  struct FetchedInstruction
  {
    std::uint64_t sequence = 0;
    std::uint32_t generation = 0;
    Instruction instruction;
  };

  struct Retirement
  {
    std::uint64_t sequence = 0;
    Instruction instruction;
    bool writes_register = false;
    std::uint32_t value = 0;
    bool halt = false;
  };

  enum class CpuStallReason
  {
    kControlDisabled,
    kIcacheInput,
    kDcacheInput,
    kOutstandingRaw,
    kOutstandingWaw,
    kFetchQueueFull,
    kPendingDataFull,
    kCompletionQueueFull,
    kExecutionUnitBusy,
    kExecutionCompletionSlot,
  };

  // Framework-owned architectural state, finite CPU queues, cache hierarchy,
  // memories, registered links, and global transaction-ID service.
  class Machine
  {
  public:
    Machine();
    ~Machine();

    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;

    bool LoadConfiguration(const std::string &file_name, std::string *error);
    bool LoadProgram(const std::string &file_name, std::string *error);
    void Reset();

    std::uint32_t ReadRegister(int index) const;
    std::uint32_t EntryPoint() const;

    // CPU/cache boundary. All operations are non-blocking and return false when
    // the corresponding finite structure or registered request link is full.
    bool FetchQueueHasCapacity() const;
    bool FetchRequestLinkCanAccept() const;
    bool TryIssueFetch(std::uint32_t pc);
    bool PeekFetched(FetchedInstruction *fetched) const;
    void ConsumeFetched();
    void InvalidateFetchesAfter(std::uint64_t sequence);

    bool CompletionQueueHasCapacity() const;
    bool HasOlderProducer(std::uint64_t sequence, int source_register) const;
    bool HasOlderDestination(std::uint64_t sequence,
                             int destination_register) const;
    bool NormalCompletionSlotAvailable(
        std::uint64_t result_boundary) const;
    bool TryAllocateCompletion(
        std::uint64_t sequence,
        const Instruction &instruction,
        bool writes_register,
        std::optional<std::uint64_t> normal_result_boundary);

    bool PendingDataHasCapacity() const;
    bool DataRequestLinkCanAccept() const;
    bool TryIssueData(std::uint64_t sequence, std::uint32_t address,
                      bool write, std::uint32_t write_value);
    void CompleteNonMemory(std::uint64_t sequence, std::uint32_t value,
                           bool halt);
    bool PeekRetirement(Retirement *retirement) const;
    void ApplyRetirement(const Retirement &retirement);

    bool IsHalted() const;
    void RecordCpuStall(CpuStallReason reason);
    std::uint64_t RetiredInstructions() const;
    std::uint64_t DataStalls() const;
    std::uint64_t ControlStalls() const;

    const cache::SystemConfig &Configuration() const;
    cache::TransactionIdService *TransactionIds();

    void PrintState(std::ostream &output) const;
    void PrintCacheState(std::ostream &output) const;
    void PrintStatistics(std::ostream &output) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    void InstallCacheControllers(
        std::unique_ptr<cache::CacheController> l1i,
        std::unique_ptr<cache::CacheController> l2i,
        std::unique_ptr<cache::CacheController> l1d,
        std::unique_ptr<cache::CacheController> l2d);
    void BeginCycle(std::uint64_t cycle);
    void EndCycle(std::uint64_t cycle);
    void CommitCycle();

    friend class Processor;
    friend class ProcessorBuilder;
  };

  enum class StageRole
  {
    kFetch,
    kDecode,
    kExecute,
    kMemory,
    kWriteback,
  };

  enum class RegisterRole
  {
    kProgramCounter,
    kIfId,
    kIdEx,
    kExMem,
    kMemWb,
    kCustom,
  };

  enum class UnitRole
  {
    kAdd,
    kSub,
    kAnd,
    kOr,
    kXor,
    kControl,
    kAgu,
    kMainControl,
    kHazardControl,
    kCustom,
  };

  enum class ConnectionType
  {
    kData,
    kClockedData,
    kControl,
    kStall,
    kBubble,
    kRedirect,
  };

  struct StageSnapshot
  {
    bool valid = false;
    std::uint32_t pc = 0;
    std::string opcode = "NOP";
    std::string action = "EMPTY";
  };

  class StageComponent
  {
  public:
    virtual ~StageComponent() = default;
    virtual void Reset() = 0;
    virtual void Evaluate() = 0;
    virtual StageSnapshot Snapshot() const = 0;
  };

  class ClockedState
  {
  public:
    virtual ~ClockedState() = default;
    virtual void Reset() = 0;
    virtual void Commit() = 0;
    virtual std::string Snapshot() const = 0;
  };

  class TrackedUnit
  {
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

  struct NodeId
  {
    int value = -1;
  };

  class Processor
  {
  public:
    ~Processor();

    void Reset();
    void Clock();
    bool IsHalted() const;
    std::uint64_t Cycles() const;

    void PrintPipeline(std::ostream &output) const;
    void PrintStructure(std::ostream &output) const;
    void PrintStatistics(std::ostream &output) const;

  private:
    class Impl;
    explicit Processor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class ProcessorBuilder;
  };

  class ProcessorBuilder
  {
  public:
    class Impl;

    ProcessorBuilder();
    ~ProcessorBuilder();

    NodeId AddStage(StageRole role, const std::string &name,
                    StageComponent *component);
    NodeId AddRegister(RegisterRole role, const std::string &name,
                       ClockedState *state);
    NodeId AddUnit(UnitRole role, const std::string &name, TrackedUnit *unit);
    NodeId AddCache(cache::CacheRole role, const std::string &name,
                    cache::CacheController *controller);
    void Connect(NodeId source, NodeId destination, ConnectionType type);

    std::unique_ptr<Processor> Build(Machine *machine);

  private:
    std::unique_ptr<Impl> impl_;
  };

  // Design entry point; fully implemented in this instructor reference tree.
  std::unique_ptr<Processor> BuildDesign(Machine *machine);

} // namespace pipesim

#endif // PIPESIM_H_
