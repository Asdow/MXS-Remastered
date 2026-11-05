// window_test.cpp
//   The OTHER tab's Fullscreen window (Config::fullscreenWindow), through a built MXCam opengl32.dll:
//   a normal window with a frame, drawn into with OpenGL, becomes borderless and covers its monitor.
//   (It shows on screen for a moment.)
//
//   Build from a Developer Command Prompt matching the DLL's bitness:
//     cl /nologo /EHsc /std:c++17 /W4 tests\window_test.cpp user32.lib gdi32.lib
//   Run:
//     window_test.exe bin\x64\Release\opengl32.dll

#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: window_test.exe path\\to\\opengl32.dll\n");
        return 2;
    }
    printf("window test\n");
    // The DLL in a folder of its own, with the option on.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"mxcam_window_test\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t source[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, source, MAX_PATH);
    const std::wstring dll = dir + L"opengl32.dll";
    if (!CopyFileW(source, dll.c_str(), FALSE)) {
        printf("could not copy %s\n", argv[1]);
        return 1;
    }
    {
        FILE* out = nullptr;
        if (_wfopen_s(&out, (dir + L"mxcam.ini").c_str(), L"wb") != 0 || !out)
            return 1;
        fputs("[Other]\nFullscreenWindow=1\n", out);
        fclose(out);
    }
    SetEnvironmentVariableA("MXCAM_TEST_MOUSE_IDLE", "1");
    HMODULE proxy = LoadLibraryW(dll.c_str());
    if (!proxy) {
        printf("could not load the DLL (error %lu)\n", GetLastError());
        return 1;
    }
    const auto createContext = reinterpret_cast<HGLRC(WINAPI*)(HDC)>(GetProcAddress(proxy, "wglCreateContext"));
    const auto makeCurrent = reinterpret_cast<BOOL(WINAPI*)(HDC, HGLRC)>(GetProcAddress(proxy, "wglMakeCurrent"));
    const auto deleteContext = reinterpret_cast<BOOL(WINAPI*)(HGLRC)>(GetProcAddress(proxy, "wglDeleteContext"));
    const auto swapBuffers = reinterpret_cast<BOOL(WINAPI*)(HDC)>(GetProcAddress(proxy, "wglSwapBuffers"));
    const auto clear = reinterpret_cast<void(WINAPI*)(GLbitfield)>(GetProcAddress(proxy, "glClear"));
    if (!createContext || !makeCurrent || !deleteContext || !swapBuffers || !clear) {
        printf("missing exports\n");
        return 1;
    }

    // A normal window: title bar, border, 800 x 600, not at the monitor's corner.
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "MXCamWindowTest";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, "MXCam window test", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, nullptr,
                                nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    HDC dc = GetDC(window);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = createContext(dc);
    if (!rc || !makeCurrent(dc, rc)) {
        printf("no GL context\n");
        return 1;
    }
    for (int frame = 0; frame < 30; ++frame) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        clear(GL_COLOR_BUFFER_BIT);
        swapBuffers(dc);
        Sleep(15);
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT rect{}, client{};
    GetWindowRect(window, &rect);
    GetClientRect(window, &client);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    printf("    window %ld,%ld - %ld,%ld (client %ldx%ld); monitor %ld,%ld - %ld,%ld\n", rect.left, rect.top, rect.right,
           rect.bottom, client.right, client.bottom, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right,
           monitor.rcMonitor.bottom);
    Check(!(style & WS_CAPTION) && !(style & WS_THICKFRAME), "the title bar and border are gone");
    // One pixel taller than the screen, off its bottom edge: exactly the screen's size, Windows would
    // stop syncing it to the screen (tearing without vsync).
    const RECT& m = monitor.rcMonitor;
    Check(rect.left == m.left && rect.top == m.top && rect.right == m.right && rect.bottom == m.bottom + 1,
          "the window covers its monitor, one pixel taller (no tearing)");
    Check(client.right == m.right - m.left && client.bottom == m.bottom - m.top + 1, "the game draws into the whole screen");
    Check((style & WS_VISIBLE) != 0, "it's still shown");

    makeCurrent(nullptr, nullptr);
    deleteContext(rc);
    ReleaseDC(window, dc);
    DestroyWindow(window);
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("All checks passed\n");
    return 0;
}
