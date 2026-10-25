# 0040. A SpaceMouse turns the camera freely, roll included

- Status: Accepted
- Date: 2026-10-25

## Context
ADR 0039 kept the orbit camera's +Z up for the SpaceMouse: the camera dropped the roll the 3DxWare Navigation Library set, and stopped the pitch short of straight up or down. That made navigation jumpy. The library computes each frame from its own copy of the camera, which still had the roll and the pitch past the pole, so what the app drew no longer matched what the library was moving: over the top, the library's eye kept going while the app's line of sight stopped, and the model jumped off screen. Tilting the cap left or right, which rolls the view, did nothing. Users expect a SpaceMouse to tumble the model in every direction, as it does in other CAD programs.

The camera also re-aimed its target each frame at the point on the new line of sight nearest the old target, while the library turns around its own pivot. The distance shrank as the two drifted apart, and fell to its minimum when the old target ended up behind the eye.

## Decision
This supersedes ADR 0039's decision that the camera keeps +Z up.
- The camera gets a roll angle around the line of sight. `Camera::setCameraToWorld` takes the library's camera-to-world transform exactly: heading, pitch (up to straight up or down) and roll, with the eye where the library put it, so `cameraToWorld()` gives back what was set. Turned over the top, the heading swings round and the roll is near 180 degrees.
- The target becomes the point on the new line of sight nearest the library's pivot, the point it turns around. When that point is not in front of the eye, the distance is kept.
- 3DxWare's Lock Horizon setting keeps the view level for those who want it; the app no longer does.
- The standard views level the camera again. A mouse orbit keeps any roll.
- While the library moves the view (`motion` is true), the viewport keeps drawing it as moving, so huge meshes stay on their coarse proxy even through a pause in its frames.

## Alternatives considered
- Turning Lock Horizon on for the app's 3DxWare profile and keeping +Z up: the library would stop rolling, but the cap's roll axis would do nothing and the jump over the pole would remain.
- A free camera stored as a quaternion: the same freedom, but the mouse orbit, the standard views and the tests are all written in heading and pitch.

## Consequences
- The 3D mouse moves the view smoothly in all six directions, and the mouse orbit afterwards turns around the point the 3D mouse last turned around.
- After rolling the view with the 3D mouse, a mouse orbit turns around +Z as before, which on a rolled or upside-down view does not follow the screen's directions. A standard view levels it first.
