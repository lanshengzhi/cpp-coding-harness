---
title: "design: C++ extension boundary decision"
type: design
status: active
date: 2026-06-20
target_repo: cpp-coding-harness
reference_repo: pi
origin: docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md
---

# design: C++ extension boundary decision

## Summary

确定 `cpp-coding-harness` 的第三方代码扩展方式。分析四种候选方案，推荐：**将现有 hook 系统正式化为 C++ 回调扩展边界（`SessionExtension`），并明确声明 pi 风格的 TypeScript 动态扩展有意不支持**。此决策解除 TUI（T7）、SDK（T8）和后续运行时设计的阻塞，同时保持 C++23 架构的简洁性和安全姿态。

---

## Problem Frame

pi 的扩展系统是 TypeScript-native 的：通过 jiti 动态加载 `.ts` 文件，订阅 20+ 生命周期事件，注册自定义工具/命令，与 TUI 交互。该模型与 C++ harness 存在根本性张力：

- C++ 无 JIT/动态加载标准基础设施。`dlopen` 共享库带来 ABI 脆弱性和 supply-chain 风险。
- C++ harness 的自我定位是"实验性 agent harness，不是生产沙箱"，README 明确将扩展和包安装列为 deferred。
- 但 T7（TUI）、T8（SDK）的设计需要知道代码是否能在运行时被外部注入——这直接影响事件总线设计、工具注册生命周期和 UI 组件架构。

路线图 T6 对此的定义是决策任务而非实现任务：**"Design a C++ extension boundary before implementing extensions."** 本计划完成此决策并输出设计文档。

---

## Requirements

- R1. 明确 C++ harness 的第三方代码扩展方式（四选一或组合）。
- R2. 决策必须保持安全姿态——"不将动态扩展关注点泄漏到核心 AI/agent 契约中"。
- R3. 决策必须与 README 已声明的 deferred items 一致（扩展、包安装、沙箱集成）。
- R4. 输出必须包含所选方案的边界定义（类型、注册点、生命周期），足够解除 TUI/SDK 设计的阻塞。
- R5. 分析每种方案与现有代码库 compatibility、维护成本和风险。

---

## Scope Boundaries

- 本计划是设计决策文档，**不实现任何扩展机制**。
- 不涉及包安装/发现（`pi install` 对等物）——这是 supply-chain 独立计划。
- 不涉及 TUI 组件设计——解除阻塞但不预先设计 UI 架构。
- 不涉及 OAuth、多 provider 注册、sandbox/容器集成——这些在路线图中已 defer。

### Deferred to Follow-Up Work

- `SessionExtension` 注册到 `RuntimeServices` 的实现——在本决策被批准后由单独实现计划跟进。
- 扩展生命周期事件（`extension_loaded`、`extension_error`）的 `AgentLifecycleEvent` 变体补充。
- 扩展状态持久化（通过 `CustomEntry`/`CustomMessageEntry`）的正式化——类型已存在，集成计划后续跟进。

---

## Context & Research

### Existing Extension-Like Patterns in the C++ Harness

| 机制 | 位置 | 能力 |
|------|------|------|
| 8 个 hook 回调 | `AsyncAgentOptions` | 拦截工具调用、转换上下文、注入消息、准备 next turn |
| `AsyncToolRegistry` | `agent/` | 运行时动态注册工具（move-only，按名称索引） |
| `CommandRegistry` | `coding_agent/` | 注册 session-lifecycle 内建命令 |
| `AgentEventSink` | `agent/` | 15 种生命周期事件变体，move-only 回调 |
| `CustomEntry` / `CustomMessageEntry` | `session/` | 扩展状态持久化（通用 JSON payload + LLM 可见消息） |
| Skill 系统 | `coding_agent/` | 项目本地 Markdown 技能文件发现、frontmatter 解析、模型可见上下文注入 |
| `--mode rpc` | `coding_agent/runtime/` | stdin/stdout JSONL 协议，可作为进程间扩展通道 |

### pi Extension System Reference

- **加载方式**：jiti（JIT TypeScript 编译器），从 `~/.pi/agent/extensions/` 和 `.pi/extensions/` 自动发现，`/reload` 热重载。
- **生命周期**：`session_start` → `resources_discover` → agent turn 循环（`turn_start`/`tool_call`/`tool_result`/`turn_end`）→ `agent_end` → `session_shutdown`。扩展可取消操作、注入消息、修改 system prompt。
- **安全模型**：扩展以用户权限运行，无沙箱。"Only install from sources you trust."
- **关键 API**：`pi.on(event, handler)`、`pi.registerTool({...})`、`pi.registerCommand(name, {...})`、`ctx.ui.notify/confirm/select`、`pi.appendEntry()`。

