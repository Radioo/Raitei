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
    std::uint32_t ordinal = 0;
    std::string_view name;
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

bool wanted(const Search& search, DWORD ordinal, LPCSTR name) {
    if (search.name.empty()) {
        return ordinal == search.ordinal;
    }
    return name != nullptr && search.name == name;
}

BOOL CALLBACK visit_function(PVOID context, DWORD ordinal, LPCSTR name, PVOID* slot) {
    auto& search = *static_cast<Search*>(context);
    if (search.in_dll && slot != nullptr && wanted(search, ordinal, name)) {
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

void** find_named_import(HMODULE module, std::string_view dll, std::string_view name) {
    Search search{.dll = dll, .name = name};
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
