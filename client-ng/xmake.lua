-- CommonLibF4 (submodulo en lib/commonlibf4)
includes("lib/commonlibf4")

set_project("FathersFallout")
set_license("GPL-3.0")
set_version("0.26.1")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Dependencia de red
add_requires("gamenetworkingsockets")
-- Voice chat: proximity push-to-talk (campaign) + radio (Wasteland Wars), Opus-coded
add_requires("libopus")

target("F4MPClient")
    -- CommonLibF4 declares the two IVirtualMachine::CreateObject overloads the wrong way round for MSVC, which
    -- puts a later overload first in the vtable: the two-argument call landed on the game's three-argument
    -- function (slot 0xB0), which then used a stale register as its result pointer. Script calls crashed on any
    -- form with no bound script object yet, such as a dialogue topic. Swap them until upstream does.
    on_load(function (target)
        local header = path.join(os.projectdir(), "lib/commonlibf4/include/RE/B/BSScript_IVirtualMachine.h")
        local text = io.readfile(header)
        local fixed, n = text:gsub("([ \t]*virtual bool%s+CreateObject%(const BSFixedString& a_objectTypeName, const BSTScrapHashMap[^\n]*\n)"
            .. "([ \t]*virtual bool%s+CreateObject%(const BSFixedString& a_objectTypeName, BSTSmartPointer[^\n]*\n)", "%2%1")
        if n > 0 then io.writefile(header, fixed) end
    end)

    add_packages("gamenetworkingsockets", "libopus")
    -- Esta regla genera automaticamente la version del plugin de F4SE
    -- (adios al "incompatible") y despliega el DLL al juego.
    add_rules("commonlibf4.plugin", {
        name = "FathersFallout",
        author = "Copyright (C) 2026 FatherLamb",
        contact = "FatherLamb (YouTube) - FathersFallout",
        description = "FathersFallout - online co-op for Fallout 4"
    })

    add_files("src/**.cpp")
    add_shflags("/MAP:build/F4MPClient.map", {force = true})        -- F4MPClient.map: crash offsets in the log -> functions
    add_syslinks("winmm", "xaudio2", "ole32")   -- voice: the mic (waveIn), playback (XAudio2 2.9, part of Windows 10+)
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