### Institutional Learnings

- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` T6 条目将扩展边界定位为设计决策，列出四个选项，并要求"done when: the decision preserves security posture and does not leak dynamic extension concerns into core AI/agent contracts."
- `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md` 将"extension event pipelines"和"harness extension API"分类为 Deferred parity，并将 RPC mode 列为可能的进程边界。

---

## Options Analysis

### Option A: Native C++ plugin (shared library / `dlopen`)

通过 `dlopen`/`LoadLibrary` 加载 `.so`/`.dll` 文件，调用约定的工厂函数获取扩展实例。

| 维度 | 评价 |
|------|------|
| **性能** | ★★★★★ 原生速度，零序列化开销 |
| **隔离性** | ★☆☆☆☆ 同进程运行，崩溃影响 harness |
| **ABI 稳定性** | ★☆☆☆☆ C++ ABI 极度脆弱（编译器版本、STL 版本、构建 flag 均影响） |
| **维护成本** | ★☆☆☆☆ 需要维护稳定的 C ABI 边界或要求用户使用完全相同的构建环境 |
| **安全** | ★☆☆☆☆ 同进程运行，恶意扩展可访问所有内存 |
| **与路线图一致性** | ★☆☆☆☆ README 明确 defer"ABI-stable binary distribution" |

**结论：排除。** ABI 脆弱性在无稳定二进制分发承诺的前提下是不可接受的维护负担。"workspace guard is not a sandbox"的安全声明在 `dlopen` 场景下沦为虚设。

### Option B: Process/RPC extension

扩展作为独立进程运行，通过 stdin/stdout JSONL（现有 `--mode rpc` 协议）或 socket 与 harness 通信。

| 维度 | 评价 |
|------|------|
| **性能** | ★★★☆☆ 进程间通信有序列化开销，但对工具调用/事件回调可接受 |
| **隔离性** | ★★★★★ 进程隔离，崩溃不影响 harness |
| **ABI 稳定性** | ★★★★★ 语言无关，JSONL 协议是唯一契约 |
| **维护成本** | ★★★☆☆ 需要维护 RPC 协议规范，但协议已存在 |
| **安全** | ★★★★☆ 进程边界是最强隔离（除 OS 级沙箱外） |
| **与路线图一致性** | ★★★☆☆ 利用了现有 `--mode rpc` 基础设施，但完整的双向事件推送未实现 |

**结论：可行但 premature。** 现有 RPC 模式是单向的（命令→响应），不支持 harness→扩展的事件推送。需要先完成 RPC 协议的扩展（事件流、工具注册协商），再在此之上构建扩展系统。作为中期方向合理，但作为当前决策并非最低成本路径。

### Option C: Embedded JS/TS bridge（QuickJS / V8 / JavaScriptCore）

嵌入 JavaScript 运行时，加载 TypeScript（经编译或 JIT），复用 pi 扩展生态。

| 维度 | 评价 |
|------|------|
| **性能** | ★★☆☆☆ JS 运行时开销 + 跨语言调用序列化 |
| **隔离性** | ★★☆☆☆ 同进程运行（除非 isolate 机制完善），安全取决于运行时沙箱质量 |
| **ABI 稳定性** | ★★★★☆ JS 层无 ABI 问题，但 native binding 层仍需维护 |
| **维护成本** | ★☆☆☆☆ 嵌入 JS 运行时 + TypeScript 编译 + pi API 兼容层 = 巨大维护负担 |
| **安全** | ★★★☆☆ 依赖运行时沙箱，V8 isolate 成熟但复杂度高 |
| **与路线图一致性** | ★☆☆☆☆ 与 C++23 定位背道而驰——将 JS 运行时设为核心依赖 |

**结论：排除。** 引入 JS 运行时作为 core dependency 与本项目的 C++23 定位冲突。维护成本（保持与 pi TS API 兼容）远超收益，尤其是在 pi 本身还在快速演进的情况下。

### Option D: Intentionally unsupported

明确声明第三方代码扩展不在 C++ harness 范围内。所有可扩展性通过编译时（修改源码）和配置（skills、prompt templates）实现。

| 维度 | 评价 |
|------|------|
| **实现成本** | ★★★★★ 零 |
| **安全** | ★★★★★ 无动态代码加载攻击面 |
| **维护成本** | ★★★★★ 零 |
| **功能损失** | ★☆☆☆☆ 无自定义工具、无运行时行为注入、无第三方集成 |
| **与路线图一致性** | ★★★★★ 与 README deferred list 完全一致 |

**结论：作为立场声明正确且诚实，但会削弱 harness 的实用性。** 内置的 hook 系统已提供了类似扩展的能力——将它们正式化比完全放弃更合理。

### Option E: Formalize existing hook system as C++ callback extension boundary

将现有的 `AsyncAgentOptions` hook 回调系统正式化为 `SessionExtension` 概念，提供结构化的扩展注册点。**不引入动态加载**——扩展是编译时注册的 C++ 对象，通过 `RuntimeServices` 注入。

| 维度 | 评价 |
|------|------|
| **实现成本** | ★★★★☆ 现有 hook 基础设施已就位，主要是重命名和组织 |
| **性能** | ★★★★★ 原生函数调用 |
| **隔离性** | ★★☆☆☆ 同进程（但这是 C++ 的固有属性） |
| **安全** | ★★★★☆ 编译时注册、无动态加载攻击面、无用户级代码边界 |
| **维护成本** | ★★★★☆ 使用现有 `std::move_only_function` 基础设施 |
| **扩展性** | ★★★☆☆ 覆盖 hook、工具、命令注册，但不覆盖 UI 交互（待 TUI 决策） |
| **与路线图一致性** | ★★★★☆ 不引入新的 distribution surface，保持 C++ 原生风格 |

**结论：推荐。** 最低成本获得可用的扩展边界——将已经存在的能力正式化，不引入新的运行时依赖。明确边界在哪（编译时注册）为后续 Option B（RPC 扩展）留出升级路径。

---

## Key Technical Decisions

### Decision: Option E — `SessionExtension` callback boundary

**选择：** 将现有的 `AsyncAgentOptions` hook 系统重构为具名的 `SessionExtension` struct 集合，在 `RuntimeServices` 中注册。

**理由：**
1. **零新依赖**：8 个 hook 类型、`AsyncToolRegistry`、`CommandRegistry`、`CustomEntry` 持久化全部已存在。只需将它们打包为 `SessionExtension`。
2. **保持安全姿态**："不将动态扩展关注点泄漏到核心 AI/agent 契约"——`SessionExtension` 是编译时注册的 C++ 对象，不涉及文件发现、动态加载、或代码生成。核心 AI/agent 类型不变。
3. **解除 TUI/SDK 阻塞**：TUI 组件和 SDK 接口可以依赖 `SessionExtension` 的存在——它们知道扩展边界是一组已注册的回调 + 工具工厂，不需要为动态不可信代码设计。
4. **为未来 RPC 扩展留升级路径**：`SessionExtension` 的回调签名可以后续适配为序列化协议（JSONL over stdin/stdout），此时从 Option E 切换到 Option B 只需在注册层做适配，不改变扩展逻辑。
5. **与现有架构一致**：遵循 "capability seams" 规则——`SessionExtension` 是面向接口的能力集合，不暴露实现细节。

### Decision: pi-style TypeScript extensions are intentionally unsupported

**选择：** 在文档中明确声明 C++ harness **不支持** TypeScript/JavaScript 扩展、动态加载和热重载。

**理由：**
- 与 README deferred list 保持一致。
- C++ 的 ABI 和运行时模型与 Node.js/TypeScript 生态系统根本不同。
- 避免用户期望 pi 兼容的扩展 API。

---

## Design: SessionExtension Boundary

> *这是定向设计指导，不是实现规范。实现代理应将其作为上下文，而非照搬代码。*

```cpp
// 概念性设计 —— 不是可编译代码

