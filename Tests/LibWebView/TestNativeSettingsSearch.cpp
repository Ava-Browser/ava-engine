/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>

#include "../../Libraries/LibPort/ladybird_port.h"

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Bounded wait on child process with deterministic timeout and fail-closed SIGKILL.
// Follows the in-tree probe pattern (MonotonicTime deadline + sleep_ms poll loop).
static bool wait_for_child_with_timeout(pid_t pid, int* out_status, AK::Duration timeout = AK::Duration::from_seconds(5))
{
    auto deadline = MonotonicTime::now() + timeout;
    while (true) {
        int status = 0;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            if (out_status)
                *out_status = status;
            return true;
        }
        if (res < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (MonotonicTime::now() > deadline) {
            // Timed out: kill runaway child and fail closed
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return false;
        }
        (void)Core::System::sleep_ms(10);
    }
}

bool write_test_settings_json(StringView directory, StringView json_content)
{
    auto profile_dir = LexicalPath::join(directory, "Ladybird"sv, "Profiles"sv, "default"sv).string();
    if (Core::Directory::create(profile_dir, Core::Directory::CreateDirectories::Yes).is_error())
        return false;
    auto path = LexicalPath::join(profile_dir, "Settings.json"sv).string();
    auto file_result = Core::File::open(path.view(), Core::File::OpenMode::Write);
    if (file_result.is_error())
        return false;
    auto write_result = file_result.value()->write_until_depleted(json_content.bytes());
    return !write_result.is_error();
}

int run_migration_subtest(StringView test_name, StringView json_content, LbSearchProvenance expected_provenance, StringView expected_provider, bool expected_suggestions, bool expected_autocomplete)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        outln("FAIL: fork failed for '{}'", test_name);
        return 1;
    }

    if (pid == 0) {
        // Child process
        auto dir = ByteString::formatted("{}/Ladybird-Migration-{}-{}", Core::StandardPaths::tempfile_directory(), test_name, generate_random_uuid());
        if (Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes).is_error())
            exit(10);
        ScopeGuard dir_cleanup = [&] { (void)FileSystem::remove(dir, FileSystem::RecursionMode::Allowed); };

        if (!json_content.is_empty()) {
            if (!write_test_settings_json(dir, json_content))
                exit(11);
        }

        if (setenv("XDG_CONFIG_HOME", dir.characters(), 1) != 0)
            exit(12);

        LbCallbacks callbacks {};
        auto* app = LbAppCreate("TestMigrationSubtest", &callbacks);
        if (!app)
            exit(13);

        LbSearchMigrationSnapshot snap {};
        snap.struct_size = sizeof(LbSearchMigrationSnapshot);
        snap.api_version = LB_PORT_API_VERSION;

        if (LbSearchReadMigrationSnapshot(app, &snap) != LbResultOk)
            exit(14);

        if (snap.provenance != expected_provenance) {
            outln("CHILD FAIL [{}]: expected provenance {}, got {}", test_name, static_cast<int>(expected_provenance), static_cast<int>(snap.provenance));
            exit(15);
        }

        if (!expected_provider.is_empty()) {
            StringView actual_provider { snap.selected_provider_name, snap.selected_provider_name_len };
            if (actual_provider != expected_provider) {
                outln("CHILD FAIL [{}]: expected provider '{}', got '{}'", test_name, expected_provider, actual_provider);
                exit(16);
            }
        } else if (snap.selected_provider_name != nullptr) {
            exit(17);
        }

        if (snap.suggestions_enabled != (expected_suggestions ? 1 : 0))
            exit(18);

        if (snap.has_legacy_autocomplete != (expected_autocomplete ? 1 : 0))
            exit(19);

        LbSearchFreeMigrationSnapshot(&snap);
        LbAppDestroy(app);
        exit(0);
    }

    int status = 0;
    if (!wait_for_child_with_timeout(pid, &status, AK::Duration::from_seconds(5))) {
        outln("FAIL: migration subtest '{}' child timed out or wait failed", test_name);
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        outln("FAIL: migration subtest '{}' child exited with status {}", test_name, WEXITSTATUS(status));
        return 1;
    }

    outln("PASS [{}]: provenance={}, provider='{}', suggestions={}, legacy_autocomplete={}",
        test_name, static_cast<int>(expected_provenance), expected_provider, expected_suggestions, expected_autocomplete);
    return 0;
}

