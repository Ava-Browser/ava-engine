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
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <QuartzCore/QuartzCore.h>

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

// Same accessor shape as the Step 2 probe: the bitmap members are protected, so a subclass is the sanctioned path, and
// the gate on m_has_usable_bitmap keeps a mid-swap bitmap from being presented as the front one. (R1: the handle is
// handed across; the bitmap is never cropped or cloned.)
class ProbeWebView : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<ProbeWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
    {
        auto view = adopt_own(*new ProbeWebView(move(theme), window_size));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    struct Surface {
        Gfx::SharedImageBuffer const* buffer;
        Gfx::IntSize painted_size;
    };

    Optional<Surface> current_surface() const
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
        return Surface { buffer, size };
    }

private:
    ProbeWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
        : HeadlessWebView(move(theme), window_size)
    {
    }
};

struct Stats {
    u64 pumps { 0 };
    u64 paints { 0 };
    u64 paints_after_load { 0 };
    u64 loads { 0 };
    u64 core_timer_ticks { 0 };
    bool loaded { false };
    bool done { false };

    // The LibCore timers that prove timers are not starved by the cooperative pumping.
    u64 heartbeat_ticks { 0 };
    MonotonicTime last_heartbeat {};
    i64 max_heartbeat_gap_ms { 0 };

    u64 presents { 0 };
    double max_pump_duration_ms { 0 };

    // Set when on_ready_to_paint re-enters pump() from inside pump(). A naive Swift callback that spins the loop would
    // do exactly this, so its safety is worth knowing before the shim is designed.
    bool nested_pump_ok { false };
    bool nested_pump_attempted { false };

    // Answers the last open question in the plan: does the pump keep running while the run loop is in
    // NSEventTrackingRunLoopMode (live resize, a held menu)? A default-mode timer does not, which would freeze the
    // engine for the whole gesture. Counted separately so the result cannot be confused with the main phase.
    u64 tracking_pumps { 0 };
    bool in_tracking_phase { false };

    MonotonicTime started;
    AK::Duration time_to_first_paint {};
    AK::Duration time_to_load {};
    AK::Duration time_to_paint_after_load {};
};

// Plain C callbacks for the run loop, so nothing here needs an ObjC block capture. (R2 in spirit: the production shim
// would carry an opaque context pointer through exactly this shape.)
struct RunLoopContext {
    Stats* stats;
    Core::EventLoop* event_loop;
    ProbeWebView* view;
    CALayer* layer;
    double device_pixel_ratio;
    CFRunLoopRef run_loop;
};

static RunLoopContext* s_context;

static bool present(RunLoopContext& context)
{
    auto surface = context.view->current_surface();
    if (!surface.has_value())
        return false;

    auto const& handle = surface->buffer->iosurface_handle();
    auto surface_width = static_cast<CGFloat>(handle.width());
    auto surface_height = static_cast<CGFloat>(handle.height());
    auto painted_width = static_cast<CGFloat>(surface->painted_size.width());
    auto painted_height = static_cast<CGFloat>(surface->painted_size.height());
    if (painted_width <= 0 || painted_height <= 0)
        return false;

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    context.layer.contentsScale = context.device_pixel_ratio;
    context.layer.contentsRect = CGRectMake(0, 1 - painted_height / surface_height, painted_width / surface_width, painted_height / surface_height);
    context.layer.contents = (id)handle.core_foundation_pointer();
    [CATransaction commit];

    ++context.stats->presents;
    return true;
}

