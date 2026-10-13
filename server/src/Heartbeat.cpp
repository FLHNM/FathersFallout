#include "Heartbeat.h"

#include <string>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

namespace {
    std::string JsonEscape(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n' || c == '\r' || c == '\t') { /* omitir */ }
            else out += c;
        }
        return out;
    }
}

namespace Heartbeat {

void Send(const std::string& url, const std::string& name,
          uint16_t port, uint32_t players, uint32_t maxPlayers)
{
    if (url.empty()) return;

    // Cuerpo JSON (comun a las dos plataformas).
    const std::string body =
        "{\"name\":\"" + JsonEscape(name) + "\",\"port\":" + std::to_string(port) +
        ",\"players\":" + std::to_string(players) +
        ",\"maxPlayers\":" + std::to_string(maxPlayers) + "}";

    // URL sin barras finales.
    std::string clean = url;
    while (!clean.empty() && clean.back() == '/') clean.pop_back();

#if defined(_WIN32)
    // ---------- Windows: WinHTTP ----------
    // Parsear:  [http(s)://]host[:puerto]
    std::string u = clean;
    bool https = false;
    if (u.rfind("https://", 0) == 0) { https = true; u = u.substr(8); }
    else if (u.rfind("http://", 0) == 0) { u = u.substr(7); }

    std::string host = u;
    INTERNET_PORT hostPort = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    const auto colon = u.find(':');
    if (colon != std::string::npos) {
        host = u.substr(0, colon);
        hostPort = static_cast<INTERNET_PORT>(atoi(u.substr(colon + 1).c_str()));
    }

    const std::wstring whost(host.begin(), host.end());

    HINTERNET hSession = WinHttpOpen(L"F4MPServer/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), hostPort, 0);
    if (hConnect) {
        const DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", L"/heartbeat",
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hReq) {
            const wchar_t* headers = L"Content-Type: application/json";
            if (WinHttpSendRequest(hReq, headers, static_cast<DWORD>(-1),
                    (LPVOID)body.data(), static_cast<DWORD>(body.size()),
                    static_cast<DWORD>(body.size()), 0)) {
                WinHttpReceiveResponse(hReq, NULL);
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);

#else
    // ---------- Linux/otros: libcurl ----------
    // Si no hay esquema, asumir http://
    if (clean.rfind("http://", 0) != 0 && clean.rfind("https://", 0) != 0) {
        clean = "http://" + clean;
    }
    const std::string endpoint = clean + "/heartbeat";

    CURL* curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "F4MPServer/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // seguro en multihilo

    curl_easy_perform(curl);  // ignoramos el resultado: es best-effort

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
#endif
}

}
