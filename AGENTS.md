# AGENTS.md

<!-- markdownlint-disable MD013 -->

本文件是本仓库的 Agent 入口路由文档。作用范围为仓库根目录及其所有子目录。

## 0. 先读顺序

1. `README.md`：项目定位、架构边界、CLI 行为、工具和安全说明。
2. `CMakeLists.txt`：构建目标、源文件清单、测试目标。
3. `docs/plans/`：当前/历史重构计划；涉及架构调整时优先读其中 `status: active` 的最新计划。
4. 涉及与 pi 对齐、模块迁移、接口契约参考时，读取参考仓库（通常是同级 `../pi`）中的对应 package/docs，再按任务进入下面的路由表。

开始改动前先执行：

```bash
git status --short
```

不要覆盖用户已有修改；只编辑与当前任务相关的最小文件集。

## 1. 项目定位

`cpp-coding-harness` 是一个实验性 C++23 coding-agent harness。核心流程：CLI/REPL 接收 prompt，经 OpenAI Chat Completions 兼容客户端请求模型，执行模型发起的本地工具调用，再把工具结果追加回对话，直到停止或达到最大 turn，并将脱敏 typed transcript 写入 JSONL session。

本项目不是生产沙箱，也不承诺兼容稳定 SDK。设计优先级是学习、可替换边界和反脆弱架构。

长期方向：本仓库应逐步成为 pi 的 C++ 实现，优先对齐 pi 的模块划分与接口契约，而不是机械翻译 TypeScript 实现。参考 package 时使用 `pi:` 前缀表明路径来自参考仓库，例如 `pi:packages/ai/src/types.ts`；本仓库实现路径仍保持 repo-relative。

## 2. 必守架构规则

1. **数据是被动值状态**：公共 contract 使用 aggregate-friendly `struct`、`std::variant`、`std::expected` 和项目内 `util::JsonValue`。
2. **能力跨物理边界**：Chat client、stream transport、execution env、session store、tool 都通过接口或隐藏实现细节的 concrete class 暴露。
3. **事件是弱连接**：Agent/provider 事件 sink 使用 `std::move_only_function`，不要回退到要求 copyable 的 `std::function`。
4. **泛型机制局部化**：Glaze DTO、schema 转换、visitor、解析 helper 等应留在 serialization/implementation 层，不要泄漏到 domain-facing API。

禁止重新引入：旧同步工具面、`util::Result`、Boost.JSON domain contract、把 `src` 当作 public include surface、兼容性空 flag。

## 2.5 pi C++ 化路线

长期方向是逐步将本仓库建设为 pi 的 C++ 实现，优先对齐 pi 的模块划分与接口契约。迁移顺序和模块映射详见 `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`。当前活跃实现计划见 `docs/plans/` 中 `status: active` 的最新文件。

## 3. 目录与职责路由

