---
status: accepted
---

# Silence vendored third-party diagnostics at the inclusion site

Vendored third-party headers under `third_party/` sit outside the project warning contract (CODING_STANDARDS.md §14 governs owned code). Diagnostics they trigger are silenced at the inclusion site with the canonical scoped `#pragma GCC diagnostic` pattern, not by teaching the zero-compiler-warning gate to skip vendored sources. This keeps the gate (issue #492) total over everything the project compiles: any future vendored header or inclusion site that forgets the silencing pattern fails closed instead of silently dropping out of the warning contract through a path-based exemption.

This decision records issue [#523](https://github.com/lanshengzhi/cpp-coding-harness/issues/523), where the ASan+UBSan lane's `cch_warning_gate` recompilation surfaced `-Werror=unused-but-set-variable` from `stb_image_resize2.h`. The sanitizer configuration changes which diagnostics a vendored header emits, so silencing must live at the inclusion site rather than in per-configuration flag lists.

## Considered options

- Teach the warning gate an explicit `third_party/` skip: rejected because it is broader policy that weakens the gate's totality and hides future vendored regressions behind a path allowlist.
- Mark vendored headers as system includes (`-isystem`): rejected for this ticket because it changes include semantics and evidence scanning for every consumer, while the scoped pragma already covers the single implementation inclusion site.
