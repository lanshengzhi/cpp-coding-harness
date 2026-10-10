# MCP 与 Codemode：Pike 对照 pi v1.0.4 的 parity 审计

**基线：** pi `7c10bd4337495ee613f2224843ecdf349b80d1df`（tag `v1.0.4`）；Pike 工作树 `50f1c73ee674d0734b64163693b2824314e7036a`。

**审计范围：** Pike MCP、Codemode、会话装配和管理接口，与 pi `packages/mcp`、`packages/coding-agent` MCP 扩展及 `packages/codemode`。

**方法：** 检查实现、ADR 0066、spec #882 验收表和冻结差异证据包 `fixtures/pi-ai/v1.0.4/mcp-codemode/`；上游源代码直接取自上述 pi commit。状态描述行为差异。优先级按用户可见影响排序：P0 安全/严重正确性，P1 能力或明显 UX 差异，P2 细节差异；Architecture 表示需先决定产品/架构归属。

## 执行摘要

Pike 已覆盖 pi v1.0.4 coding-agent 集成使用的 MCP 核心功能面：stdio 与 Streamable HTTP、JSON-RPC 初始化和工具分页、HTTP GET/SSE 恢复、OAuth 发现与注册及 `mcp-auth.json`、Roots 查询、ping、进度与取消、资源工具、服务器管理和 exposure 策略。通用 prompts 客户端 API 和 roots 变更通知需按 MCP protocol client surface 单独看待。Codemode 也采用内联模型工具，带语法约束、QuickJS/WasmEdge worker 隔离、嵌套工具调用、输出限制及安装包内 guest wasm。两边内部实现不同（pi 的 QuickJS/WASI worker 对 Pike 的 WasmEdge 承载 quickjs-wasi guest），但检查到的主要可见契约一致。

发现没有 P0 安全差异。主要未对齐集中在 ADR 记录的差异：`deferred` exposure 暂不可达、Codemode 不生成动态工具目录、MCP 输出转换与截断、stdio stderr 诊断、跨进程登录拾取、启动等待策略、TUI 选择和登录超时细节；另有 roots 变更通知这一协议细项，以及 pi 的 `defaultTools` setting 未进入 Pike subset。

一个容易混淆的边界：Pike 的 Codemode sandbox 有 `searchTools`/`describeTool`/`describeNamespace`，但它不是 pi 的顶层 `tool_search` 工具，且 MCP `deferred` exposure 工具不会进入 Codemode 可调用集合。因此这些 discovery globals 并未补上 deferred exposure 的行为。反过来，MCP `codemode` exposure 下的工具可以由脚本动态发现/调用，只是 pi 会在模型工具描述里附带动态目录，而 Pike 当前描述保持静态。

## 比较矩阵

