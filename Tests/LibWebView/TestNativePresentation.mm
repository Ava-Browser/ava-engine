/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

#import <AppKit/AppKit.h>
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

// The bitmap members are protected, so a subclass is the sanctioned access path. This mirrors
// WebContentView::current_paintable() (UI/Qt/WebContentView.cpp), including its gate on m_has_usable_bitmap, which
// keeps a bitmap that is mid-swap from being presented as the front one.
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

void save_png(Gfx::Bitmap const& bitmap, StringView path)
{
    auto encoded = MUST(Gfx::PNGWriter::encode(bitmap));
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(encoded));
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNativePresentation-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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
    window.title = @"Ladybird native presentation probe";
    auto* host_view = window.contentView;
    host_view.wantsLayer = YES;
    auto* layer = [CALayer layer];
    layer.opaque = YES;
    // Without this every new frame cross-fades in and geometry changes slide.
    layer.actions = @ { @"contents" : NSNull.null, @"contentsScale" : NSNull.null, @"contentsRect" : NSNull.null, @"bounds" : NSNull.null };
    [host_view.layer addSublayer:layer];
    layer.frame = host_view.bounds;
    [window makeKeyAndOrderFront:nil];

    // NOT hardcoded: last_painted_size is in device pixels, so the scale must be the real backbuffer ratio.
    double const device_pixel_ratio = window.backingScaleFactor;
    outln("DEVICE PIXEL RATIO: {}", device_pixel_ratio);

    auto present = [&] {
        auto surface = view->current_surface();
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
        layer.contentsScale = device_pixel_ratio;
        // The surface can be larger than what was painted into it, so show only the painted part, anchored top-left.
        // NSView's layer is not flipped, so the unit rectangle's y-origin is measured from the bottom.
        layer.contentsRect = CGRectMake(0, 1 - painted_height / surface_height, painted_width / surface_width, painted_height / surface_height);
        layer.contents = (id)handle.core_foundation_pointer();
        [CATransaction commit];

        outln("PRESENT: painted {}x{} from {}x{} surface", surface->painted_size.width(), surface->painted_size.height(), handle.width(), handle.height());
        return true;
    };

    auto capture_window = [&](StringView path) {
        [CATransaction flush];
        [window display];
        auto command = ByteString::formatted("/usr/sbin/screencapture -x -l {} {}", window.windowNumber, path);
        auto exit_code = system(command.characters());
        outln("WINDOW CAPTURE: exit={} ({})", WEXITSTATUS(exit_code), path);
    };

    // Returns the colour at (10, 10) of the surface that is on screen right now.
    auto dump_surface = [&](StringView path) {
        auto surface = view->current_surface();
        if (!surface.has_value()) {
            outln("DUMP {}: no surface", path);
            return Optional<Gfx::Color> {};
        }
        auto const& handle = surface->buffer->iosurface_handle();
        auto bitmap = surface->buffer->bitmap();

        Optional<Gfx::Color> sampled;
        if (handle.lock_read_only()) {
            sampled = bitmap->get_pixel(10, 10);
            outln("SURFACE PIXEL (10,10) of {}: #{:02x}{:02x}{:02x}", path, sampled->red(), sampled->green(), sampled->blue());
            handle.unlock_read_only();
        }
        // NB: cropped() with the full rect calls clone(), which asserts a pitch the IOSurface-backed bitmap need not
        //     have, so the whole surface is encoded as-is.
        save_png(*bitmap, path);
        return sampled;
    };

    enum class Phase {
        Example,
        Red,
    };
    auto phase = Phase::Example;
    int paints = 0;
    bool loaded = false;

    view->on_load_finish = [&](auto const& url) {
        outln("LOADED: {}", url);
        loaded = true;
    };

    view->on_ready_to_paint = [&] {
        outln("PAINT #{}", ++paints);

        if (phase == Phase::Example) {
            present();
            if (paints >= 3) {
                (void)dump_surface("/tmp/spike_probe_example.png"sv);
                capture_window("/tmp/spike_probe_window_example.png"sv);
                outln("PHASE 2: loading a pure #FF0000 page");
                phase = Phase::Red;
                loaded = false;
                view->load(URL::Parser::basic_parse("data:text/html,<body style='margin:0;background:%23ff0000'>"sv).release_value());
            }
            return;
        }

        // The compositor publishes on change, so there is one frame per load rather than a steady stream.
        if (!loaded)
            return;

        present();
        auto sampled = dump_surface("/tmp/spike_probe_red.png"sv);
        capture_window("/tmp/spike_probe_window_red.png"sv);

        // If the IOSurface's kIOSurfaceColorSpace did not propagate, the sRGB pixels would be read as being in the
        // display's color space and this would not come back exactly red.
        VERIFY(sampled == Gfx::Color { 255, 0, 0 });

        Core::EventLoop::current().quit(0);
    };

    auto fallback = Core::Timer::create_single_shot(AK::Duration::from_seconds(90), [&] {
        outln("RESULT: loaded={} paints={} (timed out)", loaded, paints);
        Core::EventLoop::current().quit(1);
    });
    fallback->start();

    view->load(URL::Parser::basic_parse("https://example.com"sv).release_value());
    auto result = TRY(app->execute());
    fallback->stop();

    outln("RESULT: loaded={} paints={}", loaded, paints);
    return result;
}