int run_custom_catalog_subtest()
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        outln("FAIL: fork failed for custom catalog subtest");
        return 1;
    }

    if (pid == 0) {
        auto dir = ByteString::formatted("{}/Ladybird-CustomCatalog-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
        if (Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes).is_error())
            exit(10);
        ScopeGuard dir_cleanup = [&] { (void)FileSystem::remove(dir, FileSystem::RecursionMode::Allowed); };

        // Genuine Settings.json schema with custom search engine array
        static constexpr auto custom_json =
            "{\"searchEngine\": {\"engine\": null, \"suggestions\": false, \"custom\": ["
            "{\"name\": \"CustomSearch\", \"url\": \"https://custom.example.com/search?q=%s\", \"suggestionsUrl\": \"https://custom.example.com/suggest?q=%s\"}"
            "]}}"sv;

        if (!write_test_settings_json(dir, custom_json))
            exit(11);

        if (setenv("XDG_CONFIG_HOME", dir.characters(), 1) != 0)
            exit(12);

        LbCallbacks callbacks {};
        auto* app = LbAppCreate("TestCustomCatalog", &callbacks);
        if (!app)
            exit(13);

        // 1. Catalog visitor must find the custom search engine
        bool found_custom = false;
        auto visitor = [](void* ctx, LbSearchEngineDescriptor const* desc) -> int {
            auto* found = static_cast<bool*>(ctx);
            if (desc->is_custom == 1) {
                StringView name { desc->name, desc->name_len };
                StringView url { desc->query_url, desc->query_url_len };
                StringView sugg { desc->suggestions_url, desc->suggestions_url_len };
                if (name == "CustomSearch"sv && url == "https://custom.example.com/search?q=%s"sv && sugg == "https://custom.example.com/suggest?q=%s"sv)
                    *found = true;
            }
            return 0;
        };

        if (LbSearchVisitCatalog(app, &found_custom, visitor) != LbResultOk || !found_custom)
            exit(14);

        // 2. Set provider to custom search engine
        static constexpr auto custom_name = "CustomSearch"sv;
        if (LbSearchSetProvider(app, custom_name.characters_without_null_termination(), custom_name.length()) != LbResultOk)
            exit(15);

        // 3. Status must be Enabled with "CustomSearch"
        LbSearchPolicy policy {};
        char* active_name = nullptr;
        size_t active_name_len = 0;
        if (LbSearchGetStatus(app, &policy, &active_name, &active_name_len) != LbResultOk || policy != LbSearchPolicyEnabled)
            exit(16);
        if (StringView { active_name, active_name_len } != custom_name)
            exit(17);
        LbStringFree(active_name);

        // 4. Resolve prose query with custom search engine
        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto query = "ladybird custom query"sv;
        auto ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        StringView resolved_str { resolved, resolved_len };
        if (ok != 1 || resolved_str != "https://custom.example.com/search?q=ladybird%20custom%20query"sv)
            exit(18);
        LbStringFree(resolved);

        // 5. Provider -> Fallback regression (Requirement 2 & 3):
        // Calling LbSearchSetUnconfiguredFallback must route next prose query to builtins[0] (Bing),
        // NOT CustomSearch, and LbSearchGetStatus must return UnconfiguredFallback with NULL name.
        if (LbSearchSetUnconfiguredFallback(app) != LbResultOk)
            exit(19);

        active_name = reinterpret_cast<char*>(0x1234);
        active_name_len = 999;
        if (LbSearchGetStatus(app, &policy, &active_name, &active_name_len) != LbResultOk)
            exit(20);
        if (policy != LbSearchPolicyUnconfiguredFallback || active_name != nullptr || active_name_len != 0)
            exit(21);

        resolved = nullptr;
        resolved_len = 0;
        ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        if (ok != 1 || !resolved_str.contains("bing.com/search?q=ladybird%20custom%20query"sv))
            exit(22);
        LbStringFree(resolved);

        // 6. Verify persistence in Settings.json: searchEngine.engine must be null
        auto profile_dir = LexicalPath::join(dir, "Ladybird"sv, "Profiles"sv, "default"sv).string();
        auto settings_path = LexicalPath::join(profile_dir, "Settings.json"sv).string();
        auto file_res = Core::File::open(settings_path.view(), Core::File::OpenMode::Read);
        if (file_res.is_error())
            exit(23);
        auto bytes_res = file_res.value()->read_until_eof();
        if (bytes_res.is_error())
            exit(24);
        auto json_res = JsonValue::from_string(StringView { bytes_res.value().bytes() });
        if (json_res.is_error() || !json_res.value().is_object())
            exit(25);
        auto se_obj = json_res.value().as_object().get_object("searchEngine"sv);
        if (!se_obj.has_value())
            exit(26);
        auto se_engine = se_obj->get("engine"sv);
        if (!se_engine.has_value() || !se_engine->is_null())
            exit(27);

        LbAppDestroy(app);
        exit(0);
    }

    int status = 0;
    if (!wait_for_child_with_timeout(pid, &status, AK::Duration::from_seconds(5))) {
        outln("FAIL: custom catalog subtest child timed out or wait failed");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        outln("FAIL: custom catalog subtest child exited with status {}", WEXITSTATUS(status));
        return 1;
    }

    outln("PASS: custom catalog parsed from real schema, visited, routes prose, and fallback resets cleanly to Bing");
    return 0;
}

