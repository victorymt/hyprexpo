# AGENTS.md

Operational guide for automated agents working on HyprExpo, a Hyprland plugin.
Read this before touching build files, version strings, or Hyprland APIs.

## What this repo is

A single shared library (`hyprexpo.so`) loaded into the running Hyprland compositor.
It hooks `Monitor::CMonitor::addDamage` (two overloads) and `renderWorkspace`,
then draws an expose-style overview instead of the normal workspace render.

Layout is flat: the plugin sources live in the repo root, not `src/`.

```
main.cpp                  PLUGIN_INIT, function hooks, event listeners
Overview.{cpp,hpp}        overview state machine, workspace layout
OverviewRender.cpp        drawing (all g_pHyprOpenGL calls live here)
OverviewInteraction.cpp   pointer/keyboard input
OverviewPassElement.*     IPassElement implementation
HyprexpoLogic.{cpp,hpp}   pure layout math (unit-tested)
Dispatchers.cpp           hyprctl/Lua dispatchers
PluginConfig.cpp          config keywords
HyprexpoWorkspaceCompat.hpp  Hyprland API shims (see below)
HyprexpoRuntime.hpp       shared forward declarations
tests/                    HyprexpoLogicTests, OverviewSourceTests
docs/                     VitePress documentation
```

## Build, test, verify

```bash
make all      # -> hyprexpo.so
make test     # HyprexpoLogicTests + OverviewSourceTests, both -Wall -Wextra -Werror
make clean
```

`make` has **no header dependency scanning** (`-MMD` is not used). Rebuilds
happen because `SRC`/`HEADERS` are explicit lists. **If you add a source or
header file, you must add it to `SRC` or `HEADERS` in the Makefile**, otherwise
edits to it will not trigger a rebuild and the build will silently link stale code.

### Verify a build honestly

`make` is a no-op when everything is up to date, so this is **not** a valid check:

```bash
make 2>&1 | grep -cE "error:"     # prints 0 even when nothing was compiled
```

Use a full rebuild and look at the exit code:

```bash
make clean >/dev/null && make 2>&1 | tail -20; echo "exit=$?"
```

Before declaring a release-ready state, verify from a **fresh clone** (not just
the working tree), because that also validates that every needed file is
committed:

```bash
git clone <repo> /tmp/verify && cd /tmp/verify && make all && make test
```

Do not run `make` twice in the same step to fetch different slices of the same
output. Capture once, then filter the captured text.

## Versioning and release

`VERSION` is the single source of truth. `scripts/version.sh` appends
`-dev+<shorthash>` automatically when HEAD is not exactly on the matching tag,
which is normal during development.

```bash
make version            # print the version currently baked into builds
make version v0.58.0    # set + commit VERSION
make tag                # annotated tag matching VERSION
make publish            # push branch + tag -> release workflow
make check-version      # CI guard: VERSION must equal the tag
```

Because the dev marker is derived from HEAD, **a binary built before the
commit reports the pre-commit hash**. If a version string matters (release,
screenshot, bug report), commit first, then build.

## Upgrading to a new Hyprland release

This is the main maintenance task. Hyprland breaks plugin APIs often and
without deprecation. Work in this order.

### 1. Establish the exact target

```bash
hyprctl version                     # commit hash + tag of the RUNNING compositor
pkg-config --modversion hyprland    # version of the headers the build WILL use
pkg-config --cflags hyprland        # which header tree that actually resolves to
```

Do not assume `/usr/include/hyprland` is what gets compiled. `hyprland.pc` often
points `prefix` at the hyprpm cache instead:

```
prefix=/var/cache/hyprpm/<user>/headersRoot/include
```

so the build uses `/var/cache/hyprpm/<user>/headersRoot/include/hyprland/src/...`
even when `/usr/include/hyprland` exists. Always confirm the ABI hash from the
tree named in `pkg-config --cflags`, not from a hardcoded path:

```bash
grep -E "GIT_TAG|GIT_COMMIT_HASH" "$(pkg-config --variable=prefix hyprland)/hyprland/src/version.h"
```

The plugin will refuse to load unless the hash it was compiled against matches
the running compositor (`__hyprland_api_get_hash()` vs
`__hyprland_api_get_client_hash()` in `main.cpp`). Both must match.

Read the 0.57 handling of this in `HyprexpoWorkspaceCompat.hpp` before starting;
it shows which APIs the previous port had to paper over.

### 2. Find the breakage before fixing anything

Compile against the new headers and collect the **full** error list first:

```bash
make clean >/dev/null && make 2>&1 | grep -E "error:" > /tmp/errors.txt
wc -l /tmp/errors.txt; cut -d: -f1 /tmp/errors.txt | sort | uniq -c | sort -rn
```

