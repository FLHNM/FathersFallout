-- CommonLibF4 (submodulo en lib/commonlibf4)
includes("lib/commonlibf4")

set_project("FathersFallout")
set_license("GPL-3.0")
set_version("0.25.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Dependencia de red
add_requires("gamenetworkingsockets")
-- Voice chat: proximity push-to-talk, Opus-coded
add_requires("libopus")

target("F4MPClient")
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
    add_syslinks("winmm", "xaudio2", "ole32")   -- voice: the mic (waveIn), playback (XAudio2 2.9, part of Windows 10+)
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
