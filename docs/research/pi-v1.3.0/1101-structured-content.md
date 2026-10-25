# #1101 MCP structured content: delivery reconciliation

## Scope

Reconcile the implementation already merged into main in `9df4f73ee` and
`06ccac84b` with the frozen `pi-v1.3.0` baseline
`71023aec1546d6d677a65669c5665c0927ce0dd0`. The independent MCP client and
application tool conversion preserve arbitrary JSON `structuredContent`.
This record covers #1101 only; OAuth, close, issuer and transport behavior remain
owned by #1102 and #1103.

## Delivered behavior

The public stdio and HTTP clients preserve structured objects, arrays, strings,
numbers, booleans and explicit null. Application conversion distinguishes absent
content from malformed content, absent structured content from null, and an empty
content list from non-empty model content. Its script details preserve the
structured value while removing `_meta`; tool errors remain distinct from
protocol errors. Model text preserves wire object order and JavaScript number
formatting.

Existing cases in `tests/mcp/McpOwnerTest.cpp`,
`tests/coding_agent/McpHttpSessionTest.cpp` and
`tests/coding_agent/McpStructuredContentTest.cpp` exercise actual local MCP
servers. The reconciliation adds one whole-obligation replay of all five frozen
`audit-1052/structured-content.json` observations through the independent public
client and the real application tool wrapper. The fixture accepts a raw result;
the client and application must perform the validation and conversion themselves.

The original audit's four row obligations map to that replay by their exact
scenario names. The array/primitive/null observations compare the public client
result; the error observation compares both client and application conversion.
The fifth observation compares the complete application conversion result.
The error case compares the client metadata and the full converted content,
error flag and script details. The only representation adjustments are:

- Pi defaults omitted client `content` to `[]`; C++ retains the wire omission in
  `ToolCallResult.value`, so the client observation supplies that default.
- The captured client projection names `_meta` as `meta`; the observation uses
  that spelling without changing the metadata value.
- Pi records absent `isError` as null; C++'s passive application flag defaults to
  false. The converted observation reports null only when the input omitted the
  field. The script details continue to distinguish omission from explicit data.

The passing missing/null, invalid-result and reverse-order cases discriminate
against an object-only schema, omission/null collapse, invalid-content defaulting
and sorted-map output.

## Validation

After incrementally building `cch_tests_coding_agent` and the complete Debug
preset, `ctest --preset vcpkg -L issue1101 --output-junit <absolute repository
path>/docs/research/pi-v1.3.0/1101-validation.xml` selected and passed all 14 cases
on 2026-10-25. The exact executed command and UTC capture time are in the receipt;
the actual [CTest JUnit result](1101-validation.xml) is pinned by SHA-256.
The complete original-audit replay passed 94 assertions. The passing missing/null
case supplies the receipt's discriminating check; additional cases cover malformed
results, all JSON types, HTTP, wire ordering and JavaScript number rendering.

The receipt pins 51 reviewed inputs: transport/result contracts and implementations,
wire-order formatting, application conversion and live adapters, shared JSON values
and parsing, build/dependency declarations, public comparison sources/helpers,
local servers, frozen expected artifacts and the evidence validator. Semantic
review confirms that the four exact row obligations map to the comparisons above;
the application model-text observation supplies additional coverage. The selected
row has no remaining comparison obligation. Neighboring MCP obligations remain
outside this claim.

Consolidation `build`/`check`, all 39 receipt/consolidation boundary tests and the
shared frozen-evidence verifier pass. Full-suite and architecture results, including
the first run's performance failure, are recorded in
[the reconciliation report](1058-delivery-reconciliation.md).

## Capability status

The source audit ledger remains unchanged. `1058-deliveries.json` binds the
whole-row comparison to its implementation commits, selected implementation,
test and fixture digests, and the actual focused CTest JUnit result. Consolidation
promotes only `cch_mcp:mcp-tools-call-structured` from `partial` /
`upstream-captured` to `implemented` / `verified` through that validated delivery;
`sourceRows` and `auditStatus` retain the audit provenance.

## Remaining boundary

OAuth cancellation, close during refresh and HTTP/HTTPS/redirect issuer/auth
semantics remain #1102/#1103 obligations. Application management and on-demand
resources remain #1104/#1105. This result does not verify the entire MCP target,
advance cch_ai, or close #1044. Selection of the pinned inputs and the semantic
mapping from scenarios to C++ cases require review; a JUnit pass alone does not
establish that mapping.