Group errors by root cause, not by file. In the 0.56 -> 0.57 port, 66 errors
across 9 files came from 26 distinct causes; fixing them cause-by-cause cleared
the build in far fewer passes than fixing them file-by-file.

### 3. Prefer adapter shims over rewriting call sites

`HyprexpoWorkspaceCompat.hpp` exists so that layout code written against the
old vocabulary (`WORKSPACEID`, `WORKSPACE_INVALID`,
`getWorkspaceIDNameFromString`, `window->m_pinned`, ...) keeps compiling
unchanged. When an API is removed, **add a shim to that header first** and only
edit call sites when no shim is possible. This keeps future ports cheap.

### 4. Check the release pin

`hyprpm.toml` maps Hyprland commit -> plugin commit:

```toml
["<hyprland-commit-hash>", "<plugin-commit-hash>"], # vX.Y.Z
```

Both hashes are real values you can obtain: the Hyprland commit from
`hyprctl version` (or the `version.h` under `pkg-config --variable=prefix
hyprland`), and the plugin commit from `git rev-parse HEAD` **after** committing
the port. Do not invent hashes, and do not decide the pin is impossible before
the commit exists — commit first, then pin.

## Hyprland API knowledge that keeps mattering

### `#define private public` is load-bearing and fragile

`Overview.cpp` and `OverviewRender.cpp` deliberately do this:

```cpp
#define private   public
#define protected public
#include <hyprland/src/...>
#undef private
#undef protected
```

so the plugin can call members Hyprland declares non-public. This is required,
not decorative. Removing the two macros was measured to produce exactly five
errors:

```
renderWorkspace(...)        is protected within this context   (x2, Overview.cpp)
beginRender(...)            is protected within this context   (x2, Overview.cpp + OverviewRender.cpp)
renderTextureInternal(...)  is private within this context     (OverviewRender.cpp)
```

Note what is *not* in that list: `m_alpha`, `m_renderOffset` and
`m_forceRendering` come from `CWorkspacePresentable`, which declares them
`public`; `m_state` on `CWindow` is public too. So most member access in this
plugin is legitimate. The macros exist for the protected render entry points
and the private `renderTextureInternal`.

**The macros only apply to headers first parsed inside the define/undef window.**
Several Hyprland headers are included earlier through the plugin's own headers.
`HyprexpoWorkspaceCompat.hpp` is pulled in at the top of `Overview.cpp` (via
`Overview.hpp`) long before `#define private public`, and that is fine — the
build passes — precisely because no call site needs a private *member* from
those headers; the compat shims use public API (`visible()`, `m_state`,
`mapped()`).

The trap: `#pragma once` means a header's access levels are fixed by the first
include. Add a call site that touches a private/protected member of a class whose
header was already parsed above the macros, and it fails to compile with an
"is a private member" error that looks like an API change but is an include-order
problem. Since the macro window and the include graph both live in the same
files, this is easy to get wrong.

If you see a sudden flood of "is a private member" / "is an inaccessible base"
errors right after touching includes, check whether the header got parsed above
the macros before concluding the API changed. To use such a member, include that
header inside the window — do not assume `#pragma once` will re-open it.

### Function hooks must match an overload exactly

`renderWorkspace` has multiple overloads in 0.57. `main.cpp` selects the right
one by demangled signature, not by index:

```cpp
const auto RENDERWORKSPACE = std::ranges::find_if(FNS, [](const auto& function) {
    return function.demangled.find("Render::CRenderContext&") != std::string::npos
        && function.demangled.find("CBox const&") != std::string::npos;
});
```

Hooking the wrong overload corrupts the stack at runtime instead of failing to
build. **Whenever an API gains overloads, verify the hook target by signature**
and fail loudly (`failNotif` + throw) when it is not found.

### Render calls take a context

Since 0.57 the render pipeline threads a `Render::CRenderContext&` through.
`COverview::render`, `fullRender` and `renderWorkspacePreview` all take it, and
every `g_pHyprOpenGL` draw call (`renderTexture`, `renderRect`,
`renderTextureInternal`, `renderBorder`) needs it as the leading argument.

### Workspace identity

0.57 replaced the flat integer id with a variant
(`std::variant<SWorkspaceNumberedID, SWorkspaceSpecialID>`). `m_visible` became
private (`visible()` / `setVisible()`), `m_space` became private (`space()`),
and `getConfigName()` was removed in favour of
`Config::Actions::changeWorkspace(PHLWORKSPACE)`. All of this is behind the
compat header — do not call the raw members directly.

### Logging

Use Hyprland's `LOG(level, fmt, ...)` macro, which fills in the location
argument that `Log::CLogger::log` gained in 0.57:

```cpp
LOG(Log::ERR, "[hyprexpo] invalid border color config: {}", effectiveSpec);
```

