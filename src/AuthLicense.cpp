#include "AuthLicense.h"
#include "HttpClient.h"
#include <windows.h>
#include <iphlpapi.h>
#include <sstream>
#include <iomanip>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")

static const wchar_t* kServerHost = L"localhost";
static const int kServerPort = 8443;
static const long long kProductId = 1;

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

static long long NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string GetMacAddress() {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, NULL, &bufLen);
    if (bufLen == 0) return "00:00:00:00:00:00";

    std::vector<char> buf(bufLen);
    IP_ADAPTER_ADDRESSES* adapters = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, adapters, &bufLen) != NO_ERROR) {
        return "00:00:00:00:00:00";
    }

    for (IP_ADAPTER_ADDRESSES* a = adapters; a; a = a->Next) {
        if (a->PhysicalAddressLength == 6 && a->IfType == IF_TYPE_ETHERNET_CSMACD) {
            std::ostringstream os;
            for (int i = 0; i < 6; i++) {
                if (i) os << ":";
                os << std::hex << std::setw(2) << std::setfill('0') << (int)a->PhysicalAddress[i];
            }
            return os.str();
        }
    }
    for (IP_ADAPTER_ADDRESSES* a = adapters; a; a = a->Next) {
        if (a->PhysicalAddressLength == 6) {
            std::ostringstream os;
            for (int i = 0; i < 6; i++) {
                if (i) os << ":";
                os << std::hex << std::setw(2) << std::setfill('0') << (int)a->PhysicalAddress[i];
            }
            return os.str();
        }
    }
    return "00:00:00:00:00:00";
}

static std::string GetDeviceName() {
    wchar_t name[256] = {};
    DWORD sz = 256;
    if (GetComputerNameW(name, &sz)) return WideToUtf8(name);
    return "Unknown";
}

AuthLicenseManager& AuthLicenseManager::Instance() {
    static AuthLicenseManager inst;
    return inst;
}

void AuthLicenseManager::Start() {
    if (m_running) return;
    m_running = true;
    m_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_thread = CreateThread(NULL, 0, RefreshThreadProc, this, 0, NULL);
}

void AuthLicenseManager::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread) {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
    std::lock_guard<std::mutex> g(m_mtx);
    ClearAuth();
    ClearLicense();
}

void AuthLicenseManager::ClearAuth() {
    SecureZeroMemory(&m_auth.accessToken[0], m_auth.accessToken.size());
    SecureZeroMemory(&m_auth.refreshToken[0], m_auth.refreshToken.size());
    m_auth = AuthState{};
}

void AuthLicenseManager::ClearLicense() {
    SecureZeroMemory(&m_license.rawTicket[0], m_license.rawTicket.size());
    m_license = LicenseState{};
}

int AuthLicenseManager::Login(const std::wstring& username, const std::wstring& password) {
    std::string body = "{\"username\":\"" + JsonEscape(WideToUtf8(username)) +
                       "\",\"password\":\"" + JsonEscape(WideToUtf8(password)) + "\"}";
    HttpResponse r = HttpPost(kServerHost, kServerPort, L"/auth/login", body, "");
    if (r.statusCode != 200) return r.statusCode;

    std::string access = JsonExtractString(r.body, "accessToken");
    std::string refresh = JsonExtractString(r.body, "refreshToken");
    if (access.empty() || refresh.empty()) return 500;

    std::lock_guard<std::mutex> g(m_mtx);
    m_auth.accessToken = access;
    m_auth.refreshToken = refresh;
    m_auth.username = username;

    std::string accessPayload = JwtDecodePayload(access);
    std::string refreshPayload = JwtDecodePayload(refresh);
    m_auth.accessExpiresAt = JsonExtractNumber(accessPayload, "exp");
    m_auth.refreshExpiresAt = JsonExtractNumber(refreshPayload, "exp");

    std::string sub = JsonExtractString(accessPayload, "sub");
    if (!sub.empty()) m_auth.username = Utf8ToWide(sub);

    RefreshLicenseLocked();
    return 200;
}

void AuthLicenseManager::Logout() {
    std::lock_guard<std::mutex> g(m_mtx);
    ClearAuth();
    ClearLicense();
}

