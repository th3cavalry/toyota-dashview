# Vehicle Profile System — strip Toyota/Tacoma hardcodes

Blocked on PR #8 (LVGL port) — the remaining UI work lands on LVGL widgets.

## Done already (engine, on feat/lvgl-port @ 0782bca)
- OBD addressing via `getReqId()/getRespId()/getFuncId()`; `syncProfileSignals()` -> telemetry struct; NVS `dashview/prof` + `/profiles/<id>.json` load; SD picker scan (`scanProfileDir`).
- Schema (profiles/SCHEMA.md): `match{make,model,year_min,year_max,gen}` IS the picker UI (Make / Model / Year-range / Engine); `inherits: j1979_base` inherits universal PIDs; signals[] carries only proprietary/extra PIDs.

## Phase 1 — engine strip (on LVGL branch)
- [ ] `TacomaTelemetry` -> generic `VehicleSignal` fed only by profile signals; delete hardcoded `decodeTacomaFrame`/`sendToyotaObdQueries` legacy paths
- [ ] Built-in default profile = generic J1979 (no built-in Toyota profile JSON in flash)

## Phase 2 — branding strip
- [ ] Toyota splash -> profile logo/brand-color splash; `TOYOTA DASHVIEW - <SCREEN>` headers -> profile name or blank

## Phase 3 — multi-engine
- [ ] `engines[]` block in schema v2 (engine picker in wizard); per-engine signal sets

## Phase 4 — UI onto LVGL (lands with PR#8 S2/S3)
- [ ] Profile picker cells -> LVGL; Custom-Dash gauges enumerate `getSignalCount()/getSignalByIndex()`
- [ ] First-boot wizard via `match{}`

## Notes
- `getFuncId()` reserved for 0x7DF functional addressing
- `isListenOnly()` gates OBD polling; `isCanFd()/getArbBitrate()` unused (TWAI 500k hardcoded; CAN-FD SKU = MCP2518FD)
