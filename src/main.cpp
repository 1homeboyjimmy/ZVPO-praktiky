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

void* __RPC_USER MIDL_user_allocate(size_t size) { return malloc(size); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

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

                StackPanel contentPanel;
                contentPanel.HorizontalAlignment(HorizontalAlignment::Center);
                contentPanel.VerticalAlignment(VerticalAlignment::Center);

                TextBlock text;
                text.Text(L"Приложение в трее (WinUI 3)");
                text.FontSize(24);
                contentPanel.Children().Append(text);

                Button button;
                button.Content(winrt::box_value(L"Скрыть в трей"));
                button.Click([&](auto&&, auto&&) { ShowWindow(m_hwnd, SW_HIDE); });
                contentPanel.Children().Append(button);

                panel.Children().Append(contentPanel);
                m_window.Content(panel);

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
