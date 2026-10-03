# AGENTS.md

Moonlight PC — a C++/Qt6/qmake game-streaming client. qmake only, **no CMake**.

## Layout

Root `moonlight-qt.pro` is `TEMPLATE = subdirs` and builds, in order:
`moonlight-common-c`, `qmdnsengine`, `app`, `h264bitstream`, plus `AntiHooking` (win32, non-winrt).

- `app/` — the client. `main.cpp` is the entry point (~1076 lines of platform/graphics/logging/QML bootstrap before any app logic).
  - `backend/` — GameStream host discovery + pairing. **All in this repo.**
  - `streaming/` — session, decoders, renderers, audio, input. The bulk of the work.
  - `gui/` — QML views + `appmodel`/`computermodel` list models.
  - `settings/` — `StreamingPreferences` + gamepad mapping persistence.
  - `cli/` — `list` / `stream` / `quit` / `pair` argument parsing and launchers.
- `moonlight-common-c/`, `qmdnsengine/`, `app/SDL_GameControllerDB/` — git submodules.
- `h264bitstream/` — in-repo vendored fork (not a submodule).
- `wix/` — WiX MSI + x86 setup bootstrapper projects.
- `config.tests/` — qmake **compile probes**, not unit tests. See "Verification".

## Prerequisites

```powershell
git submodule update --init --recursive
powershell .\setup-deps.ps1          # Windows only
```

`setup-deps.ps1` downloads prebuilt SDL2/FFmpeg/OpenSSL/libplacebo bundles into `libs/windows`.
Without it `app/app.pro:39-41` hard-errors. Re-run both after every `git pull`.
MSVC only (MinGW is unsupported); Qt 6.11+.

## Build — dev loop (shadow build)

```powershell
mkdir build\dev-x64-release
cd build\dev-x64-release
qmake ..\..\moonlight-qt.pro
nmake release          # or: jom release   (debug target also exists)
```

Binary lands at `build/dev-x64-release/app/release/Moonlight.exe`.
`CONFIG += debug_and_release` produces **separate `debug` and `release` Makefile targets** — there is no
combined `debug_and_release` target at the top level, so always name one explicitly.
Re-run `qmake` after editing any `.pro`/`.pri` file.

## Local dev isolation

**This machine has an installed official Moonlight**, and a dev build resolves to the *same* state
unless you isolate it. The failure is silent — no warning, no error:

