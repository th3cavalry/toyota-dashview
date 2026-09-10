# Profile Engine Wiring — Plan

Branch: `feat/4.3b-migration` @ 0782bca (engine commit)

## Phase 1 — main.cpp wiring (DONE, build SUCCESS)
- [x] OBD addressing from profile: `OBD_REQUEST_ID`/`OBD_RESPONSE_ID` macros -> `getReqId()`/`getRespId()` (3 call sites: sendToyotaObdQueries, sendIsotpFlowControl, decodeTacomaFrame)
- [x] `syncProfileSignals()`: fresh (`signalAge < 1500ms`) profile signals -> `vehicleData`; called in loop() before `updateDisplay()`; speed prefers `speed` (mph), falls back `speed_kmh`*0.621371
- [x] SD override in setup() step 7b: NVS `dashview/prof` -> `/profiles/<id>.json` -> `loadProfile()`; built-in `loadDefaultProfile()` stays fallback
- Legacy hardcoded Tacoma decode intentionally kept (profile wins by ordering; harmless double-write while both fresh)

## Phase 2 — verification (DONE)
- [x] `pio run` clean build (RAM 19.5%, Flash 35.7%) — .pio chowned to hermes after root drift, side build dir no longer needed
- [x] grep: no OBD_REQUEST_ID/OBD_RESPONSE_ID left; syncProfileSignals def+1 call

## Phase 3 — profile selection UX (IN PROGRESS — ConsolePC deleg_f07deff8)
- [ ] Settings screen: Vehicle Profile card — scan `/profiles/*.json` (`scanProfileDir`), 4 tap cells (up to 3 SD ids + BUILT-IN), `applyProfileSelection()` persists NVS `prof` + live `loadProfile()`, no reboot
- [x] format_sd.sh: profile seeding instructions appended

## Phase 4 — gauge unification
- [ ] custom_dash.inl `cdGetValue` keyed off profile signal keys (dynamic gauge list from `getSignalCount()`/`getSignalByIndex()`) instead of hardcoded uppercase table
- [ ] First-boot wizard using `match{}` metadata

## Notes
- `getFuncId()` unused so far — reserved for functional addressing (0x7DF broadcast queries)
- `isListenOnly()` should gate `sendToyotaObdQueries()` when a pure-tap profile is loaded
- `isCanFd()`/`getArbBitrate()` unused — TWAI init is hardcoded 500k classic; CAN-FD SKU needs MCP2518FD path
