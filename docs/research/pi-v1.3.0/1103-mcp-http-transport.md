# MCP HTTP transport and issuer validation (#1103)

ADR 0075 amends ADR 0054 for MCP: HTTP and HTTPS endpoints are accepted, and redirects follow the frozen pi-v1.3.0 fetch behavior. HTTPS certificate validation remains enabled. The shared transport and Magpie contract remain unchanged.

The former ADR 0054 registration test rejected plain HTTP; its replacement rejects a non-HTTP(S) URL, and a separate loopback test connects over plain HTTP. Trusted and untrusted HTTPS are exercised separately. The former redirect test rejected redirects; its replacement follows a same-origin 307 from `/a/b` to `next`, verifies `/a/next`, and verifies that authorization survives. During review, the initial replacement used `/mcp`, which could not distinguish relative URL resolution errors. The corrected cases cover `next`, `/abs`, `../up`, `?q=1` and a protocol-relative cross-origin hop that strips authorization.

The registered [audit-1052 transport scenario](../../../fixtures/pi-v1.3.0/audit-1052/transports.json), “cross-origin redirect is followed without Authorization and is rejected,” records a 307, an absent target Authorization header and a 401 producing `McpAuthRequiredError`. The loopback test checks the absent header and that the 401 produces Pike's re-login error.

Frozen undici `lib/web/fetch/index.js` `httpRedirectFetch` allows 20 redirects and emits `redirect count exceeded` on the next redirect. Deterministic redirect failures use the Process error channel so connection retries do not restart a failed chain. The loop test asserts exactly 21 requests: the original request and 20 redirects. The existing connection retry policy would produce 63 requests across three attempts if it treated the failure as transient. An illegal Location scheme has a separate failure test.

Issuer discovery rejects metadata naming a different issuer. The flow test checks the fake HTTP client's POST log is empty, proving no token exchange was attempted. The existing authorization-response issuer test also checks rejection before token exchange.

Focused `issue1103` validation selected 10 tests: 10 passed, 0 failed, 0 skipped. After the redirect correction, the full build and unfiltered CTest run selected 3,852 tests: 3,851 passed, 0 failed, 1 skipped. The skipped test was the Native TUI differential capture. The branch formatting check and consolidated audit check pass.

The structured-content receipt was revalidated on clean commit `d6949f261a43ebfc5eb03700465cb005a2dfdad0`: 14 selected, 14 passed, 0 failed, 0 skipped. All 51 pinned input digests match that commit; commit `8dd823e` records the revalidation and receipt bindings.
