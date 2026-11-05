# #1218 B10 Color parsing, palettes and terminal color capability recaptured at pi-v1.3.0

## Scope

Route batch B10 of 17 (`docs/research/pi-v1.3.0/1182-tui-route-batches.json`,
disposition `comparison`). Recapture color representation (Color, IndexedColor,
RgbColorValue, OklchColorValue, OkhslChannels, OklchChannels), parsing, color conversion
and gamut mapping, OKLCH and OKHSL polar color math, OKLab to OKHsl lightness conversion,
sRGB vs OKLCH color mixing, and terminal text styling (foreground/background ANSI, TextAttributes,
TextStyle, styleText, styleTextWithAnsi) at the frozen pi-v1.3.0 baseline
(`71023aec1546d6d677a65669c5665c0927ce0dd0`) and compare the C++ rows through the named caller
`cch::tui::ColorUtils` (src/tui/ColorUtils.cpp, include/cch/tui/Utils.hpp). All 49 batch rows
are receipted whole-row through the delivery contract.

## Delivered behavior

- The `audit-1218` family captures the v1.3.0 behavior of color representation and text styling
  through the frozen checkout's own modules (`packages/tui/src/colors.ts`, `packages/tui/src/oklab.ts`):
  1. `color-values-and-construction`: indexed color construction (0, 9, 15, 16, 42, 231, 232, 255),
     RGB color construction across the full dynamic channel range, and OKLCH color construction;
     verifies the canonical channel names (`r`, `g`, `b`, `l`, `c`, `h`, `index`) and `kind` tags;
  2. `color-parsing-and-hex`: 3-digit and 6-digit hex color parsing with lowercase canonical hex output,
     integer color index parsing, OKLCH percentage and angle unit parsing, and OKHSL channel parsing;
  3. `okhsl-channels-and-conversion`: OKHSL color construction, gamut-relative saturation scaling,
     and round-trip extraction of hue, saturation, and lightness channels;
  4. `color-conversion-and-gamut-mapping`: ANSI 256 color cube and grayscale ramp mapping to RGB,
     direct RGB to OKLCH conversion, and bisection gamut mapping for out-of-gamut chroma;
  5. `color-mixing-and-spaces`: polar shortest-arc OKLCH interpolation with 360-degree hue wrap
     and achromatic hue preservation, alongside Cartesian linear sRGB interpolation;
  6. `oklab-to-okhsl-lightness`: Björn Ottosson's rational-cubic OKLab to OKHsl lightness mapping;
  7. `text-style-and-ansi-formatting`: terminal color mode escape formatting (`256color` SGR 38;5/48;5
     and `truecolor` SGR 38;2/48;2), TextAttributes composition, and reverse-order SGR reset closing.
- The 1050 toolkit capability ledger rows for all 49 B10 members now name these scenarios
  and their registered artifacts under `v1.3.0-observation`.
- C++ comparisons live in `tests/tui/ColorUtilsTest.cpp` tagged `[issue1218]`. Every claim carries
  an independent discriminating case satisfying the #1229 contract.

## Validation

- Focused comparison selection: `ctest --preset vcpkg -LE architecture -L issue1218`,
  16/16 passed (`deliveries/1218-issue1218.xml`).
- The 20 receipts that pin `baselines.json` were revalidated under `revalidation1218`
  after the `audit-1218` registration.
- Full Validation: `cmake --build --preset vcpkg`, `ctest --preset vcpkg -LE architecture`,
  `ctest --preset vcpkg -L architecture`.
- `python3 -m unittest tests.evidence.ConsolidationAuditTest` passed;
  `python3 scripts/evidence/receipt_boundary.py` exit 0.

## Capability status

All 49 batch rows advance to delivery-backed `verified` against the registered
audit-1218 family.

## Remaining boundary

None. All 49 rows in route batch B10 are receipted whole-row.