The macro expands to `Log::logger->log(level, Log::logFnName(), ...)`, so a bare
`Log::logger->log(level, "fmt", ...)` will not compile. There are currently six
`LOG` call sites (Overview.cpp, OverviewInteraction.cpp x2, OverviewRender.cpp
x3). Do not use `printf`; for user-visible failures also raise a notification
via `HyprlandAPI::addNotification`.

## Testing against a live compositor

`hyprctl dispatch` no longer takes plugin dispatcher strings in 0.57+; the
argument is evaluated as Lua and the old syntax fails with a hint:

```
$ hyprctl dispatch 'hyprexpo:expo'
error: 3 [string "return hl.dispatch(hyprexpo:expo)"]:1: function arguments expected near ')'
 → Note: dispatch in lua is a shorthand for hl.dispatch(...), your syntax might need to be updated.
```

Plugin dispatchers are addressed through the plugin table:

```bash
hyprctl eval 'hl.plugin.hyprexpo.expo()'
```

Useful commands on 0.57. Note the request is two words — `hyprctl plugin list`
or `hyprctl plugins list` both work, but a bare `hyprctl plugins` fails with
"Not enough arguments in '/plugins', expected at least 1", and
`hyprctl listloadedplugins` does not exist:

```bash
hyprctl plugin list                 # verify the loaded plugin and its version string
hyprctl plugin load /abs/path/hyprexpo.so
hyprctl plugin unload /abs/path/hyprexpo.so
hyprctl version
```

`hyprctl eval` returns only `ok`. Lua `print()` output goes to the Hyprland log,
not to stdout — to read a value back, write it to a file from Lua and read that.

`HYPRLAND_INSTANCE_SIGNATURE` matters: after a Hyprland restart the previous
instance directory is left behind without a socket. Pick the entry in
`/run/user/$UID/hypr/*/` that actually contains `.socket.sock`, and set
`XDG_RUNTIME_DIR` explicitly for non-interactive shells.

## hyprpm needs sudo

`hyprpm add` and `hyprpm update` call `NSys::root::cacheSudo()` and install
headers via sudo (`make installheaders` into `/var/cache/hyprpm/<user>/`).
Without passwordless sudo they fail late, after a long clone/build, with:

```
failed to install headers with error code 4 (Headers version mismatched)
```

That message is misleading: it means the sudo step failed, not that versions
disagree. `/var/cache/hyprpm/` is root-owned, so an unprivileged agent cannot
fix it. Check `sudo -n true` **once, up front** before launching hyprpm, and hand
the step to the user if it fails.

Also budget the time: a fresh `hyprpm update` does a full recursive clone of
Hyprland including submodules and takes tens of minutes. Run it as a managed
background job and wait on it; do not start it repeatedly with short timeouts.

Loading a development build needs no sudo:

```bash
make dev-build && make dev-load     # builds to ~/.cache/hyprexpo and loads by abs path
make dev-reload                     # unload + load
./scripts/run-nested.sh             # disposable nested session
```

Note that a manually loaded `.so` and a hyprpm-installed copy are separate
plugins. Unload one before loading the other, or restart Hyprland.

## Working rules (learned the hard way)

1. **Enumerate, do not guess.** If a listing interface exists, use it first:
   GitHub trees API for file paths, `pairs(hl)` for Lua namespaces,
   `find`/`glob` for headers. Sequential guessing burns far more calls than one
   broad listing.
2. **Distinguish reads from writes before bulk edits.** A blind text replace of
   `window->m_pinned` produces syntactically invalid assignment targets
   (`state.window->m_pinned = x` -> garbage). List the sites, classify them, then
   edit deliberately. Prefer a shim or a targeted `edit` over `sed -i`.
3. **Assess global side effects of "one harmless line".** New includes,
   `#pragma once` interactions, and macro state can invalidate large parts of the
   build. Ask what gets included, how early, and under which macros.
4. **Validate prerequisites once, before long jobs.** Passwordless sudo,
   available disk, network reachability, correct instance signature — one
   command each, before committing to a 50-minute operation.
5. **One authoritative check, not repeated partial checks.** Full clean rebuild
   once beats two incremental greps. Do not re-run the same command hoping for a
   different reading.
6. **Do not publish conclusions about what is impossible until the state that
   determines it exists.** Commit before judging whether hashes can be pinned.
7. **Do not push without being asked.** Commit locally, then confirm. This repo
   has release automation (`.github/workflows`) that reacts to tags.

## Documentation

`docs/` is a VitePress site; `site/` holds the generated output. Markdown docs
are the source. When behaviour or supported versions change, update
`docs/reference/compatibility.md` (it names the verified Hyprland version and
ABI string) alongside the code, then rebuild the site with
`./scripts/build-docs.sh` / `./scripts/build-site.sh` if relevant.
