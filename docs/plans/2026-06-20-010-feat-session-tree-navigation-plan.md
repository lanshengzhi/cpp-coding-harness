---
title: "feat: Session tree branch navigation and context reconstruction"
type: feat
status: completed
date: 2026-06-20
target_repo: cpp-coding-harness
reference_repo: pi
origin: docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md
---

# feat: Session tree branch navigation and context reconstruction

## Summary

在已完成的 v3 session tree 写入支持（9 种 entry 类型、20 个测试）之上，实现会话树的分支导航（切换活跃叶节点）和从树路径重建 agent 上下文。新增 `SessionTree` 类封装内存树索引与导航逻辑，遵循 pi `SessionManager` API 契约；分支摘要生成通过 move-only hook seam 暴露给调用方，不在本计划内实现 LLM 摘要调用。

---

## Problem Frame

当前 `JsonlSessionStore` 已支持所有 9 种 v3 tree entry 类型的写入与加载，`SessionEntry` 携带 `parent_id`/`leaf_id` 字段，`LoadedSession` 返回平坦的 entries 向量。但缺少两个关键能力：

- **分支导航**：无法在已加载的会话树内切换活跃叶节点、无法查询指定路径的子节点、无法持久化当前叶位置。
- **上下文重建**：`--resume` 目前仅从平坦的 messages 向量恢复，无法处理有分支的树（多个叶节点）、无法正确处理路径上的 `CompactionEntry`（摘要→保留消息→后续消息的顺序），也无法将 `BranchSummaryEntry`/`CustomMessageEntry` 转换为 LLM 可见消息。

pi `SessionManager` 定义了目标契约：`getLeafId()`、`setLeaf(entryId)`、`getBranch(fromId)`、`getChildren(parentId)`、`buildSessionContext()`。本计划按此契约实现 C++ 对等物。

---

## Requirements

- R1. 提供内存中的会话树视图，支持按 `entry_id` 快速查找和按 `parent_id` 聚类子节点。
- R2. 实现叶节点导航：`getLeafId()`、`getLeafEntry()`、`branch(entryId)`（切换活跃叶），并通过写入 `Leaf` entry 持久化当前叶位置。
- R3. 实现 `getBranch(fromId)` 从指定节点到根的路径收集，`getChildren(parentId)` 返回直接子节点。
- R4. 实现 `buildSessionContext()` 上下文重建：叶→根遍历，遇到 `CompactionEntry` 时输出摘要→`firstKeptEntryId` 起消息→压缩后消息；将 `BranchSummaryEntry`/`CustomMessageEntry` 转为 `BranchSummaryMessage`/`CustomMessage` 纳入消息列表；提取路径上的当前 model/thinking-level 设置。
- R5. 分支切换时提供 hook seam 用于生成 `BranchSummaryEntry`，不将 LLM 调用硬编码进 session store。
- R6. `JsonlSessionStore` 的加载路径返回或创建 `SessionTree`，保持现有 `LoadedSession` 兼容性。
- R7. 所有新公共类型遵循被动值契约（aggregate struct、`std::variant`、`util::Error`/`util::Expected`）。

---

## Scope Boundaries

- 不实现自动压缩触发逻辑（`/compact` 命令、context-threshold 自动触发）。
- 不实现 LLM 分支摘要生成（仅提供 hook seam，实际摘要生成推迟到后续计划）。
- 不实现 `/fork`、`/clone` 新会话文件创建。
- 不实现交互式 `/tree` TUI 选择器。
- 不改变现有 `--resume` 线性恢复行为（线性路径自动选择唯一叶节点）。

### Deferred to Follow-Up Work

- Agent loop 在执行期间写入 tree entry（model_change、thinking_level_change 等）——当前 agent loop 仅写 message entry，tree entry 写入将在单独 PR 中集成。
- 分支摘要 LLM 生成——在扩展边界设计（T6）和 compaction 基础设施稳定后跟进。

---

## Context & Research

### Relevant Code and Patterns