| 项目 | 状态 | 优先级 | Pike 证据 | pi 上游证据 |
|---|---|---:|---|---|
| stdio JSON-RPC、initialize、tools/list 分页与工具调用 | Aligned | P1 | `src/coding_agent/mcp/McpStdioClient.*`；分页游标重复和上限检查；ADR 0066 #869/#886 | `packages/mcp/src/transports/stdio.ts`、`client.ts`、`protocol/jsonrpc.ts` |
| Streamable HTTP POST、session ID、JSON/SSE、TLS 和拒绝降级重定向 | Aligned | P1 | `McpHttpClient.*`、`McpHttpServerConfig.cpp`；ADR 0066 #873/#884 | `packages/mcp/src/transports/streamable-http.ts` |
| HTTP GET 长连接、SSE resume/Last-Event-ID、retry 与退避 | Aligned | P1 | `McpHttpClient.cpp` GET stream/reconnect；ADR 0066 #884、`McpHttpGetStreamTest` | `packages/mcp/src/transports/streamable-http.ts` |
| HTTP 404 session 过期恢复、连接失败重试、断流合成错误 | Aligned | P1 | `McpHttpClient.cpp`；ADR 0066 #886 review finding 5 | 同上，session-expiry 和 stream consumption/reconnect |
| MCP 取消通知 `notifications/cancelled` | Aligned | P1 | `McpProtocol.hpp`、stdio/HTTP client；ADR 0066 #884 | `packages/mcp/src/client.ts` `cancelPending` |
| ping keepalive 与 server request 响应 | Aligned | P2 | `McpProtocol.hpp`、HTTP GET dispatch 测试记录 | `packages/mcp/src/client.ts` 的 ping handler/client API |
| Roots capability 与 `roots/list` | Aligned | P2 | `McpHttpClient.*`、ADR 0066 #884 close-out | `packages/mcp/src/client.ts` `roots` capability/handler |
| 进度 token、`notifications/progress`、活动请求 deadline 重置 | Aligned | P2 | `McpProtocol.hpp`、stdio/HTTP progress handler；ADR 0066 #886 finding 4 | `packages/mcp/src/client.ts` `progressRequests`、`handleProgress`、`armTimeout` |
| Prompts 方法/API | Deferred | Architecture | `McpServerConnection` 当前未提供 prompts/list/get 或 prompt 工具；ADR 0066 未把 prompts 纳入已裁决 MCP 子集 | pi v1.0.4 的 `packages/mcp/src/client.ts` 暴露 `listPrompts`/`getPrompt` 客户端 API，但 `extensions/mcp/` 未将 MCP prompts 集成为模型工具或 UI 能力 |
| Resources 与 resource templates 工具 | Aligned | P1 | `McpResourceTools.*`、manager 同步；ADR 0066 #884 | `extensions/mcp/resources.ts` |
| OAuth discovery、DCR/CIMD、PKCE、token refresh、独立存储及旧凭据迁移 | Aligned | P1 | `McpOAuthDiscovery.*`、`McpOAuthFlow.*`、`McpAuthStore.*`；ADR 0066 #884 | `packages/mcp/src/oauth/*`、`extensions/mcp/oauth.ts` |
| Roots 动态变更通知 `notifications/roots/list_changed` | Missing | P2 | Pike 暴露静态 `roots/list`；未发现 roots 内容变化时主动发送 roots/list_changed 的 watcher/通知路径；ADR 0066 只记录 Roots 响应 | `packages/mcp/src/client.ts` 的 `roots.listChanged` 是对 server 请求的能力声明；pi 集成传入固定 workspace root，也没有通用文件系统 watcher 主动推送变更 |
| `exposure: deferred` 的工具发现与激活 | Missing | P1 | `McpSessionManager.cpp` 明确检测 `tool_search` 是否存在；`CodemodeDiscovery` 仅针对已给定 callable tools，deferred MCP 工具不会进入其中；ADR 0066 tool_search Deferred 行 | `extensions/tool-search/tool.ts` 提供顶层 `tool_search`，BM25 查询并激活匹配工具；`extensions/mcp/index.ts` `ensureDiscoveryActive` |
| MCP stderr tail 故障诊断 | Partial | P2 | `McpStdioClient` 读取 stdout/stderr 管道但 production source 没有 pi 的 2 KB 错误尾部呈现；ADR 0066 #886 | `packages/mcp/src/transports/stdio.ts` 64 KB ring；`extensions/mcp/runtime.ts` `stderrTail` 保留最后 2000 字符 |
| 跨进程 OAuth 登录变化拾取 | Missing | P2 | 会话 manager 的 sign-in 动作会重连；无 turn-start credential snapshot/pickup hook，ADR 0066 #886 | `extensions/mcp/index.ts` `tokensAtSignIn`、`turn_start` `reconnectSignedIn` |
| 后台连接及首条 prompt 最多等待 10 秒 | Partial | P2 | MCP connect 在 Session Assembly 同步等待；ADR 0066 #886 记录为保持 creation-result 合约的架构取舍 | `extensions/mcp/index.ts` `DEFAULT_STARTUP_WAIT_MS = 10_000`，后台连接并在首 prompt 等待 |
| `/mcp` 面板、CLI、配置读写与生命周期管理 | Aligned | P1 | `McpSessionManager.*`、config read/write、CLI/TUI flow；ADR 0066 #884、`spec-882-acceptance.md` | `extensions/mcp/{index,config,cli,ui}.ts` |
| 多候选登录服务器的交互选择 | Partial | P2 | Pike 列出候选并提示 `/mcp login <name>`；ADR 0066 #886 | `extensions/mcp/index.ts` `pickServer` 使用 `ui.select` |
| TUI OAuth 回调超时值与分类 | Partial | P2 | `McpOAuthSignIn.cpp` 单个 300 秒 deadline 并归为取消；ADR 0066 #886 | `packages/mcp/src/oauth/callback.ts` 5 分钟 callback expiry 作为失败；CLI paste wait timeout 作为取消 |
| MCP model 输出转换、资源链接/二进制呈现、20 KB middle truncation/temp file/details | Partial | P2 | 基本文本/图片与结构化结果通过通用工具结果通道；ADR 0066 #886 列出未移植部分 | `extensions/mcp/tools.ts` `limitMcpContent`、`toModelContent`、renderer details；`core/tools/truncate.ts`、`utils/output-files.ts` |
| MCP 工具命名、输出 schema、script CallToolResult 形状 | Aligned | P1 | `McpExtensionToolSource.*`；#886 script-value 修复及差异测试 | `extensions/mcp/tools.ts`、`extensions/codemode/execute.ts` |
| 内联 `codemode` 工具定义、prompt 静态文本与 grammar 约束 | Partial | P1 | `CodemodeTool.cpp`、`CodemodeSource.*`；冻结工具定义/grammar 对比通过；描述没有动态 catalog，ADR 0066 #885/#886 | `extensions/codemode/tool.ts`、`packages/codemode/src/source.ts` |
| Codemode 动态 `Nested tools:` catalog / inlineBudget | Partial | P1 | `CodemodeToolSource.cpp` 使用固定 prompt snippet；工具可执行但模型看不到 pi 的按 schema/预算生成目录；ADR 0066 #886 | `extensions/codemode/tool.ts` `createCodemodeDescription`/`prepareCodemodeLoadout`、`declarations.ts` `selectCatalog` |
| Codemode `tools.*` 路由、发现 globals、嵌套调用及返回值 | Aligned | P1 | `CodemodeToolSource.cpp`、`CodemodeDiscovery.*`、Agent `NestedToolCalls`；ADR 0066 #885/#886 | `packages/codemode/src/runtime/*`、`extensions/codemode/execute.ts`、`core/nested-tool-calls.ts` |
| Codemode worker 隔离、timeout/cancel、Wasm guest 与输出/调用/内存边界 | Aligned | P1 | `CodemodeSandbox.*`、`CodemodePrelude.hpp`；worker 线程、interrupt、输出限额、guest 安装；ADR 0066 #885 | `packages/codemode/src/runtime/{host,worker,prelude-source}.ts`；QuickJS VM worker、memoryLimit、timeout/interrupt |
| Codemode 输出与错误结果呈现 | Aligned | P2 | `CodemodeToolSource.cpp` `to_extension_result`，text/image、完成/失败头与错误 | `packages/codemode/src/execute.ts`、`extensions/codemode/execute.ts` |
| `defaultTools` 初始工具选择 setting | Deferred | Architecture | Pike 目前以固定默认集及 `--tools`/`--exclude-tools` 解析；ADR 0066 明确 Deferred | `core/settings-manager.ts` defaultTools merge/resolve 和 initial tool selection |
| MCP Apps/UI resources | Deferred | Architecture | Pike 没有 MCP App HTML/resource UI renderer；ADR 0066 未裁决该 host capability | `packages/coding-agent/src/extensions/mcp/resources.ts` 明确 MCP App UI resources 留给支持该呈现的 host |

