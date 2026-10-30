// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Based on F4MP, Copyright (C) 2020 Alin Octavian, Benjamin Kyd.
// Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.

#include <cctype>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cassert>
#include <csignal>
#include <thread>
#include <memory>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/async.h>

#include "ThirdParty/nlohmann/json.hpp"
#include "Config.hpp"
#include "Network/GameServer.h"
#include "Wars/ALife.h"

#if defined(_WINDOWS)
#include <windows.h>
#include <libloaderapi.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t g_running = 1;

// Wasteland Wars runs its own copy of the server, named FathersFalloutWarsServer.exe, from its own folder (own
// config.json, port 7790): it never shares a server or a port with the campaign.
static bool g_wars = false;

void SignalHandler(int signal) {
    g_running = 0;
    spdlog::info("[Server] Received signal {}, shutting down...", signal);
}

int UMain() {
    spdlog::info("FathersFallout Server v1.0.0{}", g_wars ? " - Wasteland Wars" : "");

    uint16_t port = static_cast<uint16_t>(Config::getInstance().Port);

    // Logs to the console AND (optionally) a file.
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    const std::string& logLoc = Config::getInstance().LogLocation;
    if (!logLoc.empty() && logLoc != "NONE") {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logLoc, true));
    }
    auto logger = std::make_shared<spdlog::logger>("server", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);

    GameServer::GetInstance().SetName(Config::getInstance().Name);
    GameServer::GetInstance().SetMaxPlayers(Config::getInstance().PlayerLimit);
    GameServer::GetInstance().SetMaster(Config::getInstance().Master);
    GameServer::GetInstance().SetTickRate(Config::getInstance().TickRate);
    GameServer::GetInstance().SetPassword(Config::getInstance().Password);
    GameServer::GetInstance().SetWars(g_wars);

    if (!GameServer::GetInstance().Initialize(port)) {
        spdlog::error("[Server] Failed to initialize server on port {}", port);
        return 1;
    }

    // Reads console commands and queues them for the main loop.
    std::thread([]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            GameServer::GetInstance().QueueCommand(line);
        }
    }).detach();

    GameServer::GetInstance().Run();

    GameServer::GetInstance().Shutdown();
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    if (argc > 0 && argv[0]) {
        std::string exe = std::filesystem::path(argv[0]).filename().string();
        for (auto& ch : exe) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        g_wars = exe.find("wars") != std::string::npos;
    }

    auto console = spdlog::stdout_color_mt("console");
    auto err_logger = spdlog::stderr_color_mt("stderr");

    spdlog::get("console")->info("FathersFallout  Copyright (C) 2026  FatherLamb \n"
                                 "Based on F4MP  Copyright (C) 2020  Alin Octavian, Benjamin Kyd \n"
                                 "This program comes with ABSOLUTELY NO WARRANTY. \n"
                                 "This is free software, and you are welcome to redistribute it \n"
                                 "under certain conditions; read LICENSE and NOTICE for full details. \n \n");

    // --alife-test <game hours>: runs the Wasteland Wars world that long at full speed, prints what happened, exits.
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--alife-test") {
            Wars::ALife alife;
            if (!alife.LoadWorld(Wars::kWorldPath)) { spdlog::get("stderr")->error("no {}", Wars::kWorldPath); return 1; }
            alife.LoadState("./wars_alife_test.json");
            const double hours = std::atof(argv[i + 1]);
            for (double h = 1; h <= hours; h += 1.0) {
                alife.Tick(180.0);   // one game hour per step
                auto said = alife.TakeChatter();
                for (size_t k = 0; k < said.size() && k < 2; ++k) spdlog::info("[Radio] {}: {}", said[k].who, said[k].text);
                if (static_cast<int>(h) % 24 == 0) alife.PrintSummary();
            }
            alife.PrintEvents(30);
            alife.PrintSummary();
            alife.PrintNear("Concord");
            alife.SaveState("./wars_alife_test.json");
            return 0;
        }

    std::filesystem::path ConfigLocation{"./config.json"};

    if (!std::filesystem::exists(ConfigLocation)) {
        nlohmann::json Config;
        Config["server-name"] = g_wars ? "FathersFallout Wasteland Wars" : "FathersFallout Server";
        Config["master-server"] = "";  // a master server URL to be listed there (empty = not listed)
        Config["ip"] = "127.0.0.1";
        Config["port"] = g_wars ? 7790 : 7779;
        Config["run-as-service"] = false;
        Config["player-limit"] = 100;
        Config["tick-rate"] = 60;  // server ticks/s (higher = smoother)
        Config["password"] = "";   // join password; empty = anyone who can reach the port
        Config["log-location"] = "./logs.log";
        std::ofstream o{ConfigLocation};
        o << std::setw(4) << Config << std::endl;
        spdlog::get("stderr")->error("ERROR: no config exists. One has been created");
        exit(0);
    }

    std::ifstream InConfig{ConfigLocation};
    InConfig >> Config::getInstance().JSON;
    Config::getInstance().Setup();

    // Windows service mode isn't implemented: runs as a console program.
    if (Config::getInstance().RunAsService == true) {
        spdlog::get("console")->warn("run-as-service is not implemented yet; running in console mode");
    }

    return UMain();
}
