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

namespace NoTearEngine {
    bool enabled = true;
    bool drawDivide = true;
    bool autoRefreshRate = true;
    int64_t manualRenderFps = 60;
    int64_t syncMode = 1; // 1: Phase-Lock, 2: Adaptive SwapInterval, 3: DwmFlush
    int64_t scanlineOffsetUs = -400;
    bool trueBorderless = true;
    bool gameplayOnly = false;

    bool shouldPresentFrame = true;
    bool sceneHiddenByMod = false;
    bool savedSceneVisibility = true;

    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER nextPresentQpc{};
    double detectedRefreshHz = 60.0;
    uint32_t tickCounter = 0;

    #ifdef GEODE_IS_WINDOWS
    HWND cachedHwnd = nullptr;
    bool borderlessApplied = false;

    using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int interval);
    using PFNWGLGETSWAPINTERVALEXTPROC = int(WINAPI*)(void);
    using PFNTIMEBEGINPERIOD = UINT(WINAPI*)(UINT uPeriod);

    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT = nullptr;
    bool wglInitialized = false;
    int lastSwapInterval = -999;

    void enableHighResTimer() {
        HMODULE hWinmm = GetModuleHandleA("winmm.dll");
        if (!hWinmm) {
            hWinmm = LoadLibraryA("winmm.dll");
        }
        if (hWinmm) {
            auto pTimeBeginPeriod = reinterpret_cast<PFNTIMEBEGINPERIOD>(
                GetProcAddress(hWinmm, "timeBeginPeriod")
            );
            if (pTimeBeginPeriod) {
                pTimeBeginPeriod(1);
            }
        }
    }

    double queryMonitorRefreshRate() {
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(DWM_TIMING_INFO);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing))) {
            if (timing.rateRefresh.uiDenominator > 0 && timing.rateRefresh.uiNumerator > 0) {
                double hz = static_cast<double>(timing.rateRefresh.uiNumerator) /
                            static_cast<double>(timing.rateRefresh.uiDenominator);
                if (hz >= 30.0 && hz <= 500.0) {
                    return hz;
                }
            }
        }

        DEVMODEA dm{};
        dm.dmSize = sizeof(DEVMODEA);
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            if (dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500) {
                return static_cast<double>(dm.dmDisplayFrequency);
            }
        }
        return 60.0;
    }
    #endif

    void initEngine() {
        #ifdef GEODE_IS_WINDOWS
        if (qpcFreq.QuadPart == 0) {
            QueryPerformanceFrequency(&qpcFreq);
            QueryPerformanceCounter(&nextPresentQpc);
            enableHighResTimer();
            detectedRefreshHz = queryMonitorRefreshRate();
        }
        #endif
    }

    void refreshSettings() {
        auto* mod = Mod::get();
        enabled = mod->getSettingValue<bool>("enable-mod");
        drawDivide = mod->getSettingValue<bool>("draw-divide");
        autoRefreshRate = mod->getSettingValue<bool>("auto-refresh-rate");
        int64_t fpsVal = mod->getSettingValue<int64_t>("render-fps");
        manualRenderFps = (fpsVal < 30) ? 30 : fpsVal;
        syncMode = mod->getSettingValue<int64_t>("sync-mode");
        scanlineOffsetUs = mod->getSettingValue<int64_t>("scanline-offset-us");
        trueBorderless = mod->getSettingValue<bool>("true-borderless");
        gameplayOnly = mod->getSettingValue<bool>("gameplay-only");

        #ifdef GEODE_IS_WINDOWS
        if (autoRefreshRate) {
            detectedRefreshHz = queryMonitorRefreshRate();
        }
        #endif
    }

    double getTargetRenderHz() {
        if (autoRefreshRate && detectedRefreshHz >= 30.0) {
            return detectedRefreshHz;
        }
        return static_cast<double>(manualRenderFps);
    }

    void restoreSceneVisibility(cocos2d::CCDirector* director) {
        if (sceneHiddenByMod && director) {
            if (auto* scene = director->getRunningScene()) {
                scene->setVisible(savedSceneVisibility);
            }
            sceneHiddenByMod = false;
        }
    }

    #ifdef GEODE_IS_WINDOWS
    void applyBorderlessWindow(HWND hwnd) {
        if (!hwnd || borderlessApplied || !trueBorderless) return;

        LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
        // Chi ap dung khi cua so dang o che do full man hinh hoac popup
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
            borderlessApplied = true;
        }
    }

    void setDriverSwapInterval(int interval) {
        if (wglSwapIntervalEXT && lastSwapInterval != interval) {
            if (wglSwapIntervalEXT(interval)) {
                lastSwapInterval = interval;
            } else if (interval == -1) {
                // Neu driver khong ho tro Adaptive VSync (-1), chuyen sang VSync (1)
                wglSwapIntervalEXT(1);
                lastSwapInterval = 1;
            }
        }
    }

    void syncWithVBlank() {
        if (qpcFreq.QuadPart == 0) return;

        if (syncMode == 2) {
            // Mode 2: Hardware Tear Control (Ket hop Draw Divide nen khong khoa Physics)
            setDriverSwapInterval(-1);
            return;
        }

        // Mode 1 & Mode 3: Tat driver V-Sync de khong gay input lag
        setDriverSwapInterval(0);

        if (syncMode == 3) {
            DwmFlush();
            return;
        }

        // Mode 1: DWM VBlank Scanline Phase-Lock
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
        double remainingMs = (static_cast<double>(remainingTicks) * 1000.0) /
                             static_cast<double>(qpcFreq.QuadPart);

        // Chi doi khi sat diem VBlank (< 2.2ms) de day duong xe ra ngoai vung hien thi
        // ma khong lam cham vong lap 240Hz/360Hz
        if (remainingMs > 0.0 && remainingMs < 2.2) {
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
        NoTearEngine::initEngine();

        if (++NoTearEngine::tickCounter >= 120) {
            NoTearEngine::tickCounter = 0;
            NoTearEngine::refreshSettings();
        }

        NoTearEngine::shouldPresentFrame = true;

        #ifdef GEODE_IS_WINDOWS
        bool activeContext = !NoTearEngine::gameplayOnly || (PlayLayer::get() != nullptr);

        if (NoTearEngine::enabled && NoTearEngine::drawDivide && activeContext) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);

            double targetHz = NoTearEngine::getTargetRenderHz();
            int64_t stepTicks = static_cast<int64_t>(
                static_cast<double>(NoTearEngine::qpcFreq.QuadPart) / targetHz
            );

            // Khoa pha nextPresentQpc theo VBlank cua DWM neu co the
            DWM_TIMING_INFO timing{};
            timing.cbSize = sizeof(DWM_TIMING_INFO);
            if (NoTearEngine::autoRefreshRate &&
                SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) &&
                timing.qpcRefreshPeriod > 0) {
                stepTicks = static_cast<int64_t>(timing.qpcRefreshPeriod);
            }

            if (now.QuadPart >= NoTearEngine::nextPresentQpc.QuadPart) {
                NoTearEngine::shouldPresentFrame = true;
                NoTearEngine::nextPresentQpc.QuadPart += stepTicks;

                // Neu bi tre qua 1 chu ky (ví dụ khi load cảnh), dat lai moc thoi gian
                if (now.QuadPart >= NoTearEngine::nextPresentQpc.QuadPart) {
                    NoTearEngine::nextPresentQpc.QuadPart = now.QuadPart + stepTicks;
                }
            } else {
                NoTearEngine::shouldPresentFrame = false;
            }
        }
        #endif

        CCDirector::drawScene();

        // Dam bao khoi phuc trang thai hien thi cua Scene sau moi tick
        NoTearEngine::restoreSceneVisibility(this);
    }
};