## 发现详述

### 1. Transport 与协议

stdio 与 HTTP 两种传输均已实现，且共用 MCP 协议/工具转换。stdio 按行读取紧凑 JSON-RPC，设置单帧 16 MiB 上限、序列化连接读写，并在服务器死亡后对下一次调用重连。HTTP 使用已有 outbound transport；强制 TLS、禁止跟随降级重定向，保留 session id，并处理 JSON 与 SSE 响应。HTTP GET server stream 支持重连、SSE event id 恢复、retry 字段、退避和流失败报告。ADR 0066 的 #886 close-out 还记录了 404 session-expiry retry、重复 cursor 防护和受控 SSE retry 解析。

协议侧，源码可见进度 token 注入/映射、进度回调重置 deadline、cancel notification、ping 以及 Roots 能力。审计未找到 Roots 变更通知的持久状态更新路径，因此报告将该细项列 Missing，避免把 Roots 静态读取误当作完整动态 roots 生命周期。Prompts API 没有出现在 Pike MCP 连接及工具表面；它不属于 #882 已验收的能力面，故归 Deferred/Architecture。

### 2. Resources、OAuth 与服务器管理

MCP resources、templates、read resource 均通过工具提供，含输出 schema 和 exposure 同步。管理层覆盖 `/mcp`、`pike mcp`、全局/受信项目配置读写、enable/disable/reconnect、OAuth 登录/登出与曝光调整。OAuth 使用 RFC 9728 发现、动态客户端注册、PKCE、请求时 token 附加和 `mcp-auth.json`；旧的 `auth.json` MCP 项有迁移路径。这些实现和 acceptance 文档把原 #865 阶段的显式缺口补齐。

