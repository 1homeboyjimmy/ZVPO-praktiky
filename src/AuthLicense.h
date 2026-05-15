#pragma once
#include <string>
#include <mutex>

struct AuthState {
    std::string accessToken;
    std::string refreshToken;
    std::wstring username;
    long long accessExpiresAt;
    long long refreshExpiresAt;
};

struct LicenseState {
    bool hasLicense;
    std::wstring expiryDate;
    long long ticketIssuedAt;
    long long ticketLifetimeSec;
    std::string rawTicket;
};

class AuthLicenseManager {
public:
    static AuthLicenseManager& Instance();

    void Start();
    void Stop();

    int Login(const std::wstring& username, const std::wstring& password);
    void Logout();
    int Activate(const std::wstring& key);

    bool IsAuthenticated();
    std::wstring GetUsername();
    bool HasLicense();
    std::wstring GetLicenseExpiry();

private:
    AuthLicenseManager() = default;
    AuthLicenseManager(const AuthLicenseManager&) = delete;

    int RefreshTokensLocked();
    int RefreshLicenseLocked();
    void ClearAuth();
    void ClearLicense();

    static DWORD WINAPI RefreshThreadProc(LPVOID p);
    void RefreshLoop();

    std::mutex m_mtx;
    AuthState m_auth{};
    LicenseState m_license{};
    HANDLE m_thread = nullptr;
    HANDLE m_stopEvent = nullptr;
    bool m_running = false;
};