| Shared location | Contents |
|---|---|
| `HKCU\Software\Moonlight Game Streaming Project\Moonlight` | QSettings on Windows uses `NativeFormat` (registry): `certificate`/`key` (client identity), `hosts\`, `gcmapping\`, every stream preference |
| `%LOCALAPPDATA%\Moonlight Game Streaming Project\Moonlight\cache` | `gamecontrollerdb.txt`, `boxart\`, `qmlcache\`, `qtpipelinecache-x86_64-*` |

Two real hazards: the client identity is shared, so a dev crash mid-write can leave *both* builds
unpaired from every host; and the Qt 6 RHI pipeline cache is shared while Debug and Release emit
different shader binaries into it.

Fix: drop an empty `portable.dat` next to `Moonlight.exe` — `main.cpp:435` switches QSettings to
`IniFormat` rooted at the CWD and `Path::initialize(true)` makes every log/cache/boxart path CWD-relative.
Qt Creator's default working directory is `%{RunConfig:Executable:Path}`, so exe-adjacent placement needs
no Run-settings change.

**Put it in *both* `app/debug/` and `app/release/` of *every* build root.** `CONFIG += debug_and_release`
(`moonlight-qt.pro:16`) makes qmake emit both Makefiles for every build config, so the exe can land in
either subdir depending on which target was built — and a missing `portable.dat` reverts to sharing
without a word:

```powershell
foreach ($r in Get-ChildItem build -Directory) {
  foreach ($sub in 'debug','release') {
    $d = Join-Path $r.FullName "app/$sub"
    New-Item -ItemType Directory -Force $d | Out-Null
    New-Item -ItemType File -Force (Join-Path $d 'portable.dat') | Out-Null
  }
}
```

Confirm it took effect — both must hold:

- `<build root>/app/<config>/Moonlight Game Streaming Project/Moonlight.ini` exists. It is *not* at the
  root: `QSettings::setPath()` supplies only a base, and `IniFormat` still appends `<org>/<app>.ini`.
- the log says `Found "gamecontrollerdb.txt" at "<exe dir>/cache/gamecontrollerdb.txt"`. A `:/data/...`
  fallback means the isolated cache is simply empty (expected on first run).

Costs of isolating: a fresh `certificate`/`key` and `uniqueid` are generated, so the dev build must
**re-pair every host**. And `portable.dat` lives under gitignored `build/`, so `make distclean` or
deleting `build/` removes it silently.

Bonus: `LOG_TO_FILE` is defined for every Windows build (`main.cpp:58-68`), but `main.cpp:462-465`
writes the log file only when stderr is *unspecified* — i.e. **not** under a debugger or with redirected
stdout. So Qt Creator shows logs in Application Output and writes no file, while launching the exe
directly drops `Moonlight-<epoch>.log` beside it, inside the isolated dir.

## Build — packaging

Run from the repo root in a **Qt command prompt** (Qt `bin` and 7-Zip must be on `%PATH%`):

```powershell
scripts\build-arch.bat release        # or debug | signed-release
scripts\generate-bundle.bat release   # x86 bootstrapper; needs both MSIs to already exist
scripts\build-portable-zip.bat        # portable zip only; no MSI, no WiX toolchain needed
```

`build-arch.bat` does windeployqt + WiX MSI + 7z portable zip, and **detects the target arch from the
qmake on `PATH`** — the second CLI argument that CI passes (`Release x64`) is ignored.
`build-portable-zip.bat` is a standalone sibling, byte-identical to `build-arch.bat` except for the
five differences it documents at each site: header comment, `vswhere -products *`, `signed-release`
dropped from the config switch (no argument means `release`), the MSI step gone, and `portable.dat`
written unconditionally instead of keying off `CI_VERSION`. It emits the same
`build\installer-<arch>-<config>\MoonlightPortable-<arch>-<version>.zip`, just without the installer.
**Keep it diffable against `build-arch.bat`** — port upstream's build changes into it rather than
letting the two drift. Never `msiexec` its output: the `UpgradeCode` in `wix/Moonlight/Product.wxs` is
the official Moonlight one, so installing it replaces an existing official install and the uninstall
custom action deletes `HKCU\Software\Moonlight Game Streaming Project`.

Note that `build-arch.bat` itself needs `vswhere -products *` to find Visual Studio at all when the
only installed product is Build Tools — without it `-latest` returns nothing, `vcvarsall` never runs,
and the build dies on the first `cl.exe`. `build-portable-zip.bat` carries that fix;
`build-arch.bat` is left byte-identical to upstream on purpose, so producing an MSI here needs the
patch applied locally.
Linux/macOS dev builds: `qmake6 moonlight-qt.pro && make release`.

## Verification

**There is no test suite, no linter, no formatter, and no codegen.** Verified: zero `QTEST`/`QtTest`/
`gtest` usage, no `.clang-format`/`.clang-tidy`/`.editorconfig`/pre-commit config, no `tests/` target.
`.github/workflows/` is build-only across four targets (AppImage, Steam Link, Windows x64 + ARM64, macOS).

So verification == "does it still compile on every affected platform". Do not invent a test command.

`moonlight-qt.pro:19-21` runs `qtCompileTest(SL)` / `qtCompileTest(EGL)` at qmake time to detect the
Steam Link SDK and the EGL dev package. Results land in `build/<dir>/config.log` and `config.log`'s
verdict is cached in `build/<dir>/.qmake.cache` (`CONFIG += done_config_SL`). **Check `config.log` when a
feature silently disappears** — the probe failing is the only signal.

## qmake gotchas

- `app/app.pro` lists **every** source and header explicitly. A new `.cpp`/`.h` is invisible until you add
  it. Same for new QML files (`app/qml.qrc`), resources (`app/resources.qrc`), and translations
  (`TRANSLATIONS` in `app.pro`).
- `app/streaming/video/ffmpeg_videosamples.cpp` is `#include`d by `ffmpeg.cpp:54` so `sizeof()` works on
  its test frames. It is deliberately **not** in `SOURCES` — do not add it.
- `app/main.cpp:25-26` defines `SDL_MAIN_HANDLED` before including `SDL_compat.h`, because Qt and SDL
  both try to own `main()`. Do not reorder.
- `app/app.pro:36` sets `QT_DISABLE_DEPRECATED_BEFORE=0x060000` — touching any Qt API deprecated before
  6.0.0 is a **compile error**, not a warning. (The README still mentions Qt 5 support; `app.pro` is the
  executable source of truth.)
- Platform features are pure qmake config. `CONFIG += embedded`, `gpuslow`, `glslow`, `vkslow`,
  `enable-cuda`, `disable-ffmpeg`, `disable-libva`, `disable-prebuilts`, ... Each guarded block also sets
  a `HAVE_*` define that the C++ `#ifdef`s on. Adding a feature means editing the `.pro` *and* the C++.
- `app/shaders/*.fxc` are **checked-in compiled binaries** from the `.hlsl` next to them. If you change a
  `.hlsl` you must recompile (`app/shaders/build_hlsl.bat`, needs the Windows SDK `fxc`) and commit the
  `.fxc`. All four are embedded via `resources.qrc` under `/data`.

