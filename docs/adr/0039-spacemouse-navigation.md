# 0039. Optional SpaceMouse navigation through the 3DxWare SDK, off by default

- Status: Accepted
- Date: 2026-10-25

## Context
SpaceMouse navigation was requested for the desktop app's viewport, using the 3Dconnexion 3DxWare SDK 4 (Navigation Library) on Windows. It is how most CAD and scan software is navigated with one hand while the other picks.

The SDK is proprietary. Its licence lets you use it, and distribute programs built with it in object code, only to work with 3Dconnexion hardware. It keeps its sources confidential, and it requires an attribution line in the About box or help. That licence is not compatible with the GPL: Mesh2Cad is GPL-3.0-or-later and links GPL parts of CGAL, so a binary that also links the SDK's `TDxNavLib.lib` can be built and used, but not distributed (the GPL needs the whole distributed work under the GPL). This SDK package is for Windows, and its C++ library is a static stub library built with MSVC that loads the 3DxWare driver at run time.

## Decision
- A CMake option `MESH2CAD_SPACEMOUSE` (default `OFF`) builds `src/app/SpaceMouse.cpp` into the app. It needs Windows, MSVC and the desktop app, and finds the SDK with `find_package(TDxWare_SDK 4.0 CONFIG)` from the `TDxWare_SDK_DIR` environment variable the SDK's readme asks for. Without it nothing changes, so CI, Linux and the release builds never see the SDK.
- The SDK is never fetched, vendored or committed; only our code that calls it is in the repository.
- `SpaceMouse` derives from the SDK's `CNavigation3D` (single-threaded, so callbacks arrive on the UI thread) and maps its accessors onto the viewport: the camera-to-world matrix (`Camera::cameraToWorld` and `setCameraToWorld`), the frustum and field of view, the visible bounds as the model extents, millimetres, a Z-up coordinate system with View > Front as the front view, the pivot (drawn as a dot) and hit-testing along a ray (`Viewport::pickRay`). The selection is never moved.
- The orbit camera keeps +Z up: roll is dropped and the pitch stops short of straight up or down, as with 3DxWare's Lock Horizon. A free camera would change every mouse and view path for a case few users want.
- With the option on, the About box carries the attribution the SDK licence asks for, and configuring prints that the build must not be distributed.

## Alternatives considered
- Reading the SpaceMouse as a HID device (Windows Raw Input), with no SDK: GPL-clean and distributable, but it bypasses 3DxWare's settings, speed, buttons and built-in views, and the Navigation Library is what was asked for.
- Turning the option on automatically when the SDK is found: convenient, but a release built on a machine with the SDK would silently link it.
- A licence exception for linking the SDK: possible for our own code, but not for CGAL's GPL code, so it would not make the binary distributable.

## Consequences
- The code is compiled and run only on a developer machine with the SDK; CI cannot build it. The camera conversion it relies on is tested on every platform (`tests/app`), and the SDK-facing file was compile-checked against the SDK's headers.
- Installers and release archives never include SpaceMouse support. Shipping it would need the SpaceMouse code moved to a separately distributed, non-GPL plugin or a HID implementation, which is a new decision.
- The driver's own view commands (Fit, Front, Top and so on) work; mapping the app's commands to the 3D mouse's buttons (`AddCommandSet`) is left for later.
