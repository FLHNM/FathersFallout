#ifndef F4MPSERVER_CONFIG_H_
#define F4MPSERVER_CONFIG_H_

#include "./ThirdParty/nlohmann/json.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>


// Missing JSON keys read with [] throw: newer keys use .value(key, default).
class Config
{
public:
    Config() {}

    // Omit copy constructor(s)
    Config(Config const&) = delete;
    void operator=(Config const&) = delete;

    static Config& getInstance()
    {
        static Config instance;
        return instance;
    }

    inline void Setup()
    {
        // .value(key, default) doesn't throw when the key is missing from the config.
        Name = JSON.value("server-name", std::string("FathersFallout Server"));
        Master = JSON.value("master-server", std::string(""));  // URL, vacio = desactivado
        TickRate = JSON.value("tick-rate", 60);                 // server ticks/s
        Ip = JSON["ip"];
        Port = static_cast<uint16_t>(JSON["port"]);
        PlayerLimit = static_cast<uint16_t>(JSON["player-limit"]);
        RunAsService = static_cast<bool>(JSON["run-as-service"]);
        LogLocation = JSON["log-location"];
        Password = JSON.value("password", std::string(""));    // "" = anyone can join
        // The host's game starts the server with the password typed in the launcher/F1 menu.
        if (const char* env = std::getenv("FF_SERVER_PASSWORD")) Password = env;
    }

    inline bool Validate()
    {
        return true;
    }


    inline bool Exists(){
            return std::filesystem::exists(ConfigLocation);
    }

    std::filesystem::path ConfigLocation { "./config.json" };


    std::string Name;
    std::string Master;
    int TickRate = 60;
    std::string Ip;
    uint16_t Port;
    uint16_t PlayerLimit;

    bool RunAsService;

    std::string LogLocation;
    std::string Password;

    nlohmann::json JSON;

};

#endif
