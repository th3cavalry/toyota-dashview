# DashView Vehicle Profile — data schema (v1)

A **profile** is one JSON file per make/generation, e.g. `toyota_tacoma_2016-2023.json`.
It carries everything vehicle-specific so the firmware stays vehicle-agnostic.
Adding a car = writing a small text file, never recompiling.

## Top-level shape
```jsonc
{
  "id": "toyota_tacoma_2016_2023",   // unique, filesystem-safe
  "name": "Toyota Tacoma (3rd Gen)",
  "match": {                          // drives the first-boot setup wizard
    "make": "Toyota", "model": "Tacoma",
    "year_min": 2016, "year_max": 2023, "gen": "3rd"
  },
  "inherits": "j1979_base",           // pulls the universal SAE J1979 signal set
  "assets": { "logo": "toyota_trd" }, // boot splash id + brand color
  "bus":    { ... },                  // Layer 1: physical + transport (below)
  "signals": [ ... ]                  // Layer 3: proprietary deltas only
}
```

## Layer 1 — `bus` (one block per profile)
| field | meaning |
|-------|---------|
| `physical` | `"classic"` \| `"canfd"` — selects TWAI vs MCP2518FD at runtime (the two SKUs) |
| `arb_bitrate` | arbitration-phase bitrate (e.g. `500000`) |
| `fd_data_bitrate` | CAN-FD data-phase bitrate, or `null` for classic |
| `protocol` | `"iso_tp"` \| `"uds"` \| `"j1850"` |
| `req_id` / `resp_id` / `func_id` | request / response / functional addressing ids (`"0x7E0"` etc), or `null` for pure listen-only |
| `listen_only` | `true` = device taps the trunk and decodes broadcasts, no requests needed |

## Layer 2 — universal baseline (NOT in the profile)
`inherits: "j1979_base"` gives every OBD-II-compliant car the standard PIDs with
**standardized SAE J1979 formulas** — identical across all makes (RPM `(256A+B)/4`,
coolant `A-40`, lambda `(256A+B)/32768`, …). So a profile only lists what is
proprietary or differs from the baseline. This is why supporting a new make is a
small file, not a full re-decode.

## Layer 3 — `signals[]` (declarative, no per-car code)
Every entry has a `key` (matches a Custom-Dash gauge / logger field) and a `kind`:

### `kind: "obd_poll"` — standard/extended polled PID
`{ mode, pid, a, b, c, unit, decimals }` → value = `a*(raw) + c` (J1979 A/B/C).
Use for any ECU you talk to over OBD/ISO-TP (baseline PIDs already inherited;
list here only proprietary modes, e.g. Toyota `0x21 0xA2`).

### `kind: "can_broadcast"` — listen-only trunk frame, linear value
```jsonc
{ "key":"rpm", "kind":"can_broadcast",
  "can_id":"0x2C4", "start_bit":0, "bit_len":16,
  "order":"motorola",          // "motorola"=MSB-first/big-endian | "intel"=little-endian
  "scale":0.25, "offset":0, "signed":false,
  "unit":"", "decimals":0 }
```

### `kind: "can_enum"` — broadcast field mapped to text
```jsonc
{ "key":"gear", "kind":"can_enum", "can_id":"0x3BC",
  "start_bit":0, "bit_len":8, "order":"motorola",
  "map": { "0":"P", "1":"R", "2":"N", "*":"D" } }   // "*" = default
```

## Reserved for v2 (fields exist so adding them isn't a schema break)
- `counter_bits`, `crc` — rolling counter / checksum on GM & Stellantis frames.
- `expr` — a tiny formula for rare nonlinear signals the A/B/C form can't express.
- `security` — UDS seed/key level needed before a `uds_read` is allowed.

## Adding a vehicle you don't ship
Write the JSON (reuse `j1979_base`; add only the proprietary signals), validate,
drop it on the SD card or push over OTA. No firmware rebuild.
