// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/CallHooks.h"

#include <Windows.h>

#include <vector>

namespace F4MP
{
    std::size_t HookAllCalls(std::uintptr_t a_target, std::uintptr_t a_hook, std::size_t a_max, std::size_t* a_found)
    {
        const std::uintptr_t img = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew);
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        std::vector<std::uintptr_t> sites;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uintptr_t start = img + sec->VirtualAddress;
            const std::uintptr_t end = start + sec->Misc.VirtualSize;
            for (std::uintptr_t at = start; at + 5 <= end; ++at) {
                if (*reinterpret_cast<const std::uint8_t*>(at) != 0xE8) continue;
                if (at + 5 + *reinterpret_cast<const std::int32_t*>(at + 1) == a_target) sites.push_back(at);
            }
        }
        if (a_found) *a_found = sites.size();
        const std::size_t n = sites.size() < a_max ? sites.size() : a_max;
        for (std::size_t i = 0; i < n; ++i) REL::GetTrampoline().write_call<5>(sites[i], a_hook);
        return n;
    }
}
