// exit_guard.cpp — 见 exit_guard.h 的说明。
// ============================================================================
// 本文件只做"检测游戏要退出了"这件事，且必须满足手动映射的硬约束：
//   · 不用 STL / 异常 / RTTI；线程用 CreateThread；状态用 volatile LONG + Interlocked
//   · 不碰游戏的对象状态（除了 SetWindowLongPtr 换窗口过程这一处写操作）
// ============================================================================
#include "exit_guard.h"
#include "ui/ui_api.h"

#include <windows.h>

// main.cpp 的日志内核（双写 文件 + 面板环形缓冲）
extern void LogRaw(const char *fmt, ...);
extern void Mod_OnGameThread();
extern void Mod_OnMainPump();

namespace
{

ModExitCleanupFn g_handler = nullptr;

volatile LONG g_installed  = 0;   // 1 = 窗口过程已替换
volatile LONG g_fired      = 0;   // 1 = 清理已触发（一次性）
volatile LONG g_watchStop  = 0;   // 1 = 让看门狗退出

HWND    g_hwnd      = nullptr;
WNDPROC g_origProc  = nullptr;    // 游戏自己的窗口过程（我们要转发给它）
HANDLE  g_watch     = nullptr;
DWORD   g_watchTid  = 0;

// 动态加载 imm32.dll 的 ImmAssociateContextEx（零静态导入表污染），
// 解决 Unity 主动禁用主窗口 IME 导致中文输入法弹不出来的问题。
typedef BOOL (WINAPI *ImmAssociateContextExFn)(HWND, HANDLE, DWORD);
static ImmAssociateContextExFn g_pImmAssocEx = nullptr;
static bool g_immResolved = false;
static bool g_imeEnabledByUs = false;

static void EnsureImeForTextInput(HWND hwnd, bool wantText)
{
    if (!hwnd || wantText == g_imeEnabledByUs) return;
    if (!g_immResolved)
    {
        g_immResolved = true;
        HMODULE hImm = GetModuleHandleW(L"imm32.dll");
        if (!hImm) hImm = LoadLibraryW(L"imm32.dll");
        if (hImm)
            g_pImmAssocEx = (ImmAssociateContextExFn)GetProcAddress(hImm, "ImmAssociateContextEx");
    }
    if (g_pImmAssocEx)
    {
        // 0x0010 = IACE_DEFAULT（启用默认中文输入法上下文）
        if (wantText)
            g_pImmAssocEx(hwnd, nullptr, 0x0010);
    }
    g_imeEnabledByUs = wantText;
}

// 清理线程体：跑真正的清理（内部会有若干秒等待，所以绝不能放在游戏消息线程上）
DWORD WINAPI CleanupThread(LPVOID)
{
    LogRaw("[exit] 清理线程启动 tid=%lu", (unsigned long)GetCurrentThreadId());
    if (g_handler)
        g_handler();
    LogRaw("[exit] 清理线程结束");
    return 0;
}

// 触发清理：**只做一次**。真正的清理放独立线程 ——
// 绝不能占用游戏消息线程（那是游戏退出的关键路径，占用它 = 换一种方式卡死）。
void Trigger(const char *why)
{
    if (InterlockedExchange(&g_fired, 1) == 1)
        return;                                  // 已经触发过，直接返回
    LogRaw("[exit] 检测到游戏退出信号（%s），启动清理线程", why ? why : "?");

    if (!g_handler)
    {
        LogRaw("[exit] WARN: 未注册清理回调，无法自动清理");
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, CleanupThread, nullptr, 0, nullptr);
    if (!th)
        LogRaw("[exit] WARN: 清理线程创建失败 err=%lu", (unsigned long)GetLastError());
    // 句柄不关：让它跑完自然回收（进程马上就要没了，不做 CloseHandle 也无害）
    (void)th;
}

// 替换后的窗口过程：
//   1) 捕获退出信号与 Mod 主线程任务消息 (WM_APP + 0x4D4F)
//   2) 当 ImGui 面板可见且悬停/弹出时拦截鼠标消息与 RawInput 鼠标包，彻底杜绝鼠标穿透到游戏
//   3) 当 ImGui 输入框获取焦点时启用 IME 并将 WM_CHAR (含中文) 推送给 ImGui，同时阻止按键穿透到游戏
LRESULT CALLBACK ExitWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:            Trigger("WM_CLOSE");            break;  // 点 X / Alt+F4
    case WM_DESTROY:          Trigger("WM_DESTROY");          break;
    case WM_QUERYENDSESSION:  Trigger("WM_QUERYENDSESSION");  break;  // 注销/关机
    case WM_ENDSESSION:       Trigger("WM_ENDSESSION");       break;
    case WM_NCDESTROY:        Trigger("WM_NCDESTROY");        break;  // 窗口彻底销毁（最后一道）
    case WM_APP + 0x4D4F:
        if (!g_fired)
        {
            EnsureImeForTextInput(hwnd, UiWantTextInput());
            Mod_OnGameThread();
            Mod_OnMainPump();
        }
        return 0;
    default: break;
    }

    if (!g_fired && UiPanelVisible())
    {
        const bool wantText = UiWantTextInput();
        EnsureImeForTextInput(hwnd, wantText);

        // ---- 1) 拦截穿透到游戏的鼠标点击 / 滚轮 / RawInput ----
        if (UiWantCaptureMouse())
        {
            switch (msg)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return 0;
            case WM_INPUT:
            {
                RAWINPUTHEADER hdr = {};
                UINT sz = sizeof(hdr);
                if (GetRawInputData((HRAWINPUT)lp, RID_HEADER, &hdr, &sz, sizeof(hdr)) != (UINT)-1)
                {
                    if (hdr.dwType == RIM_TYPEMOUSE)
                        return 0;
                }
                break;
            }
            default:
                break;
            }
        }

        // ---- 2) 原生中文 IME 与文本输入路由（输入框聚焦时阻止按键穿透到游戏）----
        if (wantText)
        {
            switch (msg)
            {
            case WM_CHAR:
                if (wp >= 0x20)
                    UiPushInputChar((unsigned short)wp);
                return 0;
            case WM_IME_SETCONTEXT:
            case WM_IME_STARTCOMPOSITION:
            case WM_IME_COMPOSITION:
            case WM_IME_ENDCOMPOSITION:
            case WM_IME_NOTIFY:
                // 绕过 Unity 的 IME 抑制逻辑，直接交给系统 DefWindowProcW 弹出候选框并产生 WM_CHAR
                return DefWindowProcW(hwnd, msg, wp, lp);
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (wp != VK_INSERT)
                    return 0;
                break;
            case WM_INPUT:
            {
                RAWINPUTHEADER hdr = {};
                UINT sz = sizeof(hdr);
                if (GetRawInputData((HRAWINPUT)lp, RID_HEADER, &hdr, &sz, sizeof(hdr)) != (UINT)-1)
                {
                    if (hdr.dwType == RIM_TYPEKEYBOARD)
                        return 0;
                }
                break;
            }
            default:
                break;
            }
        }
    }

    if (g_origProc)
        return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 看门狗：兜住"窗口被以别的方式销毁"（比如游戏自己 DestroyWindow 而不发 WM_CLOSE）。
