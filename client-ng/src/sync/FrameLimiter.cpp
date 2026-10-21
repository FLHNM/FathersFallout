// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/FrameLimiter.h"

#include <Windows.h>

namespace F4MP::FrameLimiter
{
    namespace
    {
        LARGE_INTEGER g_freq{};
        LARGE_INTEGER g_last{};
        HANDLE g_timer = nullptr;
        float g_fps = 0.0f;

        // Sleep most of the remaining time on a high-resolution waitable timer, then spin
        // the last ~1 ms for an exact frame time without burning a core.
        void WaitUntil(LONGLONG deadline)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const LONGLONG remainingTicks = deadline - now.QuadPart;
            const LONGLONG remaining100ns = remainingTicks * 10'000'000 / g_freq.QuadPart;
            if (g_timer && remaining100ns > 15'000) {  // > 1.5 ms
                LARGE_INTEGER due;
                due.QuadPart = -(remaining100ns - 10'000);  // relative, wake 1 ms early
                if (SetWaitableTimerEx(g_timer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(g_timer, INFINITE);
            }
            do {
                YieldProcessor();
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < deadline);
        }
    }

    void OnFrame()
    {
        if (!g_freq.QuadPart) {
            QueryPerformanceFrequency(&g_freq);
            QueryPerformanceCounter(&g_last);
            g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            REX::INFO("[FathersFallout] frame limiter active: {} FPS (hi-res timer: {})", kTargetFps, g_timer != nullptr);
            return;
        }

        const LONGLONG period = g_freq.QuadPart / kTargetFps;
        LONGLONG deadline = g_last.QuadPart + period;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        // Loading screens: no cap. The game loads faster the more frames it gets (a player report:
        // "extremely long loading times" for the host), and nothing is synced while loading.
        static int s_loadFrames = 0;
        static LONGLONG s_loadStart = 0;
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GetMenuOpen("LoadingMenu")) {
            if (!s_loadFrames++) s_loadStart = now.QuadPart;
            g_last = now;
            return;
        }
        if (s_loadFrames) {
            REX::INFO("[FathersFallout] loading screen: {:.1f} s, {} frames uncapped", static_cast<double>(now.QuadPart - s_loadStart) / g_freq.QuadPart, s_loadFrames);
            s_loadFrames = 0;
        }
        if (now.QuadPart < deadline) {
            WaitUntil(deadline);
        } else if (now.QuadPart - deadline > period) {
            deadline = now.QuadPart;  // fell far behind (loading, hitch): don't try to catch up
        }

        QueryPerformanceCounter(&now);
        const float dt = static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_freq.QuadPart);
        if (dt > 0.0f) g_fps = g_fps <= 0.0f ? 1.0f / dt : g_fps * 0.95f + (1.0f / dt) * 0.05f;

        static LONGLONG s_lastReport = now.QuadPart;
        if (now.QuadPart - s_lastReport > g_freq.QuadPart * 30) {
            s_lastReport = now.QuadPart;
            REX::INFO("[FathersFallout] fps {:.1f} (cap {})", g_fps, kTargetFps);
        }
        g_last.QuadPart = deadline > now.QuadPart - period ? deadline : now.QuadPart;
    }

    float GetMeasuredFps() { return g_fps; }
}
