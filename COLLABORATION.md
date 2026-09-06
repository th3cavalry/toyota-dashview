# DashView Project Collaboration Plan

## Current Status

The DashView project has successfully completed the hardware flash and boot verification on the Waveshare 4.3B board (commit ed03f32). The device is connected via `/dev/ttyACM0` but cannot be accessed directly from this container environment.

## Roles and Responsibilities

### FlowZ13 (Primary Coder)
- Has direct access to the physical hardware at `/dev/ttyACM0`
- Responsible for building, flashing, and testing on the actual device
- Can access the workspace and GitHub repository

### Hermes Agent
- Focuses on code review, documentation, and non-hardware related tasks
- Can review code changes and provide feedback on the implementation
- Will coordinate with FlowZ13 on specific implementation details

## Current Issues

1. **Touch Screen Functionality**: The TRD logo displays correctly but when moving to the next screen, the touch doesn't work properly. The screen also tweaks/side to side in different parts.
2. **Hardware Testing**: Need to verify profile picker touch geometry (cells y=272-314, x=34/222/410/598) and confirm hot-swap on a live bus.

## Next Steps

1. FlowZ13 will handle physical hardware testing
2. We'll work together on code improvements and review
3. Review and potentially update the touch handling logic in the UI code
4. Verify that the profile picker touch areas are properly mapped

## Technical Notes

- The project is on the `feat/4.3b-migration` branch
- Hardware validation is the active work
- Touch handling is in `handleTouch()` function which branches per screen based on raw `touchLastX/Y`
- Every render-card rect MUST have a matching touch y-range (see Settings cards)