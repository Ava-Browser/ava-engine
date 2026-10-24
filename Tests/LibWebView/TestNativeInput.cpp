/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

// There is no WheelEvent: a wheel is a MouseEvent with Type::MouseWheel. The tick fields are derived from the pixel
// delta, exactly as the platform backends do it.
void scroll_wheel_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position, double wheel_delta_y)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = position,
        .screen_position = position,
        .wheel_delta_y = wheel_delta_y,
        .wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(wheel_delta_y, Web::WheelDeltaPrecision::Precise),
        .wheel_delta_precision = Web::WheelDeltaPrecision::Precise,
        .browser_data = nullptr,
    });
}

void click_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

// A blocked spin would hang the probe, so every wait is bounded.
bool spin_until_or_timeout(Function<bool()> condition, AK::Duration timeout)
{
    auto deadline = MonotonicTime::now() + timeout;
    while (!condition()) {
        if (MonotonicTime::now() > deadline)
            return false;
        Core::EventLoop::current().pump();
    }
    return true;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNativeInput-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] { MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed)); };
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 1200, 800 });

    size_t loads_finished = 0;
    Optional<URL::URL> last_loaded_url;
    Utf16String last_title;

    view->on_load_finish = [&](auto const& url) {
        outln("LOADED #{}: {}", loads_finished + 1, url);
        ++loads_finished;
        last_loaded_url = url;
    };
    view->on_title_change = [&](auto const& title) { last_title = title; };

    // Reads a value out of the page. The alert round-trip is the pattern TestCrashOverlay uses.
    auto evaluate = [&](StringView source) -> Optional<JsonValue> {
        Optional<JsonValue> result;
        view->on_request_alert = [&](Utf16String const& value) {
            result = MUST(JsonValue::from_string(value.to_utf8()));
            view->alert_closed();
        };
        view->run_javascript(MUST(String::formatted("alert(JSON.stringify(({}) ?? null))", source)));
        auto responded = spin_until_or_timeout([&] { return result.has_value(); }, AK::Duration::from_seconds(15));
        view->on_request_alert = nullptr;
        if (!responded)
            return {};
        return result.release_value();
    };

    auto const wheel_position = Web::DevicePixelPoint { 600, 400 };

    // pump() defaults to WaitForEvents, which blocks in poll() until a timer expires or an IPC message arrives. With no
    // timer pending that is forever, and the timeout checks below would never run. A heartbeat timer bounds the wait.
    int heartbeats = 0;
    auto heartbeat = Core::Timer::create_repeating(AK::Duration::from_milliseconds(20), [&] { ++heartbeats; });
    heartbeat->start();
    ScopeGuard stop_heartbeat = [&] { heartbeat->stop(); };

    // Navigating before the initial about:blank load completes drops the navigation.
    if (!spin_until_or_timeout([&] { return loads_finished >= 1; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the initial load never finished");
        return 1;
    }

    // A page three viewports tall that reports where it scrolled to through its title, so the probe can wait on the
    // scroll landing instead of on a fixed delay.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>unscrolled</title>
<body style="margin:0;height:3000px">
<script>
addEventListener("scroll", () => { document.title = "scrolled to " + scrollY; });
</script>
</body>)~~~"sv);

    if (!spin_until_or_timeout([&] { return loads_finished >= 2; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the scroll page never finished loading");
        return 1;
    }
    VERIFY(last_title == "unscrolled"sv);

    auto scroll_y_before = evaluate("window.scrollY"sv);
    outln("SCROLLY BEFORE: {}", scroll_y_before.has_value() ? scroll_y_before->serialized() : "unreadable"_string);

    scroll_wheel_at(*view, wheel_position, 120);

    // Wheel is routed through the compositor, which may scroll asynchronously and consume the event. Waiting on the
    // page's own report is the only way to tell a real scroll from a dropped one.
    auto scrolled = spin_until_or_timeout([&] { return last_title.starts_with("scrolled to "sv); }, AK::Duration::from_seconds(20));
    auto settled = spin_until_or_timeout([&] { return view->pending_input_event_count_for_testing() == 0; }, AK::Duration::from_seconds(20));
    outln("WHEEL: page_reported_scroll={} input_settled={} title={}", scrolled, settled, last_title);

    auto scroll_y_after = evaluate("window.scrollY"sv);
    auto scroll_y = scroll_y_after.has_value() ? scroll_y_after->get_double_with_precision_loss().value_or(-1) : -1;
    outln("SCROLLY AFTER: {}", scroll_y);

    if (!scrolled || scroll_y <= 0) {
        outln("FAIL: the wheel event did not scroll the page");
        return 1;
    }

    // A link filling the viewport, so the click position need not be measured.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>click-me</title>
<body style="margin:0">
<a href="https://example.com/" style="display:block;width:100vw;height:100vh">go</a>
</body>)~~~"sv);

    if (!spin_until_or_timeout([&] { return loads_finished >= 3; }, AK::Duration::from_seconds(30))) {
        outln("FAIL: the click page never finished loading");
        return 1;
    }

    outln("READY: loading a paint-free state, now clicking the link");
    auto loads_before_click = loads_finished;
    click_at(*view, wheel_position);

    auto navigated = spin_until_or_timeout([&] { return loads_finished > loads_before_click; }, AK::Duration::from_seconds(45));
    outln("CLICK: navigated={} loads={} url={}", navigated, loads_finished, last_loaded_url.has_value() ? last_loaded_url->serialize() : "none"_string);

    if (!navigated) {
        outln("FAIL: clicking the link did not navigate");
        return 1;
    }

    outln("PASS: wheel scrolls the page and a click on a link navigates");
    return 0;
}
