#include "pa4.h"

namespace multicore {

    MemoryOrderDecision decide_memory_action(
        const MemoryRequest& request,
        const MemoryOrderUnit& order_unit,
        const SystemConfig& configuration) {

        MemoryOrderDecision decision;
        const auto& stores = order_unit.stores;

        switch (request.operation) {

            case MemoryOperation::Store:
                decision.kind = stores.size() < configuration.store_entries
                    ? MemoryAction::BufferStore : MemoryAction::Wait;
                break;

            case MemoryOperation::Load:
                decision.kind = MemoryAction::Issue;
                for (auto store = stores.rbegin(); store != stores.rend(); ++store) {
                    if (store->address == request.address) {
                        decision.kind = MemoryAction::Forward;
                        decision.value = store->value;
                        break;
                    }
                }
                break;

            case MemoryOperation::Fence:
                decision.kind = stores.empty() ? MemoryAction::CompleteFence : MemoryAction::Wait;
                break;

            case MemoryOperation::Exchange:
                decision.kind = stores.empty() ? MemoryAction::Issue : MemoryAction::Wait;
                break;
        }
        return decision;
    }

}
