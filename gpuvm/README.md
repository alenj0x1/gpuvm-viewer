# GpuVM Viewer

GpuVM's viewer is a fork of [Moonlight PC](https://github.com/moonlight-stream/moonlight-qt) (GPL-3.0). It only
streams: the GpuVM app starts it, shows the progress, errors and the side panel, and talks to it over a private pipe.
Everything Moonlight does well (decoding, input, audio, gamepads) stays as upstream wrote it.

## What changes from upstream

GpuVM's own code lives in `app/gpuvm/`; upstream files only call it, a few lines each:

| File | Change |
|---|---|
| `app/gpuvm/bridge.*` | The private pipe to the GpuVM app (client side, token from the environment, fixed message list). |
| `app/gpuvm/viewport.*` | The picture's area: the side panel takes the right part of the window and the picture slides aside. |
| `app/gpuvm/sessionhooks.*` | Stages, errors, window, first picture and stats sent to the app. |
| `app/main.cpp`, `app/path.*` | Own settings and pairing identity, logs and caches, all under `%USERPROFILE%\AppData\LocalLow\GpuVM\Viewer` when GpuVM runs it (it streams at Low integrity); bridge start and stop; hidden Qt window. |
| `app/streaming/session.*` | Bridge hooks, commands from the app, window title. |
| `app/streaming/input/*` | Ctrl+Alt+Shift+G opens the panel; mouse and touch follow the picture's area. |
| `app/streaming/video/*` | First picture presented and stats; the picture's area in every Windows renderer, redrawn while it moves; graphite around the picture. |
| `app/gui/main.qml`, `app/gui/StreamSegue.qml` | Hidden window and no dialogs when GpuVM drives the viewer; no Moonlight update check. |

When GpuVM runs it, the viewer's exit code is 0 after a normal end (or a quit the app asked for) and 1 after an
error; its last message is `ended` with `reason` `closed` or `error`.
| `app/moonlight.ico`, `app/res/moonlight.svg` | GpuVM's icon. |

## Build (Windows)

Requirements: Visual Studio 2022 Build Tools (MSVC), Qt 6.12.0 for MSVC 2022 x64 in `C:\Qt`, 7-Zip on the PATH.

```powershell
git submodule update --init --recursive
powershell .\setup-deps.ps1
gpuvm\build.cmd Release
```

The result is `build\deploy-x64-release`; GpuVM ships it as `viewer\GpuVMViewer.exe`.

## Following upstream

The fork is a small patch set on top of an upstream tag, kept on the `gpuvm` branch:

```powershell
git fetch upstream --tags
git rebase --onto vX.Y.Z v6.2.0 gpuvm
```

After every new upstream version: build, run GpuVM's latency and smoothness labs (020, 021) and the bridge smoke test
(lab 039) before shipping it.

## License

GPL-3.0, like Moonlight. The source of every viewer build GpuVM ships is published. GpuVM itself (MIT) is a separate
program that only talks to the viewer over a pipe.
