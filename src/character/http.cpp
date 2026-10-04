#include "castlemist/character/http.h"

#include <cstdio>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace castlemist::character {
namespace {

constexpr int kTimeoutMs = 15000;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct HandleCloser {
    void operator()(void* h) const { if (h) WinHttpCloseHandle(h); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

HttpResponse failure(const char* step) {
    DWORD code = GetLastError();
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed (WinHTTP error %lu)", step, static_cast<unsigned long>(code));
    return {0, "", buf};
}

} // namespace

HttpResponse WinHttpClient::get(const std::string& url, const Headers& headers) {
    std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return failure("parsing the URL");
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    Handle session(WinHttpOpen(L"castlemist", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return failure("opening a WinHTTP session");
    WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    Handle connect(WinHttpConnect(session.get(), host.c_str(), uc.nPort, 0));
    if (!connect) return failure("connecting");
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request(WinHttpOpenRequest(connect.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return failure("opening the request");

    std::wstring hdrs;
    for (const auto& [k, v] : headers) hdrs += widen(k) + L": " + widen(v) + L"\r\n";
    if (!WinHttpSendRequest(request.get(), hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                            hdrs.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return failure("sending the request");
    if (!WinHttpReceiveResponse(request.get(), nullptr)) return failure("receiving the response");

    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    HttpResponse r;
    r.status = static_cast<int>(status);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &avail)) return failure("reading the response");
        if (avail == 0) break;
        size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), r.body.data() + at, avail, &got)) return failure("reading the response");
        r.body.resize(at + got);
    }
    return r;
}

std::string url_encode(std::string_view utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '.' || c == '_' || c == '~';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

} // namespace castlemist::character
