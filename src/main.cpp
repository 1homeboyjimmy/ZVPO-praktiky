#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include "resources/resource.h"
#include "ServiceRpc_h.h"
#include "AuthLicenseRpc_h.h"
#include "AuthLicense.h"

static bool g_demoMode = false;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

void* __RPC_USER MIDL_user_allocate(size_t size) { return malloc(size); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

static handle_t OpenServiceBinding() {
    RPC_WSTR sb = NULL;
    handle_t h = NULL;
    if (RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)L"TrayAppRpcPort", NULL, &sb) == RPC_S_OK) {
        RpcBindingFromStringBindingW(sb, &h);
        RpcStringFreeW(&sb);
    }
    return h;
}

static long SafeCallGetCurrentUser(handle_t h, wchar_t** name) {
    RpcTryExcept { return GetCurrentUser(h, name); }
    RpcExcept(1) { return -1; } RpcEndExcept
}
static long SafeCallLogin(handle_t h, const wchar_t* u, const wchar_t* p, long* hs) {
    RpcTryExcept { return Login(h, u, p, hs); }
    RpcExcept(1) { return -1; } RpcEndExcept
}
static long SafeCallLogout(handle_t h) {
    RpcTryExcept { return Logout(h); }
    RpcExcept(1) { return -1; } RpcEndExcept
}
static long SafeCallGetLicenseInfo(handle_t h, long* hasL, wchar_t** exp) {
    RpcTryExcept { return GetLicenseInfo(h, hasL, exp); }
    RpcExcept(1) { return -1; } RpcEndExcept
}
static long SafeCallActivate(handle_t h, const wchar_t* k, long* hs) {
    RpcTryExcept { return Activate(h, k, hs); }
    RpcExcept(1) { return -1; } RpcEndExcept
}
static long SafeCallStopService(handle_t h) {
    RpcTryExcept { StopService(h); return 0; }
    RpcExcept(1) { return -1; } RpcEndExcept
}

static std::wstring RpcGetCurrentUser() {
    if (g_demoMode) return AuthLicenseManager::Instance().GetUsername();
    std::wstring out;
    handle_t h = OpenServiceBinding();
    if (!h) return out;
    wchar_t* name = NULL;
    if (SafeCallGetCurrentUser(h, &name) == S_OK && name) {
        out = name;
        MIDL_user_free(name);
    }
    RpcBindingFree(&h);
    return out;
}

static int RpcLogin(const std::wstring& u, const std::wstring& p) {
    if (g_demoMode) return AuthLicenseManager::Instance().Login(u, p);
    int status = -1;
    handle_t h = OpenServiceBinding();
    if (!h) return status;
    long hs = 0;
    SafeCallLogin(h, u.c_str(), p.c_str(), &hs);
    status = (int)hs;
    RpcBindingFree(&h);
    return status;
}

static void RpcLogout() {
    if (g_demoMode) { AuthLicenseManager::Instance().Logout(); return; }
    handle_t h = OpenServiceBinding();
    if (!h) return;
    SafeCallLogout(h);
    RpcBindingFree(&h);
}

static bool RpcGetLicenseInfo(std::wstring& expiry) {
    if (g_demoMode) {
        auto& m = AuthLicenseManager::Instance();
        expiry = m.GetLicenseExpiry();
        return m.HasLicense();
    }
    bool has = false;
    handle_t h = OpenServiceBinding();
    if (!h) return false;
    long hasL = 0;
    wchar_t* exp = NULL;
    if (SafeCallGetLicenseInfo(h, &hasL, &exp) == S_OK) {
        has = (hasL != 0);
        if (exp) { expiry = exp; MIDL_user_free(exp); }
    }
    RpcBindingFree(&h);
    return has;
}

static int RpcActivate(const std::wstring& key) {
    if (g_demoMode) return AuthLicenseManager::Instance().Activate(key);
    int status = -1;
    handle_t h = OpenServiceBinding();
    if (!h) return status;
    long hs = 0;
    SafeCallActivate(h, key.c_str(), &hs);
    status = (int)hs;
    RpcBindingFree(&h);
    return status;
}

void StopTrayAppService() {
    handle_t h = OpenServiceBinding();
    if (!h) return;
    SafeCallStopService(h);
    RpcBindingFree(&h);
}

