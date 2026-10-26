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

// Reserved so a host does not have to guess: ordinal 0 in Web::UIEvents::KeyCode is the invalid key.
#define LbKeyInvalid 0

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

    // The tab's document title changed. `title` is UTF-8 encoded and not NUL-terminated; `len` gives its length. Valid for this call only.
    void (*on_title_change)(void* context, uint64_t tab, const char* title, size_t len);

    // The tab's loading state changed: 1 if loading, 0 if idle/finished.
    void (*on_loading_state_change)(void* context, uint64_t tab, int is_loading);

    // The tab's favicon changed. If `data` is NULL or `len` is 0, the favicon was cleared.
    // When present, `data` points to PNG encoded bytes, and `hash` is an ASCII hex SHA-256 string (not NUL-terminated; `hash_len` gives its length).
    // Valid for this call only.
    void (*on_favicon_change)(void* context, uint64_t tab, const uint8_t* data, size_t len, const char* hash, size_t hash_len);

    // ---------------------------------------------------------------------------------------------------------------
    // Modal dialogs. These are the only callbacks the engine BLOCKS on: the page's script stops until one is answered,
    // and the freeze is in WebContent, so a host that renders correctly but never answers looks alive while the page is
    // wedged. Every one of these MUST be answered, with the matching LbTabAnswer* call.
    //
    // All four are optional, and absent is safe: with no callback installed the engine answers itself (alert closes,
    // confirm declines, prompt cancels, before_unload allows). The danger is present-but-silent — installing a callback
    // and not answering it hangs the page. So a host should install all four or none.
    //
    // `message` is not NUL-terminated; `len` gives its length and is valid for this call only.
    // `default_text` on the prompt is the value the field starts with, same lifetime rules.
    // ---------------------------------------------------------------------------------------------------------------

    // alert(). Answer with LbTabAnswerAlert.
    void (*on_request_alert)(void* context, uint64_t tab, const char* message, size_t len);

    // confirm(). Answer with LbTabAnswerConfirm. Declining is the safe default.
    void (*on_request_confirm)(void* context, uint64_t tab, const char* message, size_t len);

    // prompt(). Answer with LbTabAnswerPrompt.
    void (*on_request_prompt)(void* context, uint64_t tab, const char* message, size_t len, const char* default_text, size_t default_len);

    // beforeunload. Answer with LbTabAnswerBeforeUnload. NOTE THE INVERTED SENSE: `accept` of 1 means ALLOW the
    // navigation, not block it — the opposite of what the word suggests, and the bool has already misled one careful
    // reader. A page that returns false from its beforeunload handler should be answered 0.
    void (*on_request_before_unload)(void* context, uint64_t tab, const char* source, size_t len);
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

// Destroys one tab and its engine page. The host must stop asking for its surface first: a get-surface after this
// returns 0 rather than a stale frame, but presenting a frame that arrived before the call would be a use-after-free.
// Every other tab is unaffected, and the app outlives all of them.
void LbTabDestroy(LbApp* app, uint64_t tab);

void LbTabLoad(LbApp* app, uint64_t tab, const char* url, size_t len);

// What the address bar's text means, resolved by the engine rather than the host.
//
// A host cannot do this correctly on its own: deciding whether "ladybird" is a host or a search term needs the public
// suffix list and the user's configured search engine, and a second implementation would drift from the browser's real
// behaviour on the next upstream sync. This is WebView::sanitize_url -- the same call the Qt frontend makes -- so both
// frontends agree on what a given input navigates to.
//
// Returns 1 and fills the out parameters with an allocated UTF-8 URL on success, 0 if the input could be resolved
// neither as a URL nor as a search. The caller owns the buffer and must release it with LbStringFree. An empty search
// engine means a bare word that is not a URL returns 0, which is how a host detects "no engine configured".
int LbResolveUserInput(const char* utf8, size_t len, char** out_url, size_t* out_len);

// Releases a buffer returned by LbResolveUserInput. Null-safe, and does nothing for a null pointer.
void LbStringFree(char* string);

// Fills `out` with the tab's current frame. Returns 1 if a frame was written, 0 if the tab has none yet.
int LbTabGetSurface(LbApp* app, uint64_t tab, LbSurface* out);

// The viewport in CSS pixels plus the display's device pixel ratio. Call on creation and whenever the window resizes.
void LbTabSetViewport(LbApp* app, uint64_t tab, int32_t width, int32_t height, double device_pixel_ratio);

