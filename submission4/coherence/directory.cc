#include "pa4.h"

namespace multicore
{

    namespace
    {

        std::string directory_name(const MulticoreSystem &system, int home_tile)
        {
            return "directory." + std::to_string(home_tile - system.configuration.cores);
        }

        void complete_transaction(MulticoreSystem &system, DirectoryEntry &entry, int home_tile)
        {
            const std::string prefix = directory_name(system, home_tile);
            system.event(prefix, "transactions_completed");
            system.event(prefix, "completed_latency_sum", system.cycle + 1 - entry.started);

            entry.active = false;
            entry.phase = "idle";
            entry.pending_acknowledgements.clear();

            if (entry.waiting.empty())
                return;
            entry.request = entry.waiting.front();
            entry.waiting.pop_front();
            entry.active = true;
            entry.started = system.cycle;
            entry.phase = "begin";
            system.event(prefix, "transactions_started");
        }

        void remove_sharer(DirectoryEntry &entry, int core)
        {
            entry.sharers.erase(core);
            if (entry.state == CacheState::S && entry.sharers.empty())
                entry.state = CacheState::I;
        }

        void route(DirectoryEntry &entry)
        {
            for (;;)
            {
                const MessageType type = entry.request.type;
                const int requester = entry.request.requester;

                if (entry.phase == "begin")
                {
                    if (type == MessageType::PutS)
                    {
                        entry.phase = "put-s";
                    }
                    else if (type == MessageType::PutM)
                    {
                        entry.phase = "put-m";
                    }
                    else if (entry.state == CacheState::M)
                    {
                        entry.phase = "recall";
                    }
                    else if (type == MessageType::GetM && entry.state == CacheState::S)
                    {
                        entry.sharers.erase(requester);
                        entry.phase = entry.sharers.empty() ? "llc-read" : "inv";
                    }
                    else
                    {
                        entry.phase = "llc-read";
                    }
                }
                else if (entry.phase == "put-s")
                {
                    remove_sharer(entry, requester);
                    entry.phase = "put-ack";
                }
                else if (entry.phase == "put-m")
                {
                    if (entry.state == CacheState::M && entry.owner == requester)
                    {
                        entry.data = entry.request.data;
                        entry.phase = "llc-write";
                    }
                    else
                    {
                        remove_sharer(entry, requester);
                        entry.phase = "put-ack";
                    }
                }
                else if (entry.phase == "recalled")
                {
                    entry.phase = type == MessageType::GetS ? "llc-write" : "grant";
                }
                else
                {
                    return;
                }
            }
        }

        void send_recall(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile)
        {
            const MessageType type = entry.request.type == MessageType::GetS ? MessageType::RecallS
                                                                             : MessageType::RecallI;
            if (!system.can_send(home_tile, type))
                return;
            system.send(system.message(type, home_tile, entry.owner, line, entry.request.transaction_id,
                                       {}, entry.request.requester));
            entry.pending_acknowledgements = {entry.owner};
            entry.phase = "recall-wait";
            system.event(directory_name(system, home_tile), "recalls_sent");
        }

        void send_invalidations(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile)
        {
            if (!entry.sharers.empty())
            {
                const int sharer = *entry.sharers.begin();
                if (!system.can_send(home_tile, MessageType::Inv))
                    return;
                system.send(system.message(MessageType::Inv, home_tile, sharer, line,
                                           entry.request.transaction_id, {}, entry.request.requester));
                entry.sharers.erase(sharer);
                entry.pending_acknowledgements.insert(sharer);
                system.event(directory_name(system, home_tile), "invalidations_sent");
            }
            else if (entry.pending_acknowledgements.empty())
            {
                entry.phase = "llc-read";
            }
        }

        void request_llc(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile, bool write)
        {
            LLCRequest request;
            request.line_address = line;
            request.transaction_id = entry.request.transaction_id;
            request.requester = entry.request.requester;
            request.write = write;
            if (write)
                request.data = entry.data;
            if (!system.llc_request(home_tile, request))
                return;
            entry.phase = write ? "llc-write-wait" : "llc-read-wait";
        }

        void collect_llc_reply(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile)
        {
            const auto reply = system.llc_reply(home_tile, line, entry.request.transaction_id,
                                                entry.request.requester);
            if (!reply)
                return;
            entry.data = reply->data;
            if (entry.request.type == MessageType::PutM)
            {
                entry.state = CacheState::I;
                entry.owner = -1;
                entry.sharers.clear();
                entry.phase = "put-ack";
            }
            else
            {
                entry.phase = "grant";
            }
        }

        void send_grant(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile)
        {
            const bool exclusive = entry.request.type == MessageType::GetM;
            const MessageType type = exclusive ? MessageType::DataM : MessageType::DataS;
            if (!system.can_send(home_tile, type))
                return;
            const int requester = entry.request.requester;
            system.send(system.message(type, home_tile, requester, line, entry.request.transaction_id,
                                       entry.data, requester));
            if (exclusive)
            {
                entry.state = CacheState::M;
                entry.owner = requester;
                entry.sharers.clear();
                system.event(directory_name(system, home_tile), "ownership_grants");
            }
            else
            {
                if (entry.state == CacheState::M)
                    entry.sharers.insert(entry.owner);
                entry.sharers.insert(requester);
                entry.state = CacheState::S;
                entry.owner = -1;
            }
            entry.phase = "grant-wait";
        }