namespace cch::coding_agent {

/// 一个 session 扩展：将 hooks + 工具工厂 + 命令处理器打包
/// 为编译时注册的单元。
struct SessionExtension {
    /// 用于日志和诊断的人类可读名称
    std::string name;

    // --- Agent hooks（与 AsyncAgentOptions 中的现有类型相同）---
    std::optional<agent::BeforeToolCallHook> before_tool_call;
    std::optional<agent::AfterToolCallHook> after_tool_call;
    std::optional<agent::TransformContextHook> transform_context;
    std::optional<agent::ConvertToLlmHook> convert_to_llm;
    std::optional<agent::GetSteeringMessagesHook> get_steering_messages;
    std::optional<agent::GetFollowUpMessagesHook> get_follow_up_messages;
    std::optional<agent::PrepareNextTurnHook> prepare_next_turn;
    std::optional<agent::ValidateTurnUpdateHook> validate_turn_update;

    // --- 工具工厂（返回 AsyncAgentTool 实例）---
    using ToolFactory = std::move_only_function<
        std::unique_ptr<agent::AsyncAgentTool>()>;
    std::vector<ToolFactory> tool_factories;

    // --- 命令处理器（注册到 CommandRegistry）---
    using CommandFactory = std::move_only_function<
        std::pair<std::string, CommandHandler>()>;
    std::vector<CommandFactory> command_factories;