- `include/cch/harness/session/SessionEntry.hpp` — `SessionEntry`（携带 `entry_id`/`parent_id`/`leaf_id`）、`SessionEntryKind` 枚举（含 `Leaf` 值但无写入支持）、`LoadedSession`（`entries` 向量 + `messages` 向量）。
- `src/harness/session/JsonlSessionStore.cpp` — DTO 模式：每种 entry 类型有无名命名空间内的 Glaze-reflectable DTO struct（`ModelChangeDto`、`BranchSummaryDto` 等），字段名与 pi 的 JSON 格式完全对齐。Entry ID 为 8 字符随机 hex。`populate_tree_fields()` 在 `load()` 时填充 `entry_id`/`parent_id`/`leaf_id`。
- `include/cch/agent/AgentContext.hpp` — `AgentState` struct（`messages`、`streaming_message`、`active_tool_names`、`model`、`thinking_level`）定义了上下文重建需要产出的目标形状。
- `include/cch/agent/AgentLoop.hpp` — `AsyncAgentLoop::continue_with(vector<MessageVariant> history, ...)` 是上下文注入点。
- `tests/harness/session/JsonlSessionStoreTest.cpp` — 使用 `TempWorkspace` + `WorkspaceFileSystem` 的测试夹具模式；所有 v3 entry 类型有独立 round-trip 测试和混合顺序测试。
- `tests/architecture/` — 保护公共头边界、禁止 Glaze 泄漏到 domain header。

### Institutional Learnings

- `docs/plans/2026-06-19-005-feat-session-tree-write-support-plan.md`：明确将 tree navigation 和 `buildSessionContext()` 推迟到后续 T4 slice。DTO struct 选择内联字段（不继承基础 struct）以避免 Glaze 嵌套 JSON 问题——内存树结构应同样避免继承设计，优先使用以 ID 为键的 flat map。
- `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`：警告"silently mis-resuming nontrivial trees"——resume 路径必须显式处理多分支树，而非默默加载错误的线性路径。

### External References

- `pi:packages/coding-agent/docs/session-format.md` — SessionManager API 完整契约：`getLeafId()`、`getLeafEntry()`、`getEntry(id)`、`getBranch(fromId?)`、`getTree()`、`getChildren(parentId)`、`branch(entryId)`、`branchWithSummary(entryId, summary)`、`buildSessionContext()`。上下文重建算法：叶→根遍历，compaction-aware（摘要先于被保留消息），BranchSummaryEntry/CustomMessageEntry 转为消息格式。
- `pi:packages/coding-agent/docs/sessions.md` — 树结构可视化、`/tree` 导航流程、分支摘要触发时机。
- `pi:packages/coding-agent/docs/compaction.md` — CompactionEntry 结构、`firstKeptEntryId` 语义、split-turn 处理。

---

## Key Technical Decisions

- **新类 `SessionTree` 封装树索引**：与 `JsonlSessionStore`（I/O 边界）分离。`SessionTree` 持有 `LoadedSession` entries + 内存索引（`entry_id → index` map + `parent_id → children` map），提供纯内存导航操作。遵循 "capability seams" 架构规则。
- **叶位置通过 `Leaf` entry 类型持久化**：`SessionEntryKind::Leaf` 已存在于枚举中。`branch(entryId)` 操作写入 `Leaf` entry（`type: "leaf"`，携带 `entry_id` 和 `targetId`）以持久化新叶位置。这是 pi session-format.md 未单独列出的 entry 类型，但枚举已预留。
- **上下文重建产出 `SessionContext` 被动值**：包含 `vector<MessageVariant>`、`model`（可选）、`thinking_level`（可选）——可直接传入 `AsyncAgentLoop::continue_with()`。不通过全局状态或回调传递。
- **分支摘要 hook 使用 move-only callback**：`BranchSummaryHook = move_only_function<Expected<BranchSummaryEntry>(fromLeafId, toLeafId, branchEntries)>`。遵循 §2 Rule 3：事件使用 `std::move_only_function`。
- **不引入继承层次**：内存索引使用 `unordered_map`（ID→索引）和 `vector`（children），不继承 `SessionEntry`。遵循 Glaze DTO 的内联字段先例。

---

## Implementation Units

### U1. SessionTree data structure and in-memory tree index

**Goal:** 从 `LoadedSession::entries` 构建高效的内存树索引，支持按 ID 查询和父子关系遍历。

**Requirements:** R1, R7

**Dependencies:** None

**Files:**
- Create: `include/cch/harness/session/SessionTree.hpp`
- Modify: `src/harness/session/CMakeLists.txt`（如存在）或上层 `CMakeLists.txt`
- Test: `tests/harness/session/SessionTreeTest.cpp`

