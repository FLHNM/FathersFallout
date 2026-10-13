// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#pragma once

#include <ShlObj.h>
#include <filesystem>
#include <string>

namespace F4MP
{
    // Documents\My Games\Fallout4\F4SE, where the game and F4SE keep their logs. Asks Windows for the
    // real Documents folder: it is often redirected (OneDrive\Documents), so %USERPROFILE%\Documents
    // may not exist. Created if missing. "" if Windows can't say.
    inline std::string F4SEDocsDir()
    {
        PWSTR docs = nullptr;
        std::filesystem::path dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs)))
            dir = std::filesystem::path(docs) / "My Games" / "Fallout4" / "F4SE";
        CoTaskMemFree(docs);
        if (dir.empty()) return {};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir.string();
    }
}
