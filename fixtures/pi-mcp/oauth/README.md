# fixtures/pi-mcp/oauth — MCP OAuth replay evidence (spec #865, ticket #875)

Committed replay of the loopback OAuth exchange one MCP server signs in
through. It is a Pike-side recording against a local loopback authorization
server, not a pi transcript: pi's MCP OAuth implementation is TypeScript and
stores its credentials in `mcp-auth.json`, while Pike reuses the shared
`auth.json` through AuthStorage (see the `#875` ruling in ADR 0066). What is
pinned is the **wire contract** and the **credential mapping**, held where it is
observable.

## `replay.json`

One recorded exchange, replayed by `tests/coding_agent/McpOAuthSessionTest.cpp`
through the scripted `ai::auth::OAuthHttpClient` seam (the loopback callback
itself is real):

| Section | What it pins |
| --- | --- |
| `authorize_request` | the authorization-code request shape: `response_type=code`, the client id, PKCE `S256`, the loopback `redirect_uri`, `state`, and `scope`. |
| `callback` | the loopback redirect carries `code` and `state`. |
| `token_exchange` | the form-encoded authorization-code exchange and the `ai::OAuthCredential` it maps to (`access`, `refresh`, `expires`, `client_id`). |
| `refresh_exchange` | the `refresh_token` grant and the rotated credential. |
| `invalid_grant_exchange` | a rejected refresh (`invalid_grant`) that must surface an explicit re-login error, never a retry loop and never an unauthenticated request. |

## Redaction / provenance

- No live credentials and no network: the only socket is the loopback callback
  the test drives.
- Per-run secrets (authorization code, PKCE verifier, OAuth `state`) are recorded
  as the repository redaction placeholder `[REDACTED]`; replayed token values are
  `dummy-*` strings, matching the `fixtures/pi-ai/auth-storage` convention.
- The test asserts the product's explicit errors never carry a token value.