        void send_put_ack(MulticoreSystem &system, DirectoryEntry &entry, uint32_t line, int home_tile)
        {
            if (!system.can_send(home_tile, MessageType::PutAck))
                return;
            const int requester = entry.request.requester;
            system.send(system.message(MessageType::PutAck, home_tile, requester, line,
                                       entry.request.transaction_id, {}, requester));
            complete_transaction(system, entry, home_tile);
        }

        bool accept_request(MulticoreSystem &system, const Packet &packet, int home_tile)
        {
            const std::string prefix = directory_name(system, home_tile);
            auto found = system.next.directory.find(packet.line_address);

            if (found != system.next.directory.end() && (found->second.active || !found->second.waiting.empty()))
            {
                if (found->second.waiting.size() >= system.configuration.router_depth)
                    return false;
                found->second.waiting.push_back(packet);
                system.event(prefix, "requests_accepted");
                system.event(prefix, "conflicts_queued");
                return true;
            }

            DirectoryEntry &entry = system.next.directory[packet.line_address];
            entry.active = true;
            entry.request = packet;
            entry.started = system.cycle;
            entry.phase = "begin";
            entry.pending_acknowledgements.clear();
            system.event(prefix, "requests_accepted");
            system.event(prefix, "transactions_started");
            return true;
        }

        bool accept_ack(MulticoreSystem &system, const Packet &packet, int home_tile)
        {
            auto found = system.next.directory.find(packet.line_address);
            if (found == system.next.directory.end() || !found->second.active)
            {
                throw std::runtime_error("Ack with no active directory transaction");
            }
            DirectoryEntry &entry = found->second;
            const bool matches = entry.request.transaction_id == packet.transaction_id &&
                                 entry.request.requester == packet.requester &&
                                 entry.pending_acknowledgements.count(packet.source) != 0;
            if (!matches || (entry.phase != "recall-wait" && entry.phase != "inv"))
            {
                throw std::runtime_error("Ack does not match the active directory transaction");
            }
            entry.pending_acknowledgements.erase(packet.source);
            if (entry.phase == "recall-wait")
            {
                entry.data = packet.data;
                entry.phase = "recalled";
            }
            system.event(directory_name(system, home_tile), "acks_received");
            return true;
        }

        bool accept_grant_ack(MulticoreSystem &system, const Packet &packet, int home_tile)
        {
            auto found = system.next.directory.find(packet.line_address);
            if (found == system.next.directory.end() || !found->second.active)
            {
                throw std::runtime_error("GrantAck with no active directory transaction");
            }
            DirectoryEntry &entry = found->second;
            if (entry.phase != "grant-wait" || entry.request.transaction_id != packet.transaction_id ||
                entry.request.requester != packet.requester)
            {
                throw std::runtime_error("GrantAck does not match the active directory transaction");
            }
            complete_transaction(system, entry, home_tile);
            return true;
        }

    }

    bool handle_directory_message(MulticoreSystem &system, const Packet &packet)
    {
        const int home_tile = system.home(packet.line_address);
        switch (packet.type)
        {
        case MessageType::GetS:
        case MessageType::GetM:
        case MessageType::PutS:
        case MessageType::PutM:
            return accept_request(system, packet, home_tile);
        case MessageType::Ack:
            return accept_ack(system, packet, home_tile);
        case MessageType::GrantAck:
            return accept_grant_ack(system, packet, home_tile);
        default:
            throw std::runtime_error(std::string("home received unexpected message ") + name(packet.type));
        }
    }

    void advance_directory_transaction(MulticoreSystem &system, uint32_t line_address)
    {
        auto found = system.next.directory.find(line_address);
        if (found == system.next.directory.end() || !found->second.active)
            return;

        DirectoryEntry &entry = found->second;
        if (entry.phase == "begin" && entry.started == system.cycle)
            return;

        const int home_tile = system.home(line_address);
        route(entry);

        if (entry.phase == "recall")
        {
            send_recall(system, entry, line_address, home_tile);
        }
        else if (entry.phase == "inv")
        {
            send_invalidations(system, entry, line_address, home_tile);
            if (entry.phase == "llc-read")
                request_llc(system, entry, line_address, home_tile, false);
        }
        else if (entry.phase == "llc-read")
        {
            request_llc(system, entry, line_address, home_tile, false);
        }
        else if (entry.phase == "llc-read-wait" || entry.phase == "llc-write-wait")
        {
            collect_llc_reply(system, entry, line_address, home_tile);
        }
        else if (entry.phase == "llc-write")
        {
            request_llc(system, entry, line_address, home_tile, true);
        }
        else if (entry.phase == "grant")
        {
            send_grant(system, entry, line_address, home_tile);
        }
        else if (entry.phase == "put-ack")
        {
            send_put_ack(system, entry, line_address, home_tile);
        }
    }

}
