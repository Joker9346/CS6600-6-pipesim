#include "pa4.h"

namespace multicore
{

    namespace
    {

        const std::string kIdle = "idle";
        const std::string kEvictClean = "evict-S";
        const std::string kEvictDirty = "evict-M";
        const std::string kAcquire = "acquire";

        std::string cache_name(int core) { return "cache." + std::to_string(core); }

        unsigned word_of(uint32_t address) { return (address % 64) / 4; }

        bool needs_exclusive(MemoryOperation operation)
        {
            return operation == MemoryOperation::Store || operation == MemoryOperation::Exchange;
        }

        bool is_evicting(const PrivateCache &cache)
        {
            return cache.phase == kEvictClean || cache.phase == kEvictDirty;
        }

        uint32_t apply_access(CacheLine &line, const MemoryRequest &request)
        {
            uint32_t &word = line.data[word_of(request.address)];
            switch (request.operation)
            {
            case MemoryOperation::Load:
                return word;
            case MemoryOperation::Store:
                word = request.value;
                return 0;
            case MemoryOperation::Exchange:
            {
                const uint32_t old = word;
                word = request.value;
                return old;
            }
            case MemoryOperation::Fence:
                break;
            }
            throw std::runtime_error("a fence must not reach the private cache");
        }

        MessageType acquisition_type(MemoryOperation operation)
        {
            return needs_exclusive(operation) ? MessageType::GetM : MessageType::GetS;
        }

        bool install_grant(MulticoreSystem &system, int core, const Packet &packet)
        {
            PrivateCache &cache = system.next.caches[core];
            if (!cache.pending || cache.phase != kAcquire ||
                packet.transaction_id != cache.transaction_id ||
                packet.line_address != system.line_address(cache.pending->address))
            {
                throw std::runtime_error("unexpected data grant at core " + std::to_string(core));
            }
            if (!system.can_send(core, MessageType::GrantAck))
                return false;

            const MemoryRequest request = *cache.pending;
            CacheLine &line = cache.lines[system.slot(request.address)];
            line.state = packet.type == MessageType::DataM ? CacheState::M : CacheState::S;
            line.tag = packet.line_address;
            line.data = packet.data;
            line.transient = false;
            if (needs_exclusive(request.operation) && line.state != CacheState::M)
            {
                throw std::runtime_error("write access granted without ownership");
            }

            const uint32_t result = apply_access(line, request);
            system.cache_complete(core, result);
            system.send(system.message(MessageType::GrantAck, core, system.home(packet.line_address),
                                       packet.line_address, packet.transaction_id, {}, core));
            return true;
        }

        bool finish_eviction(MulticoreSystem &system, int core, const Packet &packet)
        {
            PrivateCache &cache = system.next.caches[core];
            if (!cache.pending || !is_evicting(cache) ||
                packet.transaction_id != cache.transaction_id ||
                !cache.evicted_line_address || packet.line_address != *cache.evicted_line_address)
            {
                throw std::runtime_error("unexpected PutAck at core " + std::to_string(core));
            }
            const MemoryRequest &request = *cache.pending;
            const MessageType type = acquisition_type(request.operation);
            if (!system.can_send(core, type))
                return false;

            const uint32_t line_address = system.line_address(request.address);
            system.send(system.message(type, core, system.home(line_address), line_address,
                                       cache.transaction_id, {}, core));

            CacheLine &slot = cache.lines[system.slot(request.address)];
            slot.state = CacheState::I;
            slot.transient = false;
            cache.evicted_line_address.reset();
            cache.phase = kAcquire;
            return true;
        }

        bool handle_invalidation(MulticoreSystem &system, int core, const Packet &packet)
        {
            if (!system.can_send(core, MessageType::Ack))
                return false;
            PrivateCache &cache = system.next.caches[core];
            CacheLine &line = cache.lines[system.slot(packet.line_address)];

            const std::string prefix = cache_name(core);
            const bool owns_line = line.tag == packet.line_address;
            if (owns_line && line.state == CacheState::M && !line.transient)
            {
                throw std::runtime_error("Inv sent to the owner of a line");
            }
            const bool readable = owns_line && line.state == CacheState::S;
            const bool retained_clean = owns_line && line.transient && cache.phase == kEvictClean;

            system.event(prefix, "invalidations_received");
            if (readable || retained_clean)
            {
                line.state = CacheState::I;
                system.event(prefix, "invalidations_applied");
            }
            system.send(system.message(MessageType::Ack, core, packet.source, packet.line_address,
                                       packet.transaction_id, {}, packet.requester));
            system.event(prefix, "acks_sent");
            return true;
        }