**Approach:**
- `SessionTree` 持有 `vector<SessionEntry>`（移动自 `LoadedSession`）、`unordered_map<string, size_t>`（entry ID → entries 索引）、`unordered_map<string, vector<size_t>>`（parent ID → children 索引列表）。
- 构造函数接受 `LoadedSession&&`，在构造时建立索引。
- 提供 `getEntry(id) -> optional<reference_wrapper<const SessionEntry>>`、`getChildren(parentId) -> vector<reference_wrapper<const SessionEntry>>`、`getEntries() -> const vector<SessionEntry>&`。
- 设计为 move-only（持有 `unique_ptr` 或直接持有 vector），不提供拷贝。
- 遵循被动值契约：`SessionTree` 是不透明 capability class，公共方法返回 const 引用或值类型，不暴露内部索引细节。

**Patterns to follow:**
- `JsonlSessionStore` 的 RAII + move-only 模式。
- `LoadedSession` 的 flat vector + 分离的 metadata 结构。

**Test scenarios:**
- Happy path: 从 3 个线性 entry 构建树索引，`getEntry()` 按 ID 查找成功，`getChildren()` 返回正确的子节点列表。
- Happy path: 分叉树（A→B→C 和 A→B→D），`getChildren(B)` 返回 {C, D} 两个子节点。
- Edge case: 空 entries 向量（仅 header），索引为空但不崩溃。
- Edge case: 查询不存在的 entry ID 返回 `nullopt`。
- Edge case: 叶节点 `getChildren()` 返回空向量。

**Verification:**
- `SessionTree` 可从 `LoadedSession` 构造，索引建立后基本查询正确。
- 类型不暴露 Glaze 或实现细节到公共头。

---

### U2. Leaf tracking and tree navigation

**Goal:** 实现叶节点导航：读取当前叶位置、切换活跃叶、收集从叶到根的路径。

**Requirements:** R2, R3

**Dependencies:** U1

**Files:**
- Modify: `include/cch/harness/session/SessionTree.hpp`
- Modify: `src/harness/session/JsonlSessionStore.cpp`（`Leaf` entry 写入 DTO）
- Test: `tests/harness/session/SessionTreeTest.cpp`

**Approach:**
- 叶位置状态：`SessionTree` 内部维护 `leaf_entry_id_`（可选 string）。构造时从 `LoadedSession::entries` 的最后一个 `Leaf` entry 恢复叶位置；若无则默认为最后一条 entry 的 ID（线性路径）。
- `getLeafId() -> string`：返回当前叶 ID。
- `getLeafEntry() -> const SessionEntry&`：返回当前叶 entry。
- `branch(entryId) -> ExpectedVoid`：切换活跃叶。验证 `entryId` 存在于树中，否则返回 `ErrorCode::Session` 错误。成功后更新 `leaf_entry_id_`。
- `getBranch(fromId = nullopt) -> vector<SessionEntry>`：从指定节点（默认当前叶）沿 `parent_id` 向上走到根，返回路径 entry 列表（叶→根顺序）。
- `getRoot() -> const SessionEntry&`：返回根 entry（`parent_id == nullopt` 的 entry）。
- 导航方法为 const（只读），不修改树结构。`branch()` 是唯一修改状态的方法。

**Patterns to follow:**
- pi `SessionManager` API 方法命名和语义。
- `util::Expected` 返回值约定（错误时 `ErrorCode::Session`）。

**Test scenarios:**
- Happy path: 线性 3-entry 树，`getLeafId()` 返回最后 entry ID，`branch()` 切换到中间 entry 成功，`getBranch()` 返回叶→根路径（2 个 entry）。
- Happy path: 分叉树中 `branch()` 切换到另一分支的叶节点，后续 `getBranch()` 反映新路径。
- Error path: `branch()` 传入不存在的 `entryId` 返回错误。
- Edge case: 空树（仅 header）上 `getLeafId()` 行为——应返回 nullopt 或空字符串。
- Edge case: `getBranch()` 从根节点调用只返回根自身。

**Verification:**
- 叶位置切换正确，路径收集与树结构一致。
- `branch()` 状态变更在多次操作后保持一致性。

---

### U3. Context reconstruction via buildSessionContext()

**Goal:** 实现 `buildSessionContext()`，按 pi 算法从叶→根遍历重建 LLM 上下文。

**Requirements:** R4

**Dependencies:** U2

**Files:**
- Modify: `include/cch/harness/session/SessionTree.hpp`
- Modify: `src/harness/session/`（新增 SessionTree.cpp 或内联到现有文件）
- Test: `tests/harness/session/SessionTreeTest.cpp`

