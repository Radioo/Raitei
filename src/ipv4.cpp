#include "ipv4.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>

namespace raitei {

namespace {

constexpr std::uint32_t virtual_network = 0x64400000;
constexpr std::uint32_t virtual_netmask = 0xFFC00000;

}

bool is_virtual_address(std::uint32_t network_order) {
    return (ntohl(network_order) & virtual_netmask) == virtual_network;
}

std::string format_address(std::uint32_t network_order) {
    in_addr address{};
    address.s_addr = network_order;
    std::array<char, INET_ADDRSTRLEN> text{};
    if (inet_ntop(AF_INET, &address, text.data(), text.size()) == nullptr) {
        return {};
    }
    return text.data();
}

std::optional<std::uint32_t> parse_address(const std::string& text) {
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) {
        return std::nullopt;
    }
    return address.s_addr;
}

}
