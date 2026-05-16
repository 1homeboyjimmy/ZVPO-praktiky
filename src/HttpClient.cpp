#include "HttpClient.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <sstream>
#include <cstdint>

#pragma comment(lib, "winhttp.lib")

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

static HttpResponse PerformRequest(const std::wstring& host, int port, const std::wstring& path, const std::wstring& method, const std::string& body, const std::string& bearer) {
    HttpResponse result{ 0, {} };

    HINTERNET hSession = WinHttpOpen(L"TrayAppService/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    bool useTls = (port == 443 || port == 8443);
    DWORD flags = useTls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (useTls) {
        DWORD secFlags =
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    if (!bearer.empty()) {
        headers += L"Authorization: Bearer " + Utf8ToWide(bearer) + L"\r\n";
    }

    LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
    DWORD bodyLen = (DWORD)body.size();

    BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(), bodyPtr, bodyLen, bodyLen, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    if (ok) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL);
        result.statusCode = (int)status;

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
            result.body.append(buf.data(), read);
            if (read == 0) break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

HttpResponse HttpPost(const std::wstring& host, int port, const std::wstring& path, const std::string& jsonBody, const std::string& bearerToken) {
    return PerformRequest(host, port, path, L"POST", jsonBody, bearerToken);
}

HttpResponse HttpGet(const std::wstring& host, int port, const std::wstring& path, const std::string& bearerToken) {
    return PerformRequest(host, port, path, L"GET", "", bearerToken);
}

static size_t FindKey(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t after = pos + pattern.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t' || json[after] == '\n' || json[after] == '\r')) after++;
        if (after < json.size() && json[after] == ':') return after + 1;
        pos = after;
    }
    return std::string::npos;
}

std::string JsonExtractString(const std::string& json, const std::string& key) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return "";
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char nxt = json[pos + 1];
            if (nxt == 'n') out.push_back('\n');
            else if (nxt == 't') out.push_back('\t');
            else if (nxt == 'r') out.push_back('\r');
            else out.push_back(nxt);
            pos += 2;
        } else {
            out.push_back(json[pos]);
            pos++;
        }
    }
    return out;
}

long long JsonExtractNumber(const std::string& json, const std::string& key) {
    size_t pos = FindKey(json, key);
    if (pos == std::string::npos) return 0;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    long long sign = 1;
    if (pos < json.size() && json[pos] == '-') { sign = -1; pos++; }
    long long val = 0;
    bool any = false;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        pos++; any = true;
    }
    return any ? val * sign : 0;
}

static int Base64UrlChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

std::string JwtDecodePayload(const std::string& jwt) {
    size_t a = jwt.find('.');
    if (a == std::string::npos) return "";
    size_t b = jwt.find('.', a + 1);
    if (b == std::string::npos) return "";
    std::string payload = jwt.substr(a + 1, b - a - 1);

    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : payload) {
        int v = Base64UrlChar(c);
        if (v < 0) continue;
        buffer = (buffer << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buffer >> bits) & 0xFF));
        }
    }
    return out;
}
