#include "ccj_hooks.h"

#include "unet_bridge.h"

#include "core/address_queries.h"
#include "core/import_table.h"

#include <spdlog/spdlog.h>

#include <array>
#include <mutex>
#include <string_view>

namespace raitei {

namespace {

constexpr const char* ea3_module = "avs2-ea3.dll";
constexpr std::uint32_t ea3_status_query = 14;
constexpr const wchar_t* unity_player = L"UnityPlayer.dll";
constexpr const wchar_t* execexe_module = L"execexe.dll";
constexpr WORD execexe_get_module_handle = 25;
constexpr const char* winsock_module = "ws2_32.dll";
constexpr const char* kernel_module = "kernel32.dll";
constexpr std::uint32_t winsock_closesocket = 3;
constexpr std::uint32_t winsock_sendto = 20;
constexpr std::string_view receive_from_name = "WSARecvFrom";
constexpr std::string_view completion_name = "GetQueuedCompletionStatus";

StatusQuery status_query = nullptr;
Tunnel* tunnel = nullptr;
const SignalingClient* signaling = nullptr;
std::mutex bridge_mutex;
ccj::UnetBridge* bridge = nullptr;
bool bridged = false;

int WSAAPI hooked_sendto(SOCKET socket, const char* buffer, int length, int flags, const sockaddr* to, int to_length) {
    return bridge->sendto(socket, buffer, length, flags, to, to_length);
}

int WSAAPI hooked_receive_from(SOCKET socket, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from, LPINT from_length,
    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
    return bridge->receive_from(socket, buffers, count, received, flags, from, from_length, overlapped, routine);
}

BOOL WINAPI hooked_completion(HANDLE port, LPDWORD transferred, PULONG_PTR key, LPOVERLAPPED* overlapped, DWORD milliseconds) {
    return bridge->completion(port, transferred, key, overlapped, milliseconds);
}

int WSAAPI hooked_closesocket(SOCKET socket) {
    return bridge->close(socket);
}

HMODULE find_unity_player() {
    if (const HMODULE loaded = GetModuleHandleW(unity_player); loaded != nullptr) {
        return loaded;
    }
    const HMODULE execexe = GetModuleHandleW(execexe_module);
    if (execexe == nullptr) {
        return nullptr;
    }
    const auto get_module_handle = reinterpret_cast<decltype(&GetModuleHandleW)>(GetProcAddress(execexe, MAKEINTRESOURCEA(execexe_get_module_handle)));
    if (get_module_handle == nullptr) {
        return nullptr;
    }
    const HMODULE mapped = get_module_handle(unity_player);
    if (mapped != nullptr) {
        spdlog::info("UnityPlayer.dll was mapped by execexe");
    }
    return mapped;
}

struct UnityImports {
    void** sendto;
    void** receive_from;
    void** completion;
    void** close;
};

struct ImportWrite {
    void** slot;
    void* original;
    void* detour;
};

bool bridge_unity_player() {
    const HMODULE player = find_unity_player();
    if (player == nullptr) {
        spdlog::error("UnityPlayer.dll is not loaded, UNet cannot be bridged");
        return false;
    }

    const UnityImports slots{
        .sendto = find_ordinal_import(player, winsock_module, winsock_sendto),
        .receive_from = find_named_import(player, winsock_module, receive_from_name),
        .completion = find_named_import(player, kernel_module, completion_name),
        .close = find_ordinal_import(player, winsock_module, winsock_closesocket),
    };
    if (slots.sendto == nullptr || slots.receive_from == nullptr || slots.completion == nullptr || slots.close == nullptr) {
        spdlog::error("UnityPlayer.dll lacks one of the socket imports UNet uses, UNet cannot be bridged");
        return false;
    }

    const ccj::Winsock real{
        .sendto = reinterpret_cast<decltype(ccj::Winsock::sendto)>(*slots.sendto),
        .receive_from = reinterpret_cast<decltype(ccj::Winsock::receive_from)>(*slots.receive_from),
        .completion = reinterpret_cast<decltype(ccj::Winsock::completion)>(*slots.completion),
        .close = reinterpret_cast<decltype(ccj::Winsock::close)>(*slots.close),
    };
    if (bridge == nullptr) {
        bridge = new ccj::UnetBridge(*tunnel, real);
    }

    const std::array writes{
        ImportWrite{slots.sendto, *slots.sendto, reinterpret_cast<void*>(&hooked_sendto)},
        ImportWrite{slots.receive_from, *slots.receive_from, reinterpret_cast<void*>(&hooked_receive_from)},
        ImportWrite{slots.completion, *slots.completion, reinterpret_cast<void*>(&hooked_completion)},
        ImportWrite{slots.close, *slots.close, reinterpret_cast<void*>(&hooked_closesocket)},
    };
    for (std::size_t index = 0; index < writes.size(); ++index) {
        if (write_import(writes[index].slot, writes[index].detour)) {
            continue;
        }
        for (std::size_t undo = 0; undo < index; ++undo) {
            write_import(writes[undo].slot, writes[undo].original);
        }
        spdlog::error("could not write a UnityPlayer.dll import, every import was put back");
        return false;
    }
    spdlog::info("UNet in UnityPlayer.dll is bridged");
    return true;
}

void start_matching() {
    std::scoped_lock lock(bridge_mutex);
    if (!bridged) {
        bridged = bridge_unity_player();
    }
    if (!bridged) {
        return;
    }
    bridge->start_matching();
}

std::int32_t hooked_status_query(std::uint32_t command, const char* path, void* out, std::int32_t size) {
    const auto address = tunnel_address(*signaling);
    if (address.address && is_global_address_query(command, path)) {
        start_matching();
    }
    return answer_status_query(status_query, address, command, path, out, size);
}

}

bool install_ccj_hooks(HMODULE kamunity, Tunnel& target_tunnel, SignalingClient& target_signaling) {
    void** slot = find_ordinal_import(kamunity, ea3_module, ea3_status_query);
    if (slot == nullptr) {
        spdlog::error("kamunity.dll does not import ordinal {} from {}", ea3_status_query, ea3_module);
        return false;
    }
    tunnel = &target_tunnel;
    signaling = &target_signaling;
    status_query = reinterpret_cast<StatusQuery>(*slot);
    if (!write_import(slot, reinterpret_cast<void*>(&hooked_status_query))) {
        spdlog::error("could not write kamunity.dll's import of ordinal {} from {}", ea3_status_query, ea3_module);
        return false;
    }
    spdlog::info("patched kamunity.dll's facility query, UNet is bridged when matching starts");
    return true;
}

}