### Adding a video renderer

There is no registry or factory table — renderers are `new`-ed inline. Five coordinated edits:

1. `ffmpeg-renderers/<name>.{h,cpp}`, class `<Tech>Renderer : public IFFmpegRenderer`.
2. Add the `RendererType` enum value (`renderer.h:142-156`) **and** a `getRendererName()` case (`:314-344`).
3. A `CONFIG`-gated `SOURCES`/`HEADERS`/`DEFINES += HAVE_<X>` block in `app/app.pro`.
4. `#ifdef HAVE_<X> #include "ffmpeg-renderers/<name>.h" #endif` in `ffmpeg.cpp:16-51`.
5. Hook into `FFmpegVideoDecoder::createHwAccelRenderer()` and/or `tryInitializeRendererForUnknownDecoder()`
   (`ffmpeg.cpp:991-1128`, `:1292-1759`). Files are lowercase-concatenated (`d3d11va.cpp`, `plvk.cpp`),
   classes are PascalCase + `Renderer`; non-renderer helpers drop the suffix (`swframemapper`, `genhwaccel`).

Failure-reason semantics matter: returning `NoSoftwareSupport` permanently blacklists the renderer for
the session; returning `NoHardwareSupport` aborts all remaining hwaccels for that codec.

## Architecture

- **QML registration is imperative** — `qmlRegister*` calls in `app/main.cpp:934-961`. There are no
  `QML_ELEMENT`/`QML_SINGLETON` macros anywhere. Each type is registered under a module name equal to the
  class name, so QML does `import StreamingPreferences 1.0` and then uses the bare class name.
- `AppModel` is **not** the app model — it is a `QAbstractListModel` over one host's game list
  (created in QML via `Qt.createQmlObject`). `ComputerModel` is the host list.
- **Pairing/crypto is hand-written OpenSSL in this repo** (`backend/identitymanager.cpp`,
  `backend/nvpairingmanager.cpp`) — it is *not* in moonlight-common-c. moonlight-common-c only supplies
  the `Li*` streaming core and exposes exactly one header, `Limelight.h`.
- `Session` (`streaming/session.{h,cpp}`) owns the stream and has **no C++ state enum** — the connection
  stages are moonlight-common-c's `STAGE_*` / `LiGetStageName()`.
- Decoder selection is two levels: `Session::chooseDecoder()` picks `SLVideoDecoder` (Steam Link) or
  `FFmpegVideoDecoder`; then `FFmpegVideoDecoder` picks a **backend** renderer (receives decoded frames)
  and a **frontend** renderer (presents them). Frontends like `PlVkRenderer` / `EGLRenderer` /
  `DrmRenderer` are constructed *wrapping* a backend instance.

### Threading rules you will break if you don't know these

- `Session::exec()` runs on the **Qt main thread and takes it over for the entire stream**
  (`QSG_RENDER_LOOP=basic` in `main.cpp:635` depends on this). The Qt GUI is frozen until the stream ends.
- All SDL window and `m_VideoDecoder` create/destroy must happen on the **main thread**, under
  `m_DecoderLock`. Decoder callbacks from other threads use `SDL_TryLockMutex`; failing to get the lock
  means "the decoder is going away" — return success and drop the frame.
- `LiStopConnection()` runs on a `QThreadPool` thread in `DeferredSessionCleanupTask`; the video decoder
  must already be destroyed by then (asserted).
- Only one session at a time (`s_ActiveSessionSemaphore`).
- moonlight-common-c's `cl*` callbacks (rumble, triggers, motion, LED, adaptive triggers) arrive off the
  main thread and are deliberately deferred to the main thread as SDL user events `101-105`.

## Conventions that differ from the framework defaults

- **Logging is split by directory, and both halves funnel into one sink:**
  - `app/streaming/**` (and anything touching FFmpeg/renderers): `SDL_LogInfo/LogWarn/LogError(SDL_LOG_CATEGORY_APPLICATION, ...)`.
  - `app/backend/**`, `app/settings/**`, `app/gui/**`: `qInfo()` / `qWarning()` / `qCritical()`.
  - Match the surrounding directory. Use `SDL_LogMessageV` when you already have a `va_list`.
- `logToLoggerStream()` (`main.cpp:117-154`) redacts `&rikey=` and `&rikeyid=`, stamps relative time, and
  caps logs at 10 MB. Never log pairing secrets directly.
- **Settings** use flat, lowercase `#define SER_* "<key>"` macros at the top of each `.cpp` — no groups,
  no prefixes. Enums are persisted as raw `int`, so **append new enum values at the end of the enum**;
  inserting renumbers existing values and corrupts user preferences (`streamingpreferences.h:74-76`).
  Schema migration goes through `CURRENT_DEFAULT_VER`. Nothing is written to disk until QML calls
  `StreamingPreferences.save()`.
