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

namespace NoTearState {
    bool enabled = true;
    bool drawDivide = true;
    int64_t renderFps = 60;
    bool scanlineSync = true;
    int64_t scanlineOffsetUs = -500;
    bool trueBorderless = true;

    bool shouldRenderFrame = true;
    cocos2d::CCScene* savedScene = nullptr;

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER lastRenderQpc{};
    uint32_t configTickCounter = 0;

    HWND cachedHwnd = nullptr;
    bool borderlessApplied = false;

    using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int interval);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = nullptr;
    bool wglInitialized = false;

    void initTimers() {
        if (qpcFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&qpcFreq);
            QueryPerformanceCounter(&lastRenderQpc);
        }
    }

    void refreshSettings() {
        auto* mod = Mod::get();
        enabled = mod->getSettingValue<bool>("enable-mod");
        drawDivide = mod->getSettingValue<bool>("draw-divide");
        renderFps = std::max<int64_t>(30, mod->getSettingValue<int64_t>("render-fps"));
        scanlineSync = mod->getSettingValue<bool>("scanline-sync");
        scanlineOffsetUs = mod->getSettingValue<int64_t>("scanline-offset-us");
        trueBorderless = mod->getSettingValue<bool>("true-borderless");
    }

    #ifdef GEODE_IS_WINDOWS
    void applyBorderlessWindow(HWND hwnd) {
        if (!hwnd || borderlessApplied || !trueBorderless) return;

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (GetMonitorInfoA(hMon, &mi)) {
            LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            style |= WS_POPUP | WS_VISIBLE;
            SetWindowLongPtrA(hwnd, GWL_STYLE, style);

            int width = mi.rcMonitor.right - mi.rcMonitor.left;
            int height = mi.rcMonitor.bottom - mi.rcMonitor.top;

            SetWindowPos(
                hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                width, height,
                SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
            );
            borderlessApplied = true;
        }
    }

    void waitForScanlinePhase() {
        if (!scanlineSync || qpcFreq.QuadPart == 0) return;

        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(DWM_TIMING_INFO);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || timing.qpcRefreshPeriod == 0) {
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);

        int64_t period = static_cast<int64_t>(timing.qpcRefreshPeriod);
        int64_t vblank = static_cast<int64_t>(timing.qpcVBlank);
        int64_t offsetTicks = (scanlineOffsetUs * qpcFreq.QuadPart) / 1000000LL;

        int64_t targetVBlank = vblank + offsetTicks;
        if (now.QuadPart > targetVBlank) {
            int64_t elapsedPeriods = (now.QuadPart - targetVBlank) / period + 1;
            targetVBlank += elapsedPeriods * period;
        }

        int64_t remainingTicks = targetVBlank - now.QuadPart;
        double remainingMs = (static_cast<double>(remainingTicks) * 1000.0) / static_cast<double>(qpcFreq.QuadPart);

        if (remainingMs > 0.0 && remainingMs < 4.5) {
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= targetVBlank) break;
                _mm_pause();
            }
        }
    }
    #endif
}

class $modify(NoTearDirector, cocos2d::CCDirector) {
    void suppressSceneOnTick() {
        if (this->m_pNextScene != nullptr) {
            NoTearState::shouldRenderFrame = true;
        } else if (!NoTearState::shouldRenderFrame && NoTearState::savedScene == nullptr) {
            NoTearState::savedScene = this->m_pRunningScene;
            this->m_pRunningScene = nullptr;
        }
    }

    void restoreSceneOnTick() {
        if (NoTearState::savedScene != nullptr) {
            this->m_pRunningScene = NoTearState::savedScene;
            NoTearState::savedScene = nullptr;
        }
    }

    void drawScene() {
        NoTearState::initTimers();

        if (++NoTearState::configTickCounter >= 60) {
            NoTearState::configTickCounter = 0;
            NoTearState::refreshSettings();
        }

        NoTearState::shouldRenderFrame = true;

        if (NoTearState::enabled && NoTearState::drawDivide && this->m_pNextScene == nullptr) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);

            double elapsedSec = static_cast<double>(now.QuadPart - NoTearState::lastRenderQpc.QuadPart) /
                                static_cast<double>(NoTearState::qpcFreq.QuadPart);
            double targetInterval = 1.0 / static_cast<double>(NoTearState::renderFps);

            if (elapsedSec >= targetInterval) {
                if (elapsedSec > targetInterval * 2.0) {
                    NoTearState::lastRenderQpc = now;
                } else {
                    NoTearState::lastRenderQpc.QuadPart += static_cast<LONGLONG>(
                        targetInterval * static_cast<double>(NoTearState::qpcFreq.QuadPart)
                    );
                }
                NoTearState::shouldRenderFrame = true;
            } else {
                NoTearState::shouldRenderFrame = false;
            }
        }

        CCDirector::drawScene();
        this->restoreSceneOnTick();
    }
};

class $modify(NoTearScheduler, cocos2d::CCScheduler) {
    void update(float dt) {
        CCScheduler::update(dt);

        if (NoTearState::enabled && NoTearState::drawDivide && !NoTearState::shouldRenderFrame) {
            if (auto* dir = static_cast<NoTearDirector*>(cocos2d::CCDirector::sharedDirector())) {
                dir->suppressSceneOnTick();
            }
        }
    }
};

class $modify(NoTearEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        if (auto* dir = static_cast<NoTearDirector*>(cocos2d::CCDirector::sharedDirector())) {
            dir->restoreSceneOnTick();
        }

        if (NoTearState::enabled && NoTearState::drawDivide && !NoTearState::shouldRenderFrame) {
            return;
        }

        #ifdef GEODE_IS_WINDOWS
        if (NoTearState::enabled) {
            if (!NoTearState::wglInitialized) {
                NoTearState::wglInitialized = true;
                NoTearState::wglSwapIntervalEXT = reinterpret_cast<NoTearState::PFNWGLSWAPINTERVALEXTPROC>(
                    wglGetProcAddress("wglSwapIntervalEXT")
                );
                HDC hdc = wglGetCurrentDC();
                if (hdc) {
                    NoTearState::cachedHwnd = WindowFromDC(hdc);
                }
            }

            if (NoTearState::cachedHwnd && !NoTearState::borderlessApplied && NoTearState::trueBorderless) {
                NoTearState::applyBorderlessWindow(NoTearState::cachedHwnd);
            }

            if (NoTearState::wglSwapIntervalEXT) {
                NoTearState::wglSwapIntervalEXT(0);
            }

            NoTearState::waitForScanlinePhase();
        }
        #endif

        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    NoTearState::initTimers();
    NoTearState::refreshSettings();
}
