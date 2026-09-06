
# DashView Touch Screen Collaboration Plan

## Current Status
- Hardware flash and boot verified on Waveshare 4.3B board (commit ed03f32)
- Device is connected via `/dev/ttyACM0` but cannot be accessed directly from this container
- The TRD logo displays correctly but touch functionality fails on subsequent screens
- Hardware validation is currently in progress (verify profile picker touch geometry y=272-314, x=34/222/410/598)

## Issue Analysis
The problem appears to be in the touch handling implementation in `src/main.cpp`, specifically:
- `handleTouch()` function branches per screen on raw `touchLastX/Y`
- Every render-card rect MUST have a matching touch y-range (see Settings cards)
- Profile picker touch geometry needs verification (cells y=272-314, x=34/222/410/598)

## Collaboration Tasks

### FlowZ13's Responsibilities:
1. Physical hardware testing and verification
2. Build and flash the device directly using `/dev/ttyACM0`
3. Test the touch functionality on actual hardware
4. Verify profile picker touch geometry mappings
5. Document any hardware-specific findings

### My Responsibilities:
1. Code review and analysis of touch handling implementation
2. Documentation and issue tracking
3. Providing recommendations for touch coordinate mapping fixes
4. Reviewing any code changes needed for touch functionality

## Next Steps

1. FlowZ13 will work on physical testing of touch functionality
2. I will focus on analyzing the existing touch handling code in src/main.cpp
3. We'll collaborate to identify and fix the root cause

## Communication
We'll coordinate via the #agent-collab channel in Discord.