bool IsServiceRunning() {
    bool isRunning = false;
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCManager) {
        SC_HANDLE hService = OpenServiceW(hSCManager, L"TrayAppService", SERVICE_QUERY_STATUS);
        if (hService) {
            SERVICE_STATUS status;
            if (QueryServiceStatus(hService, &status)) {
                isRunning = (status.dwCurrentState == SERVICE_RUNNING);
            }
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCManager);
    }
    return isRunning;
}

void StartTrayAppService() {
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCManager) {
        SC_HANDLE hService = OpenServiceW(hSCManager, L"TrayAppService", SERVICE_START | SERVICE_QUERY_STATUS);
        if (hService) {
            StartServiceW(hService, 0, NULL);
            SERVICE_STATUS status;
            while (QueryServiceStatus(hService, &status)) {
                if (status.dwCurrentState == SERVICE_RUNNING) break;
                if (status.dwCurrentState == SERVICE_STOPPED) break;
                Sleep(500);
            }
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCManager);
    }
}

bool IsParentService() {
    DWORD myPid = GetCurrentProcessId();
    DWORD parentPid = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid) { parentPid = pe.th32ParentProcessID; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    bool isService = false;
    if (parentPid != 0 && Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == parentPid) {
                isService = (_wcsicmp(pe.szExeFile, L"TrayAppService.exe") == 0);
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return isService;
}

#define WM_TRAYICON (WM_USER + 1)
#define ID_USERNAME_EDIT  101
#define ID_PASSWORD_EDIT  102
#define ID_LOGIN_BTN      103
#define ID_LOGOUT_BTN     104
#define ID_KEY_EDIT       105
#define ID_ACTIVATE_BTN   106
#define ID_HIDE_BTN       107
#define ID_REFRESH_TIMER  201

static HWND g_hMainWnd = NULL;
static HWND g_hTitle = NULL;
static HWND g_hStatusLine = NULL;
static HWND g_hUserLabel = NULL;
static HWND g_hUserEdit = NULL;
static HWND g_hPassLabel = NULL;
static HWND g_hPassEdit = NULL;
static HWND g_hKeyLabel = NULL;
static HWND g_hKeyEdit = NULL;
static HWND g_hPrimaryBtn = NULL;
static HWND g_hSecondaryBtn = NULL;
static HWND g_hErrorLabel = NULL;
static HFONT g_hFontTitle = NULL;
static HFONT g_hFontBody = NULL;
static HANDLE g_hMutex = NULL;
static UINT g_wmTaskbarCreated = 0;
static bool g_startHidden = false;

enum class UiState { None, Login, Activation, Licensed };
static UiState g_state = UiState::None;
static std::wstring g_lastUsername;
static bool g_lastHasLicense = false;

static void Log(const std::string& m) {
    std::ofstream f("app_init.log", std::ios::app);
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32]; ctime_s(buf, sizeof(buf), &t);
    f << buf << ": " << m << "\n";
}

static void HideAllControls() {
    HWND ctrls[] = { g_hTitle, g_hStatusLine, g_hUserLabel, g_hUserEdit, g_hPassLabel, g_hPassEdit,
                      g_hKeyLabel, g_hKeyEdit, g_hPrimaryBtn, g_hSecondaryBtn, g_hErrorLabel };
    for (HWND h : ctrls) if (h) ShowWindow(h, SW_HIDE);
}

static void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAYAPP));
    wcscpy_s(nid.szTip, L"TrayApp");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TRAY_OPEN, L"Открыть");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_TRAY_EXIT, L"Выход");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == IDM_TRAY_OPEN) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
    else if (cmd == IDM_TRAY_EXIT) { StopTrayAppService(); }
}

static void BuildLoginUi() {
    g_state = UiState::Login;
    HideAllControls();
    SetWindowTextW(g_hTitle, L"Вход в учётную запись");
    SetWindowTextW(g_hStatusLine, L"Функциональность антивируса заблокирована");
    SetWindowTextW(g_hUserLabel, L"Логин:");
    SetWindowTextW(g_hPassLabel, L"Пароль:");
    SetWindowTextW(g_hPrimaryBtn, L"Войти");
    SetWindowTextW(g_hErrorLabel, L"");

    ShowWindow(g_hTitle, SW_SHOW);
    ShowWindow(g_hStatusLine, SW_SHOW);
    ShowWindow(g_hUserLabel, SW_SHOW);
    ShowWindow(g_hUserEdit, SW_SHOW);
    ShowWindow(g_hPassLabel, SW_SHOW);
    ShowWindow(g_hPassEdit, SW_SHOW);
    ShowWindow(g_hPrimaryBtn, SW_SHOW);
    ShowWindow(g_hErrorLabel, SW_SHOW);
}

