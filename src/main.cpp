#include "pch.h"
#include <MddBootstrap.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <fstream>
#include <chrono>
#include <windows.h>
#include <shellapi.h>
#include "resources/resource.h"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <tlhelp32.h>
#include "ServiceRpc_h.h"
#include "AuthLicenseRpc_h.h"

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

static std::wstring RpcGetCurrentUser() {
    std::wstring out;
    handle_t h = OpenServiceBinding();
    if (!h) return out;
    wchar_t* name = NULL;
    RpcTryExcept {
        if (GetCurrentUser(h, &name) == S_OK && name) {
            out = name;
            MIDL_user_free(name);
        }
    } RpcExcept(1) {} RpcEndExcept
    RpcBindingFree(&h);
    return out;
}

static int RpcLogin(const std::wstring& u, const std::wstring& p) {
    int status = -1;
    handle_t h = OpenServiceBinding();
    if (!h) return status;
    RpcTryExcept {
        long hs = 0;
        Login(h, u.c_str(), p.c_str(), &hs);
        status = (int)hs;
    } RpcExcept(1) {} RpcEndExcept
    RpcBindingFree(&h);
    return status;
}

static void RpcLogout() {
    handle_t h = OpenServiceBinding();
    if (!h) return;
    RpcTryExcept { Logout(h); } RpcExcept(1) {} RpcEndExcept
    RpcBindingFree(&h);
}

static bool RpcGetLicenseInfo(std::wstring& expiry) {
    bool has = false;
    handle_t h = OpenServiceBinding();
    if (!h) return false;
    RpcTryExcept {
        long hasL = 0;
        wchar_t* exp = NULL;
        if (GetLicenseInfo(h, &hasL, &exp) == S_OK) {
            has = (hasL != 0);
            if (exp) { expiry = exp; MIDL_user_free(exp); }
        }
    } RpcExcept(1) {} RpcEndExcept
    RpcBindingFree(&h);
    return has;
}

static int RpcActivate(const std::wstring& key) {
    int status = -1;
    handle_t h = OpenServiceBinding();
    if (!h) return status;
    RpcTryExcept {
        long hs = 0;
        Activate(h, key.c_str(), &hs);
        status = (int)hs;
    } RpcExcept(1) {} RpcEndExcept
    RpcBindingFree(&h);
    return status;
}

void StopTrayAppService() {
    RPC_WSTR szStringBinding = NULL;
    RPC_STATUS status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)L"TrayAppRpcPort", NULL, &szStringBinding);
    if (status == RPC_S_OK) {
        handle_t bindingHandle;
        status = RpcBindingFromStringBindingW(szStringBinding, &bindingHandle);
        if (status == RPC_S_OK) {
            RpcTryExcept {
                StopService(bindingHandle);
            } RpcExcept(1) {
            } RpcEndExcept
            RpcBindingFree(&bindingHandle);
        }
        RpcStringFreeW(&szStringBinding);
    }
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
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID == myPid) {
                    parentPid = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        
        if (parentPid != 0) {
            if (Process32FirstW(hSnap, &pe)) {
                do {
                    if (pe.th32ProcessID == parentPid) {
                        CloseHandle(hSnap);
                        return _wcsicmp(pe.szExeFile, L"TrayAppService.exe") == 0;
                    }
                } while (Process32NextW(hSnap, &pe));
            }
        }
        CloseHandle(hSnap);
    }
    return false;
}


#undef GetCurrentTime

#define WM_TRAYICON (WM_USER + 1)

UINT g_wmTaskbarCreated = 0;
HANDLE g_hMutex = NULL;
bool g_startHidden = false;
winrt::Microsoft::UI::Xaml::Application g_app{ nullptr };

