/*
 * Phase 18.5 — Native Windows Display Window
 * CustomAndroidQEMU: Independent Win32 framebuffer presenter
 *
 * Creates an independent Win32 window that presents the Android framebuffer
 * exported by ramfb-standalone via a QEMU DisplayChangeListener.
 *
 * Architecture:
 *   - QEMU main/GUI thread: registers DCL, calls dpy_gfx_update/switch.
 *   - Win32 UI thread: owns HWND, runs GetMessage loop, blits pixels.
 *   - Shared staging buffer protected by CRITICAL_SECTION.
 *   - DCL callbacks copy surface pixels into staging buffer, post WM_USER.
 *
 * Feature flag: x-win32-display=on on the ramfb device (off by default).
 *
 * Copyright (c) 2026 CustomAndroidEmulator project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ui/console.h"
#include "ui/surface.h"
#include "ui/win32-android-display.h"
#include "qemu/thread.h"

/* =========================================================
 * Internal constants
 * ========================================================= */

#define WIN32AD_CLASS_NAME   L"QemuAndroidDisplay"
#define WIN32AD_WINDOW_TITLE L"Android Display — CustomAndroidQEMU"
#define WIN32AD_WM_REFRESH   (WM_USER + 1)   /* posted by DCL to request blit */
#define WIN32AD_WM_QUIT_REQ  (WM_USER + 2)   /* posted to stop the Win32 thread */

/* Maximum display dimensions we support */
#define WIN32AD_MAX_W 4096
#define WIN32AD_MAX_H 4096

/* =========================================================
 * Shared state (DCL thread <-> Win32 UI thread)
 * ========================================================= */

typedef struct Win32AndroidDisplay {
    /* Win32 thread */
    HANDLE          thread_handle;
    DWORD           thread_id;
    HWND            hwnd;
    HINSTANCE       hinstance;

    /* Staging buffer — written by DCL callbacks, read by WndProc */
    CRITICAL_SECTION cs;
    uint8_t        *staging;        /* BGRA32 or xRGB32 pixels */
    int             staging_w;
    int             staging_h;
    int             staging_stride; /* bytes per row */
    bool            staging_dirty;  /* new frame waiting to blit */

    /* Requested window client size (set once on first switch) */
    int             client_w;
    int             client_h;

    /* Lifecycle */
    bool            initialized;
    bool            quit_requested;

    /* Telemetry */
    uint64_t        switch_count;
    uint64_t        update_count;
    uint64_t        blit_count;

    /* DCL registered with QEMU */
    DisplayChangeListener dcl;
} Win32AndroidDisplay;

/* Singleton — only one window is created */
static Win32AndroidDisplay *s_display;

/* =========================================================
 * Logging helper
 * ========================================================= */