int AuthLicenseManager::Activate(const std::wstring& key) {
    std::string token;
    {
        std::lock_guard<std::mutex> g(m_mtx);
        if (m_auth.accessToken.empty()) return 401;
        token = m_auth.accessToken;
    }

    std::string body = "{\"activationKey\":\"" + JsonEscape(WideToUtf8(key)) +
                       "\",\"deviceMac\":\"" + GetMacAddress() +
                       "\",\"deviceName\":\"" + JsonEscape(GetDeviceName()) + "\"}";
    HttpResponse r = HttpPost(kServerHost, kServerPort, L"/api/licenses/activate", body, token);
    if (r.statusCode != 200) return r.statusCode;

    std::lock_guard<std::mutex> g(m_mtx);
    m_license.rawTicket = r.body;
    m_license.hasLicense = true;
    m_license.ticketIssuedAt = NowSeconds();
    m_license.ticketLifetimeSec = JsonExtractNumber(r.body, "lifetime");
    if (m_license.ticketLifetimeSec <= 0) m_license.ticketLifetimeSec = 3600;
    std::string exp = JsonExtractString(r.body, "expirationDate");
    if (exp.empty()) {
        RefreshLicenseLocked();
    } else {
        m_license.expiryDate = Utf8ToWide(exp);
    }
    return 200;
}

int AuthLicenseManager::RefreshTokensLocked() {
    if (m_auth.refreshToken.empty()) return 401;
    std::string body = "{\"refreshToken\":\"" + JsonEscape(m_auth.refreshToken) + "\"}";
    HttpResponse r = HttpPost(kServerHost, kServerPort, L"/auth/refresh", body, "");
    if (r.statusCode != 200) {
        ClearAuth();
        ClearLicense();
        return r.statusCode;
    }
    std::string access = JsonExtractString(r.body, "accessToken");
    std::string refresh = JsonExtractString(r.body, "refreshToken");
    if (access.empty() || refresh.empty()) return 500;

    m_auth.accessToken = access;
    m_auth.refreshToken = refresh;
    std::string accessPayload = JwtDecodePayload(access);
    std::string refreshPayload = JwtDecodePayload(refresh);
    m_auth.accessExpiresAt = JsonExtractNumber(accessPayload, "exp");
    m_auth.refreshExpiresAt = JsonExtractNumber(refreshPayload, "exp");
    return 200;
}

int AuthLicenseManager::RefreshLicenseLocked() {
    if (m_auth.accessToken.empty()) return 401;
    std::string body = "{\"deviceMac\":\"" + GetMacAddress() +
                       "\",\"productId\":" + std::to_string(kProductId) + "}";
    HttpResponse r = HttpPost(kServerHost, kServerPort, L"/api/licenses/check", body, m_auth.accessToken);
    if (r.statusCode != 200) {
        ClearLicense();
        return r.statusCode;
    }
    m_license.rawTicket = r.body;
    m_license.hasLicense = true;
    m_license.ticketIssuedAt = NowSeconds();
    m_license.ticketLifetimeSec = JsonExtractNumber(r.body, "lifetime");
    if (m_license.ticketLifetimeSec <= 0) m_license.ticketLifetimeSec = 3600;
    m_license.expiryDate = Utf8ToWide(JsonExtractString(r.body, "expirationDate"));
    return 200;
}

bool AuthLicenseManager::IsAuthenticated() {
    std::lock_guard<std::mutex> g(m_mtx);
    return !m_auth.accessToken.empty();
}

std::wstring AuthLicenseManager::GetUsername() {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_auth.username;
}

bool AuthLicenseManager::HasLicense() {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_license.hasLicense;
}

std::wstring AuthLicenseManager::GetLicenseExpiry() {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_license.expiryDate;
}

DWORD WINAPI AuthLicenseManager::RefreshThreadProc(LPVOID p) {
    ((AuthLicenseManager*)p)->RefreshLoop();
    return 0;
}

void AuthLicenseManager::RefreshLoop() {
    while (m_running) {
        DWORD wait = WaitForSingleObject(m_stopEvent, 30000);
        if (wait == WAIT_OBJECT_0) break;

        std::lock_guard<std::mutex> g(m_mtx);
        if (m_auth.accessToken.empty()) continue;

        long long now = NowSeconds();

        if (m_auth.accessExpiresAt > 0 && now >= m_auth.accessExpiresAt - 60) {
            RefreshTokensLocked();
        }

        if (m_license.hasLicense && m_license.ticketLifetimeSec > 0) {
            long long expireAt = m_license.ticketIssuedAt + m_license.ticketLifetimeSec;
            if (now >= expireAt - 60) {
                RefreshLicenseLocked();
            }
        }
    }
}
