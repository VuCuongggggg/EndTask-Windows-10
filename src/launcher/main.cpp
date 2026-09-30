#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

#define DLL_NAME L"EndTask10Hook.dll"
#define EVENT_NAME L"Global\\EndTask10_Unload"
#define READY_EVENT L"Global\\EndTask10_Ready"

// Forward declarations
BOOL UnloadDLL();
BOOL InjectDLL(DWORD pid, const wchar_t* dllPath);
static DWORD FindExplorerPID();
static BOOL IsDLLLoaded(DWORD pid);
static BOOL GetDLLPath(wchar_t* buf, size_t cch);
static void DoInjection();

static BOOL SetHotkeyRegistry(UINT mods, UINT vk)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\EndTask10", 0, nullptr, 0,
        KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) return FALSE;
    DWORD dMods = mods, dVk = vk;
    RegSetValueExW(hKey, L"HotkeyModifiers", 0, REG_DWORD, (LPBYTE)&dMods, sizeof(dMods));
    RegSetValueExW(hKey, L"HotkeyVk", 0, REG_DWORD, (LPBYTE)&dVk, sizeof(dVk));
    RegCloseKey(hKey);
    return TRUE;
}

static BOOL GetHotkeyRegistry(UINT* pMods, UINT* pVk)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\EndTask10", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    DWORD type, size = sizeof(DWORD), mods, vk;
    BOOL ok = RegQueryValueExW(hKey, L"HotkeyModifiers", nullptr, &type, (LPBYTE)&mods, &size) == ERROR_SUCCESS && type == REG_DWORD;
    size = sizeof(DWORD);
    ok = ok && RegQueryValueExW(hKey, L"HotkeyVk", nullptr, &type, (LPBYTE)&vk, &size) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(hKey);
    if (ok) { *pMods = mods; *pVk = vk; }
    return ok;
}

// GUI Elements
HWND g_hMainWnd = nullptr;
HWND g_hHotkey = nullptr;
HWND g_hBtnApply = nullptr;
HWND g_hBtnUnload = nullptr;
HWND g_hChkStartup = nullptr;

static void GetExePath(wchar_t* buf, size_t cch)
{
    GetModuleFileNameW(nullptr, buf, (DWORD)cch);
}

static BOOL IsStartupEnabledInternal()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) return FALSE;
    BOOL exists = RegQueryValueExW(hKey, L"EndTask10", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return exists;
}

