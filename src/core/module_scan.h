#pragma once

#include <windows.h>

#include <string_view>

namespace raitei {

void* find_function_referencing(HMODULE module, std::string_view text);

}
