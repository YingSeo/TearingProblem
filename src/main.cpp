#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>

using namespace geode::prelude;

namespace AutoNoTear {
    bool shouldPresentFrame = true;
    bool sceneHiddenByMod = false;
    bool savedSceneVisibility = true;

    #ifdef GEODE_IS_WINDOWS
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER nextPresentQpc{};
    int64_t refreshPeriodTicks = 0;
    bool initialized = false;

    using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI*)(int);
    using PFNWGLGETPROCADDRESS = PROC(WINAPI*)(LPCSTR);
    using PFNTIMEBEGINPERIOD = UINT(WINAPI*)(UINT);
    using PFNDWMFLUSH = HRESULT(WINAPI*)(void);

    PFNWGLSWAPINTERVALEXTPROC pSwapInterval = nullptr;
    PFNDWMFLUSH pDwmFlush = nullptr;
    bool glHooked = false;

    void initEngine() {
        if (initialized) return;
        initialized = true;

        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&nextPresentQpc);

        // Bat bo dinh thoi 1ms cua Windows de on dinh xung nhip 240Hz/360Hz
        if (HMODULE hWinmm = LoadLibraryA("winmm.dll")) {
            auto pTimeBegin = reinterpret_cast<PFNTIMEBEGINPERIOD>(
                GetProcAddress(hWinmm, "timeBeginPeriod")
            );
            if (pTimeBegin) pTimeBegin(1);
        }

        // Nap DwmFlush tu dwmapi.dll ma khong can #include <dwmapi.h>
        if (HMODULE hDwm = LoadLibraryA("dwmapi.dll")) {
            pDwmFlush = reinterpret_cast<PFNDWMFLUSH>(
                GetProcAddress(hDwm, "DwmFlush")
            );
        }

        // Tu dong doc tan so quet man hinh (60Hz / 75Hz / 120Hz / 144Hz)
        DEVMODEA dm{};
        dm.dmSize = sizeof(DEVMODEA);
        double hz = 60.0;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30) {
            hz = static_cast<double>(dm.dmDisplayFrequency);
        }
        refreshPeriodTicks = static_cast<int64_t>(static_cast<double>(qpcFreq.QuadPart) / hz);
    }

    void syncVBlankPhase() {
        if (!glHooked) {
            glHooked = true;
            if (HMODULE hGl = GetModuleHandleA("opengl32.dll")) {
                auto pGetProc = reinterpret_cast<PFNWGLGETPROCADDRESS>(
                    GetProcAddress(hGl, "wglGetProcAddress")
                );
                if (pGetProc) {
                    pSwapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
                        pGetProc("wglSwapIntervalEXT")
                    );
                }
            }
        }

        // Tat V-Sync truyen thong de khong khoa FPS vat ly
        if (pSwapInterval) {
            pSwapInterval(0);
        }

        // Can chinh diem lat khung hinh sat moc VBlank de xoa vet xe ngang
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        int64_t remainingTicks = nextPresentQpc.QuadPart - now.QuadPart;
        double remainingMs = (static_cast<double>(remainingTicks) * 1000.0) /
                             static_cast<double>(qpcFreq.QuadPart);

        if (remainingMs > 0.0 && remainingMs < 2.0) {
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= nextPresentQpc.QuadPart) break;
                YieldProcessor();
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
        AutoNoTear::initEngine();

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
        // Giu nguyen Physics & Input o toc do Unlock FPS (240Hz/360Hz)
        CCScheduler::update(dt);

        // Tam an Scene o cac frame trung gian de khong xuat khung hinh lech nhip quet 60Hz
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
        AutoNoTear::syncVBlankPhase();
        #endif

        CCEGLView::swapBuffers();
    }
};

$on_mod(Loaded) {
    #ifdef GEODE_IS_WINDOWS
    AutoNoTear::initEngine();
    #endif
}
