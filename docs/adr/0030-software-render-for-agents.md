# 0030. Agents see the document through a software-rendered PNG

- Status: Accepted
- Date: 2026-10-20

## Context
A person sees what each command did in the viewport. An agent driving the CLI, JSON channel or MCP server gets only numbers back, so it cannot tell that a fit landed on the wrong face or a solid came out inside out (#72). The viewport is OpenGL inside the Qt app, which a headless engine has neither of.

## Decision
`view.render` draws the visible objects with a small software rasteriser in `core` (`m2c/core/Raster.h`: orthographic, flat shaded like the viewport, depth tested) and writes the picture as PNG with miniz, which `io` already uses. The viewport's colour map for deviations, sketch polylines and cage edges move from `app` to `core`, so both draw the same thing. A command result that names a picture it wrote as `{"image": {"path", "mime_type"}}` is also returned by the MCP server as an image content block, so an agent sees it without reading the file.

## Alternatives considered
- Offscreen OpenGL (EGL or OSMesa): matches the viewport exactly, but needs a GPU stack or a new dependency on every machine that runs the engine headless, including CI.
- Draw with Qt's `QPainter` into a `QImage`: no depth buffer, and it would pull Qt below `app`.
- Return the PNG as base64 in every result: no file to clean up, but it floods the CLI and JSON output.

## Consequences
- Rendering works anywhere the engine runs, with the same pixels for the same document.
- The picture is a plain orthographic view: no lighting beyond the viewport's, no anti-aliasing, and one-pixel lines.
- The MCP server reads the file it was told about. Over the app's local socket that is the same machine, so the path is readable.
