#include "iidx_hooks.h"

#include "address_queries.h"
#include "virtual_network.h"

#include "core/import_table.h"
#include "core/ipv4.h"

#include <spdlog/spdlog.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace raitei {

namespace {

constexpr const char* core_module = "avs2-core.dll";
constexpr const char* ea3_module = "avs2-ea3.dll";
constexpr std::uint32_t avs_net_socket = 122;
constexpr std::uint32_t avs_net_bind = 125;
constexpr std::uint32_t avs_net_connect = 126;
constexpr std::uint32_t avs_net_listen = 127;
constexpr std::uint32_t avs_net_accept = 128;
constexpr std::uint32_t avs_net_close = 129;
constexpr std::uint32_t avs_net_shutdown = 130;
constexpr std::uint32_t avs_net_send = 131;
constexpr std::uint32_t avs_net_sendto = 132;
constexpr std::uint32_t avs_net_recv = 135;
constexpr std::uint32_t avs_net_recvfrom = 136;
constexpr std::uint32_t avs_net_poll = 139;
constexpr std::uint32_t avs_net_query = 119;
constexpr std::uint32_t ea3_status_query = 14;

std::unique_ptr<iidx::VirtualNetwork> network;
iidx::StatusQuery status_query = nullptr;
iidx::NetQuery net_query = nullptr;
SignalingClient* signaling = nullptr;

std::int32_t net_bind(std::int32_t descriptor, std::uint32_t address, std::uint16_t port) {
    return network->bind(descriptor, address, port);
}

std::int32_t net_listen(std::int32_t descriptor, std::int32_t backlog) {
    return network->listen(descriptor, backlog);
}

std::int32_t net_accept(std::int32_t descriptor, std::uint32_t* address, std::uint16_t* port) {
    return network->accept(descriptor, address, port);
}

std::int32_t net_connect(std::int32_t descriptor, std::uint32_t address, std::uint16_t port) {
    return network->connect(descriptor, address, port);
}

std::int32_t net_poll(iidx::PollEntry* entries, std::uint32_t count, std::int32_t timeout) {
    return network->poll(entries, count, timeout);
}

std::int32_t net_send(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length) {
    return network->send(descriptor, buffer, length);
}

std::int32_t net_recv(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity) {
    return network->recv(descriptor, buffer, capacity);
}

std::int32_t net_sendto(std::int32_t descriptor, const std::uint8_t* buffer, std::int32_t length, std::uint32_t address, std::uint16_t port) {
    return network->sendto(descriptor, buffer, length, address, port);
}

std::int32_t net_recvfrom(std::int32_t descriptor, std::uint8_t* buffer, std::int32_t capacity, std::uint32_t* address, std::uint16_t* port) {
    return network->recvfrom(descriptor, buffer, capacity, address, port);
}

std::int32_t net_shutdown(std::int32_t descriptor, std::int32_t how) {
    return network->shutdown(descriptor, how);
}

std::int32_t net_close(std::int32_t descriptor) {
    return network->close(descriptor);
}

iidx::TunnelAddress tunnel_address() {
    const auto self = signaling->registration();
    return iidx::TunnelAddress{.configured = signaling->configured(), .address = self ? parse_address(self->address) : std::nullopt};
}

std::int32_t hooked_status_query(std::uint32_t command, const char* path, void* out, std::int32_t size) {
    return iidx::answer_status_query(status_query, tunnel_address(), command, path, out, size);
}

std::int32_t hooked_net_query(std::uint32_t command, void* out, std::int32_t size) {
    return iidx::answer_net_query(net_query, tunnel_address(), command, out, size);
}

enum class Need {
    always,
    when_imported,
};

struct Import {
    const char* dll;
    std::uint32_t ordinal;
    void** original;
    void* detour;
    Need need = Need::always;
};

template <typename Function>
void** original_of(Function& function) {
    return reinterpret_cast<void**>(&function);
}

}

bool install_iidx_hooks(HMODULE game, Tunnel& tunnel, SignalingClient& target_signaling) {
    iidx::AvsNet real{};
    const std::array imports{
        Import{core_module, avs_net_socket, original_of(real.socket), nullptr},
        Import{core_module, avs_net_bind, original_of(real.bind), reinterpret_cast<void*>(&net_bind)},
        Import{core_module, avs_net_listen, original_of(real.listen), reinterpret_cast<void*>(&net_listen)},
        Import{core_module, avs_net_accept, original_of(real.accept), reinterpret_cast<void*>(&net_accept)},
        Import{core_module, avs_net_connect, original_of(real.connect), reinterpret_cast<void*>(&net_connect)},
        Import{core_module, avs_net_poll, original_of(real.poll), reinterpret_cast<void*>(&net_poll)},
        Import{core_module, avs_net_send, original_of(real.send), reinterpret_cast<void*>(&net_send)},
        Import{core_module, avs_net_recv, original_of(real.recv), reinterpret_cast<void*>(&net_recv)},
        Import{core_module, avs_net_sendto, original_of(real.sendto), reinterpret_cast<void*>(&net_sendto)},
        Import{core_module, avs_net_recvfrom, original_of(real.recvfrom), reinterpret_cast<void*>(&net_recvfrom)},
        Import{core_module, avs_net_shutdown, original_of(real.shutdown), reinterpret_cast<void*>(&net_shutdown), Need::when_imported},
        Import{core_module, avs_net_close, original_of(real.close), reinterpret_cast<void*>(&net_close)},
        Import{core_module, avs_net_query, original_of(net_query), reinterpret_cast<void*>(&hooked_net_query)},
        Import{ea3_module, ea3_status_query, original_of(status_query), reinterpret_cast<void*>(&hooked_status_query)},
    };

    std::vector<void**> slots;
    bool complete = true;
    for (const auto& import : imports) {
        slots.push_back(find_ordinal_import(game, import.dll, import.ordinal));
        if (slots.back() != nullptr) {
            continue;
        }
        if (import.need == Need::always) {
            spdlog::error("the game does not import ordinal {} from {}", import.ordinal, import.dll);
            complete = false;
        } else {
            spdlog::info("the game does not import ordinal {} from {}, so it never calls it", import.ordinal, import.dll);
        }
    }
    if (!complete) {
        spdlog::error("no import was patched");
        return false;
    }

    for (std::size_t index = 0; index < imports.size(); ++index) {
        if (slots[index] != nullptr) {
            *imports[index].original = *slots[index];
        }
    }
    network = std::make_unique<iidx::VirtualNetwork>(tunnel, real);
    signaling = &target_signaling;

    std::vector<std::size_t> written;
    for (std::size_t index = 0; index < imports.size(); ++index) {
        if (imports[index].detour == nullptr || slots[index] == nullptr) {
            continue;
        }
        if (!write_import(slots[index], imports[index].detour)) {
            for (const auto undo : written) {
                write_import(slots[undo], *imports[undo].original);
            }
            spdlog::error("could not write the import of ordinal {} from {}, every import was put back", imports[index].ordinal, imports[index].dll);
            return false;
        }
        written.push_back(index);
    }
    spdlog::info("patched {} imports of the game", written.size());
    return true;
}

}