    // --- 会话树 hook（来自 T4 计划）---
    std::optional<harness::session::BranchSummaryHook> branch_summary_hook;
};

} // namespace cch::coding_agent
```

**注册点：** `RuntimeServicesConfig` 新增 `std::vector<SessionExtension> extensions` 字段。`make_runtime_services()` 迭代注册：将扩展的 hooks 合并到 `AsyncAgentOptions`，将其工具工厂注册到 `AsyncToolRegistry`，将其命令注册到 `CommandRegistry`。

**生命周期：** 扩展在 `RuntimeServices` 构造时注册，在 `RuntimeServices` 析构时释放。无热重载、无动态发现——这是设计意图，不是未实现的功能。

**事件集成：** 现有 `AgentLifecycleEvent` variant 可新增 `ExtensionLoadedEvent`（`{name, version}`）和 `ExtensionErrorEvent`（`{name, error_message}`）——这些在扩展注册失败或 hook 返回错误时发出。但添加这些事件属于后续实现计划，不在本设计文档范围内。

---

## System-Wide Impact

- **Interaction graph:** `SessionExtension` 作为新的聚合类型插入 `RuntimeServicesConfig` → `RuntimeServices` → `AsyncAgentOptions` 的连线中。不改变现有 hook 的调用语义，仅改变它们的打包方式。
- **Error propagation:** 扩展 hook 错误通过现有的 `util::Expected` 通道传播。钩子在注册时验证（如 tool name 冲突），注册失败通过 stderr diagnostics 报告——与现有 skill 加载诊断模式一致。
- **API surface parity:** 不对齐 pi `ExtensionAPI`（`pi.on()`、`pi.registerTool()` 等）。`SessionExtension` 是 C++ native 设计，有自己的惯用风格。
- **Unchanged invariants:** `AsyncAgentOptions` 的 hook 字段签名不变。`AsyncToolRegistry` 和 `CommandRegistry` 的公共 API 不变。现有 8 个 hook 的回调语义不变。
- **Deferred explicitly:** 无动态加载（`dlopen`）、无 JS 运行时嵌入、无热重载、无文件系统发现、无 `pi install` 包安装。

---

## Alternatives Considered

| 方案 | 排除理由 |
|------|----------|
| A: Native C++ plugin (`dlopen`) | ABI 脆弱性、无稳定二进制分发承诺、安全边界薄弱 |
| B: Process/RPC extension | 作为中期方向可行，但需要先扩展 RPC 协议——当前并非最低成本路径。Option E 可平滑升级到 B |
| C: Embedded JS/TS bridge | 与 C++23 定位冲突、维护成本极高、引入重型运行时依赖 |
| D: Intentionally unsupported | 作为立场声明正确，但会浪费已有的 hook 基础设施——Option E 以近乎零成本获得更好的结果 |

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `SessionExtension` 过于灵活以至于泄漏实现关注点到配置层 | 保持 hook 类型与 `AsyncAgentOptions` 完全相同——不新增 API surface，仅重新组织 |
| 编译时注册限制了快速迭代（需要重新编译） | 这是设计意图。快速迭代走 skills（Markdown 文件）、prompt templates 和配置——这些不需要重新编译 |
| 未来如果决定要 RPC 扩展（Option B），需要桥接 gap | `SessionExtension` 的回调签名刻意保持了序列化友好的形状（context struct → result struct），迁移成本可控 |

---

## Sources & References

- **Origin document:** [docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md](2026-06-16-001-refactor-pi-cpp-parity-todo.md) — T6 checkbox
- **Contract inventory:** [docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md](2026-06-16-003-refactor-pi-cpp-contract-inventory.md)
- **pi extension docs:** `pi:packages/coding-agent/docs/extensions.md`
- **pi extension examples:** `pi:packages/coding-agent/examples/extensions/`
- Related code: `include/cch/agent/AgentContext.hpp` (AsyncAgentOptions, hooks), `include/cch/agent/AgentTool.hpp` (AsyncToolRegistry), `include/cch/coding_agent/PromptProcessing.hpp` (CommandRegistry), `include/cch/agent/AgentEvent.hpp` (AgentLifecycleEvent), `src/coding_agent/runtime/RuntimeServices.cpp` (扩展注册点)
- **RPC mode:** `docs/plans/2026-06-20-003-feat-t8-jsonl-rpc-mode-plan.md`