static void BuildActivationUi(const std::wstring& user) {
    g_state = UiState::Activation;
    HideAllControls();
    std::wstring greet = L"Пользователь: " + user;
    SetWindowTextW(g_hTitle, greet.c_str());
    SetWindowTextW(g_hStatusLine, L"Лицензия не активирована. Антивирус заблокирован.");
    SetWindowTextW(g_hKeyLabel, L"Код активации:");
    SetWindowTextW(g_hPrimaryBtn, L"Активировать");
    SetWindowTextW(g_hSecondaryBtn, L"Выйти из аккаунта");
    SetWindowTextW(g_hErrorLabel, L"");

    ShowWindow(g_hTitle, SW_SHOW);
    ShowWindow(g_hStatusLine, SW_SHOW);
    ShowWindow(g_hKeyLabel, SW_SHOW);
    ShowWindow(g_hKeyEdit, SW_SHOW);
    ShowWindow(g_hPrimaryBtn, SW_SHOW);
    ShowWindow(g_hSecondaryBtn, SW_SHOW);
    ShowWindow(g_hErrorLabel, SW_SHOW);
}

static void BuildLicensedUi(const std::wstring& user, const std::wstring& expiry) {
    g_state = UiState::Licensed;
    HideAllControls();
    std::wstring greet = L"Пользователь: " + user;
    SetWindowTextW(g_hTitle, greet.c_str());
    std::wstring status = L"Антивирус активен. Лицензия до: " + expiry;
    SetWindowTextW(g_hStatusLine, status.c_str());
    SetWindowTextW(g_hPrimaryBtn, L"Скрыть в трей");
    SetWindowTextW(g_hSecondaryBtn, L"Выйти из аккаунта");
    SetWindowTextW(g_hErrorLabel, L"");

    ShowWindow(g_hTitle, SW_SHOW);
    ShowWindow(g_hStatusLine, SW_SHOW);
    ShowWindow(g_hPrimaryBtn, SW_SHOW);
    ShowWindow(g_hSecondaryBtn, SW_SHOW);
}