void Log(const std::string& message)
{
    std::ofstream logFile("app_init.log", std::ios::app);
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[26];
    ctime_s(buf, sizeof(buf), &now);
    logFile << buf << ": " << message << std::endl;
    logFile.flush();
}

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::TrayApp::implementation
{
    struct App : winrt::implements<App, winrt::Microsoft::UI::Xaml::IApplicationOverrides>
    {
        HWND m_hwnd{ NULL };
        Window m_window{ nullptr };
        StackPanel m_contentHost{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer m_pollTimer{ nullptr };
        bool m_lastHasLicense{ false };
        std::wstring m_lastUsername;

        App()
        {
            Log("App constructor started");
            Log("App constructor finished");
        }

        ~App()
        {
            NOTIFYICONDATA nid = { sizeof(nid) };
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_hwnd;
            nid.uID = 1;
            Shell_NotifyIcon(NIM_DELETE, &nid);
        }

        void OnLaunched(LaunchActivatedEventArgs const&)
        {
            try {
                Log("OnLaunched started");
                
                // Add default resources for WinUI 3 controls
                Application::Current().Resources().MergedDictionaries().Append(winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
                Log("Resources initialized");

                m_window = Window();
                
                auto windowNative{ m_window.as<::IWindowNative>() };
                windowNative->get_WindowHandle(&m_hwnd);
                Log("HWND obtained: " + std::to_string((long long)m_hwnd));

                AddTrayIcon();
                Log("Tray icon added");

                m_window.Title(L"Приложение в трее");

                HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAYAPP));
                SendMessage(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessage(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

                StackPanel panel;
                MenuBar menuBar;
                MenuBarItem fileItem;
                fileItem.Title(L"Файл");
                
                MenuFlyoutItem exitItem;
                exitItem.Text(L"Выход");
                exitItem.Click([&](auto&&, auto&&) { 
                    StopTrayAppService();
                });
                
                fileItem.Items().Append(exitItem);
                menuBar.Items().Append(fileItem);
                panel.Children().Append(menuBar);

                panel.HorizontalAlignment(HorizontalAlignment::Stretch);
                panel.VerticalAlignment(VerticalAlignment::Stretch);

                m_contentHost = StackPanel();
                m_contentHost.HorizontalAlignment(HorizontalAlignment::Center);
                m_contentHost.VerticalAlignment(VerticalAlignment::Center);
                m_contentHost.Spacing(12);
                panel.Children().Append(m_contentHost);
                m_window.Content(panel);

                RefreshUI();

                m_pollTimer = winrt::Microsoft::UI::Xaml::DispatcherTimer();
                m_pollTimer.Interval(std::chrono::seconds(5));
                m_pollTimer.Tick([this](auto&&, auto&&) { RefreshUI(); });
                m_pollTimer.Start();

                m_window.Closed([&](auto&&, auto&& args)
                {
                    args.Handled(true);
                    ShowWindow(m_hwnd, SW_HIDE);
                });

                if (!g_startHidden)
                {
                    m_window.Activate();
                    Log("Window activated");
                }

                SetWindowSubclass(m_hwnd, [](HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData) -> LRESULT
                {
                    App* app = reinterpret_cast<App*>(dwRefData);
                    if (uMsg == WM_TRAYICON)
                    {
                        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP)
                        {
                            ShowWindow(hWnd, SW_SHOW);
                            SetForegroundWindow(hWnd);
                        }
                        else if (lParam == WM_RBUTTONUP)
                        {
                            app->ShowTrayContextMenu();
                        }
                        return 0;
                    }
                    else if (uMsg == g_wmTaskbarCreated)
                    {
                        app->AddTrayIcon();
                        return 0;
                    }
                    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
                }, 1, reinterpret_cast<DWORD_PTR>(this));
                Log("Subclassing completed");
            } catch (const winrt::hresult_error& e) {
                Log("Exception in OnLaunched: " + winrt::to_string(e.message()));
            } catch (...) {
                Log("Unknown Exception in OnLaunched");
            }
        }

        void RefreshUI()
        {
            std::wstring user = RpcGetCurrentUser();
            if (user.empty()) {
                m_lastUsername.clear();
                m_lastHasLicense = false;
                BuildLoginForm();
                return;
            }

            std::wstring expiry;
            bool hasLic = RpcGetLicenseInfo(expiry);

            if (user == m_lastUsername && hasLic == m_lastHasLicense && m_contentHost.Children().Size() > 0) {
                if (hasLic) {
                    UpdateLicensedView(user, expiry);
                }
                return;
            }
            m_lastUsername = user;
            m_lastHasLicense = hasLic;

            if (!hasLic) {
                BuildActivationForm(user);
            } else {
                BuildLicensedView(user, expiry);
            }
        }

        void BuildLoginForm()
        {
            m_contentHost.Children().Clear();

            TextBlock title;
            title.Text(L"Вход в учётную запись");
            title.FontSize(24);
            title.HorizontalAlignment(HorizontalAlignment::Center);
            m_contentHost.Children().Append(title);

            TextBlock warn;
            warn.Text(L"Функциональность антивируса заблокирована");
            warn.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::OrangeRed()));
            m_contentHost.Children().Append(warn);

            TextBox userBox;
            userBox.PlaceholderText(L"Логин");
            userBox.Width(280);
            m_contentHost.Children().Append(userBox);

            PasswordBox passBox;
            passBox.PlaceholderText(L"Пароль");
            passBox.Width(280);
            m_contentHost.Children().Append(passBox);

            TextBlock err;
            err.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Red()));
            m_contentHost.Children().Append(err);

            Button loginBtn;
            loginBtn.Content(winrt::box_value(L"Войти"));
            loginBtn.HorizontalAlignment(HorizontalAlignment::Center);
            loginBtn.Click([this, userBox, passBox, err](auto&&, auto&&) {
                std::wstring u(userBox.Text().c_str());
                std::wstring p(passBox.Password().c_str());
                if (u.empty() || p.empty()) {
                    err.Text(L"Введите логин и пароль");
                    return;
                }
                int status = RpcLogin(u, p);
                if (status == 200) {
                    RefreshUI();
                } else {
                    wchar_t msg[128];
                    swprintf_s(msg, L"Ошибка входа (HTTP %d)", status);
                    err.Text(msg);
                }
            });
            m_contentHost.Children().Append(loginBtn);
        }

        void BuildActivationForm(const std::wstring& user)
        {
            m_contentHost.Children().Clear();

            TextBlock greet;
            greet.Text(L"Пользователь: " + winrt::hstring(user.c_str()));
            greet.FontSize(18);
            m_contentHost.Children().Append(greet);

            TextBlock warn;
            warn.Text(L"Лицензия не активирована. Антивирус заблокирован.");
            warn.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::OrangeRed()));
            m_contentHost.Children().Append(warn);

            TextBox keyBox;
            keyBox.PlaceholderText(L"Код активации");
            keyBox.Width(320);
            m_contentHost.Children().Append(keyBox);

            TextBlock err;
            err.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::Red()));
            m_contentHost.Children().Append(err);

            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(8);
            buttons.HorizontalAlignment(HorizontalAlignment::Center);

            Button activateBtn;
            activateBtn.Content(winrt::box_value(L"Активировать"));
            activateBtn.Click([this, keyBox, err](auto&&, auto&&) {
                std::wstring k(keyBox.Text().c_str());
                if (k.empty()) { err.Text(L"Введите код"); return; }
                int status = RpcActivate(k);
                if (status == 200) {
                    m_lastHasLicense = false;
                    RefreshUI();
                } else {
                    wchar_t msg[128];
                    swprintf_s(msg, L"Ошибка активации (HTTP %d)", status);
                    err.Text(msg);
                }
            });
            buttons.Children().Append(activateBtn);

            Button logoutBtn;
            logoutBtn.Content(winrt::box_value(L"Выйти"));
            logoutBtn.Click([this](auto&&, auto&&) {
                RpcLogout();
                RefreshUI();
            });
            buttons.Children().Append(logoutBtn);

            m_contentHost.Children().Append(buttons);
        }

        void BuildLicensedView(const std::wstring& user, const std::wstring& expiry)
        {
            m_contentHost.Children().Clear();

            TextBlock greet;
            greet.Text(L"Пользователь: " + winrt::hstring(user.c_str()));
            greet.FontSize(20);
            m_contentHost.Children().Append(greet);

            TextBlock ok;
            ok.Text(L"Антивирус активен");
            ok.FontSize(18);
            ok.Foreground(winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Colors::SeaGreen()));
            m_contentHost.Children().Append(ok);

            TextBlock expiryLabel;
            expiryLabel.Text(L"Лицензия действует до: " + winrt::hstring(expiry.c_str()));
            m_contentHost.Children().Append(expiryLabel);

            StackPanel buttons;
            buttons.Orientation(Orientation::Horizontal);
            buttons.Spacing(8);
            buttons.HorizontalAlignment(HorizontalAlignment::Center);

            Button hideBtn;
            hideBtn.Content(winrt::box_value(L"Скрыть в трей"));
            hideBtn.Click([this](auto&&, auto&&) { ShowWindow(m_hwnd, SW_HIDE); });
            buttons.Children().Append(hideBtn);

            Button logoutBtn;
            logoutBtn.Content(winrt::box_value(L"Выйти из аккаунта"));
            logoutBtn.Click([this](auto&&, auto&&) {
                RpcLogout();
                RefreshUI();
            });
            buttons.Children().Append(logoutBtn);

            m_contentHost.Children().Append(buttons);
        }

        void UpdateLicensedView(const std::wstring& user, const std::wstring& expiry)
        {
            auto count = m_contentHost.Children().Size();
            for (uint32_t i = 0; i < count; i++) {
                auto child = m_contentHost.Children().GetAt(i);
                auto tb = child.try_as<TextBlock>();
                if (tb) {
                    winrt::hstring t = tb.Text();
                    std::wstring ws(t);
                    if (ws.find(L"Лицензия действует до:") == 0) {
                        tb.Text(L"Лицензия действует до: " + winrt::hstring(expiry.c_str()));
                    }
                }
            }
        }

        void AddTrayIcon()
        {
            NOTIFYICONDATA nid = { sizeof(nid) };
            nid.cbSize = sizeof(nid);
            nid.hWnd = m_hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAYAPP));
            wcscpy_s(nid.szTip, L"TrayApp (WinUI 3)");
            Shell_NotifyIcon(NIM_ADD, &nid);
        }

        void ShowTrayContextMenu()
        {
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Открыть");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Выход");

            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(m_hwnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd, NULL);
            DestroyMenu(hMenu);

            if (cmd == IDM_TRAY_OPEN)
            {
                ShowWindow(m_hwnd, SW_SHOW);
                SetForegroundWindow(m_hwnd);
            }
            else if (cmd == IDM_TRAY_EXIT)
            {
                StopTrayAppService();
            }
        }
    };
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int)
{
    if (!IsServiceRunning()) {
        StartTrayAppService();
        return 0;
    }

    if (!IsParentService()) {
        return 0;
    }
    g_hMutex = CreateMutex(NULL, TRUE, L"Global\\TrayApp_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log("Repeated launch detected - exiting");
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }

    init_apartment(winrt::apartment_type::single_threaded);
    Log("Apartment initialized");

    g_wmTaskbarCreated = RegisterWindowMessage(L"TaskbarCreated");
    std::wstring cmdLine(lpCmdLine);
    g_startHidden = (cmdLine.find(L"-hidden") != std::wstring::npos);

    PACKAGE_VERSION minVersion{};
    minVersion.Version = 0;
    HRESULT hr = MddBootstrapInitialize2(0x00010006, L"", minVersion, MddBootstrapInitializeOptions_OnNoMatch_ShowUI);
    if (FAILED(hr))
    {
        Log("MddBootstrapInitialize2 failed: " + std::to_string(hr));
        if (g_hMutex) CloseHandle(g_hMutex);
        return 1;
    }
    Log("Bootstrap initialized");

    {
        auto dispatcherQueueController = winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnDedicatedThread();
        Log("DispatcherQueueController created");

        Application::Start([](auto&&)
        {
            try {
                Log("Application::Start callback");
                auto appImpl = winrt::make_self<winrt::TrayApp::implementation::App>();
                g_app = appImpl.as<winrt::Microsoft::UI::Xaml::Application>();
                Log("App object created");
            } catch (const winrt::hresult_error& e) {
                Log("Exception in make<App>: " + winrt::to_string(e.message()));
            } catch (...) {
                Log("Unknown Exception in make<App>");
            }
        });

        Log("Application::Start returned - entering manual message loop");
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Log("Message loop exited");
    }

    MddBootstrapShutdown();
    if (g_hMutex) CloseHandle(g_hMutex);
    Log("Bootstrap shutdown");
    return 0;
}
