#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace raitei {

void** find_ordinal_import(HMODULE module, std::string_view dll, std::uint32_t ordinal);
bool write_import(void** slot, void* function);

}
