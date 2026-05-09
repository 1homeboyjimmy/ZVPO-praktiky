#include "pch.h"
#include <MddBootstrap.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <fstream>
#include <chrono>
#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>

#include <shellapi.h>
#include "resources/resource.h"
#include <microsoft.ui.xaml.window.h>
#include <commctrl.h>
#include <vector>
#include <string>

// Forward declaration of our App implementation
namespace winrt::TrayApp::implementation { struct App; }

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
using namespace Microsoft::UI::Xaml::Markup;

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
            nid.hWnd = m_hwnd;
            nid.uID = 1;
            Shell_NotifyIcon(NIM_DELETE, &nid);
            Log("Tray icon removed");
        }


        void OnLaunched(LaunchActivatedEventArgs const&)
        {
            try {
                Log("OnLaunched started");
                m_window = Window();
                
                // Get HWND
                auto windowNative{ m_window.as<::IWindowNative>() };
                windowNative->get_WindowHandle(&m_hwnd);
                Log("HWND obtained: " + std::to_string((long long)m_hwnd));

                AddTrayIcon();
                Log("Tray icon added");

                m_window.Title(L"Приложение в трее");

                // Set window icon
                HICON hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAYAPP));
                SendMessage(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessage(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);

                StackPanel panel;
                
                // Main Menu
                MenuBar menuBar;
                MenuBarItem fileItem;
                fileItem.Title(L"Файл");
                
                MenuFlyoutItem exitItem;
                exitItem.Text(L"Выход");
                exitItem.Click([&](auto&&, auto&&) { Exit(); PostQuitMessage(0); });
                
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
                button.Click([&](auto&&, auto&&)
                {
                    ShowWindow(m_hwnd, SW_HIDE);
                });
                contentPanel.Children().Append(button);

                panel.Children().Append(contentPanel);



                m_window.Content(panel);

                // Handle window close to hide instead of exit
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
                else
                {
                    Log("Started in hidden mode");
                }

                // Subclass window to handle tray and TaskbarCreated messages
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






        IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const&) { return nullptr; }
        IXamlType GetXamlType(hstring const&) { return nullptr; }
        winrt::com_array<XmlnsDefinition> GetXmlnsDefinitions() { return {}; }


    private:
        void AddTrayIcon()
        {
            NOTIFYICONDATA nid = { sizeof(nid) };
            nid.hWnd = m_hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_TRAYAPP));
            wcscpy_s(nid.szTip, L"WinUI 3 Tray App");
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
                Exit();
                PostQuitMessage(0);
            }
        }

        Window m_window{ nullptr };

        HWND m_hwnd{ nullptr };
    };
}



namespace winrt::TrayApp::factory_implementation
{
    struct App : AppT<App, implementation::App>
    {
    };
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int)
{
    // Single instance check
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
        // We need a DispatcherQueue for WinUI 3
        auto dispatcherQueueController = winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnDedicatedThread();
        Log("DispatcherQueueController created");

        Application::Start([](auto&&)
        {
            try {
                Log("Application::Start callback");
                auto appImpl = winrt::make_self<TrayApp::implementation::App>();
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


