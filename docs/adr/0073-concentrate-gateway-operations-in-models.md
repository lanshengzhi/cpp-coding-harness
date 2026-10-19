---
status: accepted
---

# Concentrate gateway discovery, connection facts and inference in Models

> **ADR 0074 refinement:** [The Chat Completions adapter is the Magpie gateway contract](0074-the-chat-completions-adapter-is-the-gateway-contract.md) retires typed `compat` and provider-id specialization for `openai-completions`. The first Decision's "Provider identity, typed compatibility and payload behavior under ADR 0063 remain unchanged" records candidate 2's scope; the adapter behavior now follows ADR 0074.

Candidate 2 of the cch_ai architecture review was confirmed by the user. Models
owns the Local LLM Router connection and its catalog discovery, refresh, status
inspection, Request Authentication and inference. Models Runtime privately retains
Models and owns application settings, default/resumed model selection, Session
assembly and actionable diagnostic wording. This refines ADR 0055 and the discovery
responsibility explicitly deferred by ADR 0072; the Owner graph does not change.

## Decisions

- Bind one gateway root URL at Models construction. Derive `/api/hello`,
  `/v1/models` and `/v1/chat/completions` inside cch_ai. Remove `Model::base_url`:
  a Model describes identity and capability, not request destination. Private
  protocol execution receives its target explicitly. Provider identity, typed
  compatibility and payload behavior under ADR 0063 remain unchanged.
- Use the same resolved gateway key for discovery, status and inference. Preserve
  explicit invocation override > nonempty MAGPIE_API_KEY > `magpie`; ambient
  environment is supplied by the host. Session assembly and the interactive host
  supply the CLI key before initial discovery and model selection.
- Fix the key when the first gateway operation actually issues a request,
  including failed requests. Rebinding the same key is harmless; a different key
  is rejected without changing the existing catalog or key. An injected shared
  Runtime with no requested override keeps its identity; another key requires
  another Runtime. Cancellation before I/O does not fix the key.
- Refresh success replaces the catalog atomically and clears its discovery error.
  Discovery/validation failure clears the catalog and records a bounded, redacted
  error; cancellation preserves both catalog and error. Status inspection returns
  independent hello/catalog facts without updating the active catalog or its
  error. A successful hello does not establish inference availability.
- TUI status inspection, including without a Session, uses the AI interface.
  Models Runtime does not parse gateway JSON, construct URLs or hold a transport.
  HTTP and ScriptedTransport remain the two adapters at the existing private
  support transport seam. Transport injection and passive catalog replacement for
  tests stay private; no gateway registry or public transport interface is added.
- Preserve move-only streams, retained operation lifetimes, serialized execution,
  cancellation, terminal/sink behavior and retry rules from ADRs 0040 and 0069.
  Keys stay out of Model values, messages, Session Entries and status values. Gateway
  diagnostics reuse AI's approved bounded/redacted error operation (ADR 0072).

## Gateway evidence

Verified against the clean Magpie source checkout at commit `d783df46` in
`/home/lansy/Work/github/magpie`. `internal/gateway/lan.go` permits direct loopback
requests with no key, placeholder keys and stale keys; enabled managed keys create
an access identity even locally. Network requests require the sharing/listening
conditions and an enabled key. `gateway.go::catalogFor` applies client visibility
and then `key_models.go::keyAllowed` applies the identified key's model/account
restrictions. `/api/hello` returns name/version through the same entry middleware.
Pike sends its configured key consistently; it does not copy Magpie's access rules
or expand its local-gateway product scope.

Five existing Magpie tests passed: TestLoopbackRemainsPermissive,
TestNetworkGatewayNeedsAKey, TestKeyListShowsWhatItDid,
TestKeyListSeesChangesNextRequest and TestGatewayKeyModelsHoldAKey. These establish
that an unauthenticated discovery followed by keyed inference can disagree, and
that changing a key after discovery can invalidate the catalog's identity.

## Verification

Exercise discovery, hello and inference through Models with one scripted transport,
checking actual outbound target/authentication and model capabilities. Cover
refresh failure/recovery, pre-I/O and in-flight cancellation, partial inspection,
operation lifetime, key fixation after discovery/inspection/failure and diagnostic
redaction. A fabricated Model cannot select another destination because that field
is absent; exercise a vendor-identified Model at a configured gateway to verify the
actual target independently of provider/payload semantics. Session assembly tests
observe the key on the first discovery request and verify shared-runtime conflict
rejection without mutation. Retain the stream lifecycle suite, application policy
coverage, architecture checks and Full Validation.
