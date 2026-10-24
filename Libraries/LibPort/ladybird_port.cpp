/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The bridge between LibWebView and a non-C++ UI. Plain C++ and CoreFoundation only: no ObjC, no CALayer, no NSWindow.
 * The host owns all of that, and per contract rule R2 the surface crosses as a bare IOSurfaceRef.
 */

#include "ladybird_port.h"

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf8View.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

namespace {

class PortApplication final : public WebView::Application {
    WEB_VIEW_APPLICATION(PortApplication)

public:
    explicit PortApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        // The host owns the event loop and the window, so the engine must not run headless or coordinate a browser.
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;

        // NOT test mode, which the probes use. Test mode is a layout-test switch, not a sandbox: it makes
        // FontPlugin::generic_font_name() return "SerenitySans" for every generic family, skipping font matching
        // entirely, so every page renders in the bundled fallback. That is correct for a probe that must produce
        // identical pixels everywhere and wrong for a browser — it was the reason this app ignored system fonts.
        // Left at the default (No).
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

// The bitmap members are protected, so a subclass is the sanctioned access path. This is current_paintable() from
// UI/Qt/WebContentView.cpp, gate included: without the m_has_usable_bitmap check a bitmap that is mid-swap would be
// presented as if it were the front one.
class PortWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<PortWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
    {
        auto view = adopt_own(*new PortWebView(move(theme), window_size));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    struct Frame {
        Gfx::SharedImageBuffer const* buffer { nullptr };
        Gfx::IntSize painted_size;
    };

    Optional<Frame> current_frame() const
    {
        Gfx::SharedImageBuffer const* buffer = nullptr;
        Gfx::IntSize size;

        if (m_has_usable_bitmap) {
            VERIFY(m_front_bitmap.shared_image_buffer);
            buffer = m_front_bitmap.shared_image_buffer.ptr();
            size = m_front_bitmap.last_painted_size.to_type<int>();
        } else if (m_backup_shared_image_buffer) {
            buffer = m_backup_shared_image_buffer.ptr();
            size = m_backup_bitmap_size.to_type<int>();
        }

        if (!buffer)
            return {};
        return Frame { buffer, size };
    }

private:
    PortWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
        : HeadlessWebView(move(theme), window_size)
    {
    }
};

struct Tab {
    AK_ALLOC_WITH_KMALLOC;

    uint64_t id { 0 };
    NonnullOwnPtr<PortWebView> view;

    // The engine's input positions are in device pixels and the host speaks CSS pixels, so the ratio has to be kept
    // alongside the tab. LbTabSetViewport is the only thing that sets it.
    double device_pixel_ratio { 1.0 };
    int32_t click_count { 0 };
};

}

struct LbApp {
    // OwnPtr, not NonnullOwnPtr: LbApp is allocated by plain new, so the kmalloc-tag static assert would fire otherwise.
    OwnPtr<PortApplication> application;
    Vector<NonnullOwnPtr<Tab>> tabs;
    LbCallbacks callbacks {};
    CFRunLoopTimerRef pump_timer { nullptr };