// The whole point of the probe: drive LibCore from a CFRunLoopTimer on the main thread while AppKit owns the loop.
static void pump_from_run_loop_timer(void* info)
{
    auto& context = *static_cast<RunLoopContext*>(info);
    auto before = MonotonicTime::now();
    // DontWaitForEvents maps to poll(timeout: 0), so this returns immediately instead of blocking the AppKit loop.
    context.event_loop->pump(Core::EventLoop::WaitMode::PollForEvents);
    auto elapsed = MonotonicTime::now() - before;
    if (elapsed.to_milliseconds() > static_cast<i64>(context.stats->max_pump_duration_ms))
        context.stats->max_pump_duration_ms = static_cast<double>(elapsed.to_milliseconds());
    ++context.stats->pumps;
    if (context.stats->in_tracking_phase)
        ++context.stats->tracking_pumps;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNativeEventLoop-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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
    auto view = ProbeWebView::create(theme, { 1200, 800 });

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];

    auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 600, 400)
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    window.title = @"Ladybird cooperative event loop probe";
    auto* host_view = window.contentView;
    host_view.wantsLayer = YES;
    auto* layer = [CALayer layer];
    layer.opaque = YES;
    layer.actions = @ { @"contents" : NSNull.null, @"contentsScale" : NSNull.null, @"contentsRect" : NSNull.null, @"bounds" : NSNull.null };
    [host_view.layer addSublayer:layer];
    layer.frame = host_view.bounds;
    [window makeKeyAndOrderFront:nil];

    double const device_pixel_ratio = window.backingScaleFactor;

    auto stats = Stats {};
    stats.started = MonotonicTime::now();

    s_context = new RunLoopContext { &stats, &Core::EventLoop::current(), view.ptr(), layer, device_pixel_ratio, CFRunLoopGetCurrent() };

    CFRunLoopTimerContext timer_context { 0, s_context, nullptr, nullptr, nullptr };
    auto* pump_timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent(),
        0.002, // 2ms
        0,
        0,
        [](CFRunLoopTimerRef, void* info) { pump_from_run_loop_timer(info); },
        &timer_context);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), pump_timer, kCFRunLoopCommonModes);

    // LibCore's own timers, to prove they are serviced by the pumped loop.
    auto heartbeat = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [&] {
        auto now = MonotonicTime::now();
        if (stats.heartbeat_ticks > 0) {
            auto gap = (now - stats.last_heartbeat).to_milliseconds();
            if (gap > stats.max_heartbeat_gap_ms)
                stats.max_heartbeat_gap_ms = gap;
        }
        stats.last_heartbeat = now;
        ++stats.heartbeat_ticks;
    });
    heartbeat->start();

    // Deliberately short: the probe finishes in a few hundred ms, so a longer one-shot would not fire before we stop
    // and the check would be measuring the probe's own runtime rather than timer latency.
    auto stall_detector = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(100), [&] { ++stats.core_timer_ticks; });
    stall_detector->start();

    view->on_load_finish = [&](auto const& url) {
        auto now = MonotonicTime::now();
        outln("LOADED #{}: {} (t={}ms)", stats.loads + 1, url, (now - stats.started).to_milliseconds());
        ++stats.loads;
        if (!stats.loaded) {
            stats.loaded = true;
            stats.time_to_load = now - stats.started;
        }
    };

    view->on_ready_to_paint = [&] {
        auto now = MonotonicTime::now();
        ++stats.paints;
        if (stats.paints == 1)
            stats.time_to_first_paint = now - stats.started;

        auto presented = present(*s_context);

        // The gate from the plan: a paint arriving AFTER the load finished, never paint #1, which is the early blank.
        if (stats.loaded) {
            ++stats.paints_after_load;
            if (stats.paints_after_load == 1)
                stats.time_to_paint_after_load = now - stats.started;
        }

        outln("PAINT #{} (t={}ms, post_load={}, presented={})", stats.paints, (now - stats.started).to_milliseconds(), stats.loaded, presented);

        // The reentrancy check: we are inside ThreadEventQueue::process() here, which already moved the queue out, so a
        // nested process() sees only newly queued events instead of redispatching the outer batch.
        if (!stats.nested_pump_attempted) {
            stats.nested_pump_attempted = true;
            s_context->event_loop->pump(Core::EventLoop::WaitMode::PollForEvents);
            stats.nested_pump_ok = true;
        }

        // R3: presentation is demand-driven, so a static page yields exactly ONE paint after load and waiting for three
        // would hang on a settled page. The plan's gate is "a paint after on_load_finish", which is this.
        if (stats.paints_after_load == 1) {
            outln("PHASE 2: settled page; loading a second page to prove paints keep flowing");
            s_context->view->load(URL::Parser::basic_parse("data:text/html,<body style='margin:0'>second</body>"sv).release_value());
            return;
        }

        // The second page's paint proves the loop is still live and that a change still publishes a frame.
        if (stats.paints_after_load >= 2 && stats.loads >= 2) {
            stats.done = true;
            CFRunLoopStop(s_context->run_loop);
        }
    };

    // A watchdog outside LibCore entirely: if the pump timer starves, this still fires because AppKit owns the loop.
    auto* watchdog = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 60,
        0,
        0,
        0,
        [](CFRunLoopTimerRef, void*) {
            outln("WATCHDOG: timed out");
            CFRunLoopStop(CFRunLoopGetCurrent());
        },
        nullptr);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), watchdog, kCFRunLoopCommonModes);

    view->load(URL::Parser::basic_parse("https://example.com"sv).release_value());

    outln("RUN: entering CFRunLoopRun(), pumping LibCore from a 2ms CFRunLoopTimer");
    CFRunLoopRun();

    auto total = MonotonicTime::now() - stats.started;

    // ---------------------------------------------------------------------------------------------
    // The last open question: does the pump survive NSEventTrackingRunLoopMode?
    //
    // Live window resize, a held menu and drag-tracking all run the loop in tracking mode, where a timer registered in
    // the default mode never fires. If the pump starved there, the engine would freeze for the whole gesture. The pump
    // timer above is registered in kCFRunLoopCommonModes, so it should keep running; this phase proves it instead of
    // assuming it, and needs no human to hold a menu open.
    // ---------------------------------------------------------------------------------------------
    stats.in_tracking_phase = true;
    auto tracking_start = MonotonicTime::now();
    // Drive the real tracking mode, exactly as a live resize does. NSRunLoopCommonModes would not isolate it.
    CFRunLoopRunInMode((CFRunLoopMode)(__bridge CFStringRef)NSEventTrackingRunLoopMode, 0.3, false);
    stats.in_tracking_phase = false;
    auto tracking_elapsed = MonotonicTime::now() - tracking_start;

    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), pump_timer, kCFRunLoopCommonModes);
    CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), watchdog, kCFRunLoopCommonModes);
    heartbeat->stop();
    stall_detector->stop();

    outln("");
    outln("=== COOPERATIVE EVENT LOOP REPORT ===");
    outln("pumps (CFRunLoopTimer invocations): {}", stats.pumps);
    outln("paints: {} (post-load: {})", stats.paints, stats.paints_after_load);
    outln("presents: {}", stats.presents);
    outln("loads (IPC round-trips): {}", stats.loads);
    outln("libcore heartbeat ticks (10ms timer): {} (expected ~{})", stats.heartbeat_ticks, total.to_milliseconds() / 10);
    outln("libcore heartbeat max gap: {}ms (nominal 10ms)", stats.max_heartbeat_gap_ms);
    outln("libcore 100ms one-shot fired: {}", stats.core_timer_ticks > 0);
    outln("max single pump duration: {}ms", stats.max_pump_duration_ms);
    outln("nested pump from paint callback survived: {}", stats.nested_pump_ok);
    outln("pumps during {}ms of NSEventTrackingRunLoopMode: {}", tracking_elapsed.to_milliseconds(), stats.tracking_pumps);
    outln("time to first paint: {}ms", stats.time_to_first_paint.to_milliseconds());
    outln("time to load finish: {}ms", stats.time_to_load.to_milliseconds());
    outln("time to first paint after load: {}ms", stats.time_to_paint_after_load.to_milliseconds());
    outln("total: {}ms", total.to_milliseconds());

    // A 10ms timer must not be starved. Measured against both the nominal gap (allow 5x) and the tick count the
    // elapsed time should have produced (allow 30% short, since the loop starts and stops mid-interval).
    auto expected_ticks = total.to_milliseconds() / 10;
    bool timers_ok = stats.core_timer_ticks > 0
        && stats.max_heartbeat_gap_ms < 50
        && static_cast<double>(stats.heartbeat_ticks) >= static_cast<double>(expected_ticks) * 0.7;
    bool ipc_ok = stats.loads > 0;
    bool paints_ok = stats.paints_after_load >= 2 && stats.loads >= 2;
    bool presented_ok = stats.presents >= 3;

    // The pump must keep firing during tracking mode. The timer is 2ms, so 300ms should give ~150 pumps; allow a wide
    // margin for scheduling while still failing loudly if the timer starved (which would give 0).
    auto tracking_expected = tracking_elapsed.to_milliseconds() / 2;
    bool tracking_ok = stats.tracking_pumps >= static_cast<u64>(tracking_expected) / 3;

    outln("");
    outln("timers: {}  ipc: {}  paints: {}  presentation: {}  tracking-mode pump: {}", timers_ok, ipc_ok, paints_ok, presented_ok, tracking_ok);

    if (!timers_ok || !ipc_ok || !paints_ok) {
        outln("FAIL: cooperative pumping did not sustain all three");
        return 1;
    }
    if (!tracking_ok) {
        outln("FAIL: the pump starved in NSEventTrackingRunLoopMode (live resize / held menu would freeze the page)");
        return 1;
    }

    outln("PASS: timers, IPC and paint callbacks all work while AppKit owns the main run loop");
    outln("PASS: the pump keeps running in NSEventTrackingRunLoopMode, so a live resize or held menu cannot freeze it");
    return 0;
}
