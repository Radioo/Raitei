#include "module_scan.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>

namespace raitei {

namespace {

constexpr std::uint8_t rex_w = 0x48;
constexpr std::uint8_t rex_wr = 0x4C;
constexpr std::uint8_t lea_opcode = 0x8D;
constexpr std::uint8_t modrm_rip_mask = 0xC7;
constexpr std::uint8_t modrm_rip_relative = 0x05;
constexpr std::size_t lea_length = 7;
constexpr std::size_t lea_displacement_offset = 3;
constexpr std::uint8_t unwind_flag_chain_info = 0x4;
constexpr std::size_t unwind_flags_shift = 3;
constexpr std::size_t unwind_header_size = 4;
constexpr std::size_t unwind_code_size = 2;

std::span<const std::uint8_t> image_of(HMODULE module) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return {base, nt->OptionalHeader.SizeOfImage};
}

const RUNTIME_FUNCTION* primary_entry(const RUNTIME_FUNCTION* entry, DWORD64 image_base) {
    for (;;) {
        const auto* unwind = reinterpret_cast<const std::uint8_t*>(image_base + entry->UnwindData);
        if (((unwind[0] >> unwind_flags_shift) & unwind_flag_chain_info) == 0) {
            return entry;
        }
        const std::size_t codes = (static_cast<std::size_t>(unwind[2]) + 1) & ~std::size_t{1};
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(unwind + unwind_header_size + codes * unwind_code_size);
    }
}

}

void* find_function_referencing(HMODULE module, std::string_view text) {
    const auto image = image_of(module);
    const std::string needle(text.data(), text.size() + 1);
    const auto chars = std::span(reinterpret_cast<const char*>(image.data()), image.size());
    const auto found = std::search(chars.begin(), chars.end(), std::boyer_moore_searcher(needle.begin(), needle.end()));
    if (found == chars.end()) {
        return nullptr;
    }
    const auto* target = image.data() + (found - chars.begin());

    for (std::size_t offset = 0; offset + lea_length <= image.size(); ++offset) {
        const auto* instruction = image.data() + offset;
        if ((instruction[0] != rex_w && instruction[0] != rex_wr) || instruction[1] != lea_opcode ||
            (instruction[2] & modrm_rip_mask) != modrm_rip_relative) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, instruction + lea_displacement_offset, sizeof displacement);
        if (instruction + lea_length + displacement != target) {
            continue;
        }

        DWORD64 image_base = 0;
        const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(instruction), &image_base, nullptr);
        if (entry == nullptr) {
            continue;
        }
        return reinterpret_cast<void*>(image_base + primary_entry(entry, image_base)->BeginAddress);
    }
    return nullptr;
}

}
