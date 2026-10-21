// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Based on F4MP, Copyright (C) 2020 Alin Octavian, Benjamin Kyd.
// Licensed under the GNU GPL v3 with additional terms: see LICENSE and NOTICE.

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

#if defined(_WINDOWS)
#include <windows.h>
#include <libloaderapi.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t g_running = 1;

void SignalHandler(int signal) {
    g_running = 0;
    spdlog::info("[Server] Received signal {}, shutting down...", signal);
}

int UMain() {
    spdlog::info("FathersFallout Server v1.0.0");

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

    auto console = spdlog::stdout_color_mt("console");
    auto err_logger = spdlog::stderr_color_mt("stderr");

    spdlog::get("console")->info("FathersFallout  Copyright (C) 2026  FatherLamb \n"
                                 "Based on F4MP  Copyright (C) 2020  Alin Octavian, Benjamin Kyd \n"
                                 "This program comes with ABSOLUTELY NO WARRANTY. \n"
                                 "This is free software, and you are welcome to redistribute it \n"
                                 "under certain conditions; read LICENSE and NOTICE for full details. \n \n");

    std::filesystem::path ConfigLocation{"./config.json"};

    if (!std::filesystem::exists(ConfigLocation)) {
        nlohmann::json Config;
        Config["server-name"] = "FathersFallout Server";
        Config["master-server"] = "";  // a master server URL to be listed there (empty = not listed)
        Config["ip"] = "127.0.0.1";
        Config["port"] = 7779;
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