**Approach:**
- 定义产出类型 `SessionContext`：`vector<ai::MessageVariant> messages`、`optional<string> model`、`optional<string> thinking_level`。
- 算法（对齐 pi `buildSessionContext()`）：
  1. 从叶到根收集路径上的所有 entry。
  2. 提取路径上最近的 `ModelChangeEntry`（model）和 `ThinkingLevelChangeEntry`（thinking_level）设置。
  3. 如果路径上存在 `CompactionEntry`：
     - 首先输出 `CompactionSummaryMessage`（含 `summary` 文本）。
     - 从 `firstKeptEntryId` 开始到压缩 entry 之间的消息按原始顺序输出。
     - 压缩 entry 之后的消息按原始顺序输出。
     - 压缩 entry 之前的消息（不含被保留段）被跳过。
  4. 遇到 `BranchSummaryEntry` 时转换为 `BranchSummaryMessage`（role: "branchSummary"）。
  5. 遇到 `CustomMessageEntry` 时转换为 `CustomMessage`（role: "custom"）。
  6. 遇到 `SessionMessageEntry` 时提取其中的 `message` 字段（`ai::MessageVariant`）。
  7. 其他 entry 类型（`ModelChange`、`ThinkingLevelChange`、`Label`、`SessionInfo`、`Leaf`、`Custom`）不产生消息，仅用于状态提取或跳过。
- 处理多个 `CompactionEntry` 的嵌套：最靠近叶的 compaction 优先，使用其 `firstKeptEntryId` 作为消息起点；更早的 compaction 被封装在摘要中。

**Patterns to follow:**
- 现有 `JsonlSessionStore::load()` 的 `populate_tree_fields()` 解包模式。
- `ai::CompactionSummaryMessage`、`ai::BranchSummaryMessage`、`ai::CustomMessage` 已在 `include/cch/ai/Message.hpp` 中定义。

**Test scenarios:**
- Happy path: 线性 user→assistant→user→assistant 树，无 compaction，`buildSessionContext()` 返回全部 4 条消息原文，model/thinking_level 为 nullopt。
- Happy path: 树含 `ModelChangeEntry`（provider: openai, model: gpt-4o）和 `ThinkingLevelChangeEntry`（high），语境重建正确提取 model="gpt-4o"、thinking_level="high"。
- Happy path: 树含 `CompactionEntry`（summary="prior work", firstKeptEntryId=第3条消息的ID），语境重建返回 CompactionSummaryMessage + 第3条起的所有消息，第1-2条被跳过。
- Happy path: 树含 `BranchSummaryEntry`，语境重建将其转为 `BranchSummaryMessage` 插入消息列表。
- Happy path: 树含 `CustomMessageEntry`（display=true），语境重建将其转为 `CustomMessage`。
- Edge case: 空树（仅 header），`buildSessionContext()` 返回空 messages。
- Edge case: 路径上有多个 `ModelChangeEntry`，取最靠近叶（最晚）的。
- Edge case: 路径上 `CompactionEntry` 的 `firstKeptEntryId` 指向不存在的 entry——应安全降级，从 compaction entry 后的第一条消息开始。
- Integration: 分叉树中 `branch()` 切换叶后，`buildSessionContext()` 返回新路径的消息（验证路径切换影响语境）。

**Verification:**
- `buildSessionContext()` 输出可直接作为 `AsyncAgentLoop::continue_with()` 的 history 参数。
- 与 pi `buildSessionContext()` 行为一致（compaction-aware、BranchSummary 转换、model/thinking 提取）。

---

### U4. Branch summary hook seam

**Goal:** 提供分支摘要生成的 hook 回调签名，不实现 LLM 调用逻辑。

**Requirements:** R5

**Dependencies:** U2

**Files:**
- Modify: `include/cch/harness/session/SessionTree.hpp`
- Test: `tests/harness/session/SessionTreeTest.cpp`

**Approach:**
- 定义 `BranchSummaryContext` struct：`from_leaf_id`（离开的分支叶 ID）、`to_entry_id`（导航目标 entry ID）、`branch_entries`（从旧叶到公共祖先的 entry 列表）。
- 定义 hook 类型：`using BranchSummaryHook = std::move_only_function<util::Expected<std::optional<BranchSummaryData>>(const BranchSummaryContext&)>`。
- `BranchSummaryData` struct：`summary`（string）、`details`（optional JsonValue）。调用方（而非 SessionTree）负责生成摘要文本。
- `branchWithSummary(entryId, hook)` 方法：先调用 hook 生成摘要，成功则将 `BranchSummaryEntry` append 到会话，然后执行 `branch(entryId)` 切换叶。
- Hook 为可选——`branch(entryId)` 不强制摘要生成。