static void G_GNUC_PRINTF(1, 2) ad_log(const char *fmt, ...)
{
    static FILE *f;
    if (!f) {
        f = fopen("C:\\qemu_work\\phase18_5_win32.log", "a");
    }
    if (!f) {
        return;
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart  = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    /* Convert to microseconds since 1601-01-01; we just print low 32 bits */
    fprintf(f, "[%10lu us] ", (unsigned long)(t.QuadPart / 10));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

/* =========================================================
 * Staging buffer helpers
 * ========================================================= */

/*
 * Copy pixels from a QEMU DisplaySurface into the staging buffer.
 * Must be called with the CRITICAL_SECTION held.
 * Surface format is PIXMAN_x8r8g8b8 (0xffRRGGBB) — we store as-is (BGRA on
 * little-endian x86, which is what StretchDIBits expects for BI_RGB 32bpp).
 */
static void staging_copy_surface_locked(Win32AndroidDisplay *d,
                                        DisplaySurface *surface)
{
    int w = surface_width(surface);
    int h = surface_height(surface);
    int src_stride = surface_stride(surface);

    if (w <= 0 || h <= 0 || w > WIN32AD_MAX_W || h > WIN32AD_MAX_H) {
        ad_log("staging_copy: rejected invalid surface %dx%d\n", w, h);
        return;
    }

    /* Reallocate staging buffer if size changed */
    int dst_stride = w * 4;
    size_t needed = (size_t)dst_stride * h;
    if (d->staging_w != w || d->staging_h != h) {
        free(d->staging);
        d->staging = (uint8_t *)malloc(needed);
        if (!d->staging) {
            ad_log("staging_copy: malloc(%zu) failed\n", needed);
            d->staging_w = d->staging_h = 0;
            return;
        }
        d->staging_w      = w;
        d->staging_h      = h;
        d->staging_stride = dst_stride;
    }

    /* Copy row-by-row (handles source stride != w*4) */
    const uint8_t *src = (const uint8_t *)surface_data(surface);
    uint8_t       *dst = d->staging;
    for (int y = 0; y < h; y++) {
        memcpy(dst, src, (size_t)dst_stride);
        src += src_stride;
        dst += dst_stride;
    }
    d->staging_dirty = true;
}

/* =========================================================
 * Win32 window procedure
 * ========================================================= */

static void win32ad_do_blit(Win32AndroidDisplay *d, HWND hwnd, HDC hdc_in, bool force_redraw)
{
    /* Snapshot staging buffer under lock */
    EnterCriticalSection(&d->cs);
    int w = d->staging_w;
    int h = d->staging_h;
    int stride = d->staging_stride;
    bool dirty = d->staging_dirty;
    uint8_t *pixels = NULL;
    if ((dirty || force_redraw) && w > 0 && h > 0 && d->staging) {
        size_t sz = (size_t)stride * h;
        pixels = (uint8_t *)malloc(sz);
        if (pixels) {
            memcpy(pixels, d->staging, sz);
            d->staging_dirty = false;
        }
    }
    LeaveCriticalSection(&d->cs);

    if (!pixels) {
        return;
    }

    /* Build BITMAPINFO for a top-down DIB (negative height = top-down) */
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   /* negative = top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    /* Get client area size for scaling */
    RECT rc;
    GetClientRect(hwnd, &rc);
    int dst_w = rc.right  - rc.left;
    int dst_h = rc.bottom - rc.top;

    bool release_dc = false;
    HDC hdc = hdc_in;
    if (!hdc) {
        hdc = GetDC(hwnd);
        release_dc = true;
    }

    if (hdc) {
        SetStretchBltMode(hdc, HALFTONE);
        StretchDIBits(hdc,
                      0, 0, dst_w, dst_h,    /* destination rect */
                      0, 0, w,    h,          /* source rect */
                      pixels,
                      &bi,
                      DIB_RGB_COLORS,
                      SRCCOPY);
        if (release_dc) {
            ReleaseDC(hwnd, hdc);
        }
        d->blit_count++;
        if ((d->blit_count % 60) == 1 || d->blit_count <= 5) {
            const uint32_t *p32 = (const uint32_t *)pixels;
            ad_log("BLIT #%llu: src=%dx%d dst=%dx%d sample_px[0..3]=0x%08x 0x%08x 0x%08x 0x%08x\n",
                   (unsigned long long)d->blit_count, w, h, dst_w, dst_h,
                   p32[0], p32[1], p32[2], p32[3]);
        }
    }

    free(pixels);
}

static LRESULT CALLBACK win32ad_wndproc(HWND hwnd, UINT msg,
                                        WPARAM wp, LPARAM lp)
{
    Win32AndroidDisplay *d = s_display;

    switch (msg) {
    case WM_CREATE:
        ad_log("WndProc: WM_CREATE\n");
        return 0;

    case WIN32AD_WM_REFRESH:
        if (d) {
            win32ad_do_blit(d, hwnd, NULL, false);
        }
        return 0;

    case WIN32AD_WM_QUIT_REQ:
        ad_log("WndProc: WM_QUIT_REQ received\n");
        PostQuitMessage(0);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (d) {
            win32ad_do_blit(d, hwnd, hdc, true);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PRINT:
    case WM_PRINTCLIENT:
        if (d) {
            win32ad_do_blit(d, hwnd, (HDC)wp, true);
        }
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        ad_log("WndProc: WM_CLOSE (user closed window — hiding)\n");
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* =========================================================
 * Win32 UI thread
 * ========================================================= */

static DWORD WINAPI win32ad_thread(LPVOID arg)
{
    Win32AndroidDisplay *d = (Win32AndroidDisplay *)arg;
    HINSTANCE hinst = GetModuleHandle(NULL);
    d->hinstance = hinst;

    ad_log("Win32 thread started (tid=%lu)\n", (unsigned long)GetCurrentThreadId());

    /* Register window class */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = win32ad_wndproc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WIN32AD_CLASS_NAME;
    RegisterClassExW(&wc); /* ignore failure if already registered */

    /* Compute window size to fit client area */
    int cw = d->client_w > 0 ? d->client_w : 540;  /* default: half of 1080 */
    int ch = d->client_h > 0 ? d->client_h : 1170; /* default: half of 2340 */

    /* Scale down to fit screen if needed */
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (ch > (screen_h * 9 / 10)) {
        double scale = (double)(screen_h * 9 / 10) / ch;
        cw = (int)(cw * scale);
        ch = (int)(ch * scale);
    }
    if (cw > (screen_w * 9 / 10)) {
        double scale = (double)(screen_w * 9 / 10) / cw;
        cw = (int)(cw * scale);
        ch = (int)(ch * scale);
    }

    /* Calculate total window size including decorations */
    RECT wr = { 0, 0, cw, ch };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = wr.right  - wr.left;
    int wh = wr.bottom - wr.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        WIN32AD_CLASS_NAME,
        WIN32AD_WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        ww, wh,
        NULL, NULL, hinst, NULL);

    if (!hwnd) {
        ad_log("CreateWindowExW failed: error=%lu\n", (unsigned long)GetLastError());
        return 1;
    }

    d->hwnd = hwnd;
    ad_log("Window created: hwnd=%p client=%dx%d\n", (void *)hwnd, cw, ch);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    /* Message loop */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ad_log("Win32 thread exiting (blit_count=%llu)\n",
           (unsigned long long)d->blit_count);
    return 0;
}

/* =========================================================
 * DisplayChangeListener callbacks
 * (called on QEMU main/GUI thread)
 * ========================================================= */

static void win32ad_dcl_gfx_update(DisplayChangeListener *dcl,
                                   int x, int y, int w, int h)
{
    Win32AndroidDisplay *d = container_of(dcl, Win32AndroidDisplay, dcl);
    if (!d->hwnd) {
        return;
    }

    /* Grab a snapshot of the current surface under the staging lock */
    DisplaySurface *surface = qemu_console_surface(dcl->con);
    if (!surface) {
        return;
    }

    EnterCriticalSection(&d->cs);
    staging_copy_surface_locked(d, surface);
    d->update_count++;
    LeaveCriticalSection(&d->cs);

    /* Signal Win32 thread to blit */
    PostMessage(d->hwnd, WIN32AD_WM_REFRESH, 0, 0);

    if ((d->update_count % 60) == 1) {
        ad_log("DCL update #%llu: region=[%d,%d %dx%d]\n",
               (unsigned long long)d->update_count, x, y, w, h);
    }
}

static void win32ad_dcl_gfx_switch(DisplayChangeListener *dcl,
                                   struct DisplaySurface *surface)
{
    Win32AndroidDisplay *d = container_of(dcl, Win32AndroidDisplay, dcl);
    d->switch_count++;

    if (!surface) {
        ad_log("DCL switch #%llu: surface=NULL (ignored)\n",
               (unsigned long long)d->switch_count);
        return;
    }

    int w = surface_width(surface);
    int h = surface_height(surface);
    ad_log("DCL switch #%llu: surface=%dx%d fmt=0x%x\n",
           (unsigned long long)d->switch_count, w, h, surface_format(surface));

    /* Copy the initial surface into the staging buffer */
    EnterCriticalSection(&d->cs);
    staging_copy_surface_locked(d, surface);
    LeaveCriticalSection(&d->cs);

    /* Update client size hint if window not yet created */
    if (!d->hwnd) {
        d->client_w = w;
        d->client_h = h;
    }

    /* If window exists, post a refresh */
    if (d->hwnd) {
        PostMessage(d->hwnd, WIN32AD_WM_REFRESH, 0, 0);
    }
}

static void win32ad_dcl_refresh(DisplayChangeListener *dcl)
{
    /* Nothing — we refresh reactively on gfx_update */
    (void)dcl;
}

static const DisplayChangeListenerOps win32ad_dcl_ops = {
    .dpy_name       = "win32-android",
    .dpy_gfx_update = win32ad_dcl_gfx_update,
    .dpy_gfx_switch = win32ad_dcl_gfx_switch,
    .dpy_refresh    = win32ad_dcl_refresh,
};

/* =========================================================
 * Public API
 * ========================================================= */

bool win32_android_display_init(QemuConsole *con)
{
    if (s_display) {
        ad_log("win32_android_display_init: already initialized\n");
        return true;
    }

    Win32AndroidDisplay *d = calloc(1, sizeof(*d));
    if (!d) {
        ad_log("win32_android_display_init: calloc failed\n");
        return false;
    }

    InitializeCriticalSection(&d->cs);
    s_display = d;

    ad_log("=== Phase 18.5 Win32 Display Init ===\n");

    /* Get initial surface size hint if console already has a surface */
    if (con) {
        DisplaySurface *surf = qemu_console_surface(con);
        if (surf) {
            d->client_w = surface_width(surf);
            d->client_h = surface_height(surf);
            ad_log("Initial surface hint: %dx%d\n", d->client_w, d->client_h);
        }
    }

    /* Start Win32 UI thread */
    d->thread_handle = CreateThread(NULL, 0, win32ad_thread, d, 0, &d->thread_id);
    if (!d->thread_handle) {
        ad_log("CreateThread failed: error=%lu\n", (unsigned long)GetLastError());
        DeleteCriticalSection(&d->cs);
        free(d);
        s_display = NULL;
        return false;
    }
    ad_log("Win32 UI thread started (tid=%lu)\n", (unsigned long)d->thread_id);

    /* Wait briefly for window to be created */
    for (int i = 0; i < 100; i++) {
        Sleep(20);
        if (d->hwnd) {
            break;
        }
    }
    if (!d->hwnd) {
        ad_log("WARNING: hwnd not set after 2s — window may still be initializing\n");
    }

    /* Register DCL on QEMU's display change system */
    d->dcl.ops = &win32ad_dcl_ops;
    d->dcl.con = con;
    register_displaychangelistener(&d->dcl);
    d->initialized = true;

    ad_log("DCL registered for console %p\n", (void *)con);
    return true;
}

void win32_android_display_cleanup(void)
{
    Win32AndroidDisplay *d = s_display;
    if (!d) {
        return;
    }

    ad_log("win32_android_display_cleanup: shutting down\n");

    if (d->initialized) {
        unregister_displaychangelistener(&d->dcl);
    }

    if (d->hwnd) {
        PostMessage(d->hwnd, WIN32AD_WM_QUIT_REQ, 0, 0);
    }

    if (d->thread_handle) {
        WaitForSingleObject(d->thread_handle, 3000);
        CloseHandle(d->thread_handle);
    }

    EnterCriticalSection(&d->cs);
    free(d->staging);
    d->staging = NULL;
    LeaveCriticalSection(&d->cs);

    DeleteCriticalSection(&d->cs);
    free(d);
    s_display = NULL;

    ad_log("win32_android_display_cleanup: done\n");
}

bool win32_android_display_is_running(void)
{
    return s_display && s_display->hwnd != NULL;
}

void win32_android_display_get_stats(uint64_t *switch_count,
                                     uint64_t *update_count,
                                     uint64_t *blit_count)
{
    Win32AndroidDisplay *d = s_display;
    if (!d) {
        *switch_count = *update_count = *blit_count = 0;
        return;
    }
    *switch_count = d->switch_count;
    *update_count = d->update_count;
    *blit_count   = d->blit_count;
}

#endif /* _WIN32 */
