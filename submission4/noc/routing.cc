#include "pa4.h"

namespace multicore
{

    namespace
    {

        constexpr unsigned kRouterInputs = 15;

        int step_x(const MeshCoordinate &current, const MeshCoordinate &destination)
        {
            if (destination.x > current.x)
                return PositiveX;
            if (destination.x < current.x)
                return NegativeX;
            return Local;
        }

        int step_y(const MeshCoordinate &current, const MeshCoordinate &destination)
        {
            if (destination.y > current.y)
                return PositiveY;
            if (destination.y < current.y)
                return NegativeY;
            return Local;
        }

        unsigned cursor_distance(unsigned input, unsigned cursor)
        {
            return (input + kRouterInputs - cursor % kRouterInputs) % kRouterInputs;
        }

    }

    int select_output_port(const RoutingContext &context, const SystemConfig &configuration)
    {
        const auto &current = context.current_router;
        const auto &destination = context.destination_router;
        if (configuration.routing == "YX")
        {
            const int port = step_y(current, destination);
            return port != Local ? port : step_x(current, destination);
        }
        const int port = step_x(current, destination);
        return port != Local ? port : step_y(current, destination);
    }

    std::optional<unsigned> select_router_input(
        const std::vector<ArbitrationCandidate> &candidates,
        unsigned round_robin_cursor,
        const SystemConfig &configuration)
    {

        if (candidates.empty())
            return std::nullopt;

        const bool oldest_first = configuration.arbitration != "rr";
        const ArbitrationCandidate *best = &candidates.front();
        for (const auto &candidate : candidates)
        {
            const bool older = candidate.packet.injected < best->packet.injected;
            const bool same_age = candidate.packet.injected == best->packet.injected;
            const bool closer = cursor_distance(candidate.input, round_robin_cursor) <
                                cursor_distance(best->input, round_robin_cursor);
            if (oldest_first ? (older || (same_age && closer)) : closer)
                best = &candidate;
        }
        return best->input;
    }

}
