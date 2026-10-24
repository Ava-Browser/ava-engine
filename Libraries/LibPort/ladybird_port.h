/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The C ABI between LibWebView and a non-C++ UI.
 *
 * This header is PURE C on purpose (contract rule R2). It declares no C++ types, no templates, no objc_msgSend, and
 * nothing that needs Swift/C++ interop: a Swift client imports it as a plain C module and needs no
 * -enable-experimental-cxx-interop. CoreFoundation is a C API, so the run loop timer the shim installs is C too.
 *
 * The shim owns the engine and never blocks. The host (SwiftUI, AppKit, anything) owns the main run loop and calls
 * LbAppPump, or calls LbAppStartPump and lets the shim install a CFRunLoopTimer that pumps for it.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LbApp LbApp;

typedef struct LbCallbacks {
    void* context;

    // A new frame is available for `tab`. Demand-driven: a settled page stops producing these (contract rule R3).
    // Called on the main thread, from inside the pump, on the run loop that LbAppStartPump or the host drives.
    void (*on_ready_to_paint)(void* context, uint64_t tab);

    // The tab's displayed URL changed. `url` is not NUL-terminated; `len` gives its length. Only valid for this call.
    void (*on_url_change)(void* context, uint64_t tab, const char* url, size_t len);

    // A navigation in `tab` finished loading. Same `url`/`len` lifetime rules as above.
    void (*on_load_finish)(void* context, uint64_t tab, const char* url, size_t len);
} LbCallbacks;

// A frame that is ready to display. The handle is a borrowed IOSurfaceRef: the caller must CFRetain it to keep it
// across calls. The pixels are never copied or cropped here (contract rule R1), so `painted_width`/`painted_height`
// may be smaller than the surface itself; display the painted part with CALayer.contentsRect.
typedef struct LbSurface {
    void* iosurface;
    int32_t painted_width;
    int32_t painted_height;
    int32_t surface_width;
    int32_t surface_height;
} LbSurface;

// Creates the engine. `argv0` is only used for logging and resource lookup.
LbApp* LbAppCreate(const char* argv0, const LbCallbacks* callbacks);

// One non-blocking iteration of the engine's event loop (Core::EventLoop::WaitMode::PollForEvents).
void LbAppPump(LbApp* app);

// Installs a CFRunLoopTimer on the current run loop that calls LbAppPump for every tick. The host then runs its own
// loop; this is the shape a SwiftUI app wants, since a blocking LbAppRun would fight it.
void LbAppStartPump(LbApp* app, double interval_seconds);

// Stops the timer LbAppStartPump installed, if any.
void LbAppStopPump(LbApp* app);

// Tears the engine down. Must be called from the main thread, after LbAppStopPump.
void LbAppDestroy(LbApp* app);

uint64_t LbTabCreate(LbApp* app);
void LbTabLoad(LbApp* app, uint64_t tab, const char* url, size_t len);

// Fills `out` with the tab's current frame. Returns 1 if a frame was written, 0 if the tab has none yet.
int LbTabGetSurface(LbApp* app, uint64_t tab, LbSurface* out);

// The viewport in CSS pixels plus the display's device pixel ratio. Call on creation and whenever the window resizes.
void LbTabSetViewport(LbApp* app, uint64_t tab, int32_t width, int32_t height, double device_pixel_ratio);

#ifdef __cplusplus
}
#endif