- `QCoreApplication::setOrganizationName/ApplicationName` must be set **before** `Path::initialize()`
  (`main.cpp:429-433`).
- A `portable.dat` file in the CWD switches QSettings to INI format rooted at the CWD and makes all
  log/cache/boxart paths CWD-relative. This is how the Windows portable ZIP works, and how a dev build
  isolates itself from an installed Moonlight — see "Local dev isolation".
- `THROW_BAD_ALLOC_IF_NULL(x)` from `app/utils.h` is the convention for OpenSSL allocation checks.
- `NvComputer` splits fields into ephemeral vs persisted; **adding a persisted field requires updating
  `isEqualSerialized()`** (`nvcomputer.h:116`).
- `NvComputer` and friends are guarded by a `CopySafeReadWriteLock` — take the read lock for reads,
  write lock for writes, and never acquire `ComputerManager::m_DelayedFlushMutex` while holding a
  computer lock.
- `app/backend/nvhttp.h` uses exceptions for error handling (`GfeHttpResponseException`,
  `QtNetworkReplyException`), not return codes.
- Style: 4-space indent, Allman braces, no formatter to run — match the surrounding file by hand.

## Runtime & debugging

- On Windows and macOS release builds, logs go to `Path::getLogDir()/Moonlight-<epoch>.log`
  (`%TEMP%` on Windows, `/tmp` on macOS); the 10 newest are kept. The path is printed on startup
  ("Redirecting log output to ..."). Debug macOS builds and Linux log to the console.
- Debug env vars use `Utils::getEnvironmentVariableOverride()` (typed, returns false if unset):
  `DECODER_CAPS`, `GL_IS_SLOW`, `VULKAN_IS_SLOW`, `COLOR_SPACE_OVERRIDE`, `COLOR_RANGE_OVERRIDE`,
  `MATCH_DISPLAY_MODE_TO_VIDEO`, `SEPARATE_TEST_DECODER`, `HAS_DESKTOP_ENVIRONMENT`, `FORCE_QT_GLES`.
- Plain `qgetenv` overrides worth knowing: `D3D11VA_ENABLED=0`, `D3D11VA_DEBUG_LAYER`,
  `D3D11VA_FORCE_SEPARATE_DEVICES`, `DXVA2_ENABLED=0|1`, `{H264,HEVC,AV1}_DECODER_HINT`,
  `VAAPI_FORCE_DIRECT|VAAPI_FORCE_INDIRECT`, `DRM_FORCE_DIRECT|DRM_FORCE_EGL`, `ML_AUDIO=sdl|slaudio`,
  `STREAM_GAMECONTROLLER_IGNORE_DEVICES`, `NO_GAMEPAD_QUIT=1`, `PREFER_VULKAN=1`.
- CLI: `Moonlight.exe list <host> [--csv] [--verbose]` is the only headless action. `stream`, `quit`, and
  `pair` all boot the full QML GUI. **There is no IPC with a running instance** — every CLI invocation is
  a new process with its own `ComputerManager`, mDNS browser, and polling threads.
- `StreamCommandLineParser` **writes flags straight into the live `StreamingPreferences` singleton**
  (`commandlineparser.cpp:388-423` sets `preferences->width`, `->fps`, `->bitrateKbps`, ...) rather than
  passing overrides through. Anything you add to a CLI parser should follow the same pattern.

## Contributing

- Branch off `master`, PR into `master`.
- Do **not** touch `app/version.txt` in feature PRs — it is only bumped by `Prepare for vX.Y.Z` release
  commits. CI overrides it with `CI_VERSION`.
- `app/languages/qml_*.ts` (and the checked-in `.qm`) are managed on Weblate. Don't hand-edit them.
- The exception is a string **this fork invented**, which upstream Weblate cannot see: its project
  (`hosted.weblate.org/projects/moonlight/moonlight-qt`) is bound to `moonlight-stream/moonlight-qt` and
  syncs only when upstream's own `Rerun lupdate` picks the source text up, so a fork-only feature's
  strings never appear there and its locale stays English until someone translates them by hand. The
  upstream division of labour still holds — write `<translation>` into the `.ts` (which is all a
  Weblate commit ever does; all 277 of them touch a `.ts` and never a `.qm`), then regenerate the
  `.qm`. Never run `lupdate` yourself to "refresh" the file: that rewrites all 30 locales and is the
  maintainer's step. `lrelease` and `lupdate` live next to `qmake` on `PATH`, not in the repo.
- Dependabot updates the three submodules daily; `moonlight-common-c` bumps are usually the fix for
  streaming regressions (frame loss, connection setup) rather than anything in `app/`.
