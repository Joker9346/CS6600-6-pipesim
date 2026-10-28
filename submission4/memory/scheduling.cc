#include "pa4.h"

namespace multicore
{

    namespace
    {

        constexpr Cycle kAgeLimit = 64;

        unsigned bank_of(uint32_t line_address, const SystemConfig &configuration)
        {
            return ((line_address / 64) / configuration.controllers) % configuration.banks;
        }

        unsigned row_of(uint32_t line_address, const SystemConfig &configuration)
        {
            return ((line_address / 64) / configuration.controllers) / configuration.banks / 16;
        }

        bool has_older_same_line_request(unsigned index, const MemoryController &controller)
        {
            for (unsigned older = 0; older < index; ++older)
            {
                if (controller.queue[older].line_address == controller.queue[index].line_address)
                    return true;
            }
            return false;
        }

        bool arrived_before(unsigned a, unsigned b, const MemoryController &controller)
        {
            const Cycle arrival_a = controller.queue[a].controller_arrival;
            const Cycle arrival_b = controller.queue[b].controller_arrival;
            return arrival_a != arrival_b ? arrival_a < arrival_b : a < b;
        }

    }

    std::optional<unsigned> select_memory_request(
        const std::vector<unsigned> &eligible_requests,
        const MemoryController &controller,
        Cycle cycle,
        const SystemConfig &configuration)
    {

        if (eligible_requests.empty())
            return std::nullopt;

        const bool fcfs = configuration.scheduler == "fcfs";
        std::optional<unsigned> oldest;
        std::optional<unsigned> oldest_row_hit;
        for (unsigned index : eligible_requests)
        {
            if (!fcfs && has_older_same_line_request(index, controller))
                continue;
            if (!oldest || arrived_before(index, *oldest, controller))
                oldest = index;

            const auto &request = controller.queue[index];
            const auto &bank = controller.banks[bank_of(request.line_address, configuration)];
            const bool row_hit = bank.open_row >= 0 &&
                                 unsigned(bank.open_row) == row_of(request.line_address, configuration);
            if (row_hit && (!oldest_row_hit || arrived_before(index, *oldest_row_hit, controller)))
            {
                oldest_row_hit = index;
            }
        }

        if (fcfs || !oldest_row_hit)
            return oldest;
        const Cycle waited = cycle - controller.queue[*oldest].controller_arrival;
        return waited >= kAgeLimit ? oldest : oldest_row_hit;
    }

}
