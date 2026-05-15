#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <tlhelp32.h>
#include <string>
#include "ServiceRpc_h.h"
#include "AuthLicenseRpc_h.h"
#include "AuthLicense.h"

SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

static void ApplyDenyTerminateDacl(HANDLE hObject) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    if (GetSecurityInfo(hObject, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD) != ERROR_SUCCESS) {
        return;
    }

    PSID pAdminSID = NULL, pUserSID = NULL;
    SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdminSID);
    AllocateAndInitializeSid(&SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &pUserSID);

    EXPLICIT_ACCESSW ea[2] = { 0 };
    ea[0].grfAccessPermissions = PROCESS_TERMINATE;
    ea[0].grfAccessMode = DENY_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[0].Trustee.ptstrName = (LPWSTR)pAdminSID;

    ea[1].grfAccessPermissions = PROCESS_TERMINATE;
    ea[1].grfAccessMode = DENY_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea[1].Trustee.ptstrName = (LPWSTR)pUserSID;

    if (SetEntriesInAclW(2, ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
        SetSecurityInfo(hObject, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
    }

    if (pAdminSID) FreeSid(pAdminSID);
    if (pUserSID) FreeSid(pUserSID);
    if (pNewDACL) LocalFree(pNewDACL);
    if (pSD) LocalFree(pSD);
}

void TerminateTrayApps() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"TrayApp.exe") == 0) {
                HANDLE hProcess = OpenProcess(WRITE_DAC | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (hProcess) {
                    PACL pNewDACL = NULL;
                    EXPLICIT_ACCESSW ea = { 0 };
                    ea.grfAccessPermissions = PROCESS_ALL_ACCESS;
                    ea.grfAccessMode = SET_ACCESS;
                    ea.grfInheritance = NO_INHERITANCE;
                    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
                    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
                    ea.Trustee.ptstrName = (LPWSTR)L"SYSTEM";
                    if (SetEntriesInAclW(1, &ea, NULL, &pNewDACL) == ERROR_SUCCESS) {
                        SetSecurityInfo(hProcess, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
                        LocalFree(pNewDACL);
                    }
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

void LaunchAppInSession(DWORD sessionId) {
    if (sessionId == 0) return;

    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) return;

    HANDLE hTokenDup = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hTokenDup)) {
        CloseHandle(hToken);
        return;
    }

    LPVOID pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hTokenDup, FALSE);

    std::wstring appPath;
    appPath.resize(MAX_PATH);
    GetModuleFileNameW(NULL, &appPath[0], MAX_PATH);
    appPath.resize(wcslen(appPath.c_str()));
    size_t pos = appPath.find_last_of(L"\\/");
    appPath = appPath.substr(0, pos) + L"\\TrayApp.exe";

    std::wstring cmdLine = L"\"" + appPath + L"\" -hidden";

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    PROCESS_INFORMATION pi = { 0 };

    if (CreateProcessAsUserW(hTokenDup, appPath.c_str(), &cmdLine[0], NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, pEnv, NULL, &si, &pi)) {

        ApplyDenyTerminateDacl(pi.hProcess);

        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hTokenDup);
    CloseHandle(hToken);
}

void StopService(handle_t Binding) {
    DWORD activeSession = WTSGetActiveConsoleSessionId();
    if (activeSession != 0xFFFFFFFF) {
        HANDLE hToken = NULL;
        if (WTSQueryUserToken(activeSession, &hToken)) {
            std::wstring appPath;
            appPath.resize(MAX_PATH);
            GetModuleFileNameW(NULL, &appPath[0], MAX_PATH);
            appPath.resize(wcslen(appPath.c_str()));
            size_t pos = appPath.find_last_of(L"\\/");
            appPath = appPath.substr(0, pos) + L"\\SecurePrompt.exe";

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };

            LPVOID pEnv = NULL;
            CreateEnvironmentBlock(&pEnv, hToken, FALSE);

            if (CreateProcessAsUserW(hToken, appPath.c_str(), NULL, NULL, NULL, FALSE,
                CREATE_UNICODE_ENVIRONMENT, pEnv, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD exitCode = 0;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);

                if (exitCode != IDYES) {
                    if (pEnv) DestroyEnvironmentBlock(pEnv);
                    CloseHandle(hToken);
                    return;
                }
            }
            if (pEnv) DestroyEnvironmentBlock(pEnv);
            CloseHandle(hToken);
        }
    }

    TerminateTrayApps();
    SetEvent(g_ServiceStopEvent);
    RpcMgmtStopServerListening(NULL);
}

