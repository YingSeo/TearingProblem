#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <dwmapi.h>
#include <intrin.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

using namespace geode::prelude;

namespace AutoNoTear {
    bool shouldPresentFrame = true;
    bool sceneHiddenByMod = false;
    bool savedSceneVisibility = true;

    #ifdef GEODE_IS_WINDOWS
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER nextPresentQpc{};
    int64_t refreshPeriodTicks = 0;

    HWND cachedHwnd = nullptr;
    bool borderlessApplied = false;

    using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int interval);
    using PFNTIMEBEGINPERIOD = UINT(WINAPI*)(UINT uPeriod);

    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = nullptr;
    bool wglInitialized = false;

    void initHardwareTimer() {
        if (qpcFreq.QuadPart != 0) return;

        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&nextPresentQpc);

        if (HMODULE hWinmm = LoadLibraryA("winmm.dll")) {
            if (auto pTimeBeginPeriod = reinterpret_cast<PFNTIMEBEGINPERIOD>(
                    GetProcAddress(hWinmm, "timeBeginPeriod"))) {
                pTimeBeginPeriod(1);
            }
        }

        DEVMODEA dm{};
        dm.dmSize = sizeof(DEVMODEA);
        double hz = 60.0;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30) {
            hz = static_cast<double>(dm.dmDisplayFrequency);
        }
        refreshPeriodTicks = static_cast<int64_t>(static_cast<double>(qpcFreq.QuadPart) / hz);
    }

    void applyBorderlessFlip(HWND hwnd) {
        if (!hwnd || borderlessApplied) return;
        borderlessApplied = true;

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoA(hMon, &mi)) {
            RECT winRect{};
            GetWindowRect(hwnd, &winRect);
            int monW = mi.rcMonitor.right - mi.rcMonitor.left;
            int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            int winW = winRect.right - winRect.left;
            int winH = winRect.bottom - winRect.top;

            if (winW >= monW - 16 && winH >= monH - 16) {
                LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
                style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
                style |= WS_POPUP | WS_VISIBLE;
                SetWindowLongPtrA(hwnd, GWL_STYLE, style);

                SetWindowPos(
                    hwnd, HWND_TOP,
                    mi.rcMonitor.left, mi.rcMonitor.top,
                    monW, monH,
                    SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
                );
            }
        }
    }

    void alignSwapToVBlank() {
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(DWM_TIMING_INFO);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || timing.qpcRefreshPeriod == 0) {
            return;
        }

        refreshPeriodTicks = static_cast<int64_t>(timing.qpcRefreshPeriod);

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        int64_t period = refreshPeriodTicks;
        int64_t offsetTicks = -(400LL * qpcFreq.QuadPart) / 1000000LL; // -400us truoc VBlank
        int64_t targetVBlank = static_cast<int64_t>(timing.qpcVBlank) + offsetTicks;

        if (now.QuadPart > targetVBlank) {
            int64_t passed = (now.QuadPart - targetVBlank) / period + 1;
            targetVBlank += passed * period;
        }

        int64_t waitTicks = targetVBlank - now.QuadPart;
        double waitMs = (static_cast<double>(waitTicks) * 1000.0) / static_cast<double>(qpcFreq.QuadPart);

        if (waitMs > 0.0 && waitMs < 2.2) {
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= targetVBlank) break;
                _mm_pause();
            }
        }
    }
    #endif

    void restoreScene(cocos2d::CCDirector* director) {
        if (sceneHiddenByMod && director) {
            if (auto* scene = director->getRunningScene()) {
                scene->setVisible(savedSceneVisibility);
            }
            sceneHiddenByMod = false;
        }
    }
}

class $modify(AutoNoTearDirector, cocos2d::CCDirector) {
    void drawScene() {
        #ifdef GEODE_IS_WINDOWS
        AutoNoTear::initHardwareTimer();

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        if (now.QuadPart >= AutoNoTear::nextPresentQpc.QuadPart) {
            AutoNoTear::shouldPresentFrame = true;
            AutoNoTear::nextPresentQpc.QuadPart += AutoNoTear::refreshPeriodTicks;
            if (now.QuadPart >= AutoNoTear::nextPresentQpc.QuadPart) {
                AutoNoTear::nextPresentQpc.QuadPart = now.QuadPart + AutoNoTear::refreshPeriodTicks;
            }
        } else {
            AutoNoTear::shouldPresentFrame = false;
        }
        #endif

        CCDirector::drawScene();
        AutoNoTear::restoreScene(this);
    }
};

class $modify(AutoNoTearScheduler, cocos2d::CCScheduler) {
    void update(float dt) {
        // Luon cap nhat Physics va Input o toc do Unlock FPS (240/360Hz)
        CCScheduler::update(dt);

        // Tam an Scene o cac frame trung gian de khong ve thua gay xe hinh
        if (!AutoNoTear::shouldPresentFrame) {
            if (auto* dir = cocos2d::CCDirector::sharedDirector()) {
                if (auto* scene = dir->getRunningScene()) {
                    AutoNoTear::savedSceneVisibility = scene->isVisible();
                    scene->setVisible(false);
                    AutoNoTear::sceneHiddenByMod = true;
                }
            }
        }
    }
};

class $modify(AutoNoTearEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        AutoNoTear::restoreScene(cocos2d::CCDirector::sharedDirector());

        if (!AutoNoTear::shouldPresentFrame) {
            return;
        }

        #ifdef GEODE_IS_WINDOWS
        if (!AutoNoTear::wglInitialized) {
            AutoNoTear::wglInitialized = true;
            AutoNoTear::wglSwapIntervalEXT = reinterpret_cast<AutoNoTear::PFNWGLSWAPINTERVALEXTPROC>(
                wglGetProcAddress("wglSwapIntervalEXT")
            );
            if (HDC hdc = wglGetCurrentDC()) {
                AutoNoTear::cachedHwnd = WindowFromDC(hdc);
            }
        }

        if (AutoNoTear::cachedHwnd && !AutoNoTear::borderlessApplied) {
            AutoNoTear::applyBorderlessFlip(AutoNoTear::cachedHwnd);
        }

        if (AutoNoTear::wglSwapIntervalEXT) {
            AutoNoTear::wglSwapIntervalEXT(0);
        }

        AutoNoTear::alignSwapToVBlank();
        #endif

        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    #ifdef GEODE_IS_WINDOWS
    AutoNoTear::initHardwareTimer();
    #endif
}
