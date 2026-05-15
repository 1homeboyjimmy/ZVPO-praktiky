#include <windows.h>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    HDESK hOldDesktop = GetThreadDesktop(GetCurrentThreadId());

    HDESK hNewDesktop = CreateDesktopW(L"SecurePromptDesktop", NULL, NULL, 0, GENERIC_ALL, NULL);
    if (!hNewDesktop) return 0;

    SwitchDesktop(hNewDesktop);
    SetThreadDesktop(hNewDesktop);

    int res = MessageBoxW(NULL, L"Остановить службу и все процессы приложения?", L"Подтверждение остановки",
                          MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);

    SwitchDesktop(hOldDesktop);
    SetThreadDesktop(hOldDesktop);
    CloseDesktop(hNewDesktop);

    return res;
}
