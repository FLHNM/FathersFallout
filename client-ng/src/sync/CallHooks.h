// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <cstddef>
#include <cstdint>

namespace F4MP
{
    // Sends every direct call (E8 rel32) to a_target in Fallout4.exe's code through a_hook instead. Found by
    // scanning the code, so no list of call sites to update after a game patch. At plugin load only (other
    // threads may run these calls later). 14 trampoline bytes per site. Returns the number hooked.
    std::size_t HookAllCalls(std::uintptr_t a_target, std::uintptr_t a_hook, std::size_t a_max, std::size_t* a_found = nullptr);

    template <class F>
    std::size_t HookAllCalls(std::uintptr_t a_target, F* a_hook, std::size_t a_max, std::size_t* a_found = nullptr)
    {
        return HookAllCalls(a_target, reinterpret_cast<std::uintptr_t>(a_hook), a_max, a_found);
    }
}
