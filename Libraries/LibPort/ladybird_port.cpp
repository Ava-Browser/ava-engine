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
#include <LibCore/File.h>
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
#include <LibWebView/Menu.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
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

    // before_unload is the one dialog the engine answers through a closure it hands us, not through a ViewImplementation
    // method, so the completion has to be stored until the host replies. Every other dialog is answered by calling into
    // the view, which keeps its own pending state.
    Function<void(bool)> pending_before_unload;
};

}

struct LbApp {
    // OwnPtr, not NonnullOwnPtr: LbApp is allocated by plain new, so the kmalloc-tag static assert would fire otherwise.
    OwnPtr<PortApplication> application;
    Vector<NonnullOwnPtr<Tab>> tabs;
    LbCallbacks callbacks {};
    CFRunLoopTimerRef pump_timer { nullptr };

    LbSearchPolicy search_policy { LbSearchPolicyUnconfiguredFallback };
    Optional<String> chosen_provider_name;
    LbSearchMigrationSnapshot migration_snapshot {};

    // Tabs are declared AFTER the application, so they are destroyed BEFORE it: each tab's HeadlessWebView has to go
    // away while the application owning its WebContent client is still alive.
};

namespace {

static LbApp* s_active_app = nullptr;

static bool is_valid_live_app(LbApp* app)
{
    return app != nullptr && app == s_active_app;
}

static bool is_valid_utf8_without_embedded_nul(char const* str, size_t len)
{
    if (!str && len > 0)
        return false;
    if (len == 0)
        return true;
    if (memchr(str, '\0', len) != nullptr)
        return false;
    Utf8View view { StringView { str, len } };
    return view.validate();
}

static void capture_migration_snapshot(LbApp& app)
{
    app.migration_snapshot.struct_size = sizeof(LbSearchMigrationSnapshot);
    app.migration_snapshot.api_version = LB_PORT_API_VERSION;
    app.migration_snapshot.provenance = LbSearchProvenanceUnconfiguredAbsent;
    app.migration_snapshot.selected_provider_name = nullptr;
    app.migration_snapshot.selected_provider_name_len = 0;
    app.migration_snapshot.suggestions_enabled = 0;
    app.migration_snapshot.has_legacy_autocomplete = 0;

    if (!WebView::Application::is_initialized())
        return;

    auto settings_path = LexicalPath::join(WebView::Application::the().profile().paths().config, "Settings.json"sv).string();
    auto file_result = Core::File::open(settings_path.view(), Core::File::OpenMode::Read);
    if (file_result.is_error()) {
        app.migration_snapshot.provenance = LbSearchProvenanceUnconfiguredAbsent;
        return;
    }

    auto content_result = file_result.value()->read_until_eof();
    if (content_result.is_error()) {
        app.migration_snapshot.provenance = LbSearchProvenanceUnconfiguredAbsent;
        return;
    }

    auto json_result = JsonValue::from_string(StringView { content_result.value().bytes() });
    if (json_result.is_error()) {
        app.migration_snapshot.provenance = LbSearchProvenanceMalformed;
        return;
    }

    if (!json_result.value().is_object()) {
        app.migration_snapshot.provenance = LbSearchProvenanceMalformed;
        return;
    }

    auto const& obj = json_result.value().as_object();
    auto search_engine_val = obj.get("searchEngine"sv);
    if (!search_engine_val.has_value()) {
        app.migration_snapshot.provenance = LbSearchProvenanceUnconfiguredAbsent;
        return;
    }

    if (!search_engine_val->is_object()) {
        app.migration_snapshot.provenance = LbSearchProvenanceMalformed;
        return;
    }

    auto const& search_obj = search_engine_val->as_object();

    if (auto auto_val = obj.get("autocompleteEngine"sv); auto_val.has_value() && auto_val->is_object()) {
        if (auto name_val = auto_val->as_object().get_string("name"sv); name_val.has_value() && !name_val->is_empty())
            app.migration_snapshot.has_legacy_autocomplete = 1;
    }

    if (auto sugg_val = search_obj.get("suggestions"sv); sugg_val.has_value() && sugg_val->is_bool()) {
        app.migration_snapshot.suggestions_enabled = sugg_val->as_bool() ? 1 : 0;
    }

    auto selected_engine = search_obj.get("engine"sv);
    if (selected_engine.has_value()) {
        if (selected_engine->is_null()) {
            app.migration_snapshot.provenance = LbSearchProvenanceAmbiguousNull;
            return;
        }
        if (selected_engine->is_string()) {
            auto name = selected_engine->as_string();
            if (!name.is_empty()) {
                app.migration_snapshot.provenance = LbSearchProvenanceConfiguredCurrent;
                auto bytes = name.bytes();
                auto* buf = static_cast<char*>(malloc(bytes.size()));
                if (buf) {
                    memcpy(buf, bytes.data(), bytes.size());
                    app.migration_snapshot.selected_provider_name = buf;
                    app.migration_snapshot.selected_provider_name_len = bytes.size();
                }
                return;
            }
            app.migration_snapshot.provenance = LbSearchProvenanceAmbiguousNull;
            return;
        }
        app.migration_snapshot.provenance = LbSearchProvenanceMalformed;
        return;
    }

    auto legacy_name = search_obj.get("name"sv);
    if (legacy_name.has_value()) {
        if (legacy_name->is_null()) {
            app.migration_snapshot.provenance = LbSearchProvenanceAmbiguousNull;
            return;
        }
        if (legacy_name->is_string()) {
            auto name = legacy_name->as_string();
            if (!name.is_empty()) {
                app.migration_snapshot.provenance = LbSearchProvenanceConfiguredLegacy;
                auto bytes = name.bytes();
                auto* buf = static_cast<char*>(malloc(bytes.size()));
                if (buf) {
                    memcpy(buf, bytes.data(), bytes.size());
                    app.migration_snapshot.selected_provider_name = buf;
                    app.migration_snapshot.selected_provider_name_len = bytes.size();
                }
                return;
            }
            app.migration_snapshot.provenance = LbSearchProvenanceAmbiguousNull;
            return;
        }
        app.migration_snapshot.provenance = LbSearchProvenanceMalformed;
        return;
    }

    app.migration_snapshot.provenance = LbSearchProvenanceUnconfiguredAbsent;
}

void set_up_isolation(ByteString& scratch_directory)
{
    if (Core::Environment::has("XDG_CONFIG_HOME"sv))
        return;
    scratch_directory = ByteString::formatted("{}/Ladybird-Port-{}", Core::StandardPaths::tempfile_directory(), getpid());
    MUST(Core::Directory::create(scratch_directory, Core::Directory::CreateDirectories::Yes));
    VERIFY(setenv("XDG_CONFIG_HOME", scratch_directory.characters(), 1) == 0);
}

// Utf16String -> the ABI's char*/len. Narrowed here because the header speaks bytes and the engine speaks UTF-16;
// `to_byte_string()` is the only correct narrowing and getting it wrong compiles either way (see Traps.md).
void dispatch_dialog_text(void (*callback)(void*, uint64_t, char const*, size_t), void* context, uint64_t tab, Utf16String const& text)
{
    if (!callback)
        return;
    auto narrow = text.to_byte_string();
    callback(context, tab, narrow.characters(), narrow.length());
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

    capture_migration_snapshot(*app);
    s_active_app = app;
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
    if (s_active_app == app)
        s_active_app = nullptr;
    LbAppStopPump(app);
    if (app->migration_snapshot.selected_provider_name) {
        free(app->migration_snapshot.selected_provider_name);
        app->migration_snapshot.selected_provider_name = nullptr;
        app->migration_snapshot.selected_provider_name_len = 0;
    }
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

    // The four blocking dialogs. Installed only when the host supplied the matching callback: an absent callback makes
    // the engine auto-answer, which is safe, whereas present-but-silent hangs the page. That asymmetry is why these are
    // guarded rather than always installed.
    if (app->callbacks.on_request_alert) {
        view->on_request_alert = [app, id](Utf16String const& message) {
            dispatch_dialog_text(app->callbacks.on_request_alert, app->callbacks.context, id, message);
        };
    }
    if (app->callbacks.on_request_confirm) {
        view->on_request_confirm = [app, id](Utf16String const& message) {
            dispatch_dialog_text(app->callbacks.on_request_confirm, app->callbacks.context, id, message);
        };
    }
    if (app->callbacks.on_request_prompt) {
        view->on_request_prompt = [app, id](Utf16String const& message, Utf16String const& default_text) {
            if (!app->callbacks.on_request_prompt)
                return;
            auto narrow_message = message.to_byte_string();
            auto narrow_default = default_text.to_byte_string();
            app->callbacks.on_request_prompt(
                app->callbacks.context, id,
                narrow_message.characters(), narrow_message.length(),
                narrow_default.characters(), narrow_default.length());
        };
    }
    if (app->callbacks.on_request_before_unload) {
        view->on_request_before_unload = [app, id](String const& source, Function<void(bool)> on_complete) {
            // The closure is the engine's own completion; store it so LbTabAnswerBeforeUnload can invoke it. A second
            // request replaces the first, which is what the engine does too (one pending dialog per page).
            for (auto const& candidate : app->tabs) {
                if (candidate->id == id) {
                    candidate->pending_before_unload = move(on_complete);
                    break;
                }
            }
            auto narrow_source = source.to_byte_string();
            if (app->callbacks.on_request_before_unload)
                app->callbacks.on_request_before_unload(app->callbacks.context, id, narrow_source.characters(), narrow_source.length());
        };
    }

    app->tabs.append(adopt_own(*new Tab { id, move(view), 1.0, 0, nullptr }));
    return id;
}

void LbTabAnswerAlert(LbApp* app, uint64_t tab)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    target->view->alert_closed();
}

