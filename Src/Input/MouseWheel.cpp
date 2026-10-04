#if defined(MODLOADER_CLIENT_BUILD)

#include "MouseWheel.h"

#include "../plugin.h"
#include "../plugin_helpers.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cstdint>
#include <vector>

#pragma comment(lib, "Comctl32.lib")

namespace
{
    struct WindowCandidate
    {
        HWND Window = nullptr;
        DWORD OwnerThreadId = 0;
        int ClientWidth = 0;
        int ClientHeight = 0;
        bool IsForeground = false;
    };


    IPluginSelf* g_inputSelf = nullptr;

    std::atomic<bool> g_uiReady = false;
    std::atomic<bool> g_acceptInstall = false;
    std::atomic<bool> g_attached = false;
    std::atomic<bool> g_zoomEnabled = false;
    std::atomic<int32_t> g_pendingRawWheelDelta = 0;

    bool g_tickRegistered = false;
    bool g_shutdownRegistered = false;
    bool g_installAttempted = false;

    HWND g_window = nullptr;
    DWORD g_windowThreadId = 0;

    UINT g_detachMessage = 0;

    constexpr UINT_PTR kSubclassId =
        static_cast<UINT_PTR>(
            0x4D4D5753u); // "MMWS"


    LRESULT CALLBACK MiniMapSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);


    void PinMiniMapModule()
    {
        HMODULE module = nullptr;

        if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(
                &MiniMapSubclassProc),
            &module))
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "MiniMap.dll pinned because safe subclass removal "
                "could not be confirmed");
        }
        else
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "failed to pin MiniMap.dll after detach failure "
                "(error=%lu)",
                static_cast<unsigned long>(
                    GetLastError()));
        }
    }


    void MarkDetached()
    {
        g_attached.store(
            false,
            std::memory_order_release);

        g_window = nullptr;
        g_windowThreadId = 0;
    }


    bool RemoveSubclassOnOwnerThread(
        const char* reason)
    {
        if (!g_attached.load(
            std::memory_order_acquire))
        {
            return true;
        }

        HWND hwnd =
            g_window;

        if (hwnd == nullptr ||
            !IsWindow(hwnd))
        {
            LOG_WARN(
                "MiniMap: window subclass: "
                "detach requested (%s), but HWND is no longer valid",
                reason != nullptr
                ? reason
                : "<unknown>");

            return false;
        }

        const DWORD currentThreadId =
            GetCurrentThreadId();

        const DWORD ownerThreadId =
            GetWindowThreadProcessId(
                hwnd,
                nullptr);

        if (currentThreadId !=
            ownerThreadId)
        {
            return false;
        }

        const BOOL removed =
            RemoveWindowSubclass(
                hwnd,
                &MiniMapSubclassProc,
                kSubclassId);

        if (!removed)
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "RemoveWindowSubclass failed (%s, error=%lu)",
                reason != nullptr
                ? reason
                : "<unknown>",
                static_cast<unsigned long>(
                    GetLastError()));

            return false;
        }

        LOG_INFO(
            "MiniMap: window subclass: detached (%s) "
            "hwnd=%p thread=%lu",
            reason != nullptr
            ? reason
            : "<unknown>",
            hwnd,
            static_cast<unsigned long>(
                currentThreadId));

        MarkDetached();

        return true;
    }


    bool DetachSubclass(
        const char* reason)
    {
        if (!g_attached.load(
            std::memory_order_acquire))
        {
            return true;
        }

        HWND hwnd =
            g_window;

        if (hwnd == nullptr ||
            !IsWindow(hwnd))
        {
            LOG_WARN(
                "MiniMap: window subclass: "
                "cannot confirm safe detach (%s): invalid HWND",
                reason != nullptr
                ? reason
                : "<unknown>");

            return false;
        }

        const DWORD currentThreadId =
            GetCurrentThreadId();

        const DWORD ownerThreadId =
            GetWindowThreadProcessId(
                hwnd,
                nullptr);

        if (currentThreadId ==
            ownerThreadId)
        {
            return RemoveSubclassOnOwnerThread(
                reason);
        }

        if (g_detachMessage == 0)
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "cannot marshal detach (%s): "
                "control message is unavailable",
                reason != nullptr
                ? reason
                : "<unknown>");

            return false;
        }

        DWORD_PTR result = 0;

        const LRESULT sent =
            SendMessageTimeoutW(
                hwnd,
                g_detachMessage,
                0,
                0,
                SMTO_ABORTIFHUNG |
                SMTO_BLOCK,
                2000,
                &result);

        if (sent == 0 ||
            result == 0 ||
            g_attached.load(
                std::memory_order_acquire))
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "synchronous detach failed (%s) "
                "caller-thread=%lu owner-thread=%lu "
                "error=%lu",
                reason != nullptr
                ? reason
                : "<unknown>",
                static_cast<unsigned long>(
                    currentThreadId),
                static_cast<unsigned long>(
                    ownerThreadId),
                static_cast<unsigned long>(
                    GetLastError()));

            return false;
        }

        return true;
    }


    BOOL CALLBACK EnumerateCandidateWindow(
        HWND hwnd,
        LPARAM parameter)
    {
        auto* candidates =
            reinterpret_cast<
            std::vector<WindowCandidate>*>(
                parameter);

        if (candidates == nullptr)
        {
            return FALSE;
        }

        DWORD processId = 0;

        const DWORD ownerThreadId =
            GetWindowThreadProcessId(
                hwnd,
                &processId);

        if (processId !=
            GetCurrentProcessId() ||
            ownerThreadId == 0 ||
            !IsWindowVisible(hwnd) ||
            GetWindow(
                hwnd,
                GW_OWNER) != nullptr)
        {
            return TRUE;
        }

        RECT clientRect = {};

        if (!GetClientRect(
            hwnd,
            &clientRect))
        {
            return TRUE;
        }

        const int width =
            clientRect.right -
            clientRect.left;

        const int height =
            clientRect.bottom -
            clientRect.top;

        if (width < 640 ||
            height < 360)
        {
            return TRUE;
        }

        WindowCandidate candidate = {};

        candidate.Window =
            hwnd;

        candidate.OwnerThreadId =
            ownerThreadId;

        candidate.ClientWidth =
            width;

        candidate.ClientHeight =
            height;

        candidate.IsForeground =
            GetForegroundWindow() ==
            hwnd;

        candidates->push_back(
            candidate);

        return TRUE;
    }


    bool FindGameWindow(
        WindowCandidate& outCandidate)
    {
        std::vector<WindowCandidate> candidates;

        if (!EnumWindows(
            &EnumerateCandidateWindow,
            reinterpret_cast<LPARAM>(
                &candidates)))
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "EnumWindows failed (error=%lu)",
                static_cast<unsigned long>(
                    GetLastError()));

            return false;
        }

        LOG_INFO(
            "MiniMap: window subclass: "
            "found %zu candidate top-level window(s)",
            candidates.size());

        for (size_t index = 0;
            index < candidates.size();
            ++index)
        {
            const WindowCandidate& candidate =
                candidates[index];

            const LONG_PTR currentWndProc =
                GetWindowLongPtrW(
                    candidate.Window,
                    GWLP_WNDPROC);

            LOG_INFO(
                "MiniMap: window subclass: "
                "candidate[%zu] hwnd=%p "
                "client=%dx%d owner-thread=%lu "
                "foreground=%s wndproc=%p",
                index,
                candidate.Window,
                candidate.ClientWidth,
                candidate.ClientHeight,
                static_cast<unsigned long>(
                    candidate.OwnerThreadId),
                candidate.IsForeground
                ? "yes"
                : "no",
                reinterpret_cast<void*>(
                    currentWndProc));
        }

        if (candidates.empty())
        {
            return false;
        }

        if (candidates.size() == 1)
        {
            outCandidate =
                candidates[0];

            return true;
        }

        WindowCandidate* foregroundCandidate =
            nullptr;

        for (WindowCandidate& candidate :
            candidates)
        {
            if (!candidate.IsForeground)
            {
                continue;
            }

            if (foregroundCandidate != nullptr)
            {
                LOG_WARN(
                    "MiniMap: window subclass: "
                    "multiple foreground candidates are ambiguous");

                return false;
            }

            foregroundCandidate =
                &candidate;
        }

        if (foregroundCandidate == nullptr)
        {
            LOG_WARN(
                "MiniMap: window subclass: "
                "multiple candidates and none is the "
                "foreground window; deferring attachment");

            return false;
        }

        outCandidate =
            *foregroundCandidate;

        return true;
    }


    bool TryInstallSubclass()
    {
        WindowCandidate candidate = {};

        if (!FindGameWindow(
            candidate))
        {
            LOG_WARN(
                "MiniMap: window subclass: "
                "no unambiguous game HWND was selected");

            return false;
        }

        const DWORD currentThreadId =
            GetCurrentThreadId();

        LOG_INFO(
            "MiniMap: window subclass: "
            "selected hwnd=%p "
            "current-thread=%lu owner-thread=%lu",
            candidate.Window,
            static_cast<unsigned long>(
                currentThreadId),
            static_cast<unsigned long>(
                candidate.OwnerThreadId));

        if (currentThreadId !=
            candidate.OwnerThreadId)
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "installation thread does not own the selected HWND; "
                "not attaching");

            return false;
        }

        const LONG_PTR wndProcBefore =
            GetWindowLongPtrW(
                candidate.Window,
                GWLP_WNDPROC);

        if (!SetWindowSubclass(
            candidate.Window,
            &MiniMapSubclassProc,
            kSubclassId,
            0))
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "SetWindowSubclass failed (error=%lu)",
                static_cast<unsigned long>(
                    GetLastError()));

            return false;
        }

        const LONG_PTR wndProcAfter =
            GetWindowLongPtrW(
                candidate.Window,
                GWLP_WNDPROC);

        g_window =
            candidate.Window;

        g_windowThreadId =
            candidate.OwnerThreadId;

        g_attached.store(
            true,
            std::memory_order_release);

        LOG_INFO(
            "MiniMap: window subclass: attached "
            "hwnd=%p thread=%lu "
            "wndproc-before=%p wndproc-after=%p",
            g_window,
            static_cast<unsigned long>(
                g_windowThreadId),
            reinterpret_cast<void*>(
                wndProcBefore),
            reinterpret_cast<void*>(
                wndProcAfter));

        return true;
    }


    LRESULT CALLBACK MiniMapSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData)
    {
        (void)wParam;
        (void)lParam;
        (void)subclassId;
        (void)referenceData;

        if (g_detachMessage != 0 &&
            message ==
            g_detachMessage)
        {
            const BOOL removed =
                RemoveWindowSubclass(
                    hwnd,
                    &MiniMapSubclassProc,
                    kSubclassId);

            if (!removed)
            {
                LOG_ERROR(
                    "MiniMap: window subclass: "
                    "owner-thread detach message failed "
                    "(error=%lu)",
                    static_cast<unsigned long>(
                        GetLastError()));

                return 0;
            }

            LOG_INFO(
                "MiniMap: window subclass: "
                "detached through owner-thread control message "
                "hwnd=%p thread=%lu",
                hwnd,
                static_cast<unsigned long>(
                    GetCurrentThreadId()));

            MarkDetached();

            return 1;
        }

        if (message ==
            WM_NCDESTROY)
        {
            LOG_INFO(
                "MiniMap: window subclass: "
                "WM_NCDESTROY received; removing subclass");

            RemoveWindowSubclass(
                hwnd,
                &MiniMapSubclassProc,
                kSubclassId);

            MarkDetached();

            return DefSubclassProc(
                hwnd,
                message,
                wParam,
                lParam);
        }

        if (message ==
            WM_MOUSEWHEEL &&
            g_zoomEnabled.load(
                std::memory_order_relaxed))
        {
            const WORD keyState =
                GET_KEYSTATE_WPARAM(
                    wParam);

            const bool controlWasDown =
                (keyState & MK_CONTROL) != 0;

            const bool leftControlDown =
                (GetKeyState(
                    VK_LCONTROL) &
                    0x8000) != 0;

            const int rawDelta =
                GET_WHEEL_DELTA_WPARAM(
                    wParam);

            if (controlWasDown &&
                leftControlDown &&
                rawDelta != 0)
            {
                g_pendingRawWheelDelta.fetch_add(
                    rawDelta,
                    std::memory_order_relaxed);

                return 0;
            }
        }

        return DefSubclassProc(
            hwnd,
            message,
            wParam,
            lParam);
    }


    void OnTick(
        float deltaSeconds)
    {
        (void)deltaSeconds;

        if (!g_acceptInstall.load(
            std::memory_order_acquire) ||
            !g_uiReady.load(
                std::memory_order_acquire) ||
            g_attached.load(
                std::memory_order_acquire) ||
            g_installAttempted)
        {
            return;
        }

        g_installAttempted =
            true;

        LOG_INFO(
            "MiniMap: window subclass: "
            "UI readiness observed; validating lifecycle");

        if (!TryInstallSubclass())
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "diagnostic attachment failed");
        }
    }


    void OnEngineShutdown()
    {
        g_zoomEnabled.store(
            false,
            std::memory_order_release);

        g_pendingRawWheelDelta.store(
            0,
            std::memory_order_relaxed);

        g_acceptInstall.store(
            false,
            std::memory_order_release);

        LOG_INFO(
            "MiniMap: window subclass: "
            "engine shutdown cleanup requested");

        if (!DetachSubclass(
            "engine shutdown"))
        {
            /*
             * Do not leave a callback into an unloadable DLL.
             * Pinning is only the final safety net; the expected path
             * is successful owner-thread removal.
             */
            if (g_attached.load(
                std::memory_order_acquire))
            {
                PinMiniMapModule();
            }
        }
    }
}


