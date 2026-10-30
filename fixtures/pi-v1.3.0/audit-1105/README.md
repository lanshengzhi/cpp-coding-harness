# MCP Session binding observations (#1105)

The `audit-1105` bundle drives the real frozen pi `AgentSession` with its native MCP
extension and a local in-memory JSON-RPC server. It supplements the registered
`audit-1052/application-initialization.json` observations: that earlier resource-failure
case checked the retained tool list but never called the tool afterwards.

Two scenarios record startup requests, a successful or failed resource listing,
two subsequent `tools/call` results, and a late `tools/list_changed` registration
followed by a real call. A third records the same replacement-schema inputs as
the C++ binding test: the initial tool requires `text`, the changed tool requires
`message`, extra properties are rejected, and both the late tool and updated echo
execute through the real Session. Request traces omit the `notifications/initialized`
notification; the C++ wire trace retains it. Non-MCP tools are omitted from the
callable snapshot. The upstream callable snapshot is alphabetically ordered even
when the server lists `late` before `echo`.

Capture from the repository root:

```sh
NODE_OPTIONS=--conditions=source \
PI_CHECKOUT=/tmp/cch-1105-pi-frozen PI_BASELINE=pi-v1.3.0 \
TSX_TSCONFIG_PATH=/tmp/cch-1105-pi-frozen/tsconfig.json \
/tmp/cch-1105-pi-frozen/node_modules/.bin/tsx \
fixtures/pi-v1.3.0/audit-1105/capture-1105.mts
```

The capture refuses a dirty tracked checkout, another revision, another baseline,
or a platform other than Linux x86-64 glibc. It uses the frozen checkout's own
Session test harness and faux model provider; no live model request is made.
For this capture, the ignored generated provider-data directory was copied with
the existing local frozen installation. Its missing `azure.json` was supplied as
an inert `{}` catalog so the registry could import; neither scenario selects Azure
or reads its models. All measured MCP and Session source remains unmodified at
`71023aec1546d6d677a65669c5665c0927ce0dd0`.

`manifest.json` records capture-script, artifact and environment digests. The
manifest is registered in `../baselines.json`. Capturing again changes a recorded
observation and requires explicit registry reconciliation; it does not verify the
C++ implementation by itself.
