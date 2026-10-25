# Phase 0a Search Contract & Native Probe Report

**Target:** `Extern/ladybird/Libraries/LibPort/` & `Extern/ladybird/Tests/LibWebView/TestNativeSettingsSearch.cpp`

**Worker:** `task_4eda0e48bf4c`, `ctx_3210ee015e84`
**Status:** Phase 0a Search contract and native probe completed and verified. Gated engine features remain blocked.

---

## 1. Provenance & Build Environment

- **Fork Submodule Head SHA:** `66616b4deeb42fbc7ce722075f22b1b1e5f5891a` (branch `ava-port`)
- **Fork Git Isolation:** Independent `.git` directory inside worktree `Extern/ladybird/.git` (no submodule pointer repointing or shared `.git` edits).
- **Compiler:** Apple clang version 17.0.0 (`/usr/bin/c++`), targeting `arm64-apple-macos14.4`, macOS SDK `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk`.
- **Prebuilt Dependency Libs:** Read-only linkage from `/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/lib` and `vcpkg_installed/arm64-osx-dynamic/lib`.
- **Isolated Build Output:** All compiler object files, shared library, and probe binary were rerouted exclusively to `/tmp/ladybird-isolated-build/` (~15 MB disk usage, zero bytes written to shared build tree).
- **Loaded Shim Verification:** `dladdr` inside `TestNativeSettingsSearch` proved the binary loaded the freshly built shim from `/private/tmp/ladybird-isolated-build/liblagom-port.0.1.0.dylib` (via `@rpath/liblagom-port.0.dylib`).

---

## 2. Exact Reproducible Full Build Commands

```bash
# 1. Compile isolated liblagom-port object
/usr/bin/c++ \
  -DENABLE_COMPILETIME_FORMAT_CHECK \
  -DLibPort_EXPORTS \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Libraries \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Services \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/Libraries \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/Services \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/vcpkg_installed/arm64-osx-dynamic/include \
  -O2 -g -DNDEBUG -std=c++23 -arch arm64 \
  -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk \
  -mmacosx-version-min=14.4 -fPIC -march=native \
  -Wno-unknown-warning-option -Wno-unused-command-line-argument -Wno-unqualified-std-cast-call \
  -fno-exceptions -fsigned-char \
  -o /tmp/ladybird-isolated-build/ladybird_port.o \
  -c /Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Libraries/LibPort/ladybird_port.cpp

# 2. Link isolated liblagom-port shared library
/usr/bin/c++ \
  -arch arm64 -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk -mmacosx-version-min=14.4 \
  -dynamiclib -Wl,-headerpad_max_install_names -fstack-protector-strong -Xlinker -dead_strip \
  -install_name @rpath/liblagom-port.0.dylib \
  -o /tmp/ladybird-isolated-build/liblagom-port.0.1.0.dylib \
  /tmp/ladybird-isolated-build/ladybird_port.o \
  -L/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/lib \
  -llagom-web -llagom-webview -llagom-compositing -llagom-sandbox -llagom-webcommon \
  -llagom-gfx -llagom-requests -llagom-ipc -llagom-url -llagom-core -llagom-unicode -llagom-ak \
  -framework CoreFoundation -framework CoreGraphics -framework CoreLocation -framework CoreServices -framework Foundation -framework IOSurface
ln -sf liblagom-port.0.1.0.dylib /tmp/ladybird-isolated-build/liblagom-port.dylib
ln -sf liblagom-port.0.1.0.dylib /tmp/ladybird-isolated-build/liblagom-port.0.dylib

# 3. Compile TestNativeSettingsSearch object
/usr/bin/c++ \
  -DENABLE_COMPILETIME_FORMAT_CHECK \
  -DLADYBIRD_BINARY_PATH=\"/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/bin/Ladybird.app/Contents/MacOS\" \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Libraries \
  -I/Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Services \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/Libraries \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/Services \
  -I/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/vcpkg_installed/arm64-osx-dynamic/include \
  -O2 -g -DNDEBUG -std=c++23 -arch arm64 \
  -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk \
  -mmacosx-version-min=14.4 -fPIE -march=native \
  -Wno-unknown-warning-option -Wno-unused-command-line-argument -Wno-unqualified-std-cast-call \
  -fno-exceptions -fsigned-char \
  -o /tmp/ladybird-isolated-build/TestNativeSettingsSearch.o \
  -c /Users/ankitchouhan/orca/workspaces/ava/settings-tdd/Extern/ladybird/Tests/LibWebView/TestNativeSettingsSearch.cpp

# 4. Link TestNativeSettingsSearch probe executable
/usr/bin/c++ \
  -arch arm64 -isysroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk -mmacosx-version-min=14.4 \
  -fPIE -Wl,-dead_strip \
  -Wl,-rpath,/tmp/ladybird-isolated-build \
  -Wl,-rpath,/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/lib \
  -Wl,-rpath,/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/vcpkg_installed/arm64-osx-dynamic/lib \
  -o /tmp/ladybird-isolated-build/TestNativeSettingsSearch \
  /tmp/ladybird-isolated-build/TestNativeSettingsSearch.o \
  /tmp/ladybird-isolated-build/liblagom-port.dylib \
  -L/Users/ankitchouhan/Documents/browsers/ladybird/Build/release/lib \
  -llagom-diff -llagom-imagedecoders -llagom-main -llagom-web -llagom-webview -llagom-imagedecoderclient -llagom-compositing -llagom-sandbox -llagom-webcommon -llagom-gfx -llagom-requests -llagom-ipc -llagom-url -llagom-filesystem -llagom-core -llagom-unicode -llagom-ak \
  -framework CoreFoundation -framework CoreGraphics -framework CoreLocation -framework CoreServices -framework Foundation -framework IOSurface

# 5. Execute probe
DYLD_LIBRARY_PATH=/tmp/ladybird-isolated-build /tmp/ladybird-isolated-build/TestNativeSettingsSearch
```

