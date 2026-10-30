// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).
//
// Wasteland Wars voice (see WarsVoice.h). The mic is read by its own thread (winmm, 20 ms blocks of 16 kHz mono)
// while a talk key is held, coded with Opus and sent two frames to a packet through the server, unreliable. Each
// speaker plays on its own XAudio2 voice in stereo: nearby voices are panned and faded by where the speaker stands
// relative to us, radio voices go through a radio's band-pass + crunch with a squelch at both ends.

#include "pch.h"
#include "sync/WarsVoice.h"
#include "sync/Wars.h"
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
#include <random>
#include <thread>
#include <unordered_map>

namespace F4MP::Wars::Voice
{
    using namespace Network;

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int kRate = 16000;
        constexpr int kFrame = 320;                  // 20 ms
        constexpr int kFramesPerPacket = 2;          // 40 ms a packet: 25 packets/s while talking
        constexpr int kMicBuffers = 8;
        constexpr float kNearFull = 350.0f;          // nearby voice: full volume up to here (game units; 1 m ~ 70)
        constexpr float kNearMax = 3200.0f;          // ...and silent from here (~45 m)
        constexpr int kTailFrames = 10;              // keeps sending 200 ms after the key is let go (last word)
        constexpr int kRingBuffers = 24;             // queued playback per speaker (~1 s)

        // ---- settings ----
        // Talk keys: any key or mouse button (Windows virtual-key codes), picked in F1 by pressing it. Not V (hold V
        // = the workshop in a settlement).
        constexpr int kDefaultNear = 'N';
        constexpr int kDefaultRadio = 'B';

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
        std::atomic<bool> g_radioOn{ true };
        std::atomic<bool> g_chatter{ true };
        std::atomic<int> g_freq{ 1015 };             // tenths of MHz
        std::atomic<int> g_keyNear{ kDefaultNear };  // virtual-key codes
        std::atomic<int> g_keyRadio{ kDefaultRadio };
        std::atomic<int> g_binding{ 0 };             // F1: 1 = waiting for the nearby key, 2 = the radio key
        std::atomic<float> g_micGain{ 1.0f };
        float g_nearVol = 1.0f, g_radioVol = 0.8f;
        // Radio messages: our own on-screen log (stays g_radioSeconds) or the game's short notification.
        int g_radioStyle = 0;                        // 0 = on-screen radio log, 1 = the game's notification
        float g_radioSeconds = 12.0f;
        struct RadioMsg { std::string who, text; std::chrono::steady_clock::time_point at; };
        std::mutex g_radioMutex;
        std::deque<RadioMsg> g_radioLog;             // the last 30 (F1 history; the newest few on screen)
        std::atomic<bool> g_hearMyself{ false };     // mic test: my own voice played back to me

        // ---- shared with the mic thread ----
        std::atomic<bool> g_live{ false };           // a Wars world, connected
        std::atomic<bool> g_canTalk{ false };        // the game has focus, no console/typing
        std::atomic<int> g_talking{ 0 };             // 0 no, 1 nearby, 2 radio (what the mic thread is sending)
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

        // Two biquads (high-pass 350 Hz, low-pass 2800 Hz) and a soft clip: a handheld radio.
        struct Biquad {
            float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            float Run(float x)
            {
                const float y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };
        Biquad MakeFilter(bool a_high, float a_hz)
        {
            const float w = 2.0f * 3.14159265f * a_hz / kRate, c = std::cos(w), s = std::sin(w), q = 0.7071f;
            const float alpha = s / (2.0f * q), a0 = 1.0f + alpha;
            Biquad f;
            if (a_high) { f.b0 = (1 + c) / 2 / a0; f.b1 = -(1 + c) / a0; f.b2 = (1 + c) / 2 / a0; }
            else { f.b0 = (1 - c) / 2 / a0; f.b1 = (1 - c) / a0; f.b2 = (1 - c) / 2 / a0; }
            f.a1 = -2 * c / a0;
            f.a2 = (1 - alpha) / a0;
            return f;
        }

