# Theme compatibility fixtures

`dark.json` and `light.json` are independent local compatibility fixtures transcribed from pi commit
`83114817c68f5413e4d7ba6d7003ddc511cd31d2` (re-pinned from `864b35c` by the pi-coding-agent phase audit, ADR 0036; the only
baseline delta in pi's builtin themes is the added optional `scrollbarThumb` token, which the fixtures
omit and remain valid without, per the optional-with-fallback rule). Tests load only these repository
files and never read pi configuration or source directories at runtime.