**Patterns to follow:**
- `AsyncAgentOptions` 中 `BeforeToolCallHook`/`AfterToolCallHook` 的 move-only callback + context struct 模式。
- `JsonlSessionStore::append_branch_summary()` 已有的写入支持。

**Test scenarios:**
- Happy path: hook 返回有效 `BranchSummaryData`，`branchWithSummary()` 写入 `BranchSummaryEntry` 并切换叶。
- Happy path: hook 返回 `nullopt`（调用方选择不摘要），`branchWithSummary()` 仅切换叶，不写入摘要。
- Error path: hook 返回 `unexpected(Error)`，`branchWithSummary()` 传播错误。
- Edge case: hook 在切换前调用，验证 `BranchSummaryContext` 中的 `branch_entries` 包含正确的旧分支路径。

**Verification:**
- Hook seam 签名与现有 hook 风格一致（move-only、context struct、Expected 返回）。
- `branchWithSummary()` 在摘要生成失败时不产生持久化写入。

---

### U5. JsonlSessionStore integration and Leaf entry write support

**Goal:** `JsonlSessionStore::open_existing()` 新增返回 `SessionTree` 的工厂方法；新增 `Leaf` entry 写入支持。

**Requirements:** R6

**Dependencies:** U1, U2

**Files:**
- Modify: `include/cch/harness/session/JsonlSessionStore.hpp`
- Modify: `src/harness/session/JsonlSessionStore.cpp`
- Modify: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- `JsonlSessionStore` 新增静态方法 `open_as_tree(path) -> Expected<SessionTree>`：内部调用 `load()`，将 `LoadedSession` 移动构造 `SessionTree`。
- 保留现有 `load()` 方法不破坏兼容性。
- `SessionTree` 新增方法 `store()` 返回底层 `JsonlSessionStore` 的非拥有引用（或 `SessionTree` 持有 `JsonlSessionStore` 实例），用于写入新 entry。
- 新增 `Leaf` entry DTO 和 `append_leaf(targetId)` 方法：`LeafDto` 包含 `type: "leaf"`、`id`、`parentId`、`timestamp`、`targetId`。
- `SessionTree::branch()` 调用 `append_leaf()` 持久化叶位置变更。
- `SessionTree` 从 `LoadedSession` 恢复叶位置：扫描 entries 找最后一个 `Leaf` entry，提取 `targetId` 作为初始叶位置；若不存在则用最后一个 entry 的 ID。

**Patterns to follow:**
- 现有 9 种 entry 类型的 DTO + append 方法模式（`ModelChangeDto` → `append_model_change()`）。
- `open_existing()` 返回 `Expected<JsonlSessionStore>` 的现有签名。
- Entry ID 生成：8 字符随机 hex，与现有 `next_entry_id_` 一致。

**Test scenarios:**
- Happy path: `open_as_tree()` 加载含 3 个 message entry 的会话，返回可用的 `SessionTree`。
- Happy path: `append_leaf()` 写入 `Leaf` entry，round-trip 加载正确恢复 `targetId`。
- Happy path: `Leaf` entry 恢复叶位置：构造 `SessionTree` 后 `getLeafId()` 等于最后一个 `Leaf` entry 的 `targetId`。
- Edge case: 无 `Leaf` entry 的会话（旧格式），恢复叶位置为最后一条 entry 的 ID（向后兼容）。
- Edge case: 空会话（仅 header），`open_as_tree()` 成功返回空树。

**Verification:**
- `open_as_tree()` 产物可通过 `SessionTree` 导航，且底层 JSONL 文件可被 `load()` 正常读取。
- 新增 `Leaf` entry 不破坏现有 round-trip 测试。

---

### U6. Architecture tests and edge case hardening

**Goal:** 保护新公共头边界、确保 `SessionTree` 不泄漏实现细节、覆盖边界和错误路径。

**Requirements:** R7

**Dependencies:** U1–U5