static wchar_t* AllocRpcWString(const std::wstring& s) {
    size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    wchar_t* out = (wchar_t*)midl_user_allocate(bytes);
    if (!out) return NULL;
    memcpy(out, s.c_str(), bytes);
    return out;
}

long GetCurrentUser(handle_t Binding, wchar_t** Username) {
    if (!Username) return E_POINTER;
    *Username = AllocRpcWString(AuthLicenseManager::Instance().GetUsername());
    return *Username ? S_OK : E_OUTOFMEMORY;
}

long Login(handle_t Binding, const wchar_t* Username, const wchar_t* Password, long* HttpStatus) {
    if (!Username || !Password || !HttpStatus) return E_POINTER;
    *HttpStatus = AuthLicenseManager::Instance().Login(Username, Password);
    return (*HttpStatus == 200) ? S_OK : E_FAIL;
}

long Logout(handle_t Binding) {
    AuthLicenseManager::Instance().Logout();
    return S_OK;
}

long GetLicenseInfo(handle_t Binding, long* HasLicense, wchar_t** ExpiryDate) {
    if (!HasLicense || !ExpiryDate) return E_POINTER;
    auto& m = AuthLicenseManager::Instance();
    *HasLicense = m.HasLicense() ? 1 : 0;
    *ExpiryDate = AllocRpcWString(m.GetLicenseExpiry());
    return *ExpiryDate ? S_OK : E_OUTOFMEMORY;
}

long Activate(handle_t Binding, const wchar_t* ActivationKey, long* HttpStatus) {
    if (!ActivationKey || !HttpStatus) return E_POINTER;
    *HttpStatus = AuthLicenseManager::Instance().Activate(ActivationKey);
    return (*HttpStatus == 200) ? S_OK : E_FAIL;
}

long ScanFile(handle_t Binding, const wchar_t* Path, long* Verdict) {
    if (!Verdict) return E_POINTER;
    if (!AuthLicenseManager::Instance().HasLicense()) {
        *Verdict = -1;
        return E_ACCESSDENIED;
    }
    *Verdict = 0;
    return S_OK;
}

void* __RPC_USER MIDL_user_allocate(size_t size) { return malloc(size); }
void __RPC_USER MIDL_user_free(void* p) { free(p); }

DWORD WINAPI ServiceCtrlHandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    switch (dwControl) {
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (dwEventType == WTS_SESSION_LOGON) {
            WTSSESSION_NOTIFICATION* sn = (WTSSESSION_NOTIFICATION*)lpEventData;
            LaunchAppInSession(sn->dwSessionId);
        }
        break;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        break;
    }
    return NO_ERROR;
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    ApplyDenyTerminateDacl(GetCurrentProcess());

    PWTS_SESSION_INFOW pSessionInfo = NULL;
    DWORD count = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &count)) {
        for (DWORD i = 0; i < count; i++) {
            if (pSessionInfo[i].State == WTSActive && pSessionInfo[i].SessionId != 0) {
                LaunchAppInSession(pSessionInfo[i].SessionId);
            }
        }
        WTSFreeMemory(pSessionInfo);
    }

    AuthLicenseManager::Instance().Start();

    RPC_STATUS status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)L"TrayAppRpcPort", NULL);
    if (status == RPC_S_OK) {
        RpcServerRegisterIf(ITrayAppService_v1_0_s_ifspec, NULL, NULL);
        RpcServerRegisterIf(IAuthLicenseService_v1_0_s_ifspec, NULL, NULL);
        RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);
    }

    WaitForSingleObject(g_ServiceStopEvent, INFINITE);
    RpcMgmtWaitServerListen();
    AuthLicenseManager::Instance().Stop();

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    return 0;
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_StatusHandle = RegisterServiceCtrlHandlerExW(L"TrayAppService", ServiceCtrlHandlerEx, NULL);
    if (!g_StatusHandle) return;

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_ServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ServiceStopEvent) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)L"TrayAppService", (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(ServiceTable);
    return 0;
}