void LbTabAnswerConfirm(LbApp* app, uint64_t tab, int accept)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    target->view->confirm_closed(accept != 0);
}

void LbTabAnswerPrompt(LbApp* app, uint64_t tab, char const* response, size_t len)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    // NULL means Cancel. An empty string means "accepted with no text" — a different outcome the engine distinguishes,
    // so it is not folded into the null case.
    if (!response) {
        target->view->prompt_closed(OptionalNone {});
        return;
    }
    auto text = Utf16String::from_utf8(StringView { response, len });
    target->view->prompt_closed(text);
}

void LbTabAnswerBeforeUnload(LbApp* app, uint64_t tab, int accept)
{
    auto* target = find_tab(app, tab);
    if (!target)
        return;
    // Take the completion before invoking it: the engine may issue another request from inside the callback, and
    // leaving a stale copy here would answer that one with this outcome.
    auto completion = move(target->pending_before_unload);
    target->pending_before_unload = nullptr;
    if (completion)
        completion(accept != 0);
}

void LbTabDestroy(LbApp* app, uint64_t tab)
{
    if (!app)
        return;
    // Unlinking from the vector is what stops callbacks and lookups from finding it; the tab's own view is destroyed by
    // the OwnPtr going away, before the application that owns its WebContent client does.
    for (auto& candidate : app->tabs) {
        if (candidate->id != tab)
            continue;
        app->tabs.remove_first_matching([&](auto const& entry) { return entry.ptr() == candidate.ptr(); });
        return;
    }
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

int LbResolveUserInput(char const* utf8, size_t len, char** out_url, size_t* out_len)
{
    if (!utf8 || !out_url || !out_len)
        return 0;
    *out_url = nullptr;
    *out_len = 0;

    Optional<WebView::SearchEngine> search_engine;
    if (s_active_app != nullptr) {
        switch (s_active_app->search_policy) {
        case LbSearchPolicyDisabled:
        case LbSearchPolicyUnavailable:
            search_engine = OptionalNone {};
            break;
        case LbSearchPolicyEnabled:
            if (s_active_app->chosen_provider_name.has_value() && WebView::Application::is_initialized()) {
                search_engine = WebView::Application::settings().find_search_engine_by_name(*s_active_app->chosen_provider_name);
                if (!search_engine.has_value()) {
                    s_active_app->search_policy = LbSearchPolicyUnavailable;
                    search_engine = OptionalNone {};
                }
            } else {
                search_engine = OptionalNone {};
            }
            break;
        case LbSearchPolicyUnconfiguredFallback:
            if (auto builtins = WebView::builtin_search_engines(); !builtins.is_empty())
                search_engine = builtins[0];
            break;
        }
    } else {
        if (auto builtins = WebView::builtin_search_engines(); !builtins.is_empty())
            search_engine = builtins[0];
    }

    auto url = WebView::sanitize_url(StringView { utf8, len }, search_engine);
    if (!url.has_value())
        return 0;

    auto serialized = url->serialize();
    auto bytes = serialized.bytes();
    // malloc, not the engine's allocator: the host is a different toolchain and frees with LbStringFree, which must not
    // reach into an allocator it does not share.
    auto* buffer = static_cast<char*>(malloc(bytes.size()));
    if (!buffer)
        return 0;
    memcpy(buffer, bytes.data(), bytes.size());

    *out_url = buffer;
    *out_len = bytes.size();
    return 1;
}

void LbStringFree(char* string)
{
    free(string);
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

void LbTabTraverseHistory(LbApp* app, uint64_t tab_id, int32_t delta)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return;

    // The ABI is the two buttons a browser has, so anything else is one of them rather than an arbitrary jump.
    auto direction = delta < 0 ? -1 : 1;
    tab->view->traverse_the_history_by_delta(direction);
}

int LbTabCanTraverseHistory(LbApp* app, uint64_t tab_id, int32_t delta)
{
    auto* tab = find_tab(app, tab_id);
    if (!tab)
        return 0;

    // The engine's actions already encode whether there is anywhere to go, and LibWebView keeps them current as
    // navigation happens. Deriving this from history step counts instead would duplicate a rule that can drift.
    auto& action = delta < 0 ? tab->view->navigate_back_action() : tab->view->navigate_forward_action();
    return action.enabled() ? 1 : 0;
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

LbResult LbSearchVisitCatalog(LbApp* app, void* context, LbSearchCatalogVisitor visitor)
{
    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;
    if (!visitor)
        return LbResultErrorNullArg;

    for (auto const& builtin : WebView::builtin_search_engines()) {
        auto name_bytes = builtin.name.bytes();
        auto query_bytes = builtin.query_url.bytes();
        ReadonlyBytes suggest_bytes {};
        if (builtin.suggestions.has_value())
            suggest_bytes = builtin.suggestions->query_url.bytes();

        LbSearchEngineDescriptor descriptor {
            .struct_size = sizeof(LbSearchEngineDescriptor),
            .api_version = LB_PORT_API_VERSION,
            .is_custom = 0,
            .name = reinterpret_cast<char const*>(name_bytes.data()),
            .name_len = name_bytes.size(),
            .query_url = reinterpret_cast<char const*>(query_bytes.data()),
            .query_url_len = query_bytes.size(),
            .suggestions_url = suggest_bytes.is_empty() ? nullptr : reinterpret_cast<char const*>(suggest_bytes.data()),
            .suggestions_url_len = suggest_bytes.size(),
        };

        if (visitor(context, &descriptor) != 0)
            return LbResultOk;
    }

    if (WebView::Application::is_initialized()) {
        auto serialized = WebView::Application::settings().serialize_json();
        if (serialized.is_object()) {
            if (auto se_val = serialized.as_object().get_object("searchEngine"sv); se_val.has_value()) {
                if (auto customs = se_val->get_array("custom"sv); customs.has_value()) {
                    for (size_t i = 0; i < customs->size(); ++i) {
                        auto const& custom_val = customs->at(i);
                        if (!custom_val.is_object())
                            continue;
                        auto const& custom_obj = custom_val.as_object();
                        auto name = custom_obj.get_string("name"sv).value_or({});
                        auto url = custom_obj.get_string("url"sv).value_or({});
                        auto sugg = custom_obj.get_string("suggestionsUrl"sv).value_or({});

                        auto name_bytes = name.bytes();
                        auto url_bytes = url.bytes();
                        auto sugg_bytes = sugg.bytes();

                        LbSearchEngineDescriptor descriptor {
                            .struct_size = sizeof(LbSearchEngineDescriptor),
                            .api_version = LB_PORT_API_VERSION,
                            .is_custom = 1,
                            .name = reinterpret_cast<char const*>(name_bytes.data()),
                            .name_len = name_bytes.size(),
                            .query_url = reinterpret_cast<char const*>(url_bytes.data()),
                            .query_url_len = url_bytes.size(),
                            .suggestions_url = sugg_bytes.is_empty() ? nullptr : reinterpret_cast<char const*>(sugg_bytes.data()),
                            .suggestions_url_len = sugg_bytes.size(),
                        };

                        if (visitor(context, &descriptor) != 0)
                            return LbResultOk;
                    }
                }
            }
        }
    }

    return LbResultOk;
}

LbResult LbSearchSetProvider(LbApp* app, char const* name, size_t name_len)
{
    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;
    if (!name)
        return LbResultErrorNullArg;
    if (!is_valid_utf8_without_embedded_nul(name, name_len) || name_len == 0)
        return LbResultErrorInvalidArg;

    if (!WebView::Application::is_initialized())
        return LbResultErrorEngineUnavailable;

    StringView name_view { name, name_len };
    auto engine = WebView::Application::settings().find_search_engine_by_name(name_view);
    if (!engine.has_value()) {
        app->search_policy = LbSearchPolicyUnavailable;
        app->chosen_provider_name = {};
        return LbResultErrorNotFound;
    }

    app->search_policy = LbSearchPolicyEnabled;
    app->chosen_provider_name = engine->name;
    auto suggestions_enabled = WebView::Application::settings().search_engine_settings().suggestions;
    WebView::Application::settings().set_search_engine_settings({ .engine = move(engine), .suggestions = suggestions_enabled });
    return LbResultOk;
}

LbResult LbSearchSetDisabled(LbApp* app)
{
    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;

    app->search_policy = LbSearchPolicyDisabled;
    app->chosen_provider_name = {};
    if (WebView::Application::is_initialized())
        WebView::Application::settings().set_search_engine_settings({ .engine = {}, .suggestions = false });
    return LbResultOk;
}

LbResult LbSearchSetUnconfiguredFallback(LbApp* app)
{
    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;

    app->search_policy = LbSearchPolicyUnconfiguredFallback;
    app->chosen_provider_name = {};
    if (WebView::Application::is_initialized())
        WebView::Application::settings().set_search_engine_settings({ .engine = {}, .suggestions = false });
    return LbResultOk;
}

LbResult LbSearchGetStatus(LbApp* app, LbSearchPolicy* out_policy, char** out_name, size_t* out_len)
{
    if (out_name)
        *out_name = nullptr;
    if (out_len)
        *out_len = 0;

    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;
    if (!out_policy)
        return LbResultErrorNullArg;

    *out_policy = app->search_policy;

    if (app->search_policy == LbSearchPolicyEnabled && app->chosen_provider_name.has_value() && out_name && out_len) {
        auto bytes = app->chosen_provider_name->bytes();
        auto* buf = static_cast<char*>(malloc(bytes.size()));
        if (!buf)
            return LbResultOk;
        memcpy(buf, bytes.data(), bytes.size());
        *out_name = buf;
        *out_len = bytes.size();
    }

    return LbResultOk;
}

LbResult LbSearchReadMigrationSnapshot(LbApp* app, LbSearchMigrationSnapshot* out_snapshot)
{
    if (!out_snapshot)
        return LbResultErrorNullArg;

    if (out_snapshot->struct_size < sizeof(LbSearchMigrationSnapshot))
        return LbResultErrorInvalidArg;
    if (out_snapshot->api_version != LB_PORT_API_VERSION)
        return LbResultErrorUnsupportedVersion;

    out_snapshot->provenance = LbSearchProvenanceUnconfiguredAbsent;
    out_snapshot->selected_provider_name = nullptr;
    out_snapshot->selected_provider_name_len = 0;
    out_snapshot->suggestions_enabled = 0;
    out_snapshot->has_legacy_autocomplete = 0;

    if (!is_valid_live_app(app))
        return LbResultErrorEngineUnavailable;

    out_snapshot->provenance = app->migration_snapshot.provenance;
    out_snapshot->suggestions_enabled = app->migration_snapshot.suggestions_enabled;
    out_snapshot->has_legacy_autocomplete = app->migration_snapshot.has_legacy_autocomplete;

    if (app->migration_snapshot.selected_provider_name && app->migration_snapshot.selected_provider_name_len > 0) {
        auto* buf = static_cast<char*>(malloc(app->migration_snapshot.selected_provider_name_len));
        if (buf) {
            memcpy(buf, app->migration_snapshot.selected_provider_name, app->migration_snapshot.selected_provider_name_len);
            out_snapshot->selected_provider_name = buf;
            out_snapshot->selected_provider_name_len = app->migration_snapshot.selected_provider_name_len;
        }
    }

    return LbResultOk;
}

void LbSearchFreeMigrationSnapshot(LbSearchMigrationSnapshot* snapshot)
{
    if (!snapshot)
        return;
    if (snapshot->selected_provider_name) {
        free(snapshot->selected_provider_name);
        snapshot->selected_provider_name = nullptr;
        snapshot->selected_provider_name_len = 0;
    }
}

}