//
// Session history.
//
// `delta` is -1 for back and +1 for forward. Any other value is clamped to those two: the ABI exposes the two buttons a
// browser has, not the engine's arbitrary-step traversal, so a host cannot ask for a jump it has no UI for.
//
// Both are safe on a closed tab (they no-op) and safe to call before the first navigation.
void LbTabTraverseHistory(LbApp* app, uint64_t tab, int32_t delta);

// Whether `LbTabTraverseHistory` would do anything right now: 1 if yes, 0 if no. Use it to enable or disable the button.
//
// This reads the engine's own `navigate_back_action` / `navigate_forward_action`, which LibWebView maintains, rather
// than deriving "is there anything behind us" from step counters. Reimplementing that rule in a host is exactly the
// kind of logic that silently drifts from the engine on an upstream change.
int LbTabCanTraverseHistory(LbApp* app, uint64_t tab, int32_t delta);

//
// Input.
//
// Positions are in CSS pixels relative to the tab's top-left, which is the same space as the viewport. The shim scales
// them by the device pixel ratio, because the engine works in device pixels.
//
// These deliberately mirror the small set of fields a browser actually sets, rather than exposing Web::MouseEvent. That
// struct has designated-default fields a host would silently leave unset: `should_insert_text` is the documented one —
// omit it and the key is delivered, handled, and inserts nothing, with no error anywhere. Filling those in is the shim's
// job, so the host states intent ("a left click at x,y") and not transport.

// Matches Web::UIEvents::MouseButton, which is a bitmask. `Primary` is the left button.
typedef enum LbMouseButton {
    LbMouseButtonNone = 0,
    LbMouseButtonPrimary = 1,
    LbMouseButtonSecondary = 2,
    LbMouseButtonMiddle = 4,
} LbMouseButton;

typedef enum LbMouseEventType {
    LbMouseEventDown = 0,
    LbMouseEventUp = 1,
    LbMouseEventMove = 2,
    LbMouseEventWheel = 3,
} LbMouseEventType;

// `button` is the button this event concerns; `buttons` is every button held during it. A button-up carries the still-held
// set, which is why they are separate arguments. For a wheel event the buttons are ignored and `wheel_delta_y` is the
// pixel delta (positive scrolls down).
void LbTabSendMouse(LbApp* app, uint64_t tab, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y);

// A single key transition. `key` is a Web::UIEvents::KeyCode ordinal; the host passes `LbKeyInvalid` (0) when it only has
// text, and the shim then leaves the key unresolved. `code_point` is the character to insert (0 for a non-text key such
// as Tab or an arrow). `modifiers` is the Web::UIEvents::KeyModifier bitmask.
//
// Note the engine dispatches keys asynchronously, through the compositor. Returning from this function means the event was
// queued, NOT that the page handled it; there is no completion callback yet (see Documentation/Traps.md).
void LbTabSendKey(LbApp* app, uint64_t tab, int32_t key, uint32_t code_point, uint32_t modifiers, int is_key_down);

// Convenience for the common case: types `utf8` as text, one key transition pair per code point. This is what an address
// bar or a `<input>` needs; use LbTabSendKey directly for keys that are not text.
void LbTabTypeText(LbApp* app, uint64_t tab, const char* utf8, size_t len);

// Dialogs: the answers to the four blocking callbacks above. Each MUST be called exactly once for the dialog it
// answers, or the page stays wedged. Answering a tab that has no dialog pending is a no-op, so a host that closes a tab
// with a dialog up can simply answer and then close.
void LbTabAnswerAlert(LbApp* app, uint64_t tab);

// `accept` of 0 declines, which is the safe default.
void LbTabAnswerConfirm(LbApp* app, uint64_t tab, int accept);

// `response` of NULL is Cancel (equivalent to dismissing the prompt). An empty string is "accepted with no text", which
// is a different outcome, so the two are kept distinct rather than folded together.
void LbTabAnswerPrompt(LbApp* app, uint64_t tab, const char* response, size_t len);

// `accept` of 1 ALLOWS the navigation. See the inverted-sense note on on_request_before_unload.
void LbTabAnswerBeforeUnload(LbApp* app, uint64_t tab, int accept);

#ifdef __cplusplus
}
#endif