---

## 3. Implemented C11 ABI Contracts (`ladybird_port.h`)

1. **Pure C11 Header Compliance:** Header check passed with `/usr/bin/clang -std=c11 -Wall -Wextra -Werror -pedantic -fsyntax-only`.
2. **Version & Status:** `#define LB_PORT_API_VERSION 2`, typed `LbResult` (`LbResultOk = 0`, `ErrorNullArg = -1`, `ErrorInvalidArg = -2`, `ErrorNotFound = -3`, `ErrorEngineUnavailable = -4`, `ErrorUnsupportedVersion = -5`).
3. **Search Policy Tri-State + Unavailable:**
   - `LbSearchPolicyUnconfiguredFallback = 0`: Out-of-the-box Bing fallback (`builtins[0]`).
   - `LbSearchPolicyEnabled = 1`: Active provider formatting.
   - `LbSearchPolicyDisabled = 2`: Explicitly off; prose queries return 0.
   - `LbSearchPolicyUnavailable = 3`: Selected provider missing/removed; fails closed, returns 0, never routes to Bing fallback.
4. **Catalog Visitor Contract:** `LbSearchVisitCatalog` visits built-in and custom search engines with borrowed `LbSearchEngineDescriptor` (`struct_size` + `api_version`). Non-zero return from visitor stops iteration early.
5. **Robust C Argument Contracts:**
   - App validation against live `s_active_app` before dereference.
   - Output pointers reset to NULL / 0 before processing.
   - Validation of string lengths, valid UTF-8, and rejection of embedded NUL characters.
   - Malloc buffers returned are freed with matching `LbStringFree` or `LbSearchFreeMigrationSnapshot`.
   - Idempotent and null-safe free helper.
6. **Ambiguous Null Provenance Resolution:**
   - Captured at `LbAppCreate` before any mutation occurs.
   - Explicitly records `LbSearchProvenanceAmbiguousNull` when `search_engine.engine` is `null` (acknowledging that `Settings::serialize_json()` writes `engine=null` on unrelated setting saves). Surfaces as needs-choice in migration, never falsely claimed as explicit user disablement.

---

## 4. Test Results Summary

All assertions in `TestNativeSettingsSearch` executed and passed deterministically:

```
=== TestNativeSettingsSearch starting ===
SHIM PROVENANCE: loaded from '/private/tmp/ladybird-isolated-build/liblagom-port.0.1.0.dylib'
CHECK: Bad argument handling before engine creation
PASS: pre-engine bad args handled safely
CHECK: Migration snapshot provenance capture
PASS [AbsentFile]: provenance=0, provider='', suggestions=false, legacy_autocomplete=false
PASS [AbsentKey]: provenance=0, provider='', suggestions=false, legacy_autocomplete=false
PASS [AmbiguousNull]: provenance=3, provider='', suggestions=false, legacy_autocomplete=false
PASS [ConfiguredCurrent]: provenance=1, provider='Google', suggestions=true, legacy_autocomplete=false
PASS [ConfiguredLegacy]: provenance=2, provider='Yahoo', suggestions=false, legacy_autocomplete=true
PASS [MalformedJSON]: provenance=4, provider='', suggestions=false, legacy_autocomplete=false
PASS: migration snapshot provenance distinguishes all 6 synthetic states accurately
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
PASS: configured provider formats query and encodes Unicode properly
CHECK: Explicit search-off prevents prose but preserves URLs
PASS: explicit off blocks prose and preserves direct URLs
CHECK: Unknown provider fails closed to Unavailable
PASS: unknown provider fails closed without Bing fallback
CHECK: Bad argument handling with live engine
PASS: bad args correctly rejected by live engine
=== ALL TestNativeSettingsSearch TESTS PASSED ===
```

---

## 5. Explicit Unrun / Blocked Capability Gates

1. **DNS Settings C ABI & DNS Probe (`TestNativeSettingsDNS`):** Gated and unrun pending Phase 0b.
2. **Custom Search Engine Add/Remove Editor:** Deferred from first slice; existing custom entries are visited and selectable, but editing UI is deferred.
3. **Address Bar Search Suggestions UI:** Gated pending native address dropdown integration.
4. **App-Side Integration:** `Sources/LadybirdBridge`, `Sources/LadybirdEngine`, `Sources/LadybirdUI`, and app tests remain untouched and pending coordinator review of this Phase 0a evidence.
