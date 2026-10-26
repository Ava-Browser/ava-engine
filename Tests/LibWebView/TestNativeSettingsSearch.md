# Phase 0a Search Contract & Native Probe Report (Updated After Independent Audit)

**Target:** `Libraries/LibPort/{ladybird_port.h,ladybird_port.cpp}` & `Tests/LibWebView/TestNativeSettingsSearch.cpp`
**Branch:** `review/search-settings-phase0a`
**Status:** Audit NO-GO items resolved; genuine schema validated; native probe passed.

---

## 1. Retraction of False Prior Claims & Audit Root Fixes

The initial Phase 0a submission contained critical discrepancies that led to an independent NO-GO audit:

1. **Retraction of False Schema & Custom Catalog Claims:**
   - *Previous failure:* Prior code and tests used fabricated JSON keys (`search_engine`, `custom_search_engines`, `suggestions_url`). Because Ladybird's genuine `Settings.cpp` uses `searchEngine`, `custom`, and `suggestionsUrl`, custom search engines were never actually parsed into `Settings` and were never visited by `LbSearchVisitCatalog`.
   - *Fix:* Aligned `capture_migration_snapshot` and `LbSearchVisitCatalog` with the genuine schema (`searchEngine`, `custom`, `name`, `url`, `engine`, `suggestions`, `suggestionsUrl`). Tests now synthesize and exercise genuine custom search catalog entries end-to-end.

2. **Retraction of Unconfigured Fallback Routing Claims:**
   - *Previous failure:* When `LbSearchSetUnconfiguredFallback` was called after a provider had been set, `Application::settings()` still retained the prior provider. `LbResolveUserInput` inspected `settings().engine`, routing subsequent queries to the old provider instead of `builtins[0]` (Bing), leaving runtime routing and persistence in disagreement.
   - *Fix:* `LbSearchSetUnconfiguredFallback` now updates `Application::settings()` with an empty engine, syncing persistence (`Settings.json`). `LbResolveUserInput` under `LbSearchPolicyUnconfiguredFallback` strictly routes to `builtins[0]`. Added explicit provider->fallback regression tests both in an isolated subprocess and in-process.

3. **Correction of `LbSearchGetStatus` Provider Name Contract:**
   - *Previous failure:* `LbSearchGetStatus` would return a provider name if `chosen_provider_name` was populated, regardless of whether policy was `Enabled`.
   - *Fix:* `LbSearchGetStatus` strictly returns a provider name only when policy is `LbSearchPolicyEnabled`. For `UnconfiguredFallback`, `Disabled`, and `Unavailable`, `*out_name` is set to NULL and `*out_len` to 0.

4. **Bounded Child Process Waits:**
   - *Previous failure:* Subprocesses were reaped via unbounded `waitpid(pid, &status, 0)` calls, which could hang the harness indefinitely if a child hung.
   - *Fix:* Bounded all child process wait paths with a deterministic 5-second timeout using the in-tree probe pattern (`MonotonicTime::now() + timeout` and `Core::System::sleep_ms(10)`), with fail-closed SIGKILL and status verification.

5. **ABI NUL & Struct Sizing Documentation:**
   - Clarified that `struct_size` and `api_version` must be caller-initialized, and string pointers cross the boundary as length-delimited byte buffers without requiring trailing NUL bytes.

6. **Investigation of `set_up_isolation`:**
   - Evaluated `set_up_isolation` in `ladybird_port.cpp`. It checks `Core::Environment::has("XDG_CONFIG_HOME"sv)` and preserves any pre-set profile path. When unset, it configures a per-PID scratch directory in `$TMPDIR`. No evidence was found that this breaks Ava's intended profile path (Ava can set `XDG_CONFIG_HOME` to point to its profile directory before engine startup). Following explicit task instructions, `set_up_isolation` was left unchanged.

---

## 2. Provenance & Isolated Build Environment

- **Worktree:** `Extern/ladybird` (branch `review/search-settings-phase0a`)
- **Compiler:** Apple clang version 17.0.0 (`/usr/bin/c++`), targeting `arm64-apple-macos14.4`
- **Prebuilt Dependencies:** Read-only linkage from a source-matching cached engine build.
- **Zero Shared Build Tree Writes:** All compilation artifacts were routed to an isolated scratch directory.
- **Loaded Shim Verification:** `dladdr` inside `TestNativeSettingsSearch` proved `LbSearchSetProvider` was loaded from the newly linked scratch shim, not the cached library.

---

## 3. Exact Reproducible Build & Test Commands

