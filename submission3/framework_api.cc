#include "pipesim.h"

#include "cache/cache_types.h"

namespace pipesim {

const char* OpcodeName(Opcode opcode) {
  switch (opcode) {
    case Opcode::kNop: return "NOP";
    case Opcode::kAdd: return "ADD";
    case Opcode::kSub: return "SUB";
    case Opcode::kAnd: return "AND";
    case Opcode::kOr: return "OR";
    case Opcode::kXor: return "XOR";
    case Opcode::kAddi: return "ADDI";
    case Opcode::kLw: return "LW";
    case Opcode::kSw: return "SW";
    case Opcode::kBeq: return "BEQ";
    case Opcode::kBne: return "BNE";
    case Opcode::kJ: return "J";
    case Opcode::kHalt: return "HALT";
  }
  return "UNKNOWN";
}

std::uint64_t TrackedUnit::Uses() const { return uses_; }

void TrackedUnit::Touch() {
  cache::CheckedIncrement(&uses_, "unit uses");
}

}  // namespace pipesim