        struct Speaker {
            OpusDecoder* dec = nullptr;
            IXAudio2SourceVoice* voice = nullptr;
            std::vector<int16_t> ring[kRingBuffers];   // XAudio2 plays straight from these: kept alive here
            int next = 0;
            uint16_t seq = 0;
            bool talking = false;
            bool radio = false;
            Clock::time_point heard{};
            Biquad hp, lp;
            std::mt19937 noise{ 7 };
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
            s.hp = MakeFilter(true, 350.0f);
            s.lp = MakeFilter(false, 2800.0f);
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

        // Radio static: a burst of filtered noise (the squelch when someone keys up or lets go).
        void Squelch(Speaker& a_s, float a_vol, int a_ms)
        {
            const int n = kRate * a_ms / 1000;
            std::vector<float> l(n), r(n);
            std::uniform_real_distribution<float> u(-1.0f, 1.0f);
            for (int i = 0; i < n; ++i) {
                const float env = 1.0f - static_cast<float>(i) / n;
                const float v = a_s.lp.Run(a_s.hp.Run(u(a_s.noise))) * 0.35f * env * a_vol;
                l[i] = r[i] = v;
            }
            Submit(a_s, l.data(), r.data(), n);
        }

        // Where the speaker is relative to me: volume and left/right (constant power).
        void Placement(const WarsVoiceMsg& a_m, float& a_left, float& a_right)
        {
            Me me;
            {
                std::lock_guard l(g_meMutex);
                me = g_me;
            }
            const float dx = a_m.x - me.x, dy = a_m.y - me.y, dz = a_m.z - me.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            float gain = d <= kNearFull ? 1.0f : std::max(0.0f, 1.0f - (d - kNearFull) / (kNearMax - kNearFull));
            gain = gain * gain * g_nearVol;
            // The game's heading: 0 = north (+y), clockwise. Side = sin of the angle from where I look.
            const float bearing = std::atan2(dx, dy);
            const float side = d > 1.0f ? std::sin(bearing - g_heading) : 0.0f;   // -1 left .. 1 right
            const float pan = (side * 0.8f + 1.0f) * 0.25f * 3.14159265f;          // 0..pi/2
            a_left = gain * std::cos(pan) * 1.41421356f;
            a_right = gain * std::sin(pan) * 1.41421356f;
        }

