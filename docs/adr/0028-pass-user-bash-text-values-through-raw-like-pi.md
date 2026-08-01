---
status: accepted
---

# Pass User Bash text values through raw like pi

The Native TUI User Bash text values follow pi baseline `864b35c`'s raw
pipeline. No secret redaction is applied to User Bash command, output, or
error-diagnostic text; the only transformations are pi's own ones.

This decision supersedes the secret-redaction consequence of ADR 0026 for
the User Bash command, output, and error-diagnostic values: where ADR 0026
stated that parity does not authorize weakening secret-redaction, this ADR
is the recorded, explicit override for those values. ADR 0026's other
guardrails (output-bounding, environment-filtering, workspace containment,
quiescent-close) remain in force.

## Aligned value pipeline (verified against pi source at the baseline)

Command (pi `interactive-mode.ts` submit path, `agent-session.ts`
`recordBashResult`):

- Parse: trim the full submission, then slice the `!`/`!!` prefix and trim
  the command (pi: `text.trim()` → `startsWith("!")` → `slice(1).trim()`).
- Busy recall: restore the original trimmed submission verbatim (pi:
  `editor.setText(text)`); no re-serialization.
- Execution: raw trimmed command; shell prefix is applied only to the
  executed script, never to the stored command.
- Display: raw `$ ${command}`.
- `BashExecutionMessage.command`: raw trimmed command.

Output (pi `bash-executor.ts` `executeBashWithOperations` + `shell-output.ts`
`sanitizeBinaryOutput`):

- Strip ANSI escape sequences (`stripAnsi`).
- Drop C0 controls except tab, LF, CR; drop U+FFF9–U+FFFB (binary-garbage
  filter); keep DEL.
- Remove carriage returns entirely (`.replace(/\r/g, "")`); `\r\n` thus
  collapses to `\n`.
- Bound by tail retention with a spill file (`truncateTail`,
  `fullOutputPath`).
- No redaction.

Error diagnostics: passed through without redaction.

## Considered options

- Keep the two existing command sanitizers
  (`safe_user_bash_invocation` in the TUI, `safe_user_bash_command`/
  `normalize_terminal_text` in the runtime) and unify their recipe:
  rejected. The two recipes already diverged (tab/DEL/CRLF handling), and
  pi performs no command sanitization at all. The terminal-injection
  exposure the sanitizers incidentally covered is accepted as
  pi-equivalent; if display protection is ever wanted again, it belongs at
  the renderer seam, not on the value.
- Keep redaction but drop sanitization only: rejected. The decision
  criterion for this work is strict pi alignment for User Bash text
  values; pi carries command and output into model context and history
  without redaction.
- Keep the accumulator's incremental redaction stage (RedactEmit):
  rejected. Pi has no output redaction; the stage was a documented
  divergence surface (`kMaxAtRiskBytes`/`kMaxKeyBytes`/`kMaxEscapeBytes`)
  that alignment removes.
- Convert lone `\r` to `\n` as the accumulator does today: rejected. Pi
  removes carriage returns from the stored value; conversion is an
  observable divergence.
- Treat ADR 0026's secret-redaction clause as absolute for User Bash text
  values: rejected by this decision. The clause remains in force for
  everything else; only the User Bash command, output, and error-diagnostic
  values are narrowed here.

## Consequences

- `safe_user_bash_command`, `safe_user_bash_invocation`, and the command/
  path uses of `normalize_terminal_text` are deleted. Progress display,
  `BashExecutionMessage.command`, the busy-recall route, and the
  full-output-path display value carry the trimmed raw text.
- Busy editor recall restores the original trimmed submission verbatim,
  replacing the re-serialized `!`/`!!` form. `UserBashSyntax` keeps its
  parse and bash-mode vocabulary; the sanitization functions go away.
- `UserBashOutputAccumulator` drops its RedactEmit stage, removes carriage
  returns instead of converting them, and adds the U+FFF9–U+FFFB drop to
  its control filter; its ANSI stripping, UTF-8 window safety, and tail
  bounding remain (all present in pi).
- `safe_user_bash_error` no longer redacts shell error diagnostics.
- Tests that pinned "control bytes never reach the editor/diagnostics" or
  redaction of User Bash text are removed or updated to raw-text
  expectations matching the pi recipes above.
- Scope boundary: agent-message redaction elsewhere (provider-message
  redaction, non-User-Bash diagnostics) is untouched; pi itself redacts at
  the AI provider-message seam, so that redaction aligns rather than
  diverges. Output-bounding, environment-filtering, workspace containment,
  and quiescent-close policies are untouched.
- The parity map's User Bash entry records this divergence narrowing when
  the work lands. Architecture reviews should not re-propose redacting or
  sanitizing User Bash text values.