| 任务类型 | 优先入口 | 说明 |
| --- | --- | --- |
| Agent loop / turn 流程 / tool-call 编排 | `include/cch/agent/`, `src/agent/AgentLoop.cpp`, `tests/agent/` | `AsyncAgentLoop` 负责 provider-neutral loop；事件通过 `AgentLifecycleEvent` 发出；`beforeToolCall`/`afterToolCall`、context transform、LLM conversion、steering/follow-up、prepare-next-turn 和 tool execution mode 通过 `AsyncAgentOptions` 配置。 |
| AI 消息、内容、tool schema、usage contract | `include/cch/ai/`, `tests/ai/` | 公共消息/内容类型必须保持 passive value contract。 |
| OpenAI-compatible provider / SSE / HTTP transport | `include/cch/ai/providers/`, `src/ai/providers/`, `src/ai/glaze/`, `tests/ai/providers/` | provider wire DTO 和 Glaze mapping 放实现/serialization 层，不要放回公共 domain contract。 |
| Provider/model registry | `include/cch/ai/ProviderRegistry.hpp`, `src/ai/ProviderRegistry.cpp`, `src/ai/providers/FakeChatClient.*`, `include/cch/ai/providers/OpenAICompletionsCompat.hpp` | 注册 fake/OpenAI-compatible provider；CLI/runtime 只解析 provider/model，不硬编码具体 client 构造；compat flags 控制 provider adapter 行为。 |
| Config / 模型解析 | `include/cch/coding_agent/Config.hpp`, `src/coding_agent/ConfigLoader.cpp`, `tests/coding_agent/ConfigLoaderTest.cpp` | `~/.cpp-harness/config.json` 加载 provider/model/base-url/api-key-env 默认值；env var 链解析；CLI 覆盖优先级；session resume 保留存储的 provider/model。 |
| JSON 序列化/反序列化 | `include/cch/ai/glaze/`, `src/ai/glaze/`, `include/cch/util/Json*.hpp`, `tests/ai/GlazeRoundTripTest.cpp` | Domain 用 `JsonValue`，Glaze 只在边界做 DTO 转换。 |
| 内置工具 read/write/edit/bash | `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/` | 工具通过 `AsyncAgentTool` 暴露；文件/进程能力走 execution env。edit 使用 `edits[]` 数组多编辑语义；read 在截断时追加续读提示；write 隐式创建父目录。 |
| Slash 命令 / Prompt 处理 | `include/cch/coding_agent/PromptProcessing.hpp`, `src/coding_agent/PromptProcessing.cpp`, `src/coding_agent/PromptExpander.cpp`, `tests/coding_agent/BuiltinCommandsTest.cpp`, `tests/coding_agent/PromptExpanderTest.cpp`, `tests/coding_agent/SkillIntegrationTest.cpp` | `CommandRegistry` 管理 session-lifecycle 内建命令（`/help`, `/clear`, `/compact`, `/exit`）；`expand_prompt_template()` 支持 bash-style 参数替换；`process_prompt()` 缝合 REPL/runner/RPC，并在命令分发前展开 `/skill:<name>`。 |
| Skills / Resource loading | `include/cch/coding_agent/Skill*.hpp`, `src/coding_agent/Skill*.cpp`, `src/coding_agent/runtime/RuntimeServices.*`, `src/coding_agent/runtime/AgentSessionRunner.*`, `tests/coding_agent/Skill*Test.cpp` | 项目本地 `.cpp-harness/skills/**/SKILL.md` 发现、frontmatter 解析、diagnostics、`<available_skills>` 注入和 `/skill:<name>` 显式调用；global/config-driven skill dirs、package install、project trust 仍是后续 T6。 |
| 工作区、路径防护、shell 执行 | `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/LocalExecutionEnv.hpp`, `src/harness/`, `src/tools/PathGuard.hpp`, `src/util/Process.*`, `tests/harness/` | 路径 containment 和安全检查；async shell I/O 走 `ProcessRunner`。 |
| Session JSONL / resume | `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/` | JSONL 脱敏 typed transcript；v2 message resume 稳定；v3 tree metadata 已有写入支持，tree context reconstruction / branch navigation 仍 deferred。 |
| CLI / REPL / runtime wiring | `src/main.cpp`, `src/AsyncCliRuntime.*`, `src/coding_agent/runtime/`, `tests/cli/` | CLI 输出稳定 semantic event line；参数解析使用 CLI11；runtime 负责组装服务，不要恢复 legacy `--async` 等兼容 flag。 |
| JSON / RPC 输出模式 | `src/coding_agent/runtime/JsonEventPrinter.*`, `src/coding_agent/runtime/RpcMode.*`, `src/coding_agent/runtime/RpcJsonl.*`, `tests/coding_agent/runtime/JsonEventPrinterTest.cpp`, `tests/cli/CliSmokeTest.cpp` | `--mode json` 输出 JSONL stdout（session header + pi-named lifecycle events + `runtime_terminal`）；`--mode rpc` 通过 stdin/stdout JSONL 命令（`prompt`, `get_state`, `get_last_assistant_text`, `shutdown`）驱动 session。 |
| 公共边界/架构守卫 | `tests/architecture/`, `CMakeLists.txt`, `include/cch/` | 改公共 header、依赖或 include surface 时必须跑 architecture tests。 |
| 文档与计划 | `README.md`, `docs/plans/` | 重大行为或边界变化需同步 README/计划。 |

## 4. 构建与验证

默认构建、测试切片、fake/real provider 运行示例详见 `README.md`。

需要 `OPENAI_API_KEY` 的 real provider 验证除非任务明确要求，不要依赖真实网络/API 做默认验证。

## 5. 改动守则

以下守则是对 §2 架构规则在改动实践中的补充，不重复架构规则已覆盖的禁入项。

- 新增工具时：先定义 `ai::Tool` schema，再实现 `AsyncAgentTool`，通过 `AsyncToolRegistry` 注册，并补 `tests/tools/`。
- 新增 provider 或 transport 时：保持 `StreamingChatClient` / `StreamTransport` seam，wire DTO 留在 provider/serialization 层。
- 新增 session 字段时：保持 append-only JSONL 和对未知 entry 的容忍。
- 涉及 shell、文件、环境变量、session 的改动必须考虑 workspace containment、secret redaction 和输出截断。
- 测试应保护当前架构意图与安全性质，不要只保护旧类名、旧 JSONL 形状或旧 transcript 文案。

## 5.5 分支、合入与 PR 约定

本仓库按单人维护/实验仓库处理，默认不强制 PR。

- 默认用 feature branch 开发，避免 `main` 留下半成品。
- 未经用户明确要求，不要自动 commit、merge、push 或删除分支。
- 用户要求合入/发布且验证通过时，优先使用 `git switch main && git merge --ff-only <branch> && git push origin main`，再删除已被 `main` 包含的 feature branch。
- PR 仅在用户明确要求、需要 CI/review 记录，或变更风险较高时建议使用。
- 合入前若 `origin/main` 更新或分叉，先 fetch 并检查关系，不要强推。

## 6. 提交前自检清单

- [ ] `git status --short` 中只包含本任务相关文件。
- [ ] 符合 §2 禁止重新引入列表（`util::Result`、Boost.JSON domain contract、legacy sync tool、`src` 作为 public include surface、兼容性空 flag）。
- [ ] 对应测试切片已运行；若未运行，在最终回复中说明原因。
- [ ] README/计划在行为、CLI、session 或架构边界变化时已同步。