Set `repo` to this checkout, `engine_build` to a source-matching configured build, and
`scratch` to an empty temporary directory. Extract the exact compiler/linker flags from the
configured build rather than copying machine-specific SDK or dependency paths.

```bash
repo="$(pwd -P)"
engine_build="<source-matching-engine-build>"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/ladybird-search-probe.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT

tmp_c="${scratch}/header-check.c"
printf '#include "ladybird_port.h"\nint main(void){return 0;}\n' > "${tmp_c}"
clang -x c -std=c11 -fsyntax-only -Wall -Wextra -Werror \
  -I "${repo}/Libraries/LibPort" "${tmp_c}"

# Recompile ladybird_port.cpp and TestNativeSettingsSearch.cpp with the configured
# build's exact compile flags. Link a scratch liblagom-port and probe with the same
# dependency list, but put the scratch directory first in the probe's rpath.
DYLD_LIBRARY_PATH="${scratch}" "${scratch}/TestNativeSettingsSearch"
```

The audited run used the configured build's emitted commands with only source/output/rpath
locations redirected. No generated files or libraries in the cached build were modified.

---

## 4. Test Execution Output

```
=== TestNativeSettingsSearch starting ===
SHIM PROVENANCE: loaded from '<scratch>/liblagom-port.0.1.0.dylib'
CHECK: Bad argument handling before engine creation
PASS: pre-engine bad args handled safely
CHECK: Migration snapshot provenance capture with genuine schema
PASS [AbsentFile]: provenance=0, provider='', suggestions=false, legacy_autocomplete=false
PASS [AbsentKey]: provenance=0, provider='', suggestions=false, legacy_autocomplete=false
PASS [AmbiguousNull]: provenance=3, provider='', suggestions=false, legacy_autocomplete=false
PASS [AmbiguousLegacyNull]: provenance=3, provider='', suggestions=false, legacy_autocomplete=false
PASS [ConfiguredCurrent]: provenance=1, provider='Google', suggestions=true, legacy_autocomplete=false
PASS [ConfiguredLegacy]: provenance=2, provider='Yahoo', suggestions=false, legacy_autocomplete=true
Unable to read Ladybird settings: JsonParser: Expected '"'
PASS [MalformedJSON]: provenance=4, provider='', suggestions=false, legacy_autocomplete=false
PASS [MalformedType]: provenance=4, provider='', suggestions=false, legacy_autocomplete=false
PASS: migration snapshot provenance distinguishes all 8 genuine schema states accurately
CHECK: Custom catalog parsing, visitor discovery, routing, and fallback reset
PASS: custom catalog parsed from real schema, visited, routes prose, and fallback resets cleanly to Bing
CHECK: Lifecycle reset across process instances
PASS: lifecycle subprocess isolation verified (no cross-instance leakage)
CHECK: First-run fallback with unconfigured profile
RESOLVE (fresh fallback): 'ladybird browser' -> https://www.bing.com/search?q=ladybird%20browser
PASS: first-run fallback is Bing and preserves URLs
CHECK: LbSearchVisitCatalog contract
VISITED ENGINES: 10
PASS: LbSearchVisitCatalog meets size, version, and early-stop contracts
CHECK: Configured provider (DuckDuckGo) and Unicode encoding
RESOLVE (DuckDuckGo): 'web standards' -> https://duckduckgo.com/?q=web%20standards
RESOLVE (Unicode): 'ladybird 🐞' -> https://duckduckgo.com/?q=ladybird%20%F0%9F%90%9E
CHECK: In-process Provider -> Fallback regression
RESOLVE (reset fallback): 'web standards' -> https://www.bing.com/search?q=web%20standards
PASS: configured provider formats query, encodes Unicode properly, and fallback resets cleanly to Bing
CHECK: Explicit search-off prevents prose but preserves URLs
PASS: explicit off blocks prose and preserves direct URLs
CHECK: Unknown provider fails closed to Unavailable
PASS: unknown provider fails closed without Bing fallback
CHECK: Bad argument handling with live engine
PASS: bad args correctly rejected by live engine
=== ALL TestNativeSettingsSearch TESTS PASSED ===
```

---

## 5. Scope & Remaining Blockers

1. **Gated Phase 0b DNS Settings Probe:** Gated and unrun pending Phase 0b approval.
2. **Custom Search Engine UI Editor:** Catalog discovery and execution are verified; editing UI is deferred to Phase 2.
3. **App-Side Layers:** Swift targets (`LadybirdBridge`, `LadybirdEngine`, `LadybirdUI`) in the Ava repository remain untouched pending coordinator acceptance of this probe report.