    // Tabs are declared AFTER the application, so they are destroyed BEFORE it: each tab's HeadlessWebView has to go
    // away while the application owning its WebContent client is still alive.
};

namespace {

void set_up_isolation(ByteString& scratch_directory)
{
    scratch_directory = ByteString::formatted("{}/Ladybird-Port-{}", Core::StandardPaths::tempfile_directory(), getpid());
    MUST(Core::Directory::create(scratch_directory, Core::Directory::CreateDirectories::Yes));
    VERIFY(setenv("XDG_CONFIG_HOME", scratch_directory.characters(), 1) == 0);
}

void dispatch_paint(LbApp& app, uint64_t tab)
{
    if (app.callbacks.on_ready_to_paint)
        app.callbacks.on_ready_to_paint(app.callbacks.context, tab);
}

void dispatch_url(LbApp& app, uint64_t tab, auto const& url, void (*callback)(void*, uint64_t, char const*, size_t))
{
    if (!callback)
        return;
    // The bytes are valid for this call only, which the header states. URL::serialize() is an AK::String (UTF-16), so
    // it has to be narrowed before it can cross a char* boundary.
    auto serialized = url.serialize().to_byte_string();
    callback(app.callbacks.context, tab, serialized.characters(), serialized.length());
}

}

namespace {

// Shared lookup: every input entry point needs the same tab, and every one of them must tolerate a stale id.
Tab* find_tab(LbApp* app, uint64_t tab)
{
    if (!app)
        return nullptr;
    for (auto const& candidate : app->tabs) {
        if (candidate->id == tab)
            return candidate.ptr();
    }
    return nullptr;
}

}

extern "C" {

LbApp* LbAppCreate(char const* argv0, LbCallbacks const* callbacks)
{
    auto scratch_directory = ByteString {};
    set_up_isolation(scratch_directory);

    // The engine parses arguments itself, so it is handed a minimal, well-formed argv that cannot select headless mode
    // or a profile of its own.
    auto program_name = argv0 ? ByteString { StringView { argv0, strlen(argv0) } } : ByteString { "Ladybird"sv };
    auto arguments = Vector<ByteString> {};
    arguments.append(move(program_name));
    auto argument_strings = Vector<StringView> {};
    for (auto const& argument : arguments)
        argument_strings.append(argument.view());

    auto main_arguments = Main::Arguments {
        .argc = static_cast<int>(argument_strings.size()),
        .argv = nullptr,
        .strings = argument_strings.span(),
    };

    // The engine finds its helper processes and its Resources directory relative to the application directory. Tests get
    // it from a compile definition; here the host passes it, defaulting to the CLI's crude self-location.
    auto binary_path = Core::Environment::get("LADYBIRD_BINARY_PATH"sv);
    auto application = binary_path.has_value()
        ? PortApplication::create(main_arguments, *binary_path)
        : PortApplication::create(main_arguments, OptionalNone {});
    if (application.is_error()) {
        warnln("LbAppCreate: unable to initialize the engine: {}", application.error());
        return nullptr;
    }

    auto app = new LbApp {};
    app->application = application.release_value();
    if (callbacks)
        app->callbacks = *callbacks;
    return app;
}

void LbAppPump(LbApp* app)
{
    if (!app)
        return;
    // PollForEvents maps to poll(timeout: 0), so this returns immediately. WaitForEvents would block the host's loop.
    Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

static void pump_from_run_loop_timer(CFRunLoopTimerRef, void* info)
{
    LbAppPump(static_cast<LbApp*>(info));
}

void LbAppStartPump(LbApp* app, double interval_seconds)
{
    if (!app || app->pump_timer)
        return;

    CFRunLoopTimerContext context { 0, app, nullptr, nullptr, nullptr };
    auto* timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        interval_seconds,
        0,
        0,
        pump_from_run_loop_timer,
        &context);
    if (!timer)
        return;

    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);
    app->pump_timer = timer;
}

void LbAppStopPump(LbApp* app)
{
    if (!app || !app->pump_timer)
        return;
    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), app->pump_timer, kCFRunLoopCommonModes);
    CFRelease(app->pump_timer);
    app->pump_timer = nullptr;
}

void LbAppDestroy(LbApp* app)
{
    if (!app)
        return;
    LbAppStopPump(app);
    delete app;
}

uint64_t LbTabCreate(LbApp* app)
{
    if (!app)
        return 0;

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = Gfx::load_system_theme(theme_path.string());
    if (theme.is_error()) {
        warnln("LbTabCreate: unable to load the system theme: {}", theme.error());
        return 0;
    }

    auto view = PortWebView::create(theme.release_value(), { 1024, 768 });
    auto id = static_cast<uint64_t>(app->tabs.size() + 1);

    view->on_ready_to_paint = [app, id] { dispatch_paint(*app, id); };
    view->on_url_change = [app, id](auto const& url) {
        dispatch_url(*app, id, url, app->callbacks.on_url_change);
    };
    view->on_load_finish = [app, id](auto const& url) {
        dispatch_url(*app, id, url, app->callbacks.on_load_finish);
    };

    app->tabs.append(adopt_own(*new Tab { id, move(view) }));
    return id;
}

void LbTabLoad(LbApp* app, uint64_t tab, char const* url, size_t len)
{
    if (!app || !url)
        return;
    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;
        auto parsed = URL::Parser::basic_parse(StringView { url, len });
        if (!parsed.has_value()) {
            warnln("LbTabLoad: unable to parse the URL");
            return;
        }
        candidate->view->load(parsed.release_value());
        return;
    }
}

int LbTabGetSurface(LbApp* app, uint64_t tab, LbSurface* out)
{
    if (!app || !out)
        return 0;
    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;

        auto frame = candidate->view->current_frame();
        if (!frame.has_value())
            return 0;

        auto const& handle = frame->buffer->iosurface_handle();
        if (frame->painted_size.is_empty())
            return 0;

        // R1: hand the reference across; never crop or clone the bitmap behind it. The ref is borrowed, so the caller
        // retains it if it needs the surface past the next paint.
        out->iosurface = handle.core_foundation_pointer();
        out->painted_width = frame->painted_size.width();
        out->painted_height = frame->painted_size.height();
        out->surface_width = static_cast<int32_t>(handle.width());
        out->surface_height = static_cast<int32_t>(handle.height());
        return 1;
    }
    return 0;
}

