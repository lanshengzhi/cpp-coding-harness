---
status: accepted
---

# Separate User Bash from model Bash authorization

The Native TUI will promote pi-baseline User Bash as a direct-user capability independently of the model-requested Bash Tool. Interactive `!` and `!!` execution will not require `--enable-bash`; that flag will continue to authorize only registration of the Bash Tool for model calls. This distinction is hard to reverse once the input syntax and Session history become supported behavior, and it preserves the different trust boundaries between an explicit user command and a model-selected tool call.

## Considered options

- Keep User Bash deferred: rejected because its message, persistence, provider-context, Shell, and transcript foundations already exist, making it the narrowest high-value Native TUI parity gap.
- Gate User Bash with `--enable-bash`: rejected because it conflates direct user intent with model authorization and differs from pi baseline `864b35c`.
- Publish User Bash through SDK or RPC at the same time: rejected because the approved capability is a Native TUI operation; widening public or wire contracts is a separate parity decision.

## Consequences

- Only direct, focused Native TUI editor submissions interpret `!` and `!!`; positional initial input, one-shot, JSON, RPC, SDK, Skills, and Prompt Template expansion retain ordinary Agent Prompt semantics.
- Session assembly provides a private, Session-owned User Shell capability so User Bash may overlap an Agent Turn without widening the shared Execution Environment's public concurrency contract. The runtime owns execution, cancellation, Live Session State commitment, and persistence; the TUI owns only input and presentation.
- User Bash and an enabled Bash Tool share the effective user-level `shellPath` and `shellCommandPrefix` configuration while retaining independent authorization.
- User Bash preserves the baseline context distinction: `!` results may enter later model context, while `!!` results remain in Agent Session history but are excluded from model conversion.
- The repository's mandatory secret-redaction, environment-filtering, output-bounding, containment, and quiescent-close policies remain intentional security divergences where pi is weaker; parity does not authorize weakening those guardrails.
- User Bash remained a Deferred Capability until its end-to-end implementation, tests, documentation, and parity-map status landed together. That promotion is complete: production Native TUI Session assembly always provides the independent Session-owned User Shell on supported Linux/macOS, and the parity map records Native TUI User Bash as a Supported Capability at the unchanged baseline.
