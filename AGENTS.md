# AGENTS.md

本文件是本仓库的 Agent 入口路由文档。作用范围为仓库根目录及其所有子目录。

## 0. 先读顺序

1. `README.md`：项目定位、架构边界、CLI 行为、工具和安全说明。
2. `CMakeLists.txt`：构建目标、源文件清单、测试目标。
3. `docs/plans/`：当前/历史重构计划；涉及架构调整时优先读最新相关计划。
4. 按任务进入下面的路由表。

开始改动前先执行：

```bash
git status --short
```

不要覆盖用户已有修改；只编辑与当前任务相关的最小文件集。

## 1. 项目定位

`cpp-coding-harness` 是一个实验性 C++23 coding-agent harness。核心流程：CLI/REPL 接收 prompt，经 OpenAI Chat Completions 兼容客户端请求模型，执行模型发起的本地工具调用，再把工具结果追加回对话，直到停止或达到最大 turn，并将脱敏 typed transcript 写入 JSONL session。

本项目不是生产沙箱，也不承诺兼容稳定 SDK。设计优先级是学习、可替换边界和反脆弱架构。

## 2. 必守架构规则

1. **数据是被动值状态**：公共 contract 使用 aggregate-friendly `struct`、`std::variant`、`std::expected` 和项目内 `util::JsonValue`。
2. **能力跨物理边界**：Chat client、stream transport、execution env、session store、tool 都通过接口或隐藏实现细节的 concrete class 暴露。
3. **事件是弱连接**：Agent/provider 事件 sink 使用 `util::MoveOnlyCallback`，不要回退到要求 copyable 的 `std::function`。
4. **泛型机制局部化**：Glaze DTO、schema 转换、visitor、解析 helper 等应留在 serialization/implementation 层，不要泄漏到 domain-facing API。

禁止重新引入：旧同步工具面、`util::Result`、Boost.JSON domain contract、把 `src` 当作 public include surface、兼容性空 flag。

## 3. 目录与职责路由

| 任务类型 | 优先入口 | 说明 |
| --- | --- | --- |
| Agent loop / turn 流程 / tool-call 编排 | `include/cch/agent/`, `src/agent/AgentLoop.cpp`, `tests/agent/` | `AsyncAgentLoop` 负责 provider-neutral loop；事件通过 `AgentLifecycleEvent` 发出。 |
| AI 消息、内容、tool schema、usage contract | `include/cch/ai/`, `tests/ai/` | 公共消息/内容类型必须保持 passive value contract。 |
| OpenAI-compatible provider / SSE / HTTP transport | `include/cch/ai/providers/`, `src/ai/providers/`, `src/ai/glaze/`, `tests/ai/providers/` | provider wire DTO 和 Glaze mapping 放实现/serialization 层，不要放回公共 domain contract。 |
| JSON 序列化/反序列化 | `include/cch/ai/glaze/`, `src/ai/glaze/`, `include/cch/util/Json*.hpp`, `tests/ai/GlazeRoundTripTest.cpp` | Domain 用 `JsonValue`，Glaze 只在边界做 DTO 转换。 |
| 内置工具 read/write/edit/bash | `include/cch/tools/ToolFactories.hpp`, `src/tools/AsyncToolFactories.cpp`, `tests/tools/` | 工具通过 `AsyncAgentTool` 暴露；文件/进程能力走 execution env。 |
| 工作区、路径防护、shell 执行 | `include/cch/harness/ExecutionEnv.hpp`, `include/cch/harness/LocalExecutionEnv.hpp`, `src/harness/`, `src/tools/PathGuard.hpp`, `src/util/Process.hpp`, `tests/harness/` | workspace guard 不是沙箱；保持路径 containment、symlink escape、防泄密环境变量等安全检查。 |
| Session JSONL / resume | `include/cch/harness/session/`, `src/harness/session/JsonlSessionStore.cpp`, `tests/harness/session/` | JSONL 是脱敏 typed transcript；未来 entry 类型应可安全忽略。 |
| CLI / REPL / runtime wiring | `src/main.cpp`, `src/AsyncCliRuntime.*`, `tests/cli/` | CLI 输出稳定 semantic event line；不要恢复 legacy `--async` 等兼容 flag。 |
| 公共边界/架构守卫 | `tests/architecture/`, `CMakeLists.txt`, `include/cch/` | 改公共 header、依赖或 include surface 时必须跑 architecture tests。 |
| 文档与计划 | `README.md`, `docs/plans/` | 重大行为或边界变化需同步 README/计划。 |

## 4. 构建与验证

默认构建：

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

常用切片：

```bash
./build/cpp_harness_tests "[architecture]"
./build/cpp_harness_tests "[ai][u2]"
./build/cpp_harness_tests "[ai][provider]"
./build/cpp_harness_tests "[agent][async]"
./build/cpp_harness_tests "[tools][async]"
./build/cpp_harness_tests "[harness][session]"
./build/cpp_harness_tests "[cli]"
```

Fake provider 运行示例：

```bash
./build/cpp_harness --fake --session /tmp/cpp-session.jsonl "hello"
./build/cpp_harness --fake --workspace . --session /tmp/cpp-read.jsonl "read README.md"
./build/cpp_harness --fake --repl --session /tmp/cpp-repl.jsonl
```

Real provider 需要 `OPENAI_API_KEY`，除非任务明确要求，不要依赖真实网络/API 做默认验证。

## 5. 改动守则

- 公共 API 改动优先考虑 `include/cch/...` 是否泄漏实现细节。
- 行为实现优先放 `src/...`；需要暴露能力时用纯虚接口或隐藏实现细节的 class。
- 新增工具时：先定义 `ai::Tool` schema，再实现 `AsyncAgentTool`，通过 `AsyncToolRegistry` 注册，并补 `tests/tools/`。
- 新增 provider 或 transport 时：保持 `StreamingChatClient` / `StreamTransport` seam，wire DTO 留在 provider/serialization 层。
- 新增 session 字段时：保持 append-only JSONL 和对未知 entry 的容忍。
- 涉及 shell、文件、环境变量、session 的改动必须考虑 workspace containment、secret redaction 和输出截断。
- 测试应保护当前架构意图与安全性质，不要只保护旧类名、旧 JSONL 形状或旧 transcript 文案。

## 6. 提交前自检清单

- [ ] `git status --short` 中只包含本任务相关文件。
- [ ] 公共 header 没有包含私有 `src/...` 或 provider DTO 细节。
- [ ] 没有重新引入 `util::Result`、Boost.JSON domain contract、legacy sync tool。
- [ ] 对应测试切片已运行；若未运行，在最终回复中说明原因。
- [ ] README/计划在行为、CLI、session 或架构边界变化时已同步。
