# Issue Tracking: Touch Screen Functionality

## Problem Statement
The TRD logo displays correctly but when moving to the next screen, the touch doesn't work properly. The screen also tweaks/side to side in different parts.

## Root Cause Analysis
Based on the project documentation and the current state, this appears to be a touch handling issue in the UI implementation. The touch handling logic in the UI code needs to be verified, specifically:

1. The `handleTouch()` function in `src/main.cpp` which branches per screen on raw `touchLastX/Y`
2. Every render-card rect MUST have a matching touch y-range (see Settings cards)
3. Profile picker touch geometry needs to be verified (cells y=272-314, x=34/222/410/598)

## Acceptance Criteria
1. Touch functionality works correctly on all screens
2. Profile picker touch areas are properly mapped and functional
3. No screen tweaking or side-to-side movement during touch operations
4. All UI elements respond to touch as expected

## Related Documentation
- AGENTS.md: The hardware validation on the bench is the active work
- The touch handling section in AGENTS.md mentions that every render-card rect MUST have a matching touch y-range
- Settings screen touch geometry needs verification: cells y=272-314, x=34/222/410/598

## Tasks to Complete
1. [ ] Review the current touch handling implementation
2. [ ] Verify the touch coordinate mappings for all screens
3. [ ] Test and validate the profile picker touch areas
4. [ ] Implement any necessary fixes
5. [ ] Document the solution

## Related Issues
This issue is related to the "IN PROGRESS" section in AGENTS.md:
- Hardware validation on the bench is the active work: flash the build, verify profile picker touch geometry (cells y=272-314, x=34/222/410/598), and confirm hot-swap on a live bus.