int run_lifecycle_subtest()
{
    // Subprocess 1: sets disabled
    fflush(stdout);
    pid_t pid1 = fork();
    if (pid1 < 0)
        return 1;
    if (pid1 == 0) {
        auto dir = ByteString::formatted("{}/Ladybird-Lifecycle-1-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
        (void)Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes);
        (void)setenv("XDG_CONFIG_HOME", dir.characters(), 1);

        LbCallbacks callbacks {};
        auto* app = LbAppCreate("TestLifecycle1", &callbacks);
        if (!app)
            exit(1);

        if (LbSearchSetDisabled(app) != LbResultOk)
            exit(2);

        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto prose = "prose query"sv;
        auto ok = LbResolveUserInput(prose.characters_without_null_termination(), prose.length(), &resolved, &resolved_len);
        if (ok != 0)
            exit(3);

        LbAppDestroy(app);
        (void)FileSystem::remove(dir, FileSystem::RecursionMode::Allowed);
        exit(0);
    }

    int status1 = 0;
    if (!wait_for_child_with_timeout(pid1, &status1, AK::Duration::from_seconds(5))) {
        outln("FAIL: lifecycle child 1 timed out or wait failed");
        return 1;
    }
    if (!WIFEXITED(status1) || WEXITSTATUS(status1) != 0) {
        outln("FAIL: lifecycle child 1 failed with status {}", WEXITSTATUS(status1));
        return 1;
    }

    // Subprocess 2: fresh run, must NOT inherit child 1's disabled state
    fflush(stdout);
    pid_t pid2 = fork();
    if (pid2 < 0)
        return 1;
    if (pid2 == 0) {
        auto dir = ByteString::formatted("{}/Ladybird-Lifecycle-2-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
        (void)Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes);
        (void)setenv("XDG_CONFIG_HOME", dir.characters(), 1);

        LbCallbacks callbacks {};
        auto* app = LbAppCreate("TestLifecycle2", &callbacks);
        if (!app)
            exit(1);

        LbSearchPolicy policy {};
        char* active_name = reinterpret_cast<char*>(0x1234);
        size_t active_name_len = 999;
        LbSearchGetStatus(app, &policy, &active_name, &active_name_len);
        if (policy != LbSearchPolicyUnconfiguredFallback || active_name != nullptr || active_name_len != 0)
            exit(2);

        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto prose = "prose query"sv;
        auto ok = LbResolveUserInput(prose.characters_without_null_termination(), prose.length(), &resolved, &resolved_len);
        StringView resolved_str { resolved, resolved_len };
        if (ok != 1 || !resolved_str.contains("bing.com"sv))
            exit(3);

        LbStringFree(resolved);
        LbAppDestroy(app);
        (void)FileSystem::remove(dir, FileSystem::RecursionMode::Allowed);
        exit(0);
    }

    int status2 = 0;
    if (!wait_for_child_with_timeout(pid2, &status2, AK::Duration::from_seconds(5))) {
        outln("FAIL: lifecycle child 2 timed out or wait failed");
        return 1;
    }
    if (!WIFEXITED(status2) || WEXITSTATUS(status2) != 0) {
        outln("FAIL: lifecycle child 2 failed with status {}", WEXITSTATUS(status2));
        return 1;
    }

    outln("PASS: lifecycle subprocess isolation verified (no cross-instance leakage)");
    return 0;
}

}

