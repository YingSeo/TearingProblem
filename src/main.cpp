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

namespace NoTearState {
    bool enabled = true;
    bool drawDivide = true;
    int64_t renderFps = 60;
    bool scanlineSync = true;
    int64_t scanlineOffsetUs = -500;
    bool trueBorderless = true;

    bool isIntermediateTick = false;
    bool overrideSchedulerDt = false;
    float currentRealDt = 1.0f / 240.0f;
    double renderAccum = 0.0;

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER lastTickQpc{};
    uint32_t configTickCounter = 0;

    #ifdef GEODE_IS_WINDOWS
    HWND cachedHwnd = nullptr;
    bool borderlessApplied = false;

    using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int interval);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = nullptr;
    bool wglInitialized = false;
    #endif

    void initTimers() {
        #ifdef GEODE_IS_WINDOWS
        if (qpcFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&qpcFreq);
            QueryPerformanceCounter(&lastTickQpc);
        }
        #endif
    }

    void refreshSettings() {
        auto* mod = Mod::get();
        enabled = mod->getSettingValue<bool>("enable-mod");
        drawDivide = mod->getSettingValue<bool>("draw-divide");
        int64_t target = mod->getSettingValue<int64_t>("render-fps");
        renderFps = (target < 30) ? 30 : target;
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
    void drawScene() {
        NoTearState::initTimers();

        if (++NoTearState::configTickCounter >= 60) {
            NoTearState::configTickCounter = 0;
            NoTearState::refreshSettings();
        }

        #ifdef GEODE_IS_WINDOWS
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double realDt = static_cast<double>(now.QuadPart - NoTearState::lastTickQpc.QuadPart) /
                        static_cast<double>(NoTearState::qpcFreq.QuadPart);
        NoTearState::lastTickQpc = now;

        if (realDt <= 0.0 || realDt > 0.1) {
            realDt = 1.0 / 240.0;
        }
        NoTearState::currentRealDt = static_cast<float>(realDt);

        // Chi kich hoat Draw Divide khi dang trong man choi (PlayLayer) de tranh loi chuyen canh
        if (NoTearState::enabled && NoTearState::drawDivide && PlayLayer::get() != nullptr) {
            NoTearState::renderAccum += realDt;
            double targetInterval = 1.0 / static_cast<double>(NoTearState::renderFps);

            if (NoTearState::renderAccum >= targetInterval) {
                NoTearState::renderAccum -= targetInterval;
                if (NoTearState::renderAccum > targetInterval) {
                    NoTearState::renderAccum = 0.0;
                }
                NoTearState::isIntermediateTick = false;
                NoTearState::overrideSchedulerDt = true;
                CCDirector::drawScene();
                NoTearState::overrideSchedulerDt = false;
            } else {
                // Frame trung gian: Chi cap nhat Physics & Input o toc do 240/360Hz, bo qua Render
                NoTearState::isIntermediateTick = true;
                if (!this->isPaused() && this->getScheduler() != nullptr) {
                    this->getScheduler()->update(NoTearState::currentRealDt);
                }
                NoTearState::isIntermediateTick = false;
            }
            return;
        }
        #endif

        NoTearState::isIntermediateTick = false;
        NoTearState::overrideSchedulerDt = false;
        CCDirector::drawScene();
    }
};

class $modify(NoTearScheduler, cocos2d::CCScheduler) {
    void update(float dt) {
        if (NoTearState::overrideSchedulerDt) {
            dt = NoTearState::currentRealDt;
            NoTearState::overrideSchedulerDt = false;
        }
        CCScheduler::update(dt);
    }
};

class $modify(NoTearEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        if (NoTearState::enabled && NoTearState::drawDivide && NoTearState::isIntermediateTick) {
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
