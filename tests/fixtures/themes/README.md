# Theme compatibility fixtures

`dark.json` and `light.json` are byte-identical transcriptions of pi's builtin themes at the frozen
baseline `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (re-pinned from `864b35c` by the pi-coding-agent phase
audit, ADR 0036; the only baseline delta is the added optional `scrollbarThumb: "selectedBg"` color
entry). Tests load only these repository files and never read pi configuration or source directories
at runtime.

`goldens/` pins pi's theme validation diagnostics verbatim (byte-for-byte, no trailing newline,
derived from the TypeBox `theme-schema.json` runtime validation in `theme.ts` at the same baseline):

- `missing-colors.txt` — the `Invalid theme "<label>":` message for missing required color
  tokens, including the sorted token list and the built-in reference hint.
- `export-not-object.txt` — the `export` section failing the object type check.
- `export-bad-value.txt` — an `export` color value failing the `ColorValueSchema` union.
- `circular-variable-reference.txt` — the recursive `vars` resolution cycle error.
- `name-slash-rejection.txt` — the `/` reserved-name rejection.

Regenerate deterministically with `CCH_CAPTURE_GOLDENS=1 ./build/cpp_harness_tests "[coding_agent][theme]"`.
The fixture dark/light JSONs regenerate from `pi:packages/coding-agent/src/modes/interactive/theme/`.