        bool handle_recall(MulticoreSystem &system, int core, const Packet &packet)
        {
            if (!system.can_send(core, MessageType::Ack))
                return false;
            PrivateCache &cache = system.next.caches[core];
            CacheLine &line = cache.lines[system.slot(packet.line_address)];

            const bool owner = line.tag == packet.line_address && line.state == CacheState::M && !line.transient;
            const bool dirty_victim = line.tag == packet.line_address && line.transient &&
                                      cache.phase == kEvictDirty;
            if (!owner && !dirty_victim)
            {
                throw std::runtime_error("recall for a line that is not modified at core " + std::to_string(core));
            }

            system.send(system.message(MessageType::Ack, core, packet.source, packet.line_address,
                                       packet.transaction_id, line.data, packet.requester));
            system.event(cache_name(core), "acks_sent");
            if (owner)
            {
                line.state = packet.type == MessageType::RecallS ? CacheState::S : CacheState::I;
            }
            return true;
        }

    }

    bool accept_cache_request(
        MulticoreSystem &system,
        int core,
        const MemoryRequest &request)
    {

        if (request.operation == MemoryOperation::Fence)
        {
            throw std::runtime_error("a fence must not reach the private cache");
        }

        PrivateCache &cache = system.next.caches[core];
        const uint32_t line_address = system.line_address(request.address);
        CacheLine &line = cache.lines[system.slot(request.address)];
        const std::string prefix = cache_name(core);

        const bool exclusive = needs_exclusive(request.operation);
        const bool present = line.state != CacheState::I && line.tag == line_address;
        const bool has_permission = present && (!exclusive || line.state == CacheState::M);

        const char *accepted_event = request.operation == MemoryOperation::Load    ? "loads_accepted"
                                     : request.operation == MemoryOperation::Store ? "stores_accepted"
                                                                                   : "atomics_accepted";

        if (has_permission)
        {
            const char *hit_event = request.operation == MemoryOperation::Load    ? "load_permission_hits"
                                    : request.operation == MemoryOperation::Store ? "store_permission_hits"
                                                                                  : "atomic_permission_hits";
            system.event(prefix, accepted_event);
            system.event(prefix, hit_event);
            cache.pending = request;
            cache.transaction_id = request.id;
            system.cache_complete(core, apply_access(line, request));
            return true;
        }

        const bool victim = line.state != CacheState::I && !present;
        const MessageType release = line.state == CacheState::M ? MessageType::PutM : MessageType::PutS;
        const MessageType acquire = acquisition_type(request.operation);
        if (!system.can_send(core, victim ? release : acquire))
            return false;

        system.event(prefix, accepted_event);
        if (request.operation == MemoryOperation::Load)
        {
            system.event(prefix, "read_misses");
        }
        else if (request.operation == MemoryOperation::Exchange)
        {
            system.event(prefix, "atomic_misses");
        }
        else if (present)
        {
            system.event(prefix, "upgrades");
        }
        else
        {
            system.event(prefix, "write_misses");
        }

        cache.pending = request;
        cache.transaction_id = request.id;

        if (victim)
        {
            const uint32_t victim_address = line.tag;
            const bool dirty = release == MessageType::PutM;
            system.send(system.message(release, core, system.home(victim_address), victim_address,
                                       request.id, dirty ? line.data : CacheLineData{}, core));
            if (dirty)
            {
                system.event(prefix, "dirty_evictions");
                system.event(prefix, "writebacks_sent");
            }
            line.state = CacheState::I;
            line.transient = true;
            cache.evicted_line_address = victim_address;
            cache.phase = dirty ? kEvictDirty : kEvictClean;
        }
        else
        {
            system.send(system.message(acquire, core, system.home(line_address), line_address,
                                       request.id, {}, core));
            cache.phase = kAcquire;
        }
        return true;
    }

    bool handle_cache_message(
        MulticoreSystem &system,
        int core,
        const Packet &packet)
    {

        switch (packet.type)
        {
        case MessageType::DataS:
        case MessageType::DataM:
            return install_grant(system, core, packet);
        case MessageType::PutAck:
            return finish_eviction(system, core, packet);
        case MessageType::Inv:
            return handle_invalidation(system, core, packet);
        case MessageType::RecallS:
        case MessageType::RecallI:
            return handle_recall(system, core, packet);
        default:
            throw std::runtime_error(std::string("core received unexpected message ") + name(packet.type));
        }
    }

}