小型管理 UX 差异仍在：多候选服务器时 pi 直接弹 `ui.select`，Pike 呈现候选列表及命令提示；TUI OAuth 的 callback 到期在 pi 视为登录失败，Pike 的 300 秒请求 timer 到期归为取消。正常登录/登出和显式取消路径已对齐，残差限于候选选择和超时边界/分类。

### 3. Exposure 与发现

`direct` 工具直接声明，`codemode` 工具通过 Codemode 可调用，`hidden` 工具不声明；resource 工具曝光随服务器集合计算。需要特别注意 `deferred`：pi 的 MCP 扩展会激活顶层 `tool_search`，搜索结果再声明对应工具。Pike 的 codemode 实现有同名概念的 `searchTools()` discovery global，但它只搜索已传入 codemode 的 callable 工具集合；MCP manager 在 deferred exposure 时检查的是名为 `tool_search` 的顶层 tool，且 codemode callable 列表按 exposure 过滤。因此 deferred MCP tools 没有可达路径。这是可观察的能力差异，应继续视为 P1 Missing，除非产品明确决定不支持 deferred exposure。

### 4. Codemode 执行与描述

运行时架构不同但主要语义一致：pi 用 worker thread 与 QuickJS VM；Pike 在独立 worker thread 驱动 WasmEdge 执行 pi 的 quickjs-wasi guest。两边支持执行期限/中断、嵌套工具调用、脚本输出和图像，并对运行环境能力做隔离。Pike 使用 interrupt flag 处理 wasm 自旋任务，且限制输出数量/大小和嵌套调用预算。pi 的 QuickJS memory cap 可配置；Pike guest 的内存限制应按其 WasmEdge store/guest 配置对照具体默认值理解，现有验收资料没有显示可见资源语义差异，故不将引擎差别单独判为 gap。

可见差异在 Codemode 描述：pi 每次按可调用工具、schema、namespace 和 inlineBudget 生成 `Nested tools:` catalog，Pike 使用冻结的静态空目录描述。工具仍可通过 discovery globals 查询和通过 `tools.*` 调用，但模型缺少 pi 默认给出的接口样例和类型提示；复杂工作区中这会影响调用发现和脚本正确性，优先级 P1。状态列为 Partial，因为脚本执行、发现接口和调用路由本身存在，缺失的是自动生成的模型侧目录。

### 5. 输出呈现

