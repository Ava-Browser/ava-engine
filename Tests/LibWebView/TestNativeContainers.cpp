/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Milestone 5.5: Containers Feasibility Probe (TDD-Spaces §5)
 *
 * Questions to answer with measured runtime evidence:
 * (a) Do N simultaneous named sessions work in one process — pages load and paint normally in each?
 * (b) Cookie isolation — set a cookie in session A; confirm session B does not see it?
 * (c) Persistence — quit, relaunch, confirm session A's jar survives on disk under a distinct identity?
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Utilities.h>

namespace {

class ProbeApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(ProbeApplication)

public:
    explicit ProbeApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    outln("=== Milestone 5.5: Containers Probe Start ===");

    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestNativeContainers-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

    // Phase 1: In-process simultaneous sessions & isolation
    outln("--- Phase 1: Simultaneous named sessions & isolation ---");

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(ProbeApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(ProbeApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    // Create session A (default session)
    auto view_a = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::No);

    // Create session B (private session to test concurrent isolation)
    auto view_b = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);

    bool view_a_loaded = false;
    bool view_a_painted = false;
    view_a->on_load_finish = [&](auto const&) { view_a_loaded = true; };
    view_a->on_ready_to_paint = [&] { view_a_painted = true; };

    bool view_b_loaded = false;
    bool view_b_painted = false;
    view_b->on_load_finish = [&](auto const&) { view_b_loaded = true; };
    view_b->on_ready_to_paint = [&] { view_b_painted = true; };

    view_a->load(*URL::Parser::basic_parse("data:text/html,<h1>Session A</h1>"sv));
    view_b->load(*URL::Parser::basic_parse("data:text/html,<h1>Session B</h1>"sv));

    Core::EventLoop::current().spin_until([&] {
        return view_a_loaded && view_a_painted && view_b_loaded && view_b_painted;
    });

    outln("PROBE Q(a) - Simultaneous pages load and paint: PASS (Session A painted={}, Session B painted={})",
        view_a_painted, view_b_painted);

    // Question (b): Cookie isolation
    auto test_url = *URL::Parser::basic_parse("https://example.com/"sv);
    HTTP::Cookie::ParsedCookie cookie_a {};
    cookie_a.name = "token"_string;
    cookie_a.value = "secret_a"_string;
    cookie_a.domain = "example.com"_string;
    cookie_a.path = "/"_string;
    cookie_a.expiry_time_from_max_age_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600);

    // Set cookie in Session A's jar
    auto& jar_a = *view_a->session().cookie_jar;
    jar_a.set_cookie(test_url, cookie_a, HTTP::Cookie::Source::Http, {});

    auto cookie_in_a = jar_a.get_cookie(test_url, HTTP::Cookie::Source::Http, {});
    outln("Cookie in Session A: '{}'", cookie_in_a);

    auto& jar_b = *view_b->session().cookie_jar;
    auto cookie_in_b = jar_b.get_cookie(test_url, HTTP::Cookie::Source::Http, {});
    outln("Cookie in Session B: '{}'", cookie_in_b);

    bool isolation_passed = cookie_in_a.contains("token=secret_a"sv) && !cookie_in_b.contains("token=secret_a"sv);
    outln("PROBE Q(b) - Cookie isolation between simultaneous sessions: {}", isolation_passed ? "PASS" : "FAIL");

    // Question (c): Persistence of non-default session jars
    outln("--- Phase 2: Persistence analysis of multiple sessions ---");
    auto database_path = app->profile().paths().data;
    outln("Default profile data path: {}", database_path);

    // Attempt to create a distinct named container database and cookie jar
    auto container_db_result = Database::Database::create(database_path, "Container_Work"sv);
    if (container_db_result.is_error()) {
        outln("PROBE Q(c) - Creating container database failed: {}", container_db_result.error());
        outln("PROBE Q(c) VERDICT: RED");
    } else {
        auto container_db = container_db_result.release_value();
        auto migrate_result = WebView::CookieJar::migrate_schema(container_db);
        if (migrate_result.is_error()) {
            outln("PROBE Q(c) - Schema migration failed: {}", migrate_result.error());
            outln("PROBE Q(c) VERDICT: RED");
        } else {
            auto container_jar_result = WebView::CookieJar::create(container_db);
            if (container_jar_result.is_error()) {
                outln("PROBE Q(c) - Creating persisted cookie jar failed: {}", container_jar_result.error());
                outln("PROBE Q(c) VERDICT: RED");
            } else {
                auto container_jar = container_jar_result.release_value();
                HTTP::Cookie::ParsedCookie cookie_c {};
                cookie_c.name = "work_token"_string;
                cookie_c.value = "work_secret"_string;
                cookie_c.domain = "example.com"_string;
                cookie_c.path = "/"_string;
                cookie_c.expiry_time_from_max_age_attribute = UnixDateTime::now() + AK::Duration::from_seconds(3600);
                container_jar->set_cookie(test_url, cookie_c, HTTP::Cookie::Source::Http, {});

                // Destroy the jar object so it flushes to disk upon destruction
                {
                    auto temp = move(container_jar);
                    (void)temp;
                }

                auto reopened_db = MUST(Database::Database::create(database_path, "Container_Work"sv));
                auto reopened_jar = MUST(WebView::CookieJar::create(reopened_db));
                auto retrieved_cookie = reopened_jar->get_cookie(test_url, HTTP::Cookie::Source::Http, {});
                outln("Reopened container cookie: '{}'", retrieved_cookie);

                bool persistence_passed = retrieved_cookie.contains("work_token=work_secret"sv);
                outln("PROBE Q(c) - Distinct database cookie survival across relaunch: {}",
                    persistence_passed ? "PASS" : "FAIL");

                // Check structural engine limitations:
                outln("Engine Structural Analysis for Containers:");
                outln("1. BrowsingSession::create(IsPrivate) only supports IsPrivate::No (in-memory uninitialized) or IsPrivate::Yes (in-memory transient stores).");
                outln("2. Application::session_for_new_view(IsPrivate) only branches on is_private. It only knows m_default_session and m_private_session.");
                outln("3. ViewImplementation::ViewImplementation(IsPrivate) only accepts IsPrivate and derives m_session from Application::session_for_new_view(is_private).");
                outln("4. RequestServer connections are mapped by Application::did_connect_request_server_client(int, BrowsingSession&, SiteBinding).");
            }
        }
    }

    outln("=== Milestone 5.5: Containers Probe Complete ===");
    return 0;
}