static void SetStartupEnabledInternal(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetExePath(exePath, MAX_PATH);
        wchar_t cmdLine[MAX_PATH + 16];
        swprintf_s(cmdLine, L"\"%s\" /silent", exePath);
        RegSetValueExW(hKey, L"EndTask10", 0, REG_SZ, (LPBYTE)cmdLine,
            (DWORD)((wcslen(cmdLine) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, L"EndTask10");
    }
    RegCloseKey(hKey);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Select Hotkey to kill frozen apps:",
            WS_CHILD | WS_VISIBLE, 20, 20, 300, 20, hwnd, nullptr, nullptr, nullptr);

        g_hHotkey = CreateWindowW(HOTKEY_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER,
            20, 45, 200, 25, hwnd, nullptr, nullptr, nullptr);

        // Load current from registry
        UINT mods = MOD_CONTROL | MOD_SHIFT, vk = 'E';
        GetHotkeyRegistry(&mods, &vk);

        // Convert MOD_* to HOTKEYF_*
        UINT hkf = 0;
        if (mods & MOD_ALT) hkf |= HOTKEYF_ALT;
        if (mods & MOD_CONTROL) hkf |= HOTKEYF_CONTROL;
        if (mods & MOD_SHIFT) hkf |= HOTKEYF_SHIFT;
        SendMessage(g_hHotkey, HKM_SETHOTKEY, MAKEWORD(vk, hkf), 0);

        g_hBtnApply = CreateWindowW(L"BUTTON", L"Apply && Inject",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            20, 90, 120, 30, hwnd, (HMENU)1, nullptr, nullptr);

        g_hBtnUnload = CreateWindowW(L"BUTTON", L"Unload DLL",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            150, 90, 100, 30, hwnd, (HMENU)2, nullptr, nullptr);

        g_hChkStartup = CreateWindowW(L"BUTTON", L"Run at Windows startup",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            20, 130, 200, 20, hwnd, nullptr, nullptr, nullptr);

        if (IsStartupEnabledInternal()) {
            SendMessage(g_hChkStartup, BM_SETCHECK, BST_CHECKED, 0);
        }

        // Set font
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        EnumChildWindows(hwnd, [](HWND child, LPARAM font) {
            SendMessage(child, WM_SETFONT, font, TRUE);
            return TRUE;
        }, (LPARAM)hFont);
        return 0;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 1) { // Apply
            LRESULT res = SendMessage(g_hHotkey, HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(res);
            UINT hkf = HIBYTE(res);
            UINT mods = 0;
            if (hkf & HOTKEYF_ALT) mods |= MOD_ALT;
            if (hkf & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
            if (hkf & HOTKEYF_SHIFT) mods |= MOD_SHIFT;

            if (vk == 0) {
                MessageBoxW(hwnd, L"Please select a valid hotkey.", L"Error", MB_ICONERROR);
                return 0;
            }
            if (mods == 0) {
                MessageBoxW(hwnd, L"Please include a modifier (Ctrl, Shift, or Alt).", L"Error", MB_ICONERROR);
                return 0;
            }
            SetHotkeyRegistry(mods, vk);
            // Sync the auto-start checkbox state to the registry Run key
            SetStartupEnabledInternal(SendMessage(g_hChkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
            DoInjection();
            MessageBoxW(hwnd, L"Applied successfully! The tool is now active in the taskbar.", L"EndTask10", MB_ICONINFORMATION);
            DestroyWindow(hwnd);
        }
        else if (LOWORD(wParam) == 2) { // Unload
            UnloadDLL();
            MessageBoxW(hwnd, L"DLL unloaded successfully.", L"EndTask10", MB_ICONINFORMATION);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowGUI()
{
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"EndTask10ConfigClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    // Center window
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 320, wh = 210;

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"EndTask10 Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - ww) / 2, (sh - wh) / 2, ww, wh,
        nullptr, nullptr, wc.hInstance, nullptr);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

static BOOL GetDLLPath(wchar_t* buf, size_t cch)
{
    if (!GetModuleFileNameW(nullptr, buf, (DWORD)cch)) return FALSE;
    wchar_t* p = wcsrchr(buf, L'\\');
    if (!p) return FALSE;
    wcscpy_s(p + 1, cch - (p + 1 - buf), DLL_NAME);
    return TRUE;
}

static DWORD FindExplorerPID()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) do {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) { pid = pe.th32ProcessID; break; }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

static BOOL IsDLLLoaded(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me = { sizeof(me) };
    BOOL found = FALSE;
    if (Module32FirstW(snap, &me)) do {
        if (_wcsicmp(me.szModule, DLL_NAME) == 0) { found = TRUE; break; }
    } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    return found;
}

BOOL UnloadDLL()
{
    HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVENT_NAME);
    if (!hEvt) return TRUE;
    SetEvent(hEvt);
    CloseHandle(hEvt);
    DWORD pid = FindExplorerPID();
    for (int i = 0; i < 25; i++) {
        if (!IsDLLLoaded(pid)) return TRUE;
        Sleep(200);
    }
    return TRUE;
}

BOOL InjectDLL(DWORD pid, const wchar_t* dllPath)
{
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) return FALSE;

    size_t cb = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(hProc, nullptr, cb, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { CloseHandle(hProc); return FALSE; }

    if (!WriteProcessMemory(hProc, rem, (void*)dllPath, cb, nullptr)) {
        VirtualFreeEx(hProc, rem, 0, MEM_RELEASE); CloseHandle(hProc); return FALSE;
    }

    LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, rem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, rem, 0, MEM_RELEASE); CloseHandle(hProc); return FALSE;
    }

    WaitForSingleObject(hThread, 10000);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, rem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    return IsDLLLoaded(pid);
}

static void DoInjection()
{
    // Try to stop services that auto-restart killed apps (only works if elevated)
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION te = {};
        DWORD sz = 0;
        if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &sz))
            isElevated = te.TokenIsElevated;
        CloseHandle(hToken);
    }
    if (isElevated) {
        const wchar_t* services[] = {
            L"Steam Client Service",
            L"Steam Client Service64",
            L"Epic Online Services",
            L"EpicGamesLauncher",
        };
        for (int i = 0; i < _countof(services); i++) {
            SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (scm) {
                SC_HANDLE svc = OpenServiceW(scm, services[i], SERVICE_STOP | SERVICE_QUERY_STATUS);
                if (svc) {
                    SERVICE_STATUS ss;
                    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
                    CloseServiceHandle(svc);
                }
                CloseServiceHandle(scm);
            }
        }
    }

    DWORD pid = FindExplorerPID();
    if (!pid) return;

    if (IsDLLLoaded(pid)) {
        UnloadDLL();
        pid = FindExplorerPID();
    }

    wchar_t path[MAX_PATH];
    if (!GetDLLPath(path, MAX_PATH)) return;

    HANDLE hReady = CreateEventW(nullptr, TRUE, FALSE, READY_EVENT);
    ResetEvent(hReady);
    CloseHandle(hReady);

    if (InjectDLL(pid, path)) {
        hReady = OpenEventW(SYNCHRONIZE, FALSE, READY_EVENT);
        if (hReady) {
            WaitForSingleObject(hReady, 3000);
            CloseHandle(hReady);
        }
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    // Switch from main() to wWinMain() to avoid flashing a black console window
    // Parse command line arguments manually
    bool silent = false;
    bool unload = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (_wcsicmp(argv[i], L"/silent") == 0) silent = true;
            if (_wcsicmp(argv[i], L"/unload") == 0) unload = true;
        }
        LocalFree(argv);
    }

    if (unload) {
        return UnloadDLL() ? 0 : 1;
    }

    if (!silent) {
        ShowGUI();
        return 0;
    } else {
        DoInjection();
        return 0;
    }
}
