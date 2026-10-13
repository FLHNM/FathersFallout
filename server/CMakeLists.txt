cmake_minimum_required(VERSION 3.13)
project(F4MPServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-D_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/resources/")

# --- Info de version (opcional, no rompe si git no esta) ---
execute_process(COMMAND git log --pretty=format:'%H' -n 1
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)
add_definitions(-DGIT_VERSION="${GIT_REV}")

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M")
add_definitions(-DBUILD_TIME="${BUILD_TIME}")

# --- Fuentes reales del servidor ---
# (Se excluye a proposito el codigo legacy: Server.cpp/.hpp y Logger.cpp/.hpp)
add_executable(F4MPServer
    src/main.cpp
    src/Network/GameServer.cpp
    src/Heartbeat.cpp
)

if (WIN32)
    # ====================== Windows ======================
    target_compile_definitions(F4MPServer PRIVATE _WINDOWS)

    # Dependencias vendidas en el repo (third_party) para no necesitar vcpkg.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/win-x64/share")
        list(INSERT CMAKE_PREFIX_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/win-x64")
        message(STATUS "F4MP: usando dependencias de third_party/win-x64 (sin vcpkg)")
    endif()

    find_package(GameNetworkingSockets CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)

    target_link_libraries(F4MPServer
        GameNetworkingSockets::GameNetworkingSockets
        spdlog::spdlog
        ws2_32
        crypt32
        advapi32
        winhttp      # heartbeat (HTTP) en Windows
    )
else()
    # ====================== Linux / otros ======================
    # Dependencias del sistema. En Debian/Ubuntu:
    #   sudo apt install build-essential cmake pkg-config \
    #        libgamenetworkingsockets-dev libspdlog-dev libcurl4-openssl-dev
    # (si tu distro no empaqueta GameNetworkingSockets, compilalo desde
    #  https://github.com/ValveSoftware/GameNetworkingSockets e instalalo.)
    find_package(Threads REQUIRED)
    find_package(CURL REQUIRED)
    find_package(spdlog REQUIRED)

    # GameNetworkingSockets: primero su config de CMake, si no via pkg-config.
    find_package(GameNetworkingSockets CONFIG QUIET)
    if (GameNetworkingSockets_FOUND)
        set(F4MP_GNS GameNetworkingSockets::GameNetworkingSockets)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GNS REQUIRED IMPORTED_TARGET GameNetworkingSockets)
        set(F4MP_GNS PkgConfig::GNS)
    endif()

    target_link_libraries(F4MPServer
        ${F4MP_GNS}
        spdlog::spdlog
        CURL::libcurl          # heartbeat (HTTP/HTTPS) en Linux
        Threads::Threads
    )
endif()
