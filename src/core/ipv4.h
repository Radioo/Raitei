#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace raitei {

bool is_virtual_address(std::uint32_t network_order);
std::string format_address(std::uint32_t network_order);
std::optional<std::uint32_t> parse_address(const std::string& text);

}