**Files:**
- Modify: `tests/architecture/PublicHeaderBoundaryTest.cpp`
- Modify: `tests/harness/session/SessionTreeTest.cpp`
- Modify: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- 在 `PublicHeaderBoundaryTest` 中新增断言：`SessionTree.hpp` 不包含 `<glaze/`>、不暴露 Glaze DTO。
- `SessionTreeTest` 补充边界场景：紧凑化+分支摘要混合树、深层嵌套（10 层）、大子节点数（50+）、循环引用检测（虽然 JSONL 不应产生，但索引构建应能容忍）。
- `JsonlSessionStoreTest` 新增：`Leaf` entry 与现有 entry 类型混合写入、`Leaf` entry round-trip 验证。
- 确保 `SessionTree` 遵循 move-only 契约（无拷贝构造/赋值）。

**Test scenarios:**
- Architecture: `SessionTree.hpp` 公共头不包含 Glaze 或 provider DTO。
- Edge case: 50 个子节点的分叉树，`getChildren()` 正确返回全部。
- Edge case: 10 层线性嵌套，`getBranch()` 返回完整路径，`buildSessionContext()` 产生正确消息序列。
- Error path: 在构造后追加新 entry 到 JSONL 文件（绕过 SessionTree 的外部修改），SessionTree 应能安全处理（索引不变，新 entry 不在索引中但文件完整）。

**Verification:**
- 架构测试全部通过。
- 新公共头 `SessionTree.hpp` 只暴露被动值类型和 capability class，不含实现细节。

---

## System-Wide Impact

- **Interaction graph:** `SessionTree` 作为新的 capability class 插入 `JsonlSessionStore`（I/O）与 `AgentSessionRunner`（消费方）之间。`AgentSessionRunner::run_prompt()` 未来可从 `SessionTree` 读取语境并写入 tree entry；当前计划仅实现基础设施，不改变 agent loop 行为。
- **Error propagation:** 导航错误通过 `util::Expected` 传播，使用 `ErrorCode::Session`。叶 ID 无效、entry 不存在等错误不静默降级。
- **State lifecycle risks:** `SessionTree` 持有 entries 的内存索引副本，不与 JSONL 文件自动同步。追加操作需通过 `SessionTree` → `JsonlSessionStore` 的双写路径。**本计划仅实现基础设施**，agent loop 集成（tree entry 写入）推迟到后续 PR。
- **API surface parity:** `getLeafId()`、`getBranch()`、`buildSessionContext()`、`getChildren()` 对齐 pi `SessionManager` API。`getTree()`（返回完整树结构）推迟到有 TUI 需求时再实现。
- **Unchanged invariants:** `JsonlSessionStore::load()` 返回 `LoadedSession` 的语义不变。`--resume` 线性恢复行为不变（自动选择唯一叶或最后 entry）。

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `buildSessionContext()` 算法与 pi 行为偏差 | 以 pi session-format.md 伪代码和 pi 源码为权威参考；编写 compaction-aware 集成测试覆盖关键场景 |
| `Leaf` entry 格式不被 pi 兼容 | `Leaf` entry 仅用于 C++ 内部叶位置持久化，不影响与 pi 的跨实现兼容性（pi 不使用 `Leaf` entry，但容忍未知 entry 类型） |
| 内存索引与 JSONL 文件不同步 | SessionTree 构造时从 LoadedSession 建立索引，后续写入通过 SessionTree 方法代理到 JsonlSessionStore。agent loop 集成推迟避免竞态 |
| 大会话（10k+ entry）索引构建性能 | 使用 hash map（O(1) 查找），构造复杂度 O(n)；如后续出现性能问题可在写入时维护增量索引 |

---

## Sources & References

- **Origin document:** [docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md](2026-06-16-001-refactor-pi-cpp-parity-todo.md) — T4 checkbox
- **Completed foundation:** [docs/plans/2026-06-19-005-feat-session-tree-write-support-plan.md](2026-06-19-005-feat-session-tree-write-support-plan.md)
- **Contract inventory:** [docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md](2026-06-16-003-refactor-pi-cpp-contract-inventory.md)
- **pi SessionManager API:** `pi:packages/coding-agent/docs/session-format.md`
- **pi tree navigation:** `pi:packages/coding-agent/docs/sessions.md`
- **pi compaction:** `pi:packages/coding-agent/docs/compaction.md`
- Related code: `include/cch/harness/session/SessionEntry.hpp`, `src/harness/session/JsonlSessionStore.cpp`, `include/cch/agent/AgentContext.hpp`, `include/cch/agent/AgentLoop.hpp`