ErrorOr<int> ladybird_main(Main::Arguments)
{
#if defined(LADYBIRD_BINARY_PATH)
    setenv("LADYBIRD_BINARY_PATH", LADYBIRD_BINARY_PATH, 0);
#endif

    outln("=== TestNativeSettingsSearch starting ===");

    // 0. Prove the probe loaded the freshly built shim library via dladdr
    {
        Dl_info info {};
        int dl_res = dladdr(reinterpret_cast<void*>(&LbSearchSetProvider), &info);
        if (dl_res == 0 || !info.dli_fname) {
            outln("FAIL: dladdr failed to resolve LbSearchSetProvider symbol");
            return 1;
        }
        outln("SHIM PROVENANCE: loaded from '{}'", info.dli_fname);
        StringView shim_path { info.dli_fname, strlen(info.dli_fname) };
        if (!shim_path.contains("liblagom-port"sv)) {
            outln("FAIL: symbol did not come from liblagom-port: '{}'", shim_path);
            return 1;
        }
    }

    // 1. Bad arguments and contract robustness before any engine exists
    {
        outln("CHECK: Bad argument handling before engine creation");
        LbSearchPolicy policy = LbSearchPolicyEnabled;
        char* name = reinterpret_cast<char*>(0x1234);
        size_t name_len = 999;

        if (LbSearchSetProvider(nullptr, "Bing", 4) != LbResultErrorEngineUnavailable) {
            outln("FAIL: LbSearchSetProvider(null) should return LbResultErrorEngineUnavailable");
            return 1;
        }
        if (LbSearchSetDisabled(nullptr) != LbResultErrorEngineUnavailable) {
            outln("FAIL: LbSearchSetDisabled(null) should return LbResultErrorEngineUnavailable");
            return 1;
        }
        if (LbSearchSetUnconfiguredFallback(nullptr) != LbResultErrorEngineUnavailable) {
            outln("FAIL: LbSearchSetUnconfiguredFallback(null) should return LbResultErrorEngineUnavailable");
            return 1;
        }
        if (LbSearchGetStatus(nullptr, &policy, &name, &name_len) != LbResultErrorEngineUnavailable) {
            outln("FAIL: LbSearchGetStatus(null) should return LbResultErrorEngineUnavailable");
            return 1;
        }
        if (name != nullptr || name_len != 0) {
            outln("FAIL: output pointers were not reset on error");
            return 1;
        }

        LbSearchFreeMigrationSnapshot(nullptr);
        LbSearchMigrationSnapshot zero_snap {};
        LbSearchFreeMigrationSnapshot(&zero_snap);
        outln("PASS: pre-engine bad args handled safely");
    }

    // 2. Migration snapshot provenance across synthetic profile files using genuine Settings JSON schema
    {
        outln("CHECK: Migration snapshot provenance capture with genuine schema");
        if (run_migration_subtest("AbsentFile"sv, ""sv, LbSearchProvenanceUnconfiguredAbsent, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("AbsentKey"sv, "{\"appearance\": \"system\"}"sv, LbSearchProvenanceUnconfiguredAbsent, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("AmbiguousNull"sv, "{\"searchEngine\": {\"engine\": null, \"suggestions\": false}}"sv, LbSearchProvenanceAmbiguousNull, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("AmbiguousLegacyNull"sv, "{\"searchEngine\": {\"name\": null}}"sv, LbSearchProvenanceAmbiguousNull, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("ConfiguredCurrent"sv, "{\"searchEngine\": {\"engine\": \"Google\", \"suggestions\": true}}"sv, LbSearchProvenanceConfiguredCurrent, "Google"sv, true, false) != 0)
            return 1;
        if (run_migration_subtest("ConfiguredLegacy"sv, "{\"searchEngine\": {\"name\": \"Yahoo\"}, \"autocompleteEngine\": {\"name\": \"Yahoo\"}}"sv, LbSearchProvenanceConfiguredLegacy, "Yahoo"sv, false, true) != 0)
            return 1;
        if (run_migration_subtest("MalformedJSON"sv, "{not valid json}"sv, LbSearchProvenanceMalformed, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("MalformedType"sv, "{\"searchEngine\": 12345}"sv, LbSearchProvenanceMalformed, ""sv, false, false) != 0)
            return 1;
        outln("PASS: migration snapshot provenance distinguishes all 8 genuine schema states accurately");
    }

    // 3. Custom catalog subtest: exercises real custom search schema, visitor, custom routing, and fallback reset
    {
        outln("CHECK: Custom catalog parsing, visitor discovery, routing, and fallback reset");
        if (run_custom_catalog_subtest() != 0)
            return 1;
    }

    // 4. Lifecycle reset across isolated processes
    {
        outln("CHECK: Lifecycle reset across process instances");
        if (run_lifecycle_subtest() != 0)
            return 1;
    }

    // 5. In-process tests with isolated profile directory
    auto test_directory = ByteString::formatted("{}/Ladybird-TestNativeSettingsSearch-Main-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed); };
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);

    LbCallbacks callbacks {};
    auto* app = LbAppCreate("TestNativeSettingsSearch", &callbacks);
    if (!app) {
        outln("FAIL: LbAppCreate returned null");
        return 1;
    }
    ScopeGuard destroy_app = [&] {
        if (app) {
            LbAppDestroy(app);
            app = nullptr;
        }
    };

    // 6. First-run fallback: unconfigured fresh directory
    {
        outln("CHECK: First-run fallback with unconfigured profile");
        LbSearchPolicy policy {};
        char* provider_name = reinterpret_cast<char*>(0x1234);
        size_t provider_name_len = 999;
        if (LbSearchGetStatus(app, &policy, &provider_name, &provider_name_len) != LbResultOk) {
            outln("FAIL: LbSearchGetStatus failed on fresh app");
            return 1;
        }
        if (policy != LbSearchPolicyUnconfiguredFallback) {
            outln("FAIL: fresh app policy should be LbSearchPolicyUnconfiguredFallback ({}), got {}", static_cast<int>(LbSearchPolicyUnconfiguredFallback), static_cast<int>(policy));
            return 1;
        }
        if (provider_name != nullptr || provider_name_len != 0) {
            outln("FAIL: provider name should be null for unconfigured fallback");
            LbStringFree(provider_name);
            return 1;
        }

        // Address resolution on fresh app falls back to first built-in (Bing)
        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto query = "ladybird browser"sv;
        auto ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        StringView resolved_str { resolved, resolved_len };
        outln("RESOLVE (fresh fallback): '{}' -> {}", query, resolved_str);

        if (ok != 1 || !resolved_str.contains("bing.com/search?q=ladybird%20browser"sv)) {
            outln("FAIL: fresh profile did not fall back to Bing query URL: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        // Raw URLs and host:port still resolve
        static constexpr auto raw_url = "https://example.com/test"sv;
        ok = LbResolveUserInput(raw_url.characters_without_null_termination(), raw_url.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        if (ok != 1 || resolved_str != "https://example.com/test"sv) {
            outln("FAIL: raw URL failed to resolve: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        static constexpr auto host_port = "localhost:8080"sv;
        ok = LbResolveUserInput(host_port.characters_without_null_termination(), host_port.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        if (ok != 1 || !resolved_str.contains(":8080"sv)) {
            outln("FAIL: host:port failed to resolve: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);
        outln("PASS: first-run fallback is Bing and preserves URLs");
    }

    // 7. Catalog visitor contract
    {
        outln("CHECK: LbSearchVisitCatalog contract");
        size_t visited_count = 0;

        auto visitor = [](void* ctx, LbSearchEngineDescriptor const* desc) -> int {
            auto* counter = static_cast<size_t*>(ctx);
            (*counter)++;
            if (desc->struct_size != sizeof(LbSearchEngineDescriptor) || desc->api_version != LB_PORT_API_VERSION)
                return -1;
            return 0;
        };

        if (LbSearchVisitCatalog(app, &visited_count, visitor) != LbResultOk) {
            outln("FAIL: LbSearchVisitCatalog failed");
            return 1;
        }
        outln("VISITED ENGINES: {}", visited_count);
        if (visited_count < 5) {
            outln("FAIL: expected at least 5 built-in search engines, got {}", visited_count);
            return 1;
        }

        // Verify early stop: return non-zero after 2 items
        size_t early_stop_count = 0;
        auto early_visitor = [](void* ctx, LbSearchEngineDescriptor const*) -> int {
            auto* counter = static_cast<size_t*>(ctx);
            (*counter)++;
            return (*counter >= 2) ? 1 : 0;
        };
        LbSearchVisitCatalog(app, &early_stop_count, early_visitor);
        if (early_stop_count != 2) {
            outln("FAIL: visitor early stop contract violated, visited {}", early_stop_count);
            return 1;
        }
        outln("PASS: LbSearchVisitCatalog meets size, version, and early-stop contracts");
    }

    // 8. Configured provider & Unicode percent-encoding + Fallback reset regression
    {
        outln("CHECK: Configured provider (DuckDuckGo) and Unicode encoding");
        static constexpr auto ddg = "DuckDuckGo"sv;
        if (LbSearchSetProvider(app, ddg.characters_without_null_termination(), ddg.length()) != LbResultOk) {
            outln("FAIL: LbSearchSetProvider('DuckDuckGo') failed");
            return 1;
        }

        LbSearchPolicy policy {};
        char* active_name = nullptr;
        size_t active_name_len = 0;
        if (LbSearchGetStatus(app, &policy, &active_name, &active_name_len) != LbResultOk || policy != LbSearchPolicyEnabled) {
            outln("FAIL: policy should be LbSearchPolicyEnabled after setting provider");
            LbStringFree(active_name);
            return 1;
        }
        if (StringView { active_name, active_name_len } != ddg) {
            outln("FAIL: active provider name mismatch: '{}' vs '{}'", StringView { active_name, active_name_len }, ddg);
            LbStringFree(active_name);
            return 1;
        }
        LbStringFree(active_name);

        // Standard query
        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto query = "web standards"sv;
        auto ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        StringView resolved_str { resolved, resolved_len };
        outln("RESOLVE (DuckDuckGo): '{}' -> {}", query, resolved_str);
        if (ok != 1 || !resolved_str.contains("duckduckgo.com/?q=web%20standards"sv)) {
            outln("FAIL: query did not resolve through DuckDuckGo: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        // Unicode query: ladybird 🐞
        static constexpr auto unicode_query = "ladybird \xf0\x9f\x90\x9e"sv;
        ok = LbResolveUserInput(unicode_query.characters_without_null_termination(), unicode_query.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        outln("RESOLVE (Unicode): '{}' -> {}", unicode_query, resolved_str);
        if (ok != 1 || !resolved_str.contains("%F0%9F%90%9E"sv) || !resolved_str.contains("duckduckgo.com"sv)) {
            outln("FAIL: Unicode query percent-encoding failed: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        // In-process Provider -> Fallback regression (Requirement 2 & 3):
        outln("CHECK: In-process Provider -> Fallback regression");
        if (LbSearchSetUnconfiguredFallback(app) != LbResultOk) {
            outln("FAIL: LbSearchSetUnconfiguredFallback failed");
            return 1;
        }

        active_name = reinterpret_cast<char*>(0x1234);
        active_name_len = 999;
        if (LbSearchGetStatus(app, &policy, &active_name, &active_name_len) != LbResultOk) {
            outln("FAIL: LbSearchGetStatus failed after fallback reset");
            return 1;
        }
        if (policy != LbSearchPolicyUnconfiguredFallback || active_name != nullptr || active_name_len != 0) {
            outln("FAIL: status after fallback reset should be UnconfiguredFallback with null name");
            LbStringFree(active_name);
            return 1;
        }

        resolved = nullptr;
        resolved_len = 0;
        ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        outln("RESOLVE (reset fallback): '{}' -> {}", query, resolved_str);
        if (ok != 1 || !resolved_str.contains("bing.com/search?q=web%20standards"sv)) {
            outln("FAIL: reset fallback did not route to builtins[0] (Bing): '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        // Re-set provider to DuckDuckGo for subsequent sections
        if (LbSearchSetProvider(app, ddg.characters_without_null_termination(), ddg.length()) != LbResultOk) {
            outln("FAIL: re-setting DuckDuckGo failed");
            return 1;
        }
        outln("PASS: configured provider formats query, encodes Unicode properly, and fallback resets cleanly to Bing");
    }

    // 9. Explicitly disabled search
    {
        outln("CHECK: Explicit search-off prevents prose but preserves URLs");
        if (LbSearchSetDisabled(app) != LbResultOk) {
            outln("FAIL: LbSearchSetDisabled failed");
            return 1;
        }

        LbSearchPolicy policy {};
        char* active_name = reinterpret_cast<char*>(0x1234);
        size_t active_name_len = 999;
        LbSearchGetStatus(app, &policy, &active_name, &active_name_len);
        if (policy != LbSearchPolicyDisabled || active_name != nullptr || active_name_len != 0) {
            outln("FAIL: policy should be LbSearchPolicyDisabled with null name, got {}", static_cast<int>(policy));
            LbStringFree(active_name);
            return 1;
        }

        // Prose query MUST return 0 (no search!)
        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto prose = "search something here"sv;
        auto ok = LbResolveUserInput(prose.characters_without_null_termination(), prose.length(), &resolved, &resolved_len);
        if (ok != 0) {
            outln("FAIL: disabled search resolved prose to a search URL: '{}'", StringView { resolved, resolved_len });
            LbStringFree(resolved);
            return 1;
        }

        // Direct URL MUST STILL WORK
        static constexpr auto direct_url = "https://ladybird.org"sv;
        ok = LbResolveUserInput(direct_url.characters_without_null_termination(), direct_url.length(), &resolved, &resolved_len);
        StringView resolved_str { resolved, resolved_len };
        if (ok != 1 || resolved_str != "https://ladybird.org/"sv) {
            outln("FAIL: disabled search broke direct URL: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);

        // host:port MUST STILL WORK
        static constexpr auto port_url = "localhost:3000"sv;
        ok = LbResolveUserInput(port_url.characters_without_null_termination(), port_url.length(), &resolved, &resolved_len);
        resolved_str = StringView { resolved, resolved_len };
        if (ok != 1 || !resolved_str.contains(":3000"sv)) {
            outln("FAIL: disabled search broke host:port: '{}'", resolved_str);
            LbStringFree(resolved);
            return 1;
        }
        LbStringFree(resolved);
        outln("PASS: explicit off blocks prose and preserves direct URLs");
    }

    // 10. Unknown provider fails closed (policy becomes Unavailable, prose blocked, no fallback)
    {
        outln("CHECK: Unknown provider fails closed to Unavailable");
        static constexpr auto fake = "NonExistentSearchEngineXYZ"sv;
        auto res = LbSearchSetProvider(app, fake.characters_without_null_termination(), fake.length());
        if (res != LbResultErrorNotFound) {
            outln("FAIL: setting unknown provider should return LbResultErrorNotFound ({}), got {}", static_cast<int>(LbResultErrorNotFound), static_cast<int>(res));
            return 1;
        }

        LbSearchPolicy policy {};
        char* active_name = reinterpret_cast<char*>(0x1234);
        size_t active_name_len = 999;
        LbSearchGetStatus(app, &policy, &active_name, &active_name_len);
        if (policy != LbSearchPolicyUnavailable || active_name != nullptr || active_name_len != 0) {
            outln("FAIL: unknown provider should set policy to LbSearchPolicyUnavailable with null name ({}), got {}", static_cast<int>(LbSearchPolicyUnavailable), static_cast<int>(policy));
            LbStringFree(active_name);
            return 1;
        }

        char* resolved = nullptr;
        size_t resolved_len = 0;
        static constexpr auto query = "prose query under unavailable"sv;
        auto ok = LbResolveUserInput(query.characters_without_null_termination(), query.length(), &resolved, &resolved_len);
        if (ok != 0) {
            outln("FAIL: unavailable provider fell back to Bing instead of failing closed: '{}'", StringView { resolved, resolved_len });
            LbStringFree(resolved);
            return 1;
        }
        outln("PASS: unknown provider fails closed without Bing fallback");
    }

    // 11. Bad arguments with live engine
    {
        outln("CHECK: Bad argument handling with live engine");
        if (LbSearchSetProvider(app, nullptr, 10) != LbResultErrorNullArg) {
            outln("FAIL: LbSearchSetProvider(app, null, 10) should return LbResultErrorNullArg");
            return 1;
        }
        if (LbSearchSetProvider(app, "Bing", 0) != LbResultErrorInvalidArg) {
            outln("FAIL: LbSearchSetProvider(app, 'Bing', 0) should return LbResultErrorInvalidArg");
            return 1;
        }
        char embedded_nul[] = { 'B', 'i', '\0', 'g' };
        if (LbSearchSetProvider(app, embedded_nul, 4) != LbResultErrorInvalidArg) {
            outln("FAIL: LbSearchSetProvider with embedded NUL should return LbResultErrorInvalidArg");
            return 1;
        }
        if (LbSearchGetStatus(app, nullptr, nullptr, nullptr) != LbResultErrorNullArg) {
            outln("FAIL: LbSearchGetStatus(app, null, null, null) should return LbResultErrorNullArg");
            return 1;
        }
        LbSearchMigrationSnapshot bad_snap {};
        bad_snap.struct_size = sizeof(LbSearchMigrationSnapshot) - 4;
        bad_snap.api_version = LB_PORT_API_VERSION;
        if (LbSearchReadMigrationSnapshot(app, &bad_snap) != LbResultErrorInvalidArg) {
            outln("FAIL: LbSearchReadMigrationSnapshot with small struct_size should return LbResultErrorInvalidArg");
            return 1;
        }
        bad_snap.struct_size = sizeof(LbSearchMigrationSnapshot);
        bad_snap.api_version = 9999;
        if (LbSearchReadMigrationSnapshot(app, &bad_snap) != LbResultErrorUnsupportedVersion) {
            outln("FAIL: LbSearchReadMigrationSnapshot with bad api_version should return LbResultErrorUnsupportedVersion");
            return 1;
        }
        outln("PASS: bad args correctly rejected by live engine");
    }

    outln("=== ALL TestNativeSettingsSearch TESTS PASSED ===");
    return 0;
}
