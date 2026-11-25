#include "import_table.h"

#include <detours/detours.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace raitei {

namespace {

struct Search {
    std::string_view dll;
    std::uint32_t ordinal;
    bool in_dll = false;
    void** slot = nullptr;
};

bool same_file(std::string_view imported, std::string_view wanted) {
    const auto name = std::filesystem::path(imported).filename().string();
    return std::ranges::equal(name, wanted, [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    });
}

BOOL CALLBACK visit_file(PVOID context, HMODULE, LPCSTR file) {
    auto& search = *static_cast<Search*>(context);
    search.in_dll = file != nullptr && same_file(file, search.dll);
    return TRUE;
}

BOOL CALLBACK visit_function(PVOID context, DWORD ordinal, LPCSTR, PVOID* slot) {
    auto& search = *static_cast<Search*>(context);
    if (search.in_dll && slot != nullptr && ordinal == search.ordinal) {
        search.slot = reinterpret_cast<void**>(slot);
        return FALSE;
    }
    return TRUE;
}

}

void** find_ordinal_import(HMODULE module, std::string_view dll, std::uint32_t ordinal) {
    Search search{.dll = dll, .ordinal = ordinal};
    DetourEnumerateImportsEx(module, &search, &visit_file, &visit_function);
    return search.slot;
}

bool write_import(void** slot, void* function) {
    DWORD previous = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        return false;
    }
    *slot = function;
    VirtualProtect(slot, sizeof(void*), previous, &previous);
    return true;
}

}