class $modify(NoTearScheduler, cocos2d::CCScheduler) {
    void update(float dt) {
        // 1. Chay day du Physics, Input va Logic o toc do Unlock FPS (240Hz/360Hz)
        CCScheduler::update(dt);

        // 2. Neu la tick trung gian, tam an Scene de bo qua visit() nang ne cua OpenGL
        if (NoTearEngine::enabled && NoTearEngine::drawDivide && !NoTearEngine::shouldPresentFrame) {
            auto* director = cocos2d::CCDirector::sharedDirector();
            if (director) {
                if (auto* scene = director->getRunningScene()) {
                    NoTearEngine::savedSceneVisibility = scene->isVisible();
                    scene->setVisible(false);
                    NoTearEngine::sceneHiddenByMod = true;
                }
            }
        }
    }
};

class $modify(NoTearEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        auto* director = cocos2d::CCDirector::sharedDirector();
        NoTearEngine::restoreSceneVisibility(director);

        // Bo qua SwapBuffers o cac tick vat ly trung gian -> Xoa bo hoan toan xe hinh do du thua FPS
        if (NoTearEngine::enabled && NoTearEngine::drawDivide && !NoTearEngine::shouldPresentFrame) {
            return;
        }

        #ifdef GEODE_IS_WINDOWS
        if (NoTearEngine::enabled) {
            if (!NoTearEngine::wglInitialized) {
                NoTearEngine::wglInitialized = true;
                NoTearEngine::wglSwapIntervalEXT = reinterpret_cast<NoTearEngine::PFNWGLSWAPINTERVALEXTPROC>(
                    wglGetProcAddress("wglSwapIntervalEXT")
                );
                HDC hdc = wglGetCurrentDC();
                if (hdc) {
                    NoTearEngine::cachedHwnd = WindowFromDC(hdc);
                }
            }

            if (NoTearEngine::cachedHwnd && !NoTearEngine::borderlessApplied && NoTearEngine::trueBorderless) {
                NoTearEngine::applyBorderlessWindow(NoTearEngine::cachedHwnd);
            }

            NoTearEngine::syncWithVBlank();
        }
        #endif

        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    NoTearEngine::initEngine();
    NoTearEngine::refreshSettings();
}
