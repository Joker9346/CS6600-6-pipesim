#include "pa4.h"
#include <algorithm>

namespace multicore
{

    namespace
    {

        bool ends_with(const std::string &text, const std::string &suffix)
        {
            return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        void sample_queue(MulticoreSystem &system, const std::string &prefix,
                          const std::string &sum_name, const std::string &max_name,
                          uint64_t length)
        {
            system.event(prefix, sum_name, length);
            system.event(prefix, max_name, length);
            system.event(prefix, "samples", 1);
        }

    }

    void count_event(
        MulticoreSystem &system,
        const std::string &component,
        const std::string &event,
        uint64_t amount)
    {

        const std::string name = component + "." + event;
        if (ends_with(event, "_max"))
        {
            system.counters.maximum(name, amount);
        }
        else
        {
            system.counters.add(name, amount);
        }
    }

    void sample_occupancy_and_stalls(MulticoreSystem &system)
    {

        const auto &state = system.current;
        const auto &configuration = system.configuration;

        for (int tile = 0; tile < configuration.tiles(); ++tile)
        {
            const std::string router = "noc." + std::to_string(tile);
            uint64_t router_total = 0;
            for (unsigned queue = 0; queue < 15; ++queue)
            {
                const uint64_t length = state.network.buffers[tile][queue].size();
                router_total += length;
                sample_queue(system, router + ".queue." + std::to_string(queue),
                             "occupancy_sum", "occupancy_max", length);
            }
            sample_queue(system, router, "queue_occupancy_sum", "queue_occupancy_max", router_total);
        }

        for (int index = 0; index < configuration.controllers; ++index)
        {
            sample_queue(system, "mc." + std::to_string(index),
                         "queue_occupancy_sum", "queue_occupancy_max",
                         state.controllers[index].queue.size());
        }

        for (int index = 0; index < configuration.homes; ++index)
        {
            sample_queue(system, "llc." + std::to_string(index),
                         "queue_occupancy_sum", "queue_occupancy_max",
                         state.llc[index].requests.size());
        }

        for (int core = 0; core < configuration.cores; ++core)
        {
            const std::string prefix = "order." + std::to_string(core);
            const auto &unit = state.order[core];
            sample_queue(system, prefix, "queue_occupancy_sum", "queue_occupancy_max", unit.stores.size());

            const auto &waiting = state.cpu[core];
            if (!waiting)
                continue;

            const bool cache_busy = unit.inflight.has_value();
            const bool stores_buffered = !unit.stores.empty();
            const bool is_store = waiting->operation == MemoryOperation::Store;
            const bool is_fence = waiting->operation == MemoryOperation::Fence;
            const bool is_exchange = waiting->operation == MemoryOperation::Exchange;

            if (is_store && unit.stores.size() >= configuration.store_entries)
            {
                system.event(prefix, "buffer_full_cycles");
            }
            if (is_fence && (stores_buffered || cache_busy))
            {
                system.event(prefix, "fence_wait_cycles");
            }
            if (cache_busy || ((is_fence || is_exchange) && stores_buffered))
            {
                system.event(prefix, "ordering_stall_cycles");
            }
        }
    }

}
