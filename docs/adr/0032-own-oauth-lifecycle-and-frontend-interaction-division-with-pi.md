---
status: accepted
---

# Own the OAuth lifecycle and frontend interaction division with pi

OAuth login, refresh, cancellation, expiry, persistence, logout, and user interaction are divided exactly where pi divides them at the parity baseline, as a capability subset by omission only. `cch_ai` owns the interaction content and auth orchestration, the CLI/Native TUI owns presentation and cancellation, `SessionFactory` owns none of it, and SDK/RPC hosts carry no built-in login surface. This implements parity-map ticket [#328](https://github.com/lanshengzhi/cpp-coding-harness/issues/328), refining the seams frozen by [#326](https://github.com/lanshengzhi/cpp-coding-harness/issues/326) and the secret boundary frozen by [#327](https://github.com/lanshengzhi/cpp-coding-harness/issues/327). C++ remains a pi capability subset by omission only; no backward compatibility, migration shims, aliases, or fallback reads.

## Considered options

- Reproduce the `lazyOAuth` AbortSignal drop from `auth/helpers.ts` as a wrapper layer in C++: rejected because C++ has no dynamic-import/bundle layer for OAuth flows; the observable contract is that the request-path refresh is uncancellable, which pi's `resolveStoredOAuth` already produces without the lazy wrapper (`auth/resolve.ts` never passes a signal to `oauth.refresh`).
- Fix refresh cancellation as an intentional divergence (cancel an in-flight request-path refresh): rejected because pi's observable behavior is that request-path refresh runs to completion or failure independent of request cancellation; a cancellable C++ refresh would substitute observable semantics without a scoped consumer.
- Render login interaction inside `cch_ai` (owned dialog/selector components): rejected because pi-ai only emits `AuthEvent`s and receives `AuthPrompt` answers as strings; every pi host renders its own surface, and any product UI inside the ai layer would leak interaction policy into provider implementations.
- Give `SessionFactory` a login/refresh trigger or an auth snapshot: rejected by the frozen "auth resolves live before each request; login is explicit; no OAuth snapshot at Session construction" rule (#326); session creation and ordinary requests never start login.
- Add login/logout/credential commands to the CLI or RPC command surface: rejected because pi's CLI has only `--api-key` (runtime override, #327) and pi's RPC mode exposes zero auth surface (`rpc-types.ts`/`rpc-mode.ts`); hosts establish auth through `ModelRuntime.login` with their own interaction.
- Distinguish login cancellation from failure by string-matching the message: rejected in favor of a stable `util::Error` kind (message stays "Login cancelled"), following the recorded enrichment precedent of the six-category terminal payload (#326).
- Open the browser through a shell (`cmd /c start` etc.): rejected because pi's `open-browser.ts` documents that a shell re-parses metacharacters and creates an injection surface; argv spawn only.
- Port the Anthropic subscription-auth warnings and easter eggs from the interactive mode: rejected as out of the three-path scope.

## Consequences

### Frontend ownership

- `cch_ai` owns the interaction *content* and auth orchestration: the `AuthInteraction`/`AuthPrompt`/`AuthEvent` contract carried by `std::move_only_function` (guardrail 3), the `openai-codex` and `kimi-coding` OAuth implementations (prompt texts, instructions, HTML pages, error messages), the shared device-polling helper, and `Models` login/logout/checkAuth/getAuth orchestration. It never renders.
- `SessionFactory` owns none of it: no auth snapshot, no login/refresh trigger, no interaction surface; it only injects the `ModelRuntime`, and every request resolves auth live through `Models.stream → applyAuth → getAuth`.
- The CLI/Native TUI owns all presentation: the LoginDialog equivalent (event rendering, prompt input, Esc/Ctrl+C cancellation through a dialog-owned `std::stop_source`), the OAuthSelector equivalent (login/logout modes, provider list with status), `/login` `/logout` commands, post-login default-model auto-selection, and browser opening via argv spawn (`xdg-open`/`open`/`rundll32`) that never passes through a shell. The non-interactive CLI has no login surface; only the `--api-key` runtime override exists.
- SDK hosts bring their own `AuthInteraction` or use `set_runtime_api_key`; no built-in UI obligation. RPC exposes zero auth surface; model listing uses `getAvailable()` only.

### Login, persistence, refresh, and the Kimi defect

- Login is explicit only: `Models.login(providerId, type, interaction)` runs the provider flow and persists via `CredentialStore::modify` — the only write path. Login-flow failures propagate unwrapped; only `CredentialStore` failures wrap as `auth`.
- Request-time `getAuth` is the only in-scope refresh trigger: expiry ≤ 5 minutes refreshes under the store lock with double-checking, persisting the rotated credential before release; failure reports `oauth` with the stored credential preserved for retry; no silent fallback. `checkAuth` never refreshes; there are no background timers, session-level invalidation, or status-UI expiry display.
- **Kimi refresh-signal defect (aligned, no divergence):** the C++ request-path refresh is not cancellable and takes no cancellation token, identical to pi's observable behavior. The C++ port has no lazy dynamic-import layer, so the `lazyOAuth` signal drop is not reproduced as a layer; the Kimi refresh implementation keeps the signal parameter but the in-scope call path does not pass one. The network model-refresh path that would consume it is out of scope for the static catalogs; a future scoped path follows pi's per-implementation semantics.

### Cancellation, flows, and expiry

- `AuthInteraction` carries a `std::stop_token` (host owns the `std::stop_source`); `AuthPrompt` optionally carries a per-prompt token for the Codex manual-code race. Login HTTP requests and the device-poll loop observe the token (abortable sleep, deadline checks). Cancellation normalizes to a stable `util::Error` kind with message "Login cancelled"; the TUI suppresses failure UI on that kind. Login errors flow through `std::expected<Credential, util::Error>` and stay outside the six-category Models channel.
- Codex browser login: PKCE S256, 16-byte hex state, local callback server on `127.0.0.1:1455` (`PI_OAUTH_CALLBACK_HOST`), `/auth/callback` validation and HTML pages in pi's exact order, callback-vs-manual-code race (prompt win closes the acceptor, callback win cancels the prompt), listen errors degrade to manual input, JWT parsed unverified with mandatory `accountId` extraction at login and refresh, authorization URL keeping `originator=pi` byte-identical. Kimi login: RFC 8628 device flow with `verification_uri_complete`, `waitBeforeFirstPoll`, 30s request timeout composed with the cancellation token, refresh with 1/2/4s backoff across at most four attempts and immediate failure on 401/403/`invalid_grant`, `toAuth` producing `Authorization: Bearer`. Shared poll semantics (pending / slow_down / failed / complete, deadline, abortable sleep, the slow_down clock-drift message) are preserved verbatim.
- Expiry surfaces only at request time: refresh failure → `oauth` terminal error event. Dead credentials (401/403/`invalid_grant`) stay in `auth.json` and every request fails with re-auth guidance owned by the C++ session layer (pi: "Run '/login <provider>' to re-authenticate.").
- Logout is `CredentialStore::delete` — local removal, no server-side revocation — followed by `recomposeProvider` then `refresh` in `ModelRuntime::logout` (order preserved). The logout list is `CredentialStore::list()` metadata; environment/config-based auth is not logged out.

### Delegation and post-login behavior

- `ModelRuntime` is the only public seam; `ai::Models` stays privately held. `ModelRuntime::login` = `Models::login` + `refresh()`; `ModelRuntime::logout` = `Models::logout` + `recomposeProvider` + `refresh()`; post-login/logout `refresh` failures are recorded in the composition-errors map and never fail the login/logout call.
- Post-login, the TUI refreshes availability and auto-selects the provider's default model only when the current model is the unknown default; otherwise it keeps the current model. The four selection-error messages are preserved verbatim; the status message includes "Credentials saved to <authPath>". The default-model table lives in `coding_agent` core (pi: `core/model-resolver.ts`) and its values follow the catalog freeze. Anthropic subscription warnings and easter eggs are not ported.

## References

- Parity-map ticket [#328](https://github.com/lanshengzhi/cpp-coding-harness/issues/328) and its [resolution record](https://gist.github.com/lanshengzhi/4b757195c5e7e5db2ad6d25ab091fe46).
- [#326](https://github.com/lanshengzhi/cpp-coding-harness/issues/326) resolution: auth seam, `CredentialStore::modify`-only persistence, six-category channel, terminal-error stream semantics.
- [#327](https://github.com/lanshengzhi/cpp-coding-harness/issues/327) resolution: OAuth secret boundary, CLI flag surface, settings contract.
- Frozen pi baseline `83114817c68f5413e4d7ba6d7003ddc511cd31d2`: `packages/ai/src/auth/{types,resolve,helpers}.ts`, `packages/ai/src/auth/oauth/{openai-codex,kimi-coding,device-code,pkce}.ts`, `packages/ai/src/models.ts`, `packages/coding-agent/src/core/{model-runtime,auth-storage,model-resolver}.ts`, `packages/coding-agent/src/modes/interactive/{interactive-mode.ts,components/login-dialog.ts,components/oauth-selector.ts}`, `packages/coding-agent/src/modes/rpc/*`, `packages/coding-agent/src/{main.ts,cli/args.ts,core/agent-session.ts}`.
- ADRs [0029](0029-align-models-provider-and-authentication-ownership-with-pi.md), [0030](0030-share-pi-agent-config-directory-and-credential-store.md), [0031](0031-align-settings-shared-file-cli-and-resume-configuration-with-pi.md).
