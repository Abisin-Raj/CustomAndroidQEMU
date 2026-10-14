/*
 * Phase 18.5 — Native Windows Display Window
 * Public header for win32-android-display.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_WIN32_ANDROID_DISPLAY_H
#define UI_WIN32_ANDROID_DISPLAY_H

#include "ui/console.h"

#ifdef _WIN32

/**
 * win32_android_display_init:
 * @con: The QemuConsole to attach to (typically console 0).
 *
 * Creates an independent Win32 display window and registers a
 * DisplayChangeListener on @con.  Must be called from the QEMU main thread
 * after the console has been initialized.
 *
 * Returns true on success, false on failure.
 */
bool win32_android_display_init(QemuConsole *con);

/**
 * win32_android_display_cleanup:
 *
 * Unregisters the DCL, posts WM_QUIT to the Win32 thread, and frees all
 * resources.  Safe to call even if init was not called.
 */
void win32_android_display_cleanup(void);

/**
 * win32_android_display_is_running:
 *
 * Returns true if the Win32 window has been created and is visible.
 */
bool win32_android_display_is_running(void);

/**
 * win32_android_display_get_stats:
 *
 * Writes cumulative telemetry counters into the provided pointers.
 * All pointers must be non-NULL.
 */
void win32_android_display_get_stats(uint64_t *switch_count,
                                     uint64_t *update_count,
                                     uint64_t *blit_count);

#else /* !_WIN32 */

/* Stub implementations for non-Windows builds */
static inline bool win32_android_display_init(QemuConsole *con)
{
    (void)con;
    return false;
}
static inline void win32_android_display_cleanup(void) {}
static inline bool win32_android_display_is_running(void) { return false; }
static inline void win32_android_display_get_stats(uint64_t *s,
                                                   uint64_t *u,
                                                   uint64_t *b)
{
    *s = *u = *b = 0;
}

#endif /* _WIN32 */

#endif /* UI_WIN32_ANDROID_DISPLAY_H */
