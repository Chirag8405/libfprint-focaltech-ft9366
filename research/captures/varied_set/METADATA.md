# varied_set capture metadata (2026-09-16/17)

Collected specifically to address the earlier `calibrated_set`'s insufficient variation (angle spread only
~3-5°, constant pressure), which left it unable to distinguish "no finger-identity signal" from "dataset didn't
vary enough to expose signal." See `research/PROTOCOL.md` for the full context and the real-pipeline results
computed from this set.

All captures: `FIXED_DAC=0x35` (same locked calibration point as `calibrated_set`, for consistent gain/FPN
conditions), taken live against the real FT9366/RTS5811 hardware (2808:a658) via `tools/rts5811_wake_test.c`,
with a full lift-off between every capture. Each capture's `Img_Get_Avg_Middle()` value confirmed in the
expected finger-present range (roughly 300-330, consistent with `calibrated_set`'s established 307-321 range)
before moving to the next capture -- no corrupt/blank frames in this set.

Angles are approximate, self-reported by the person capturing in real time (rotating the finger relative to
its neutral/straight orientation on the sensor), not measured with an instrument -- but span a REAL, verified
spread (0 to +/-15 degrees) rather than the previous dataset's narrow 3-5 degree clustering. Pressure is
likewise a qualitative self-report (light/medium/firm).

## Index finger (SAME finger set, 10 captures) -- one physical person's right index finger

| file | angle | pressure | avg_middle |
|---|---|---|---|
| idx01_0deg_light.raw       | ~0deg   | light  | 328 |
| idx02_0deg_firm.raw        | ~0deg   | firm   | 315 |
| idx03_5deg_light.raw       | ~5deg   | light  | 311 |
| idx04_5deg_firm.raw        | ~5deg   | firm   | 303 |
| idx05_10deg_light.raw      | ~10deg  | light  | 306 |
| idx06_10deg_firm.raw       | ~10deg  | firm   | 305 |
| idx07_15deg_light.raw      | ~15deg  | light  | 305 |
| idx08_15deg_firm.raw       | ~15deg  | firm   | 307 |
| idx09_neg10deg_medium.raw  | ~-10deg | medium | 318 |
| idx10_neg5deg_medium.raw   | ~-5deg  | medium | 319 |

Angle spread: -10 to +15 degrees (25-degree total range). Pressure spread: light/medium/firm all represented.

## Middle finger (DIFFERENT finger #1, 5 captures) -- same person's right middle finger

| file | angle | pressure | avg_middle |
|---|---|---|---|
| mid01_0deg_light.raw       | ~0deg   | light | 316 |
| mid02_5deg_firm.raw        | ~5deg   | firm  | 312 |
| mid03_10deg_light.raw      | ~10deg  | light | 307 |
| mid04_neg5deg_medium.raw   | ~-5deg  | medium| 310 |
| mid05_15deg_firm.raw       | ~15deg  | firm  | 306 |

## Ring finger (DIFFERENT finger #2, 5 captures) -- same person's right ring finger

| file | angle | pressure | avg_middle |
|---|---|---|---|
| ring01_0deg_firm.raw       | ~0deg   | firm  | 305 |
| ring02_5deg_light.raw      | ~5deg   | light | 304 |
| ring03_neg10deg_medium.raw | ~-10deg | medium| 305 |
| ring04_10deg_firm.raw      | ~10deg  | firm  | 305 |
| ring05_15deg_light.raw     | ~15deg  | light | 304 |

## Pairing convention for same/different-finger tests

- SAME-finger pairs: any two of `idx01..idx10` (C(10,2) = 45 possible pairs).
- DIFFERENT-finger pairs: any `idx*` paired with any `mid*` or `ring*`, OR any `mid*` paired with any `ring*`
  (mid and ring are different fingers from each other too, and both different from idx).
