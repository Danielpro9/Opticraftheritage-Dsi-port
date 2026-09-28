#pragma once

// Included directly (not via the DsiTuning.h umbrella, which is pulled in
// later than this file -- see PlatformTuning.h's include order) so the DSI_
// DIRECT_* constants below are already defined by the time the PLATFORM_DSI
// branch references them. Same reasoning as Ps2Tuning.h being pulled in
// early enough for the PLATFORM_PS2 branch's PS2_DIRECT_* references.
#if PLATFORM_DSI
#  include "dsi/tuning/DsiInputTuning.h"
#endif

// -----------------------------------------------------------------------------
// Input policy aliases
// -----------------------------------------------------------------------------
#if PLATFORM_PS2
#  define PLATFORM_ANALOG_MOVE_DEADZONE       PS2_DIRECT_MOVE_DEADZONE
#  define PLATFORM_ANALOG_MOVE_SCALE          PS2_DIRECT_MOVE_SCALE
#  define PLATFORM_DIRECT_CAMERA_ENABLED      PS2_DIRECT_PAD_CAMERA
#  define PLATFORM_DIRECT_CAMERA_DEADZONE     PS2_DIRECT_CAMERA_DEADZONE
#  define PLATFORM_DIRECT_CAMERA_SCALE        PS2_DIRECT_CAMERA_SCALE
#  define PLATFORM_DIRECT_CAMERA_INVERT_X     PS2_DIRECT_CAMERA_INVERT_X
#  define PLATFORM_DIRECT_CAMERA_INVERT_Y     PS2_DIRECT_CAMERA_INVERT_Y
#  define PLATFORM_DIRECT_CAMERA_REFERENCE_FPS PS2_DIRECT_CAMERA_REFERENCE_FPS
#  define PLATFORM_DIRECT_CAMERA_MAX_DT       PS2_DIRECT_CAMERA_MAX_DT
#elif PLATFORM_DSI
// D-pad -> movement (still the PLATFORM_DIRECT_ANALOG_MOVEMENT path PS2's
// analog stick also uses -- see DsiInputTuning.h for DSI_DIRECT_MOVE_SCALE
// and InputBackend_DSI.cpp's platformGamepadSnapshot() for how the D-pad
// produces leftX/leftY). Touch screen -> camera, but NOT through
// PLATFORM_DIRECT_CAMERA_ENABLED's stick-rate model: touch is a drag, not a
// self-centering stick, and integrating a held offset as a turn RATE (what
// that path does, correctly, for PS2's real stick) read as slow and laggy
// on real hardware. Left at 0 so EntityRenderer.cpp falls through to its
// ordinary PC-style mouse-delta branch instead, fed by
// dsiUpdateTouchCameraDelta() pushing real frame-to-frame drag pixels into
// lwjgl::Mouse the same way a real mouse's hardware delta would arrive --
// see that function's own comment (InputBackend_DSI.cpp) for the two
// earlier designs this replaced and why. The DEADZONE/SCALE/INVERT/
// REFERENCE_FPS/MAX_DT knobs below are meaningless once
// PLATFORM_DIRECT_CAMERA_ENABLED is 0 (EntityRenderer.cpp's stick-rate
// branch that reads them does not compile in), so DSi has no equivalents
// to plug in here any more -- sensitivity is instead
// GameSettings::mouseSensitivity, the same option PC's mouse already uses.
#  define PLATFORM_ANALOG_MOVE_DEADZONE       0.0f
#  define PLATFORM_ANALOG_MOVE_SCALE          DSI_DIRECT_MOVE_SCALE
#  define PLATFORM_DIRECT_CAMERA_ENABLED      0
#  define PLATFORM_DIRECT_CAMERA_DEADZONE     0.0f
#  define PLATFORM_DIRECT_CAMERA_SCALE        0.0f
#  define PLATFORM_DIRECT_CAMERA_INVERT_X     0
#  define PLATFORM_DIRECT_CAMERA_INVERT_Y     0
#  define PLATFORM_DIRECT_CAMERA_REFERENCE_FPS 60.0f
#  define PLATFORM_DIRECT_CAMERA_MAX_DT       0.10f
#else
#  define PLATFORM_ANALOG_MOVE_DEADZONE       0.20f
#  define PLATFORM_ANALOG_MOVE_SCALE          1.0f
#  define PLATFORM_DIRECT_CAMERA_ENABLED      0
#  define PLATFORM_DIRECT_CAMERA_DEADZONE     0.18f
#  define PLATFORM_DIRECT_CAMERA_SCALE        96.0f
#  define PLATFORM_DIRECT_CAMERA_INVERT_X     0
#  define PLATFORM_DIRECT_CAMERA_INVERT_Y     0
#  define PLATFORM_DIRECT_CAMERA_REFERENCE_FPS 60.0f
#  define PLATFORM_DIRECT_CAMERA_MAX_DT       0.10f
#endif