static void RefreshUi(HWND hwnd) {
    std::wstring user = RpcGetCurrentUser();
    if (user.empty()) {
        if (g_state != UiState::Login) BuildLoginUi();
        g_lastUsername.clear();
        g_lastHasLicense = false;
        return;
    }
    std::wstring expiry;
    bool has = RpcGetLicenseInfo(expiry);
    if (g_state == UiState::Licensed && has && user == g_lastUsername) {
        std::wstring status = L"Антивирус активен. Лицензия до: " + expiry;
        SetWindowTextW(g_hStatusLine, status.c_str());
        return;
    }
    g_lastUsername = user;
    g_lastHasLicense = has;
    if (has) BuildLicensedUi(user, expiry);
    else BuildActivationUi(user);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_wmTaskbarCreated) { AddTrayIcon(hwnd); return 0; }
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        if (id == IDM_TRAY_EXIT) { StopTrayAppService(); return 0; }
        if (id == IDM_TRAY_OPEN) { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); return 0; }
        if (id == ID_LOGIN_BTN) {
            if (g_state == UiState::Login) {
                wchar_t u[128] = {}, p[128] = {};
                GetWindowTextW(g_hUserEdit, u, 128);
                GetWindowTextW(g_hPassEdit, p, 128);
                if (!u[0] || !p[0]) { SetWindowTextW(g_hErrorLabel, L"Введите логин и пароль"); return 0; }
                int s = RpcLogin(u, p);
                if (s == 200) { RefreshUi(hwnd); }
                else { wchar_t e[128]; swprintf_s(e, L"Ошибка входа (HTTP %d)", s); SetWindowTextW(g_hErrorLabel, e); }
            } else if (g_state == UiState::Activation) {
                wchar_t k[256] = {};
                GetWindowTextW(g_hKeyEdit, k, 256);
                if (!k[0]) { SetWindowTextW(g_hErrorLabel, L"Введите код"); return 0; }
                int s = RpcActivate(k);
                if (s == 200) { RefreshUi(hwnd); }
                else { wchar_t e[128]; swprintf_s(e, L"Ошибка активации (HTTP %d)", s); SetWindowTextW(g_hErrorLabel, e); }
            } else if (g_state == UiState::Licensed) {
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        }
        if (id == ID_LOGOUT_BTN) {
            RpcLogout();
            RefreshUi(hwnd);
            return 0;
        }
        break;
    }
    case WM_TIMER:
        if (wp == ID_REFRESH_TIMER) RefreshUi(hwnd);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void CreateControls(HWND hwnd) {
    LOGFONTW lf = {}; lf.lfHeight = -20; wcscpy_s(lf.lfFaceName, L"Segoe UI"); lf.lfWeight = FW_SEMIBOLD;
    g_hFontTitle = CreateFontIndirectW(&lf);
    lf.lfHeight = -14; lf.lfWeight = FW_NORMAL;
    g_hFontBody = CreateFontIndirectW(&lf);

    int x = 30, y = 20, w = 460;
    g_hTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, y, w, 28, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hTitle, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);
    y += 36;
    g_hStatusLine = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, y, w, 40, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hStatusLine, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    y += 50;

    g_hUserLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, y, 100, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hUserLabel, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    g_hUserEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                 x + 110, y, 320, 24, hwnd, (HMENU)ID_USERNAME_EDIT, NULL, NULL);
    SendMessageW(g_hUserEdit, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    y += 32;

    g_hPassLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, y, 100, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hPassLabel, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    g_hPassEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                                 x + 110, y, 320, 24, hwnd, (HMENU)ID_PASSWORD_EDIT, NULL, NULL);
    SendMessageW(g_hPassEdit, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);

    int yk = 102;
    g_hKeyLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, yk, 120, 20, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hKeyLabel, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    g_hKeyEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                x + 130, yk, 300, 24, hwnd, (HMENU)ID_KEY_EDIT, NULL, NULL);
    SendMessageW(g_hKeyEdit, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);

    g_hPrimaryBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   x, 180, 140, 32, hwnd, (HMENU)ID_LOGIN_BTN, NULL, NULL);
    SendMessageW(g_hPrimaryBtn, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
    g_hSecondaryBtn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_TABSTOP,
                                     x + 160, 180, 200, 32, hwnd, (HMENU)ID_LOGOUT_BTN, NULL, NULL);
    SendMessageW(g_hSecondaryBtn, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);

    g_hErrorLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT, x, 222, w, 40, hwnd, NULL, NULL, NULL);
    SendMessageW(g_hErrorLabel, WM_SETFONT, (WPARAM)g_hFontBody, TRUE);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_demoMode = GetEnvironmentVariableW(L"TRAYAPP_DEMO", NULL, 0) != 0;
    if (!g_demoMode) {
        if (!IsServiceRunning()) { StartTrayAppService(); return 0; }
        if (!IsParentService()) { return 0; }
    } else {
        AuthLicenseManager::Instance().Start();
    }

    g_hMutex = CreateMutexW(NULL, TRUE, L"Global\\TrayApp_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    std::wstring cmd(lpCmdLine ? lpCmdLine : L"");
    g_startHidden = (cmd.find(L"-hidden") != std::wstring::npos);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TrayAppMainWnd";
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_TRAYAPP));
    RegisterClassExW(&wc);

    g_hMainWnd = CreateWindowExW(0, L"TrayAppMainWnd", L"TrayApp",
                                  WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 540, 320,
                                  NULL, NULL, hInst, NULL);
    if (!g_hMainWnd) { Log("CreateWindow failed"); return 1; }

    CreateControls(g_hMainWnd);
    AddTrayIcon(g_hMainWnd);
    BuildLoginUi();
    RefreshUi(g_hMainWnd);
    SetTimer(g_hMainWnd, ID_REFRESH_TIMER, 5000, NULL);

    if (!g_startHidden) {
        ShowWindow(g_hMainWnd, SW_SHOW);
        UpdateWindow(g_hMainWnd);
        SetForegroundWindow(g_hMainWnd);
    }
    Log("UI shown");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_hMainWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (g_hFontTitle) DeleteObject(g_hFontTitle);
    if (g_hFontBody) DeleteObject(g_hFontBody);
    if (g_hMutex) CloseHandle(g_hMutex);
    return 0;
}
