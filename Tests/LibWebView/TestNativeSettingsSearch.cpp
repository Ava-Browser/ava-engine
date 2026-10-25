/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
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
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>

#include "../../Libraries/LibPort/ladybird_port.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

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
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        outln("FAIL: migration subtest '{}' child exited with status {}", test_name, WEXITSTATUS(status));
        return 1;
    }

    outln("PASS [{}]: provenance={}, provider='{}', suggestions={}, legacy_autocomplete={}",
        test_name, static_cast<int>(expected_provenance), expected_provider, expected_suggestions, expected_autocomplete);
    return 0;
}

int run_lifecycle_subtest()
{
    // Subprocess 1: sets disabled
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
    waitpid(pid1, &status1, 0);
    if (!WIFEXITED(status1) || WEXITSTATUS(status1) != 0) {
        outln("FAIL: lifecycle child 1 failed with status {}", WEXITSTATUS(status1));
        return 1;
    }

    // Subprocess 2: fresh run, must NOT inherit child 1's disabled state
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
        LbSearchGetStatus(app, &policy, nullptr, nullptr);
        if (policy != LbSearchPolicyUnconfiguredFallback)
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
    waitpid(pid2, &status2, 0);
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

    // 2. Migration snapshot provenance across synthetic profile files in isolated subprocesses
    {
        outln("CHECK: Migration snapshot provenance capture");
        if (run_migration_subtest("AbsentFile"sv, ""sv, LbSearchProvenanceUnconfiguredAbsent, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("AbsentKey"sv, "{\"appearance\": \"system\"}"sv, LbSearchProvenanceUnconfiguredAbsent, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("AmbiguousNull"sv, "{\"search_engine\": {\"engine\": null, \"search_suggestions_enabled\": false}}"sv, LbSearchProvenanceAmbiguousNull, ""sv, false, false) != 0)
            return 1;
        if (run_migration_subtest("ConfiguredCurrent"sv, "{\"search_engine\": {\"engine\": \"Google\", \"search_suggestions_enabled\": true}}"sv, LbSearchProvenanceConfiguredCurrent, "Google"sv, true, false) != 0)
            return 1;
        if (run_migration_subtest("ConfiguredLegacy"sv, "{\"search_engine\": {\"name\": \"Yahoo\"}, \"autocompleteEngine\": {\"name\": \"Yahoo\"}}"sv, LbSearchProvenanceConfiguredLegacy, "Yahoo"sv, false, true) != 0)
            return 1;
        if (run_migration_subtest("MalformedJSON"sv, "{not valid json}"sv, LbSearchProvenanceMalformed, ""sv, false, false) != 0)
            return 1;
        outln("PASS: migration snapshot provenance distinguishes all 6 synthetic states accurately");
    }

    // 3. Lifecycle reset across isolated processes
    {
        outln("CHECK: Lifecycle reset across process instances");
        if (run_lifecycle_subtest() != 0)
            return 1;
    }

    // 4. In-process tests with isolated profile directory
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

    // 5. First-run fallback: unconfigured fresh directory
    {
        outln("CHECK: First-run fallback with unconfigured profile");
        LbSearchPolicy policy {};
        char* provider_name = nullptr;
        size_t provider_name_len = 0;
        if (LbSearchGetStatus(app, &policy, &provider_name, &provider_name_len) != LbResultOk) {
            outln("FAIL: LbSearchGetStatus failed on fresh app");
            return 1;
        }
        if (policy != LbSearchPolicyUnconfiguredFallback) {
            outln("FAIL: fresh app policy should be LbSearchPolicyUnconfiguredFallback ({}), got {}", static_cast<int>(LbSearchPolicyUnconfiguredFallback), static_cast<int>(policy));
            return 1;
        }
        if (provider_name != nullptr) {
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

    // 6. Catalog visitor contract
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

    // 7. Configured provider & Unicode percent-encoding
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
        outln("PASS: configured provider formats query and encodes Unicode properly");
    }

    // 8. Explicitly disabled search
    {
        outln("CHECK: Explicit search-off prevents prose but preserves URLs");
        if (LbSearchSetDisabled(app) != LbResultOk) {
            outln("FAIL: LbSearchSetDisabled failed");
            return 1;
        }

        LbSearchPolicy policy {};
        char* active_name = nullptr;
        size_t active_name_len = 0;
        LbSearchGetStatus(app, &policy, &active_name, &active_name_len);
        if (policy != LbSearchPolicyDisabled) {
            outln("FAIL: policy should be LbSearchPolicyDisabled, got {}", static_cast<int>(policy));
            LbStringFree(active_name);
            return 1;
        }
        LbStringFree(active_name);

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

    // 9. Unknown provider fails closed (policy becomes Unavailable, prose blocked, no fallback)
    {
        outln("CHECK: Unknown provider fails closed to Unavailable");
        static constexpr auto fake = "NonExistentSearchEngineXYZ"sv;
        auto res = LbSearchSetProvider(app, fake.characters_without_null_termination(), fake.length());
        if (res != LbResultErrorNotFound) {
            outln("FAIL: setting unknown provider should return LbResultErrorNotFound ({}), got {}", static_cast<int>(LbResultErrorNotFound), static_cast<int>(res));
            return 1;
        }

        LbSearchPolicy policy {};
        LbSearchGetStatus(app, &policy, nullptr, nullptr);
        if (policy != LbSearchPolicyUnavailable) {
            outln("FAIL: unknown provider should set policy to LbSearchPolicyUnavailable ({}), got {}", static_cast<int>(LbSearchPolicyUnavailable), static_cast<int>(policy));
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

    // 10. Bad arguments with live engine
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