void LbTabSetViewport(LbApp* app, uint64_t tab, int32_t width, int32_t height, double device_pixel_ratio)
{
    if (!app || width <= 0 || height <= 0)
        return;
    if (device_pixel_ratio <= 0)
        device_pixel_ratio = 1.0;

    for (auto const& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;

        // The ratio has to be in place before the resize, because the resize is what pushes the viewport. This is the
        // only public way to set it without touching Libraries/; a real port wants a non-testing setter here.
        candidate->view->set_device_pixel_ratio_for_testing(candidate->view->page(), device_pixel_ratio);
        // Kept for input: positions arrive in CSS pixels and the engine wants device pixels.
        candidate->device_pixel_ratio = device_pixel_ratio;

        // A headless viewport is in device pixels, the same way Qt's update_viewport_size() scales by the ratio.
        candidate->view->reset_viewport_size({
            static_cast<double>(width) * device_pixel_ratio,
            static_cast<double>(height) * device_pixel_ratio,
        });
        return;
    }
}

void LbTabSendMouse(LbApp* app, uint64_t tab_id, LbMouseEventType type, double x, double y, LbMouseButton button, LbMouseButton buttons, int32_t click_count, double wheel_delta_y)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    // CSS pixels in, device pixels out: the same conversion LbTabSetViewport applies to the viewport, so a click lands
    // where the user sees the cursor regardless of the display's scale.
    auto const ratio = tab->device_pixel_ratio;
    Web::DevicePixelPoint position {
        static_cast<double>(x) * ratio,
        static_cast<double>(y) * ratio,
    };

    Web::MouseEvent event {
        .type = [&] {
            switch (type) {
            case LbMouseEventDown:
                return Web::MouseEvent::Type::MouseDown;
            case LbMouseEventUp:
                return Web::MouseEvent::Type::MouseUp;
            case LbMouseEventMove:
                return Web::MouseEvent::Type::MouseMove;
            case LbMouseEventWheel:
                return Web::MouseEvent::Type::MouseWheel;
            }
            return Web::MouseEvent::Type::MouseMove;
        }(),
        .position = position,
        // The engine uses this only for synthesizing events it delivers itself, so mirroring position is correct here.
        .screen_position = position,
        .button = static_cast<Web::UIEvents::MouseButton>(button),
        .buttons = static_cast<Web::UIEvents::MouseButton>(buttons),
        .wheel_delta_y = wheel_delta_y,
        .browser_data = nullptr,
    };

    if (type == LbMouseEventWheel) {
        // There is no WheelEvent: a wheel is a MouseEvent, and the tick fields are derived exactly the way the platform
        // backends derive them. Leaving them zero makes the compositor treat the scroll as something other than precise.
        event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(wheel_delta_y, Web::WheelDeltaPrecision::Precise);
        event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;
    } else {
        event.click_count = click_count;
        // A down/up pair with no movement is what the engine reads as a click; the second click of a double-click
        // reuses click_count = 2, which the host counts because the engine does not.
        if (type == LbMouseEventDown)
            tab->click_count = click_count;
    }

    tab->view->enqueue_input_event(move(event));
}

void LbTabSendKey(LbApp* app, uint64_t tab_id, int32_t key, uint32_t code_point, uint32_t modifiers, int is_key_down)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    auto type = is_key_down ? Web::KeyEvent::Type::KeyDown : Web::KeyEvent::Type::KeyUp;
    Web::KeyEvent event {
        .type = type,
        .key = static_cast<Web::UIEvents::KeyCode>(key),
        .modifiers = static_cast<Web::UIEvents::KeyModifier>(modifiers),
        .code_point = code_point,
        .repeat = false,
        // The one field a host would never set by accident and cannot omit without a silent no-op: without it the key is
        // delivered, handled, and inserts nothing. Text goes in on keydown, never on keyup.
        .should_insert_text = is_key_down && code_point != 0,
        .browser_data = nullptr,
    };

    tab->view->enqueue_input_event(move(event));
}

void LbTabTypeText(LbApp* app, uint64_t tab_id, char const* utf8, size_t len)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab || !utf8)
        return;

    // Decode UTF-8 to code points here rather than in the host: the ABI is C, and a Swift caller passing a Swift String
    // is handing over exactly these bytes anyway.
    auto view = Utf8View { StringView { utf8, len } };
    for (auto cp : view) {
        Web::KeyEvent event {
            .type = Web::KeyEvent::Type::KeyDown,
            .key = Web::UIEvents::KeyCode::Key_Invalid,
            .modifiers = Web::UIEvents::KeyModifier::Mod_None,
            .code_point = cp,
            .repeat = false,
            .should_insert_text = true,
            .browser_data = nullptr,
        };
        tab->view->enqueue_input_event(move(event));
    }
}

}
