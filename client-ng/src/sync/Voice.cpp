// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Proximity voice (see Voice.h), ported from Wasteland Wars' nearby voice. The mic is read by its own thread (winmm,
// 20 ms blocks of 16 kHz mono) while the talk key is held, coded with Opus and sent two frames to a packet through the
// server, unreliable. Each speaker plays on its own XAudio2 voice in stereo, panned and faded by where the speaker
// stands relative to us.

#include "pch.h"
#include "sync/Voice.h"
#include "sync/Appearance.h"
#include "sync/Authority.h"
#include "network/NetworkClient.h"
#include "F4SEMenuFramework.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <opus/opus.h>
#undef min
#undef max

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace F4MP::Voice
{
    using namespace Network;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int kRate = 16000;
        constexpr int kFrame = 320;                  // 20 ms
        constexpr int kFramesPerPacket = 2;          // 40 ms a packet: 25 packets/s while talking
        constexpr int kMicBuffers = 8;
        constexpr float kNearFull = 350.0f;          // full volume up to here (game units; 1 m ~ 70)
        constexpr float kNearMax = 3200.0f;          // ...and silent from here (~45 m)
        constexpr int kTailFrames = 10;              // keeps sending 200 ms after the key is let go (last word)
        constexpr int kRingBuffers = 24;             // queued playback per speaker (~1 s)

        // ---- settings ----
        // The talk key: any key or mouse button (Windows virtual-key code), picked in F1 by pressing it. Not V (hold V
        // = the workshop in a settlement).
        constexpr int kDefaultKey = 'N';

        std::string KeyName(int a_vk)
        {
            switch (a_vk) {
            case VK_LBUTTON: return "Left mouse";
            case VK_RBUTTON: return "Right mouse";
            case VK_MBUTTON: return "Middle mouse";
            case VK_XBUTTON1: return "Mouse 4";
            case VK_XBUTTON2: return "Mouse 5";
            case VK_LMENU: return "Left Alt";
            case VK_RMENU: return "Right Alt";
            case VK_LCONTROL: return "Left Ctrl";
            case VK_RCONTROL: return "Right Ctrl";
            case VK_LSHIFT: return "Left Shift";
            case VK_RSHIFT: return "Right Shift";
            default: break;
            }
            LONG sc = static_cast<LONG>(MapVirtualKeyA(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC)) << 16;
            if ((a_vk >= VK_PRIOR && a_vk <= VK_DOWN) || a_vk == VK_INSERT || a_vk == VK_DELETE) sc |= 1 << 24;   // extended
            char buf[64]{};
            if (GetKeyNameTextA(sc, buf, sizeof(buf)) > 0) return buf;
            return std::format("key {:#04x}", a_vk);
        }

        // The key being pressed right now that wasn't held a moment ago (0 = none): for "press a key to bind".
        int NewlyPressedKey()
        {
            static bool s_down[256]{};
            int found = 0;
            for (int vk = 1; vk < 256; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_ESCAPE || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_F1) {
                    s_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    continue;   // left click is for the menu, Esc cancels, generic modifiers have L/R versions, F1 = the menu
                }
                const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (down && !s_down[vk] && !found) found = vk;
                s_down[vk] = down;
            }
            return found;
        }

        std::string g_ini;
        std::atomic<bool> g_voiceOn{ true };
        std::atomic<int> g_key{ kDefaultKey };       // virtual-key code
        std::atomic<bool> g_binding{ false };        // F1: waiting for the talk key
        std::atomic<float> g_micGain{ 1.0f };
        float g_volume = 1.0f;
        std::atomic<bool> g_hearMyself{ false };     // mic test: my own voice played back to me

        // ---- shared with the mic thread ----
        std::atomic<bool> g_live{ false };           // connected (or testing the mic)
        std::atomic<bool> g_canTalk{ false };        // the game has focus, no console/typing
        std::atomic<bool> g_talking{ false };        // the key is held: what the mic thread is sending
        std::atomic<float> g_micLevel{ 0.0f };       // peak of the last block, 0..1
        std::atomic<bool> g_micError{ false };
        struct Me { float x = 0, y = 0, z = 0; uint32_t space = 0; };
        std::mutex g_meMutex;
        Me g_me;
        float g_heading = 0;                         // main thread only

        // My own packets for "hear myself" (made on the mic thread, played on the main thread).
        std::mutex g_loopMutex;
        std::deque<std::vector<uint8_t>> g_loop;

        // ---- playback ----
        IXAudio2* g_xa = nullptr;
        IXAudio2MasteringVoice* g_master = nullptr;
        bool g_audioTried = false;

        struct Speaker {
            OpusDecoder* dec = nullptr;
            IXAudio2SourceVoice* voice = nullptr;
            std::vector<int16_t> ring[kRingBuffers];   // XAudio2 plays straight from these: kept alive here
            int next = 0;
            uint16_t seq = 0;
            bool talking = false;
            Clock::time_point heard{};
        };
        std::unordered_map<uint32_t, Speaker> g_speakers;   // by player id (0 = me, hear-myself)
        std::recursive_mutex g_playMutex;                    // g_speakers: the game's task thread + the HUD pass

        bool StartAudio()
        {
            if (g_xa) return true;
            if (g_audioTried) return false;
            g_audioTried = true;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_xa) {
                REX::WARN("[Voice] XAudio2 unavailable: no voice playback");
                g_xa = nullptr;
                return false;
            }
            if (FAILED(g_xa->CreateMasteringVoice(&g_master)) || !g_master) {
                REX::WARN("[Voice] no audio output device: no voice playback");
                g_xa->Release();
                g_xa = nullptr;
                return false;
            }
            REX::INFO("[Voice] playback ready");
            return true;
        }

        Speaker* SpeakerOf(uint32_t a_id)
        {
            auto it = g_speakers.find(a_id);
            if (it != g_speakers.end()) return &it->second;
            if (!StartAudio()) return nullptr;
            Speaker s;
            int err = 0;
            s.dec = opus_decoder_create(kRate, 1, &err);
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = kRate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 4;
            wf.nAvgBytesPerSec = kRate * 4;
            if (!s.dec || FAILED(g_xa->CreateSourceVoice(&s.voice, &wf)) || !s.voice) {
                if (s.dec) opus_decoder_destroy(s.dec);
                return nullptr;
            }
            s.voice->Start();
            return &g_speakers.emplace(a_id, std::move(s)).first->second;
        }

        // A block of stereo samples onto the speaker's voice (dropped when ~1 s is already waiting: we fell behind).
        void Submit(Speaker& a_s, const float* a_left, const float* a_right, int a_n)
        {
            XAUDIO2_VOICE_STATE st{};
            a_s.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= kRingBuffers - 1) return;
            auto& buf = a_s.ring[a_s.next];
            a_s.next = (a_s.next + 1) % kRingBuffers;
            buf.resize(static_cast<size_t>(a_n) * 2);
            for (int i = 0; i < a_n; ++i) {
                buf[i * 2] = static_cast<int16_t>(std::clamp(a_left[i], -1.0f, 1.0f) * 32767.0f);
                buf[i * 2 + 1] = static_cast<int16_t>(std::clamp(a_right[i], -1.0f, 1.0f) * 32767.0f);
            }
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = static_cast<UINT32>(buf.size() * sizeof(int16_t));
            xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
            a_s.voice->SubmitSourceBuffer(&xb);
        }

        // Where the speaker is relative to me: volume and left/right (constant power).
        void Placement(const CoopVoiceMsg& a_m, float& a_left, float& a_right)
        {
            Me me;
            {
                std::lock_guard l(g_meMutex);
                me = g_me;
            }
            const float dx = a_m.x - me.x, dy = a_m.y - me.y, dz = a_m.z - me.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            float gain = d <= kNearFull ? 1.0f : std::max(0.0f, 1.0f - (d - kNearFull) / (kNearMax - kNearFull));
            gain = gain * gain * g_volume;
            // The game's heading: 0 = north (+y), clockwise. Side = sin of the angle from where I look.
            const float bearing = std::atan2(dx, dy);
            const float side = d > 1.0f ? std::sin(bearing - g_heading) : 0.0f;   // -1 left .. 1 right
            const float pan = (side * 0.8f + 1.0f) * 0.25f * 3.14159265f;          // 0..pi/2
            a_left = gain * std::cos(pan) * 1.41421356f;
            a_right = gain * std::sin(pan) * 1.41421356f;
        }

        void Play(uint32_t a_from, const uint8_t* a_data, size_t a_size)
        {
            if (a_size < sizeof(CoopVoiceMsg)) return;
            std::lock_guard pl(g_playMutex);
            CoopVoiceMsg m;
            std::memcpy(&m, a_data, sizeof(m));
            if (m.bytes > kVoiceMaxBytes || sizeof(m) + m.bytes > a_size || !SanePosition(m.x, m.y, m.z)) return;
            float gl = g_volume, gr = g_volume;
            if (a_from != 0) {
                uint32_t mySpace;
                {
                    std::lock_guard l(g_meMutex);
                    mySpace = g_me.space;
                }
                if (m.space != mySpace) return;   // somewhere else: another interior / outside
                Placement(m, gl, gr);
                if (gl + gr < 0.002f && !(m.flags & kVoiceEnd)) return;   // too far away to hear
            }
            Speaker* s = SpeakerOf(a_from);
            if (!s) return;
            const auto now = Clock::now();
            if (!s->talking || now - s->heard > std::chrono::milliseconds(600)) {
                // A new transmission: a little silence first to ride out network jitter.
                if (a_from) REX::INFO("[Voice] hearing player {} ({:.2f} {:.2f})", a_from, gl, gr);
                s->talking = true;
                s->seq = static_cast<uint16_t>(m.seq - 1);
                std::vector<float> z(kRate * 60 / 1000, 0.0f);
                Submit(*s, z.data(), z.data(), static_cast<int>(z.size()));
            }
            s->heard = now;
            // Lost packets (up to 3): the decoder fills the gap.
            const uint16_t gap = static_cast<uint16_t>(m.seq - s->seq - 1);
            float pcm[kFrame], left[kFrame], right[kFrame];
            auto out = [&](int a_n) {
                for (int i = 0; i < a_n; ++i) {
                    left[i] = pcm[i] * gl;
                    right[i] = pcm[i] * gr;
                }
                Submit(*s, left, right, a_n);
            };
            if (gap > 0 && gap <= 3)
                for (int k = 0; k < gap * kFramesPerPacket; ++k)
                    if (int n = opus_decode_float(s->dec, nullptr, 0, pcm, kFrame, 0); n > 0) out(n);
            s->seq = m.seq;
            const uint8_t* p = a_data + sizeof(m);
            const uint8_t* end = p + m.bytes;
            for (int f = 0; f < m.frames && p < end; ++f) {
                const int len = *p++;
                if (len <= 0 || p + len > end) break;
                if (int n = opus_decode_float(s->dec, p, len, pcm, kFrame, 0); n > 0) out(n);
                p += len;
            }
            if (m.flags & kVoiceEnd) s->talking = false;
        }

        // ---- the mic ----

        void Send(const std::vector<uint8_t>& a_packet)
        {
            auto& net = NetworkClient::GetInstance();
            if (net.IsConnected()) net.SendPacket(MessageType::CoopVoice, a_packet.data(), static_cast<uint32_t>(a_packet.size()));
            if (g_hearMyself) {
                std::lock_guard l(g_loopMutex);
                if (g_loop.size() < 50) g_loop.push_back(a_packet);
            }
        }

        bool KeyDown(int a_vk)
        {
            return a_vk > 0 && a_vk < 256 && (GetAsyncKeyState(a_vk) & 0x8000) != 0;
        }

        void MicThread()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            int err = 0;
            OpusEncoder* enc = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
            if (!enc) { REX::WARN("[Voice] Opus encoder failed ({}): no voice", err); return; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            HWAVEIN mic = nullptr;
            WAVEHDR hdr[kMicBuffers]{};
            int16_t data[kMicBuffers][kFrame]{};
            int nextHdr = 0;
            Clock::time_point lastTalk{};
            int tail = 0;
            bool sending = false;     // a transmission is going on
            bool localOnly = false;   // hear-myself without the key held: played back here, never sent
            uint16_t seq = 0;
            int sent = 0;                  // packets in this transmission (log)
            std::vector<uint8_t> frames;   // [len][bytes]...
            int inPacket = 0;
            auto flush = [&](bool a_end) {
                if (!inPacket && !a_end) return;
                std::vector<uint8_t> pkt(sizeof(CoopVoiceMsg) + frames.size());
                CoopVoiceMsg h{};
                h.flags = static_cast<uint8_t>(a_end ? kVoiceEnd : 0);
                h.seq = seq++;
                {
                    std::lock_guard l(g_meMutex);
                    h.space = g_me.space;
                    h.x = g_me.x;
                    h.y = g_me.y;
                    h.z = g_me.z;
                }
                h.frames = static_cast<uint8_t>(inPacket);
                h.bytes = static_cast<uint16_t>(frames.size());
                std::memcpy(pkt.data(), &h, sizeof(h));
                if (!frames.empty()) std::memcpy(pkt.data() + sizeof(h), frames.data(), frames.size());
                if (localOnly) {
                    std::lock_guard l(g_loopMutex);
                    if (g_loop.size() < 50) g_loop.push_back(std::move(pkt));
                } else {
                    Send(pkt);
                    ++sent;
                    if (a_end) {
                        REX::INFO("[Voice] sent {} packet(s)", sent);
                        sent = 0;
                    }
                }
                frames.clear();
                inPacket = 0;
            };
            auto close = [&]() {
                if (!mic) return;
                waveInReset(mic);
                for (auto& h : hdr) waveInUnprepareHeader(mic, &h, sizeof(h));
                waveInClose(mic);
                mic = nullptr;
                g_micLevel = 0;
                REX::INFO("[Voice] mic closed");
            };
            for (;;) {
                const bool live = g_live && g_voiceOn;
                const bool want = live && g_canTalk && !g_binding && KeyDown(g_key);
                const auto now = Clock::now();
                if (want) lastTalk = now;
                // The mic opens on the first press and stays open 8 s after the last one (Windows shows it in use).
                if (!mic && (want || g_hearMyself) && live) {
                    WAVEFORMATEX wf{};
                    wf.wFormatTag = WAVE_FORMAT_PCM;
                    wf.nChannels = 1;
                    wf.nSamplesPerSec = kRate;
                    wf.wBitsPerSample = 16;
                    wf.nBlockAlign = 2;
                    wf.nAvgBytesPerSec = kRate * 2;
                    if (waveInOpen(&mic, WAVE_MAPPER, &wf, reinterpret_cast<DWORD_PTR>(ev), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
                        mic = nullptr;
                        if (!g_micError.exchange(true)) REX::WARN("[Voice] no microphone could be opened");
                    } else {
                        g_micError = false;
                        for (int i = 0; i < kMicBuffers; ++i) {
                            hdr[i] = {};
                            hdr[i].lpData = reinterpret_cast<LPSTR>(data[i]);
                            hdr[i].dwBufferLength = sizeof(data[i]);
                            waveInPrepareHeader(mic, &hdr[i], sizeof(hdr[i]));
                            waveInAddBuffer(mic, &hdr[i], sizeof(hdr[i]));
                        }
                        nextHdr = 0;
                        waveInStart(mic);
                        REX::INFO("[Voice] mic open");
                    }
                }
                if (mic && !want && !tail && !g_hearMyself && (now - lastTalk > std::chrono::seconds(8) || !live)) close();
                if (!mic) {
                    if (sending) { flush(true); sending = false; g_talking = false; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                WaitForSingleObject(ev, 40);
                // Every finished block, in order.
                while (hdr[nextHdr].dwFlags & WHDR_DONE) {
                    auto& h = hdr[nextHdr];
                    const int n = static_cast<int>(h.dwBytesRecorded / 2);
                    float peak = 0;
                    const float gain = g_micGain;
                    for (int i = 0; i < n; ++i) {
                        const int v = static_cast<int>(data[nextHdr][i] * gain);
                        data[nextHdr][i] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
                        peak = std::max(peak, std::abs(data[nextHdr][i]) / 32768.0f);
                    }
                    g_micLevel = peak;
                    // Talking while the key is held, then the tail; the mic test plays back here only.
                    const bool talkNow = want || tail > 0 || g_hearMyself;
                    if (want) tail = kTailFrames;
                    else if (tail > 0) --tail;
                    const bool local = !want && tail == 0;
                    if (talkNow != sending || (sending && local != localOnly)) {
                        if (sending) flush(true);
                        sending = talkNow;
                        localOnly = local;
                    }
                    g_talking = want;
                    if (sending && n == kFrame) {
                        uint8_t out[256];
                        const int len = opus_encode(enc, data[nextHdr], kFrame, out, sizeof(out));
                        if (len > 0 && len < 256) {
                            frames.push_back(static_cast<uint8_t>(len));
                            frames.insert(frames.end(), out, out + len);
                            if (++inPacket >= kFramesPerPacket) flush(false);
                        }
                    }
                    h.dwFlags &= ~WHDR_DONE;
                    h.dwBytesRecorded = 0;
                    waveInAddBuffer(mic, &h, sizeof(h));
                    nextHdr = (nextHdr + 1) % kMicBuffers;
                }
                if (sending && !want && tail == 0 && !g_hearMyself) {
                    flush(true);
                    sending = false;
                }
            }
        }

        void Save()
        {
            if (g_ini.empty()) return;
            auto put = [](const char* k, const std::string& v) { WritePrivateProfileStringA("Voice", k, v.c_str(), g_ini.c_str()); };
            put("Voice", g_voiceOn ? "1" : "0");
            put("KeyTalk", std::format("{:#04x}", g_key.load()));   // virtual-key code
            put("MicGain", std::format("{:.2f}", g_micGain.load()));
            put("Volume", std::format("{:.2f}", g_volume));
        }
    }

    void Init(const std::string& a_iniPath)
    {
        g_ini = a_iniPath;
        const char* ini = g_ini.c_str();
        g_voiceOn = GetPrivateProfileIntA("Voice", "Voice", 1, ini) != 0;
        char buf[64]{};
        GetPrivateProfileStringA("Voice", "KeyTalk", "", buf, sizeof(buf), ini);
        const long vk = std::strtol(buf, nullptr, 0);
        g_key = vk > 0 && vk < 256 ? static_cast<int>(vk) : kDefaultKey;
        auto num = [&](const char* a_k, float a_def) {
            char b[32]{};
            GetPrivateProfileStringA("Voice", a_k, "", b, sizeof(b), ini);
            return b[0] ? std::clamp(static_cast<float>(std::atof(b)), 0.0f, 3.0f) : a_def;
        };
        g_micGain = num("MicGain", 1.0f);
        g_volume = num("Volume", 1.0f);
        std::thread(MicThread).detach();
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "hear") g_hearMyself = !g_hearMyself;
        std::lock_guard pl(g_playMutex);
        REX::INFO("[Voice] voice {} | key {} | can talk {} talking {} | mic {} level {:.2f} | hear myself {} | {} speaker(s), audio {}",
            g_voiceOn.load(), KeyName(g_key), g_canTalk.load(), g_talking.load(), g_micError ? "ERROR" : "ok", g_micLevel.load(),
            g_hearMyself.load(), g_speakers.size(), g_xa ? "ok" : "not started");
    }

    void OnPacket(const std::vector<uint8_t>& a_payload)
    {
        if (!g_voiceOn || a_payload.size() < sizeof(CoopVoiceMsg)) return;
        uint32_t from;
        std::memcpy(&from, a_payload.data(), sizeof(from));
        if (from == 0) return;   // 0 is me (hear-myself)
        Play(from, a_payload.data(), a_payload.size());
    }

    void Frame()
    {
        auto& net = NetworkClient::GetInstance();
        g_live = net.IsConnected() || g_hearMyself;
        if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
            const auto p = pl->GetPosition();
            std::lock_guard l(g_meMutex);
            g_me.x = p.x;
            g_me.y = p.y;
            g_me.z = p.z + 110.0f;   // the mouth, not the feet
            g_me.space = Authority::LocalSpace();
            g_heading = pl->data.angle.z;
        }
        // Talk only while playing: the game window in front, no console, no text box being typed in.
        bool can = false;
        if (HWND fg = GetForegroundWindow()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            can = pid == GetCurrentProcessId();
        }
        if (can) {
            if (auto* ui = RE::UI::GetSingleton(); ui && (ui->GetMenuOpen("Console") || ui->GetMenuOpen("LoadingMenu"))) can = false;
            if (auto* io = ImGuiMCP::GetIO(); io && io->WantTextInput) can = false;
        }
        g_canTalk = can;
        // Hear myself: my packets back through my own speaker.
        std::deque<std::vector<uint8_t>> loop;
        {
            std::lock_guard l(g_loopMutex);
            loop.swap(g_loop);
        }
        for (auto& pkt : loop) Play(0, pkt.data(), pkt.size());
        // Speakers gone quiet for a minute: their voices go.
        std::lock_guard pl(g_playMutex);
        const auto now = Clock::now();
        for (auto it = g_speakers.begin(); it != g_speakers.end();) {
            if (now - it->second.heard > std::chrono::seconds(60) && it->first != 0) {
                if (it->second.voice) it->second.voice->DestroyVoice();
                if (it->second.dec) opus_decoder_destroy(it->second.dec);
                it = g_speakers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void RenderMenu()
    {
        ImGuiMCP::SeparatorText("Voice chat (nearby)");
        bool b = g_voiceOn;
        if (ImGuiMCP::Checkbox("Voice chat", &b)) { g_voiceOn = b; Save(); }
        ImGuiMCP::BeginDisabled(!g_voiceOn);
        // Rebinding: click Change, then press any key or mouse button (Esc cancels).
        if (g_binding) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                g_binding = false;
            } else if (const int vk = NewlyPressedKey()) {
                g_key = vk;
                g_binding = false;
                Save();
            }
        }
        ImGuiMCP::Text("Push to talk (hold): %s", g_binding ? "press a key or mouse button... (Esc = cancel)" : KeyName(g_key).c_str());
        ImGuiMCP::SameLine();
        if (!g_binding && ImGuiMCP::SmallButton("Change")) { g_binding = true; NewlyPressedKey(); }
        if (g_key == 'V')
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "V is the game's workshop key in settlements: pick another one.");
        float f = g_micGain;
        if (ImGuiMCP::SliderFloat("Mic volume", &f, 0.2f, 3.0f, "%.2f")) g_micGain = f;
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        ImGuiMCP::SliderFloat("Voices volume", &g_volume, 0.0f, 2.0f, "%.2f");
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        b = g_hearMyself;
        if (ImGuiMCP::Checkbox("Mic test: hear myself", &b)) g_hearMyself = b;
        ImGuiMCP::ProgressBar(std::min(1.0f, g_micLevel.load() * 1.5f), ImGuiMCP::ImVec2(-1.0f, 0.0f), g_micError ? "no microphone" : "mic");
        ImGuiMCP::TextDisabled("Your voice carries about 45 m and only where you are (same building / outdoors).");
        ImGuiMCP::EndDisabled();
    }

    void RenderHud()
    {
        std::vector<std::string> lines;
        if (g_talking) lines.push_back("[TALKING]");
        const auto now = Clock::now();
        std::unique_lock pl(g_playMutex);
        for (auto& [id, s] : g_speakers) {
            if (!id || !s.talking || now - s.heard > std::chrono::milliseconds(500)) continue;
            std::string who = Appearance::NameOf(id);
            if (who.empty()) who = "Player " + std::to_string(id);
            lines.push_back(std::format("< {} >", who));
        }
        pl.unlock();
        if (lines.empty()) return;
        auto* io = ImGuiMCP::GetIO();
        auto* dl = ImGuiMCP::GetForegroundDrawList();
        if (!io || !dl) return;
        namespace DL = ImGuiMCP::ImDrawListManager;
        float y = io->DisplaySize.y * 0.62f;
        for (const auto& t : lines) {
            const float x = io->DisplaySize.x * 0.04f;
            DL::AddText(dl, ImGuiMCP::ImVec2(x + 1.0f, y + 1.0f), 0xC0000000u, t.c_str());
            DL::AddText(dl, ImGuiMCP::ImVec2(x, y), t[0] == '[' ? 0xFF40C8FFu : 0xFF80FF1Au, t.c_str());
            y += 20.0f;
        }
    }
}