Codemode 输出（text/image、成功/失败标题和错误块）走通用执行结果路径。MCP 结果的核心 schema 和嵌套脚本返回值已按 pi 修复；仍缺 pi 的 model-facing 输出政策：合并内容超过 20 KiB 时 middle truncation、以权限受限临时文件保留完整输出、resource link 标注/读取提示、二进制资源落文件，以及 renderer details channel。常见短文本结果基本一致，长结果和非文本资源则会出现不同的上下文体积、可读性与后续访问方式，因此 P2 Partial。

### 6. 未记录为 MCP/Codemode gap 的项目

ADR 0066 还包含 image generation、classifier models、durable、SQLite session store、server/protocol/telemetry 等 `No decision` 项。这些是更广泛 pi subset 的待决能力，不应在本次 MCP/Codemode 审计中误标为“意外缺失”或已同意 Deferred。Codemode 的 `models.*` globals 只有 classifier/image generation models 存在时才出现；现阶段它是条件性差异，随相应能力 membership 决定。

## 建议后续步骤

1. **先决定 `deferred` exposure 的支持契约。** 若继续保留配置选项，应加入 pi 语义的顶层 `tool_search` 或等价的模型可达机制，确保命中工具仅按下一轮所需范围声明；若不支持，应在配置校验阶段拒绝该 exposure，避免保存后不可调用。
2. **补齐 Codemode 动态描述目录。** 在 prompt 构建边界读取当前 callable tools 与 schemas，按 pi 的 `codemode.mode`、`inlineBudget` 和 namespace 渲染样例；加入超预算、无 MCP、混合 exposure 和 schema 边界的差异证据。
3. **实现一致的 MCP 结果限额和非文本处理。** 采用 20 KiB model context 限额、临时完整输出引用、resource link 及二进制资源规则，并保留 renderer details；对超长输出、无临时目录权限及混合 content 验证。
4. **改善 stdio 故障诊断。** 在有界内存中保留 stderr 尾部，连接/工具失败时附带最后 2 KB，与 stdout/错误文本分离，避免无界日志累积。
5. **补会话生命周期细项。** 在 turn start 对 OAuth token snapshot 做轻量变化检查并按需重连；评估后台连接及首条 prompt 等待 10 秒能否与当前同步创建结果契约兼容。
6. **统一管理 UX 边界。** 多候选登录使用交互选择器；拆分 TUI callback expiry（失败）与用户取消（取消），并对齐 pi 的 5 分钟 callback expiry。
7. **把 settings 与 protocol 边界分别裁决。** `defaultTools` 是否进入产品 subset 属 Architecture 决策；Prompts client API 与 MCP Apps UI 应分别作 membership ruling。pi coding-agent 扩展没有把 prompts 暴露成 UI/模型工具，底层 client API 不等同于 coding-agent parity 要求。

## 核验依据与范围限制

- Pike：`src/coding_agent/mcp/`、`src/coding_agent/extensions/codemode/`、`src/coding_agent/runtime/McpSessionManager.*`、`docs/adr/0066-pi-capability-scope-decisions-and-open-questions.md`、`docs/research/spec-882-acceptance.md`。
- pi：固定 checkout `7c10bd4337495ee613f2224843ecdf349b80d1df`（tag `v1.0.4`），重点检查 `packages/mcp/src/`、`packages/coding-agent/src/extensions/mcp/`、`packages/coding-agent/src/extensions/tool-search/`、`packages/codemode/src/` 和 Codemode extension。
- 差异证据包：`fixtures/pi-ai/v1.0.4/mcp-codemode/`，含来源 provenance、MCP tool/protocol/config surface、Codemode tool 定义及 grammar。
- 已读取仓库验证入口与 spec #865/#880/#881/#882/#884/#885/#886 的记录。报告仅作只读审计，没有改代码或执行测试；验证方式是对照源码、已冻结 differential bundle、验收映射和 ADR，而不是重新运行已有测试套件。
- 请求目标文件名中的 `v1.4` 与用户说明的基线 `pi v1.0.4` 不一致；本报告按明确指定的 v1.0.4 commit/tag 审计，并保留指定文件名。
