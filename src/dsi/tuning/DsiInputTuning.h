#pragma once

// D-pad drives movement directly (MovementInputFromOptions.cpp), the same
// scheme PS2 already uses for its analog stick -- see PLATFORM_DIRECT_ANALOG_
// MOVEMENT in PlatformConfig.h. The D-pad is fully digital (each axis is
// -1, 0 or +1, from InputBackend_DSI.cpp's platformGamepadSnapshot()), so
// there is no deadzone to tune and no scale beyond 1:1.
#define DSI_DIRECT_MOVE_SCALE 1.0f

// The touch screen drives the camera through EntityRenderer.cpp's ordinary
// PC-style mouse-delta path (PlatformInputTuning.h sets
// PLATFORM_DIRECT_CAMERA_ENABLED to 0 for DSi, not 1 -- see its own comment
// for why touch, a drag rather than a self-centering stick, does not fit
// PS2's stick-rate model). There is no DSi-specific scale/invert constant
// to tune here any more: dsiUpdateTouchCameraDelta() (InputBackend_DSI.cpp)
// feeds a real frame-to-frame drag-pixel delta into lwjgl::Mouse, and from
// there GameSettings::mouseSensitivity (the same option PC's real mouse
// uses) is what controls feel -- one existing, already-exposed setting
// instead of a parallel DSi-only one.