        void Play(uint32_t a_from, const uint8_t* a_data, size_t a_size)
        {
            if (a_size < sizeof(WarsVoiceMsg)) return;
            std::lock_guard pl(g_playMutex);
            WarsVoiceMsg m;
            std::memcpy(&m, a_data, sizeof(m));
            if (m.bytes > kWarsVoiceMaxBytes || sizeof(m) + m.bytes > a_size || !SanePosition(m.x, m.y, m.z)) return;
            const bool radio = (m.flags & kVoiceRadio) != 0;
            if (radio && (!g_radioOn || m.freq != g_freq)) return;   // not what my radio is tuned to
            float gl = 1, gr = 1;
            if (!radio) {
                uint32_t mySpace;
                {
                    std::lock_guard l(g_meMutex);
                    mySpace = g_me.space;
                }
                if (a_from != 0 && m.space != mySpace) return;   // somewhere else: another interior / outside
                if (a_from != 0) Placement(m, gl, gr);
                else gl = gr = g_nearVol;
                if (gl + gr < 0.002f && !(m.flags & kVoiceEnd)) return;   // too far away to hear
            } else {
                gl = gr = g_radioVol;
            }
            Speaker* s = SpeakerOf(a_from);
            if (!s) return;
            const auto now = Clock::now();
            if (!s->talking || now - s->heard > std::chrono::milliseconds(600) || s->radio != radio) {
                // A new transmission: a little silence first to ride out network jitter, the radio's squelch.
                if (a_from) REX::INFO("[Voice] hearing player {} {}", a_from, radio ? std::format("on {:.1f} MHz", m.freq / 10.0f) : std::format("nearby ({:.0f} {:.0f})", gl, gr));
                s->talking = true;
                s->radio = radio;
                s->seq = static_cast<uint16_t>(m.seq - 1);
                std::vector<float> z(kRate * 60 / 1000, 0.0f);
                Submit(*s, z.data(), z.data(), static_cast<int>(z.size()));
                if (radio) Squelch(*s, g_radioVol, 90);
            }
            s->heard = now;
            // Lost packets (up to 3): the decoder fills the gap.
            const uint16_t gap = static_cast<uint16_t>(m.seq - s->seq - 1);
            float pcm[kFrame], left[kFrame], right[kFrame];
            auto out = [&](int a_n) {
                for (int i = 0; i < a_n; ++i) {
                    float v = pcm[i];
                    if (radio) {
                        v = s->lp.Run(s->hp.Run(v)) * 2.2f;
                        v = std::tanh(v * 1.6f) * 0.75f;
                        v += std::uniform_real_distribution<float>(-0.012f, 0.012f)(s->noise);
                    }
                    left[i] = v * gl;
                    right[i] = v * gr;
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
            if (m.flags & kVoiceEnd) {
                if (radio) Squelch(*s, g_radioVol, 140);
                s->talking = false;
            }
        }

        // ---- the mic ----

        void Send(const std::vector<uint8_t>& a_packet)
        {
            auto& net = NetworkClient::GetInstance();
            if (net.IsConnected()) net.SendPacket(MessageType::WarsVoice, a_packet.data(), static_cast<uint32_t>(a_packet.size()));
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
            int tail = 0, mode = 0;   // what the current transmission is (1 nearby, 2 radio)
            bool localOnly = false;   // hear-myself without a key held: played back here, never sent
            uint16_t seq = 0;
            int sent = 0;                  // packets in this transmission (log)
            std::vector<uint8_t> frames;   // [len][bytes]...
            int inPacket = 0;
            auto flush = [&](bool a_end) {
                if (!inPacket && !a_end) return;
                std::vector<uint8_t> pkt(sizeof(WarsVoiceMsg) + frames.size());
                WarsVoiceMsg h{};
                h.flags = static_cast<uint8_t>((mode == 2 ? kVoiceRadio : 0) | (a_end ? kVoiceEnd : 0));
                h.freq = static_cast<uint16_t>(g_freq.load());
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
                        REX::INFO("[Voice] sent {} packet(s) {}", sent, mode == 2 ? std::format("on {:.1f} MHz", g_freq / 10.0f) : std::string("nearby"));
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
                const bool can = live && g_canTalk;
                const bool radioKey = can && g_radioOn && g_binding == 0 && KeyDown(g_keyRadio);
                const bool nearKey = can && g_binding == 0 && KeyDown(g_keyNear);
                const int want = radioKey ? 2 : nearKey ? 1 : 0;
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
                    if (mode) { flush(true); mode = 0; g_talking = 0; }
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
                    // Talking: this key (a switch nearby <-> radio starts a new transmission), then the tail.
                    const int talkNow = want ? want : (tail > 0 ? mode : (g_hearMyself ? 1 : 0));
                    if (want) tail = kTailFrames;
                    else if (tail > 0) --tail;
                    const bool local = !want && tail == 0;
                    if (talkNow != mode || (mode && local != localOnly)) {
                        if (mode) flush(true);
                        mode = talkNow;
                        localOnly = local;
                    }
                    g_talking = want ? want : 0;
                    if (mode && n == kFrame) {
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
                if (mode && !want && tail == 0 && !g_hearMyself) {
                    flush(true);
                    mode = 0;
                }
            }
        }

        void Save()
        {
            if (g_ini.empty()) return;
            auto put = [](const char* k, const std::string& v) { WritePrivateProfileStringA("Voice", k, v.c_str(), g_ini.c_str()); };
            put("Voice", g_voiceOn ? "1" : "0");
            put("Radio", g_radioOn ? "1" : "0");
            put("Chatter", g_chatter ? "1" : "0");
            put("Frequency", std::to_string(g_freq.load()));
            put("KeyNearby", std::format("{:#04x}", g_keyNear.load()));   // virtual-key code
            put("KeyRadio", std::format("{:#04x}", g_keyRadio.load()));
            put("MicGain", std::format("{:.2f}", g_micGain.load()));
            put("NearbyVolume", std::format("{:.2f}", g_nearVol));
            put("RadioVolume", std::format("{:.2f}", g_radioVol));
            put("RadioDisplay", std::to_string(g_radioStyle));
            put("RadioSeconds", std::format("{:.0f}", g_radioSeconds));
        }

        void SendTune()
        {
            auto& net = NetworkClient::GetInstance();
            if (!net.IsConnected() || !Wars::Live()) return;
            WarsRadioTuneMsg t{};
            t.freq = static_cast<uint16_t>(g_freq.load());
            t.on = g_radioOn ? 1 : 0;
            t.chatter = g_chatter ? 1 : 0;
            net.SendPacket(MessageType::WarsRadioTune, &t, sizeof(t));
        }
    }

    void Init(const std::string& a_iniPath)
    {
        g_ini = a_iniPath;
        const char* ini = g_ini.c_str();
        g_voiceOn = GetPrivateProfileIntA("Voice", "Voice", 1, ini) != 0;
        g_radioOn = GetPrivateProfileIntA("Voice", "Radio", 1, ini) != 0;
        g_chatter = GetPrivateProfileIntA("Voice", "Chatter", 1, ini) != 0;
        g_freq = std::clamp(static_cast<int>(GetPrivateProfileIntA("Voice", "Frequency", 1015, ini)), 880, 1080);
        auto key = [&](const char* a_k, int a_def) {
            char buf[64]{};
            GetPrivateProfileStringA("Voice", a_k, "", buf, sizeof(buf), ini);
            const long vk = std::strtol(buf, nullptr, 0);
            return vk > 0 && vk < 256 ? static_cast<int>(vk) : a_def;
        };
        g_keyNear = key("KeyNearby", kDefaultNear);
        g_keyRadio = key("KeyRadio", kDefaultRadio);
        auto num = [&](const char* a_k, float a_def) {
            char buf[32]{};
            GetPrivateProfileStringA("Voice", a_k, "", buf, sizeof(buf), ini);
            return buf[0] ? std::clamp(static_cast<float>(std::atof(buf)), 0.0f, 3.0f) : a_def;
        };
        g_micGain = num("MicGain", 1.0f);
        g_nearVol = num("NearbyVolume", 1.0f);
        g_radioVol = num("RadioVolume", 0.8f);
        g_radioStyle = GetPrivateProfileIntA("Voice", "RadioDisplay", 0, ini) == 1 ? 1 : 0;
        g_radioSeconds = std::clamp(static_cast<float>(GetPrivateProfileIntA("Voice", "RadioSeconds", 12, ini)), 3.0f, 60.0f);
        std::thread(MicThread).detach();
    }

    void OnConnected() { SendTune(); }

    void RadioLine(const std::string& a_who, const std::string& a_text)
    {
        {
            std::lock_guard l(g_radioMutex);
            g_radioLog.push_back({ a_who, a_text, Clock::now() });
            if (g_radioLog.size() > 30) g_radioLog.pop_front();
        }
        if (g_radioStyle == 1) {
            const std::string line = std::format("[RADIO] {}: {}", a_who, a_text);
            RE::SendHUDMessage::ShowHUDMessage(line.c_str(), "UIPipboyRadioOn", false, false);
        } else {
            RE::UIUtils::PlayMenuSound("UIPipboyRadioOn");
        }
    }

    void Bridge(const std::string& a_what)
    {
        if (a_what == "hear") g_hearMyself = !g_hearMyself;
        else if (a_what.rfind("tune ", 0) == 0) { g_freq = std::clamp(static_cast<int>(std::lround(std::atof(a_what.c_str() + 5) * 10.0)), 880, 1080); SendTune(); }
        else if (a_what == "radio") { g_radioOn = !g_radioOn; SendTune(); }
        std::lock_guard pl(g_playMutex);
        REX::INFO("[Voice] voice {} radio {} {:.1f} MHz chatter {} | keys {} / {} | can talk {} talking {} | mic {} level {:.2f} | hear myself {} | {} speaker(s), audio {}",
            g_voiceOn.load(), g_radioOn.load(), g_freq / 10.0f, g_chatter.load(), KeyName(g_keyNear), KeyName(g_keyRadio), g_canTalk.load(),
            g_talking.load(), g_micError ? "ERROR" : "ok", g_micLevel.load(), g_hearMyself.load(), g_speakers.size(), g_xa ? "ok" : "not started");
    }

    bool RadioOn() { return g_radioOn; }
    bool ChatterOn() { return g_radioOn && g_chatter; }

    void OnPacket(const std::vector<uint8_t>& a_payload)
    {
        if (!Wars::Live() || !g_voiceOn || a_payload.size() < sizeof(WarsVoiceMsg)) return;
        uint32_t from;
        std::memcpy(&from, a_payload.data(), sizeof(from));
        if (from == 0) return;   // 0 is me (hear-myself)
        Play(from, a_payload.data(), a_payload.size());
    }

    void Frame()
    {
        static bool s_wasLive = false;
        auto& net = NetworkClient::GetInstance();
        const bool live = Wars::Live() && net.IsConnected();
        g_live = live || (Wars::Live() && g_hearMyself);
        if (live && !s_wasLive) SendTune();
        s_wasLive = live;
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

    void TuneTo(int a_tenthsMHz)
    {
        g_freq = std::clamp(a_tenthsMHz, 880, 1080);
        Save();
        SendTune();
    }

    void RenderMenu()
    {
        ImGuiMCP::SeparatorText("Wasteland Wars: radio + voice");
        bool b = g_radioOn;
        if (ImGuiMCP::Checkbox("Radio on (A-Life chatter and radio voice)", &b)) { g_radioOn = b; Save(); SendTune(); }
        ImGuiMCP::BeginDisabled(!g_radioOn);
        b = g_chatter;
        if (ImGuiMCP::Checkbox("A-Life chatter on the radio (squads talking)", &b)) { g_chatter = b; Save(); SendTune(); }
        float mhz = g_freq / 10.0f;
        if (ImGuiMCP::SliderFloat("Frequency (MHz)", &mhz, 88.0f, 108.0f, "%.1f")) g_freq = static_cast<int>(std::lround(mhz * 10.0f));
        // Sent when the slider is let go (not on every pixel of a drag).
        static int s_sentFreq = -1;
        if (s_sentFreq != g_freq && !ImGuiMCP::IsItemActive()) { s_sentFreq = g_freq; Save(); SendTune(); }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::SmallButton("-0.1")) { g_freq = std::max(880, g_freq - 1); }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::SmallButton("+0.1")) { g_freq = std::min(1080, g_freq + 1); }
        ImGuiMCP::TextDisabled("Everyone tuned to %.1f hears you on the radio, anywhere in the Commonwealth.", g_freq / 10.0f);
        ImGuiMCP::EndDisabled();

        static const char* kStyles[] = { "On-screen radio log (stays as long as set below)", "Game notification (short)" };
        if (ImGuiMCP::Combo("Radio messages", &g_radioStyle, kStyles, 2)) Save();
        ImGuiMCP::SliderFloat("Show each radio message for (seconds)", &g_radioSeconds, 3.0f, 60.0f, "%.0f");
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        if (ImGuiMCP::CollapsingHeader("Radio log (last 30)")) {
            std::lock_guard l(g_radioMutex);
            for (auto it = g_radioLog.rbegin(); it != g_radioLog.rend(); ++it)
                ImGuiMCP::TextWrapped("%s: %s", it->who.c_str(), it->text.c_str());
            if (g_radioLog.empty()) ImGuiMCP::TextDisabled("Nothing heard yet.");
        }

        b = g_voiceOn;
        if (ImGuiMCP::Checkbox("Voice chat", &b)) { g_voiceOn = b; Save(); }
        ImGuiMCP::BeginDisabled(!g_voiceOn);
        // Rebinding: click Change, then press any key or mouse button (Esc cancels).
        if (g_binding) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                g_binding = 0;
            } else if (const int vk = NewlyPressedKey()) {
                (g_binding == 1 ? g_keyNear : g_keyRadio) = vk;
                g_binding = 0;
                Save();
            }
        }
        auto keyRow = [&](const char* a_label, int a_which, int a_vk) {
            ImGuiMCP::PushID(a_which);
            ImGuiMCP::Text("%s: %s", a_label, g_binding == a_which ? "press a key or mouse button... (Esc = cancel)" : KeyName(a_vk).c_str());
            ImGuiMCP::SameLine();
            if (g_binding != a_which && ImGuiMCP::SmallButton("Change")) { g_binding = a_which; NewlyPressedKey(); }
            ImGuiMCP::PopID();
        };
        keyRow("Talk nearby (hold)", 1, g_keyNear);
        keyRow("Talk on the radio (hold)", 2, g_keyRadio);
        if (g_keyNear == 'V' || g_keyRadio == 'V')
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "V is the game's workshop key in settlements: pick another one.");
        float f = g_micGain;
        if (ImGuiMCP::SliderFloat("Mic volume", &f, 0.2f, 3.0f, "%.2f")) g_micGain = f;
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        ImGuiMCP::SliderFloat("Nearby voices", &g_nearVol, 0.0f, 2.0f, "%.2f");
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        ImGuiMCP::SliderFloat("Radio voices", &g_radioVol, 0.0f, 2.0f, "%.2f");
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) Save();
        b = g_hearMyself;
        if (ImGuiMCP::Checkbox("Mic test: hear myself (hold the radio key to hear the radio sound)", &b)) g_hearMyself = b;
        ImGuiMCP::ProgressBar(std::min(1.0f, g_micLevel.load() * 1.5f), ImGuiMCP::ImVec2(-1.0f, 0.0f), g_micError ? "no microphone" : "mic");
        ImGuiMCP::TextDisabled("Nearby voice carries about 45 m and only where you are (same building / outdoors).");
        ImGuiMCP::EndDisabled();
    }

    void RenderHud()
    {
        if (!Wars::Live()) return;
        // The radio log: the newest lines, each for g_radioSeconds, fading out over its last 1.5 s.
        if (g_radioStyle == 0) {
            auto* io = ImGuiMCP::GetIO();
            auto* dl = ImGuiMCP::GetForegroundDrawList();
            std::vector<std::pair<std::string, float>> show;
            {
                std::lock_guard l(g_radioMutex);
                const auto now = Clock::now();
                for (const auto& m : g_radioLog) {
                    const float age = std::chrono::duration<float>(now - m.at).count();
                    if (age < g_radioSeconds) show.push_back({ std::format("[RADIO] {}: {}", m.who, m.text), std::min(1.0f, (g_radioSeconds - age) / 1.5f) });
                }
            }
            if (show.size() > 4) show.erase(show.begin(), show.end() - 4);
            if (io && dl && !show.empty()) {
                namespace DL = ImGuiMCP::ImDrawListManager;
                const float size = std::max(18.0f, io->DisplaySize.y * 0.02f);
                const float wrap = io->DisplaySize.x * 0.38f;
                float y = io->DisplaySize.y * 0.02f;   // the very top: the game's notifications pop up from 7-8% down (they overlapped at 9%)
                const float x = io->DisplaySize.x * 0.035f;
                for (const auto& [t, alpha] : show) {
                    const auto a = static_cast<ImGuiMCP::ImU32>(alpha * 255.0f) << 24;
                    const int rows = 1 + static_cast<int>(t.size() * size * 0.5f / wrap);
                    DL::AddRectFilled(dl, ImGuiMCP::ImVec2(x - 8.0f, y - 4.0f), ImGuiMCP::ImVec2(x + wrap + 8.0f, y + rows * size * 1.15f + 4.0f),
                        (static_cast<ImGuiMCP::ImU32>(alpha * 120.0f) << 24), 4.0f, 0);
                    DL::AddText(dl, nullptr, size, ImGuiMCP::ImVec2(x + 1.0f, y + 1.0f), a | 0x000000u, t.c_str(), nullptr, wrap);
                    DL::AddText(dl, nullptr, size, ImGuiMCP::ImVec2(x, y), a | 0x80FF1Au, t.c_str(), nullptr, wrap);
                    y += rows * size * 1.15f + 12.0f;
                }
            }
        }
        const int talking = g_talking;
        std::vector<std::string> lines;
        if (talking == 2) lines.push_back(std::format("[TALKING ON {:.1f} MHz]", g_freq / 10.0f));
        else if (talking == 1) lines.push_back("[TALKING NEARBY]");
        const auto now = Clock::now();
        std::unique_lock pl(g_playMutex);
        for (auto& [id, s] : g_speakers) {
            if (!id || !s.talking || now - s.heard > std::chrono::milliseconds(500)) continue;
            std::string who = Appearance::NameOf(id);
            if (who.empty()) who = "Player " + std::to_string(id);
            lines.push_back(s.radio ? std::format("(( {} on {:.1f} ))", who, g_freq / 10.0f) : std::format("< {} >", who));
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
