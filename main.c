/*
 * InboundFlip — 单按钮切换 Windows 防火墙“入站用户通知”。
 *
 * 写入：程序内调用系统自带 netsh.exe（取完整系统路径，防 PATH 劫持）：
 *   netsh advfirewall set allprofiles settings inboundusernotification enable|disable
 * 读取：防火墙策略注册表键 DisableNotifications（只读，不写注册表）。
 * 零副作用：不写任何文件 / 注册表；系统状态变化仅限该设置本身。
 */
/* -municode 已定义 UNICODE/_UNICODE 并选择 wWinMain 入口 */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define APP_TITLE  L"InboundFlip"
#define WC_MAIN    L"InboundFlipWnd"
#define MUTEX_NAME L"Local\\InboundFlip.SingleInstance"

#define IDC_STATUS 1001
#define IDC_BUTTON 1002

#define CLIENT_CX 440
#define CLIENT_CY 210
#define WIN_STYLE (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX))

static HWND  g_status, g_button;
static HFONT g_fontStatus, g_fontButton;

/* ---------- 状态读取 ---------- */

/* 返回：0=通知开启(弹窗) 1=通知关闭 2=各 profile 不一致 -1=读取失败 */
static int ReadNotifyState(void)
{
    static const wchar_t *const keys[3] = {
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess"
          L"\\Parameters\\FirewallPolicy\\DomainProfile",
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess"
          L"\\Parameters\\FirewallPolicy\\StandardProfile",
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess"
          L"\\Parameters\\FirewallPolicy\\PublicProfile",
    };
    int enabled = 0;

    for (int i = 0; i < 3; i++) {
        DWORD v = 0, cb = sizeof(v);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keys[i], L"DisableNotifications",
                         RRF_RT_REG_DWORD, NULL, &v, &cb) != ERROR_SUCCESS)
            return -1;
        if (!v)
            enabled++;
    }
    return enabled == 3 ? 0 : (enabled == 0 ? 1 : 2);
}

/* ---------- netsh 执行 ---------- */

/* 同步执行 netsh，输出（OEM 代码页）转宽字符；成功返回 TRUE，退出码写 *exitCode */
static BOOL RunNetsh(const wchar_t *args, wchar_t *out, int outCch, DWORD *exitCode)
{
    wchar_t sysdir[MAX_PATH], cl[512];
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    char acc[8192], buf[2048];
    DWORD n, ec = 0;
    size_t used = 0;
    BOOL ok;

    GetSystemDirectoryW(sysdir, MAX_PATH);
    _snwprintf(cl, 511, L"\"%s\\netsh.exe\" %s", sysdir, args);
    cl[511] = L'\0';

    if (!CreatePipe(&rd, &wr, &sa, 0))
        return FALSE;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;

    ok = CreateProcessW(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return FALSE;
    }

    while (used < sizeof(acc) - 1 &&
           ReadFile(rd, buf, sizeof(buf), &n, NULL) && n) {
        size_t take = (size_t)n;
        if (used + take > sizeof(acc) - 1)
            take = sizeof(acc) - 1 - used;
        memcpy(acc + used, buf, take);
        used += take;
    }
    acc[used] = '\0';
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exitCode)
        *exitCode = ec;
    if (out && outCch > 0) {
        if (MultiByteToWideChar(CP_OEMCP, 0, acc, -1, out, outCch) <= 0)
            out[0] = L'\0';
        out[outCch - 1] = L'\0';
    }
    return TRUE;
}

/* ---------- UI ---------- */

static void RefreshUI(void)
{
    int st = ReadNotifyState();
    const wchar_t *txt, *btn;

    switch (st) {
    case 0:
        txt = L"入站通知：开启（程序监听端口时会弹窗询问）";
        btn = L"关闭入站通知";
        break;
    case 1:
        txt = L"入站通知：关闭（程序监听端口时不弹窗）";
        btn = L"开启入站通知";
        break;
    case 2:
        txt = L"入站通知：域 / 专用 / 公用配置不一致";
        btn = L"统一关闭入站通知";
        break;
    default:
        txt = L"状态读取失败（无法访问防火墙策略注册表）";
        btn = L"不可用";
        break;
    }
    SetWindowTextW(g_status, txt);
    SetWindowTextW(g_button, btn);
    EnableWindow(g_button, st != -1);
}

static void DoToggle(void)
{
    int st = ReadNotifyState();
    wchar_t out[4096];
    DWORD ec = 0;

    if (st == 0 || st == 1 || st == 2) {
        /* 开启 → 关闭；关闭或各 profile 不一致 → 关闭（统一） */
        const wchar_t *args =
            (st == 1)
                ? L"advfirewall set allprofiles settings inboundusernotification enable"
                : L"advfirewall set allprofiles settings inboundusernotification disable";
        if (!RunNetsh(args, out, 4096, &ec)) {
            wchar_t msg[512], sys[256] = L"";
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, GetLastError(), 0, sys, 256, NULL);
            _snwprintf(msg, 511, L"启动 netsh 失败：%s",
                       sys[0] ? sys : L"未知错误");
            msg[511] = L'\0';
            MessageBoxW(NULL, msg, APP_TITLE, MB_ICONERROR | MB_SETFOREGROUND);
        } else if (ec != 0) {
            wchar_t msg[4200];
            _snwprintf(msg, 4199, L"切换失败（netsh 退出码 %lu）。\n\n%s",
                       ec, out[0] ? out : L"（netsh 未返回错误信息）");
            msg[4199] = L'\0';
            MessageBoxW(NULL, msg, APP_TITLE, MB_ICONERROR | MB_SETFOREGROUND);
        }
    }
    RefreshUI();
}

static BOOL IsElevated(void)
{
    HANDLE tok;
    TOKEN_ELEVATION te;
    DWORD len = 0;
    BOOL el = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return FALSE;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &len))
        el = te.TokenIsElevated;
    CloseHandle(tok);
    return el;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        g_status = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            20, 40, CLIENT_CX - 40, 30,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS, cs->hInstance, NULL);
        g_button = CreateWindowExW(0, L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE,
            (CLIENT_CX - 280) / 2, 100, 280, 76,
            hwnd, (HMENU)(INT_PTR)IDC_BUTTON, cs->hInstance, NULL);

        g_fontStatus = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fontButton = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fontStatus, TRUE);
        SendMessageW(g_button, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
        RefreshUI();
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BUTTON && HIWORD(wp) == BN_CLICKED)
            DoToggle();
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show)
{
    WNDCLASSW wc;
    RECT rc;
    MSG msg;
    HWND hwnd;

    (void)hPrev; (void)cmd; (void)show;

    if (!IsElevated()) {
        MessageBoxW(NULL,
            L"InboundFlip 需要管理员权限。\n请右键选择“以管理员身份运行”。",
            APP_TITLE, MB_ICONERROR);
        return 1;
    }

    CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        hwnd = FindWindowW(WC_MAIN, NULL);
        if (hwnd) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WC_MAIN;
    RegisterClassW(&wc);

    rc.left = 0; rc.top = 0; rc.right = CLIENT_CX; rc.bottom = CLIENT_CY;
    AdjustWindowRectEx(&rc, WIN_STYLE, FALSE, 0);

    hwnd = CreateWindowExW(0, WC_MAIN, APP_TITLE, WIN_STYLE,
        (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 2,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
