// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

#include "pch.h"
#include "sync/CrashLog.h"

#include <Windows.h>
#include <ShlObj.h>
#include <cstdio>
#include <cwchar>
#include <atomic>

namespace F4MP::CrashLog
{
    namespace
    {
        wchar_t g_path[MAX_PATH]{};
        LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
        std::atomic<bool> g_inside{ false };
        DWORD g_mainThread = 0;
        void* volatile g_lastReported = nullptr;   // where the last exception OnException wrote out happened

        // No allocation, no logger locks (the crash may have happened inside either): plain WriteFile.
        void Write(HANDLE f, const char* text)
        {
            DWORD n = 0;
            WriteFile(f, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
        }

        // "Fallout4.exe+0x1A2B3C" for an address, or the raw address outside every module.
        void Describe(uint64_t addr, char* out, size_t size)
        {
            HMODULE mod = nullptr;
            char path[MAX_PATH]{};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(addr), &mod) && mod && GetModuleFileNameA(mod, path, MAX_PATH)) {
                const char* name = std::strrchr(path, '\\');
                name = name ? name + 1 : path;
                std::snprintf(out, size, "%s+0x%llX", name, static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(mod)));
            } else {
                std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(addr));
            }
        }

        // Writes the exception, its registers and the crashed thread's stack to the end of the log.
        void Report(EXCEPTION_POINTERS* ep, const char* title)
        {
            if (!ep || !ep->ExceptionRecord || !ep->ContextRecord || !g_path[0]) return;
            HANDLE f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) return;
            char line[512], where[300];
            SYSTEMTIME t;
            GetLocalTime(&t);
            const auto* er = ep->ExceptionRecord;
            Describe(reinterpret_cast<uint64_t>(er->ExceptionAddress), where, sizeof(where));
            std::snprintf(line, sizeof(line),
                "\r\n[%02d:%02d:%02d.%03d] [CRASH] ===== %s =====\r\n"
                "[CRASH] exception 0x%08lX at %s (thread %lu%s)\r\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, title, er->ExceptionCode, where, GetCurrentThreadId(),
                GetCurrentThreadId() == g_mainThread ? ", main thread" : "");
            Write(f, line);
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
                std::snprintf(line, sizeof(line), "[CRASH] %s address 0x%llX\r\n",
                    er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                    static_cast<unsigned long long>(er->ExceptionInformation[1]));
                Write(f, line);
            }
            const auto& c = *ep->ContextRecord;
            std::snprintf(line, sizeof(line), "[CRASH] RAX %llX RBX %llX RCX %llX RDX %llX RSI %llX RDI %llX RSP %llX\r\n",
                c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rsp);
            Write(f, line);

            // Unwind the crashed thread's own stack from its context.
            CONTEXT ctx = c;
            for (int i = 0; i < 32 && ctx.Rip; ++i) {
                Describe(ctx.Rip, where, sizeof(where));
                std::snprintf(line, sizeof(line), "[CRASH]   #%02d %s\r\n", i, where);
                Write(f, line);
                DWORD64 base = 0;
                auto* fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fn) {   // a leaf function: the return address is on top of the stack
                    if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
            }
            Write(f, "[CRASH] ===== please send this log (launcher: SHARE LOG) =====\r\n");
            FlushFileBuffers(f);
            CloseHandle(f);
        }

        LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep)
        {
            // Another handler that chained to us can call back in here: don't loop.
            if (g_inside.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
            if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionAddress == g_lastReported && g_path[0]) {
                // Already written in full by OnException a moment ago: just say it was fatal.
                HANDLE f = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    Write(f, "[CRASH] ===== GAME CRASHED (the exception above) =====\r\n");
                    FlushFileBuffers(f);
                    CloseHandle(f);
                }
            } else {
                Report(ep, "GAME CRASHED");
            }
            return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
        }

        // Sees every exception before the game's own handlers do. The two player crash reports of
        // Update 22/23 had no crash lines at all: something replaced or bypassed the unhandled-exception
        // filter. Only the kinds that crash a game are written, and only the first few: the game may
        // handle one itself and carry on, so these are marked that way.
        std::atomic<int> g_firstChance{ 0 };

        LONG CALLBACK OnException(EXCEPTION_POINTERS* ep)
        {
            if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
            switch (ep->ExceptionRecord->ExceptionCode) {
            case EXCEPTION_ACCESS_VIOLATION:
            case EXCEPTION_ILLEGAL_INSTRUCTION:
            case EXCEPTION_PRIV_INSTRUCTION:
            case EXCEPTION_STACK_OVERFLOW:
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_IN_PAGE_ERROR:
            case 0xC0000374:   // heap corruption
            case 0xC0000409:   // stack buffer overrun (__fastfail)
                break;
            default:
                return EXCEPTION_CONTINUE_SEARCH;
            }
            if (g_firstChance.fetch_add(1) < 3) {
                Report(ep, "EXCEPTION (the game may still handle it)");
                g_lastReported = ep->ExceptionRecord->ExceptionAddress;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    std::wstring LogPath()
    {
        PWSTR docs = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
            out = std::wstring(docs) + L"\\My Games\\Fallout4\\F4SE\\FathersFallout.log";
            CoTaskMemFree(docs);
        }
        return out;
    }

    void KeepPreviousLog()
    {
        const auto log = LogPath();
        if (log.empty()) return;
        const auto previous = log.substr(0, log.size() - 4) + L"-previous.log";
        MoveFileExW(log.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void Install()
    {
        g_mainThread = GetCurrentThreadId();
        const auto log = LogPath();
        if (!log.empty() && log.size() < MAX_PATH) std::wcscpy(g_path, log.c_str());
        g_previous = SetUnhandledExceptionFilter(OnCrash);
        AddVectoredExceptionHandler(1, OnException);
        REX::INFO("[FathersFallout] crash logger installed{}", g_previous ? " (chained to an earlier crash handler)" : "");
    }

    void Reassert()
    {
        const auto current = SetUnhandledExceptionFilter(OnCrash);
        if (current == OnCrash) return;
        g_previous = current;   // whoever took over still runs after us
        REX::INFO("[FathersFallout] crash logger put back in front (another crash handler had replaced it)");
    }
}
