// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/WorldPause.h"
#include "network/NetworkClient.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace F4MP::WorldPause
{
    namespace
    {
        std::atomic<bool> g_paused{ false };
        std::atomic<bool> g_realTimeVats{ false };
        std::mutex g_lock;
        double g_pausedSecs = 0.0;
        float g_waitedHours = 0.0f;
        float g_waitStartDays = -1.0f;
        std::chrono::steady_clock::time_point g_lastFrame{};

        float GameDays()
        {
            auto* cal = RE::Calendar::GetSingleton();
            return cal && cal->gameDaysPassed ? cal->gameDaysPassed->value : 0.0f;
        }

        // Wait/Sleep: how far the clock jumped while the menu was up.
        class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event.menuName != "SleepWaitMenu") return RE::BSEventNotifyControl::kContinue;
                std::lock_guard l(g_lock);
                if (a_event.opening) {
                    g_waitStartDays = GameDays();
                } else if (g_waitStartDays >= 0.0f) {
                    const float hours = (GameDays() - g_waitStartDays) * 24.0f;
                    g_waitStartDays = -1.0f;
                    if (hours > 0.2f) {
                        g_waitedHours += hours;
                        REX::INFO("[WorldPause] waited/slept {:.1f} game hours", hours);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuWatcher g_watcher;
    }

    void Install()
    {
        if (auto* ui = RE::UI::GetSingleton()) ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_watcher);
    }

    void OnFrame()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        // menuMode counts the open menus that pause the game. VATS slows
        // the world instead of stopping it: just as unable to run the others' NPCs at normal speed - unless it runs
        // in real time (co-op default), then nothing needs handing over.
        const bool loading = ui->GetMenuOpen("LoadingMenu");
        const bool waiting = ui->GetMenuOpen("SleepWaitMenu");
        const bool vats = ui->GetMenuOpen("VATSMenu");
        const bool paused = !loading && (vats ? !g_realTimeVats.load() : ui->menuMode > 0);
        if (paused != g_paused.load()) REX::INFO("[WorldPause] our game {}", paused ? "paused in a menu: the others run its NPCs" : "running again");
        g_paused = paused;

        const auto now = std::chrono::steady_clock::now();
        // Only while playing together: the main menu before connecting is a pause too.
        if (paused && !waiting && g_lastFrame != std::chrono::steady_clock::time_point{} && Network::NetworkClient::GetInstance().IsConnected()) {
            const double dt = std::chrono::duration<double>(now - g_lastFrame).count();
            if (dt > 0.0 && dt < 1.0) {
                std::lock_guard l(g_lock);
                g_pausedSecs += dt;
            }
        }
        g_lastFrame = now;
    }

    bool LocalPaused() { return g_paused.load(); }

    void SetRealTimeVats(bool a_on)
    {
        // fVATSTimeMultTargetSelect: how fast the world runs while picking targets (vanilla 0.04 = nearly stopped).
        static float s_vanilla = -1.0f;
        static bool s_on = false;
        if (a_on == s_on) return;
        auto* gmst = RE::GameSettingCollection::GetSingleton();
        auto* s = gmst ? gmst->GetSetting("fVATSTimeMultTargetSelect") : nullptr;
        if (!s || s->GetType() != RE::Setting::SETTING_TYPE::kFloat) return;
        if (s_vanilla < 0.0f) s_vanilla = s->GetFloat();
        s->SetFloat(a_on ? 1.0f : s_vanilla);
        s_on = a_on;
        g_realTimeVats = a_on;
        REX::INFO("[WorldPause] VATS targeting {} (fVATSTimeMultTargetSelect {:.2f})", a_on ? "in real time" : "slowed as usual", s->GetFloat());
    }

    double TakePausedSeconds()
    {
        std::lock_guard l(g_lock);
        return std::exchange(g_pausedSecs, 0.0);
    }

    float TakeWaitedHours()
    {
        std::lock_guard l(g_lock);
        return std::exchange(g_waitedHours, 0.0f);
    }
}