namespace MiniMapMouseWheel
{
    bool Initialize(
        IPluginSelf* self)
    {
        if (self == nullptr ||
            self->hooks == nullptr ||
            self->hooks->Engine == nullptr)
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "initialization failed: Engine interface unavailable");

            return false;
        }

        if (g_inputSelf != nullptr)
        {
            return true;
        }

        g_inputSelf =
            self;

        g_uiReady.store(
            false,
            std::memory_order_release);

        g_acceptInstall.store(
            true,
            std::memory_order_release);

        g_attached.store(
            false,
            std::memory_order_release);

        g_zoomEnabled.store(
            false,
            std::memory_order_release);

        g_pendingRawWheelDelta.store(
            0,
            std::memory_order_relaxed);

        g_installAttempted =
            false;

        g_window =
            nullptr;

        g_windowThreadId =
            0;

        g_detachMessage =
            RegisterWindowMessageW(
                L"StarRupture.MiniMap.WindowSubclass.Detach");

        if (g_detachMessage == 0)
        {
            LOG_ERROR(
                "MiniMap: window subclass: "
                "RegisterWindowMessageW failed (error=%lu)",
                static_cast<unsigned long>(
                    GetLastError()));

            g_inputSelf =
                nullptr;

            return false;
        }

        g_inputSelf->
            hooks->
            Engine->
            RegisterOnTick(
                &OnTick);

        g_tickRegistered =
            true;

        g_inputSelf->
            hooks->
            Engine->
            RegisterOnShutdown(
                &OnEngineShutdown);

        g_shutdownRegistered =
            true;

        LOG_INFO(
            "MiniMap: window subclass: "
            "diagnostic initialized; waiting for first widget render");

        return true;
    }


    void Shutdown()
    {
        g_zoomEnabled.store(
            false,
            std::memory_order_release);

        g_pendingRawWheelDelta.store(
            0,
            std::memory_order_relaxed);

        g_acceptInstall.store(
            false,
            std::memory_order_release);

        if (g_attached.load(
            std::memory_order_acquire))
        {
            if (!DetachSubclass(
                "plugin shutdown"))
            {
                if (g_attached.load(
                    std::memory_order_acquire))
                {
                    PinMiniMapModule();
                }
            }
        }

        if (g_inputSelf != nullptr &&
            g_inputSelf->hooks != nullptr &&
            g_inputSelf->hooks->Engine != nullptr)
        {
            if (g_shutdownRegistered)
            {
                g_inputSelf->
                    hooks->
                    Engine->
                    UnregisterOnShutdown(
                        &OnEngineShutdown);
            }

            if (g_tickRegistered)
            {
                g_inputSelf->
                    hooks->
                    Engine->
                    UnregisterOnTick(
                        &OnTick);
            }
        }

        g_shutdownRegistered =
            false;

        g_tickRegistered =
            false;

        g_uiReady.store(
            false,
            std::memory_order_release);

        g_installAttempted =
            false;

        g_detachMessage =
            0;

        g_window =
            nullptr;

        g_windowThreadId =
            0;

        g_inputSelf =
            nullptr;

        LOG_INFO(
            "MiniMap: window subclass: "
            "diagnostic shutdown complete");
    }


    void SetZoomEnabled(
        bool enabled)
    {
        g_zoomEnabled.store(
            enabled,
            std::memory_order_release);

        if (!enabled)
        {
            g_pendingRawWheelDelta.store(
                0,
                std::memory_order_relaxed);
        }
    }


    float DrainZoomDelta()
    {
        const int32_t rawDelta =
            g_pendingRawWheelDelta.exchange(
                0,
                std::memory_order_relaxed);

        return static_cast<float>(
            rawDelta) /
            static_cast<float>(
                WHEEL_DELTA);
    }


    void ClearPendingZoom()
    {
        g_pendingRawWheelDelta.store(
            0,
            std::memory_order_relaxed);
    }


    void SignalUiReady()
    {
        bool expected =
            false;

        if (g_uiReady.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
        {
            LOG_INFO(
                "MiniMap: window subclass: "
                "first widget render observed");
        }
    }
}

#endif