// 只读 IsWindow，对游戏零侵入。
DWORD WINAPI WatchdogThread(LPVOID)
{
    g_watchTid = GetCurrentThreadId();
    LogRaw("[exit] 看门狗启动（tid=%lu，监视 hwnd=%p）", (unsigned long)g_watchTid, (void *)g_hwnd);

    for (;;)
    {
        if (g_watchStop) break;
        HWND h = g_hwnd;
        if (h && !IsWindow(h))
        {
            Trigger("窗口已销毁(IsWindow=false)");
            break;
        }
        Sleep(100);
    }
    LogRaw("[exit] 看门狗退出");
    g_watchTid = 0;
    return 0;
}

} // namespace

void ExitGuard_SetHandler(ModExitCleanupFn fn) { g_handler = fn; }

void ExitGuard_AttachToWindow(HWND hwnd)
{
    if (!hwnd)
    {
        // 窗口句柄拿不到（BindToChain 里 GetDesc/GetForegroundWindow 都失败）。
        // 不打日志的话，表现是"点 X 还是卡死但看不出为什么"，所以这里必须喊一声。
        LogRaw("[exit] WARN: hwnd 为空 —— 退出守护未安装（点 X 仍可能卡死）");
        return;
    }
    if (InterlockedExchange(&g_installed, 1) == 1) return;   // 幂等

    g_hwnd = hwnd;

    // ① 子类化：保存原过程，换成我们的。
    //    用 SetWindowLongPtrW，取返回值前先清 GetLastError，因为"原过程恰好是 0"
    //    与"调用失败"都返回 0，只能靠 GetLastError 区分。
    SetLastError(0);
    LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&ExitWndProc);
    if (!prev && GetLastError() != 0)
    {
        LogRaw("[exit] WARN: 子类化窗口过程失败 err=%lu（退化为只看门狗）",
               (unsigned long)GetLastError());
        g_origProc = nullptr;
    }
    else
    {
        g_origProc = (WNDPROC)prev;
        UiSetWndProcAttached(true);
        LogRaw("[exit] 已子类化游戏窗口过程 %p -> %p（已启用退出守护 + 防鼠标穿透 + 原生中文IME）",
               (void *)prev, (void *)&ExitWndProc);
    }

    // ② 看门狗
    g_watchStop = 0;
    g_watch = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (!g_watch)
        LogRaw("[exit] WARN: 看门狗线程创建失败 err=%lu", (unsigned long)GetLastError());
}

void ExitGuard_Detach(void)
{
    if (InterlockedExchange(&g_installed, 0) == 0 && !g_watch)
        return;

    g_watchStop = 1;
    if (g_watch)
    {
        // 不在看门狗线程自等（正常调用点是 ModUnload，不会撞上；保险起见判一下）
        if (g_watchTid != GetCurrentThreadId())
            WaitForSingleObject(g_watch, 500);
        CloseHandle(g_watch);
        g_watch = nullptr;
    }

    if (g_hwnd && g_origProc && IsWindow(g_hwnd))
    {
        SetLastError(0);
        LONG_PTR prev = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
        if (prev || GetLastError() == 0)
        {
            UiSetWndProcAttached(false);
            LogRaw("[exit] 已还原游戏窗口过程");
            g_origProc = nullptr;
        }
        else
        {
            // 还原失败：**绝不能**把 g_origProc 置空 —— 窗口过程还是我们的，
            // 一旦置空，之后任何消息都会退化成 DefWindowProc，游戏窗口逻辑直接坏掉。
            LogRaw("[exit] WARN: 还原游戏窗口过程失败 err=%lu（继续用原过程转发）",
                   (unsigned long)GetLastError());
        }
    }
    else
    {
        g_origProc = nullptr;
    }
    g_hwnd = nullptr;
}

int ExitGuard_Fired(void) { return g_fired ? 1 : 0; }
