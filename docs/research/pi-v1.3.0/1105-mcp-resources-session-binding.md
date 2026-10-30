# MCP resources and real Session bindings (#1105)

## Scope

This slice follows ADR 0075 and #1105 (B47 / MBind) at Named Baseline
`pi-v1.3.0`, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, on Linux
x86-64 glibc. It owns lazy resource access, resource-failure isolation and the
application Session's live MCP binding. #1104 owns connection management and
startup/close policy. #1118 owns catalog/declaration/exposure presentation;
#1119 owns script execution/store/output; #1177 owns the application receipt.

## Delivered behavior

The application reuses the factory's initialized transport and derives resource
availability from its advertised capabilities. Startup sends `initialize`,
`notifications/initialized` and `tools/list`; it sends no resource or template
request. Resource tools list on demand. A JSON-RPC resource-list error remains
local to that operation, while the same Session can call the server's tool twice.

The production connection consumes `tools/list_changed` on both transport
interfaces. Stdio now reads notifications while idle, waking that single reader
when a request is queued. Re-listing updates the existing Agent registry, which
holds the declaration and executable binding together. Real Session tests call a
new tool and call an existing tool with its replacement argument schema, through
both direct model calls and codemode. The ordered handoff for #1118 is the
existing `Agent::tool_definitions()` / `NestedToolCallRunner::tools()` snapshot:
`ToolRegistry::definitions()` sorts it by name. No second catalog is introduced.

Closing a client stops idle observation and rejects queued requests without
reopening the transport. A re-list completing after close, sign-out or reconnect
checks that its client is still current before publishing. The Session close test
leaves a re-list unanswered, closes and joins the runtime, and checks that the
closed connection has no late refresh error or extra handshake.

Independent observations are registered in
`fixtures/pi-v1.3.0/audit-1105/session-binding.json`; the earlier on-demand
resource and pending-initialize lifecycle observations remain in `audit-1052`
and `audit-1104` under their original identities.

The new capture uses the real upstream Agent Session. It records both a normal
resource listing and a JSON-RPC resource-list failure followed by two successful
`tools/call` requests, then a `tools/list_changed` registration and an actual call
to the late tool. A retained descriptor alone would not distinguish a broken
executable binding, so both scenarios exercise the tool through Session prompt
execution.

## Validation

- Focused Validation: `cmake --build --preset vcpkg --target cch_tests_coding_agent`,
  then `ctest --preset vcpkg -L '^issue1105$' --output-on-failure`: **3 selected,
  3 passed**, covering five generated scenario executions. Both the initial
  eager-resource regression and the missing-notification regression failed before
  their implementations and passed afterwards.
- Full Validation and existing-receipt revalidation are recorded after the clean
  implementation revision is committed, as required by the receipt contract.

## Capability status

Capability statuses and the implementation frontier remain unchanged. The
consolidated ledger is regenerated only to refresh existing receipt inputs and
classify the supplemental observations. This record supplies slice evidence,
while #1177 owns the application receipt follow-up. It does not promote the whole
`mcp-resources-list-read`, `mcp-application-resource-init` or
`mcp-reconnect-tools-changed` rows. Target membership, implementation and
verification remain separate claims.

## Remaining boundary

The standards and spec reviews report no remaining findings after correcting
executor initiation, the move-only HTTP callback override and the pending
operations' ownership. Catalog presentation, exposure filtering and script
execution semantics remain in their named follow-ups; whole-row reconnect/drop
verification remains unclaimed. Resource counts are no longer eagerly populated.
Magpie-only model access, the XDG state root, strict no-exception behavior and the
ordinary application Session remain in place. #1044 remains open.
