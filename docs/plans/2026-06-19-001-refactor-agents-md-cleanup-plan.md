---
title: "refactor: Clean up AGENTS.md for progressive disclosure"
type: refactor
status: completed
date: 2026-06-19
---

# refactor: Clean up AGENTS.md for progressive disclosure

## Summary

清理 `AGENTS.md` 入口路由文档中的渐进展开违规：压缩内联迁移路线图（§2.5）、消除三处重复的禁止规则（§2/§5/§6）、用 README 引用替换重复的构建命令（§4）、收缩 §0 过细子条目、降维 §3 路由表的实现细节泄漏。只修改 `AGENTS.md`，不改其他文件。

---

## Requirements

- R1. `AGENTS.md` 保持功能完整的 agent 入口路由文档——Section 3 路由表、Section 0 先读顺序、Section 2 架构规则、Section 5 改动守则、Section 6 自检清单均保留并工作。
- R2. 不丢失信息——被删除或压缩的内容必须可通过替换的引用找到（README.md、plan 文件）。
- R3. 重复的禁止规则在三处出现（§2/§5/§6）→ 只在 §2 保留一份权威列表；§5 和 §6 改为交叉引用。
- R4. §2.5 内联迁移路线图 → 压缩为方向性陈述 + plan 文件引用，移除已完成的 stale 引用（002 plan）。
- R5. §4 构建命令与 fake provider 示例 → 替换为指向 README.md 的引用（README 已有等效且更完整的内容）。
- R6. §0 子条目 → 将两个具体 plan 文件名泛化为 `docs/plans/` 目录级指导。
- R7. §3 路由表 → "说明"列收缩为路由意图（什么任务找什么文件），实现细节（workspace guard、path containment 等）移入 §2 或 §5。
- R8. 现有 plan 文件中对 `AGENTS.md` 的交叉引用（`docs/plans/2026-06-16-001-*`、`002-*`、`018-*`、`011-*`、`013-*`）在清理后仍然可解析——不删除被引用到的实质内容。

---

## Scope Boundaries

- 不重组 Section 0–6 的编号结构。
- 不改变中文为主的文档语言约定。
- 不新增超出清理目的的章节或内容。
- 不修改 `README.md`、`docs/plans/` 下任何文件、`CMakeLists.txt` 或 C++ 源码。

---

## Context & Research

### Relevant Code and Patterns

- `AGENTS.md` §2 是禁止规则的权威来源——其余位置（§5 改动守则、§6 自检清单）是对同一组规则的运营化重述。
- `README.md` 是构建/运行/测试的权威文档。AGENTS.md §4 的构建命令、测试切片、fake provider 示例三部分在 README 中均有逐字等效或超集覆盖（唯一例外是 `OPENAI_API_KEY` 提醒，该提醒在 README 中也有等效上下文）。
- `docs/plans/` 下的 plan 文件遵循统一 frontmatter 约定（`type`、`status`、`date`），其中 `status: active` 是当前活跃计划的可编程标识。

### Institutional Learnings

- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`（`status: active`）：长期 pi C++ 化路线图。其 Module Parity Map 和 Suggested Attack Order 是 AGENTS.md §2.5 表与优先级段落的权威来源。该文档还包含 T10 文档卫生任务 "Keep AGENTS.md aligned"。
- `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md`（`status: completed`）：AGENTS.md §0 和 §2.5 引用该文档为 "先执行"，但 U1–U9 已完成。对首次阅读 agent 是浪费时间。
- `docs/plans/2026-06-18-001-feat-pi-ai-contract-parity-plan.md`（`status: active`）：当前活跃的实现计划，要求更新 AGENTS.md 路由表——该任务尚未反映在 AGENTS.md 中。应取代 002 作为活跃引用的代表。
- 5 个 plan 文件包含对 AGENTS.md 的交叉引用。清理不能删除这些被引用到的实质内容（如 provider registry 路由条目、session 路由条目等）。

### Research Conclusions

| 发现 | 行动 |
|------|------|
| README 是构建/测试的权威版本，AGENTS.md §4 是冗余子集 | 删除 §4，替换为 README 引用 |
| §2 禁止列表在 §5、§6 重述了两遍 | §2 保留为权威来源，§5/§6 交叉引用 |
| 002 plan（completed）不应出现在活跃先读列表 | 泛化为 `docs/plans/` 目录级指导 |
| 001 plan 是 §2.5 表的权威来源 | 压缩 §2.5 为方向性陈述 + 001 plan 引用 |
| AGENTS.md 无代码/CI 依赖，可自由重构 | 无外部破坏风险 |

---

## Key Technical Decisions

- **§2 保留为禁止规则的单一权威来源。** §5"改动守则"中与 §2 重叠的条目删除，改为 "遵守 §2 架构规则"。§6 自检清单中与 §2 重叠的条目合并为一条交叉引用。
- **§2.5 压缩为方向性陈述 + 引用，不保留内联迁移表。** 迁移表权威来源在 001 plan 中；入口路由文档只应告知方向，不应内联执行路线图。
- **§4 完全替换为 README 引用，不保留冗余速查。** 速查便利 ≠ 渐进展开；agent 具备跨文件读取能力，索引到 README 不增加有效负担。
- **§0 子条目泛化。** 用 "优先读 `docs/plans/` 中 `status: active` 的最新计划" 替换两个写死的文件名。
- **§3 路由表 "说明" 列收紧到路由意图——什么任务找什么文件。** 实现规则（workspace guard、path containment、secret redaction 等）属于 §2 或 §5。

---

## Implementation Units

### U1. Condense §2.5 and drop stale pre-read reference

**Goal:** 将 §2.5 的内联迁移表删除，替换为 2-3 句方向性陈述 + 指向 `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` 的引用；移除对已完成的 002 plan 的 "先执行" 引用。

**Requirements:** R4, R8

**Dependencies:** None

**Files:**

- Modify: `AGENTS.md`

**Approach:**

- 删除 §2.5 的 4 行迁徒映射表和 "迁移优先级" 段落。
- 替换为方向性陈述：长期目标是 pi C++ 化，迁移顺序见 001 plan，当前活跃实现计划见 `docs/plans/` 中最新 `status: active` 文件。
- §0 item 3 中 002 plan 子条目也一并泛化（见 U3）。

**Patterns to follow:**

- §1 "长期方向" 段落已有的简洁表述风格——一两句方向 + 引用。

**Test scenarios:**

- Happy path: 阅读 §2.5 后，agent 能通过引用定位到 001 plan 和 `docs/plans/` 目录获取完整路线图，无需在 AGENTS.md 中内联消费迁移细节。
- Edge case: 确认删除的表中条目（`pi:packages/ai`→`include/cch/ai/` 等）在 §3 路由表中已有等效行——信息未丢失。

**Verification:**

- §2.5 不再包含 4 行迁移表或 "迁移优先级" 段落。
- §2.5 包含对 `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` 的明确引用。
- 不再提及 002 plan 为 "先执行"。

---

### U2. Unify duplicate prohibition rules

**Goal:** 消除 §2/§5/§6 之间重复出现的禁止规则，§2 保留为单一权威来源。

**Requirements:** R3

**Dependencies:** None

**Files:**

- Modify: `AGENTS.md`

**Approach:**

- §2 保留现有 "禁止重新引入" 列表不变（它是权威来源）。
- §5 中删除与 §2 重叠的条目（"不要重新引入 `util::Result`" 等）。保留 §5 独有的运营层面的守则（如 "测试应保护当前架构意图与安全性质"、"涉及 shell/文件/环境变量/session 的改动必须考虑..."）。
- §6 自检清单中删除两条冗余条目（"公共 header 没有包含私有 `src/...` 或 provider DTO 细节"、"没有重新引入 `util::Result`、Boost.JSON domain contract、legacy sync tool"），合并为一条 "符合 §2 禁止重新引入列表"。
- 在 §5 开头加一句 "以下守则是对 §2 架构规则在改动实践中的补充，不重复架构规则已覆盖的禁入项"。

**Patterns to follow:**

- 现有 §5 末尾的独立测试守则——不与 §2 重叠，保留不动。

**Test scenarios:**

- Happy path: 通读 §2/§5/§6，每个禁止项只出现一次（在 §2），§5 和 §6 通过交叉引用指向它。
- Edge case: §6 自检清单仍是可逐项打勾的操作检查表——条目数减少但语义不丢失。

**Verification:**

- `util::Result`、`Boost.JSON`、`legacy sync tool` 等字符串仅在 §2 的一个位置出现。
- §5 和 §6 包含明确的 "见 §2" 式交叉引用。

---

### U3. Replace §4, trim §0, tighten §3

**Goal:** 一个实现单元收敛三处操作级清理：用 README 引用替换 §4 构建命令；泛化 §0 过细子条目；收缩 §3 路由表 "说明" 列的实现细节。

**Requirements:** R5, R6, R7

**Dependencies:** None（可与 U1/U2 并行）

**Files:**

- Modify: `AGENTS.md`

**Approach:**

- **§4：** 将整个节替换为：

  ```
  默认构建、测试切片、fake/real provider 运行示例详见 `README.md`。

  需要 OPENAI_API_KEY 的 real provider 验证除非任务明确要求，不要依赖真实网络/API 做默认验证。
  ```

  保留最后一句（它是 agent 行为准则，不是构建命令重复）但将前面所有命令替换为 README 引用。
- **§0 item 3：** 将两个缩进子条目替换为：

  ```
  - `docs/plans/`：当前/历史重构计划；涉及架构调整时优先读其中 `status: active` 的最新计划。
  ```

- **§3：** 逐行审查 "说明" 列，将实现细节提取到 §2 或 §5（除非该细节正是路由信息本身）。
  - "workspace guard 不是沙箱；保持路径 containment、symlink escape、防泄密环境变量等安全检查" → 移入 §5（改动守则）或 §2 架构规则的安全相关条目；路由表列保留 "路径 containment 和安全检查"
  - "pi-style v3 tree metadata 目前只 parse/preserve，非平凡 tree resume 要 fail closed" → 保留前半句（路由关联：Session 模块的当前能力边界），后半句移入 §5
  - 其余行审查后类似处理

**Patterns to follow:**

- 现有 §3 简练行的样式：如 "CLI 输出稳定 semantic event line；参数解析使用 CLI11" 就是合理的路由粒度。

**Test scenarios:**

- Happy path: 阅读 §4 后 agent 能定位到 README 获取构建命令。阅读 §0 item 3 后 agent 知道去 `docs/plans/` 自行发现活跃计划。阅读 §3 路由表后每行 "说明" 不超过 2 句路由信息。
- Edge case: §4 被完全清空但 `OPENAI_API_KEY` 行为准则被保留——agent 仍被告知不要默认依赖真实 API。

**Verification:**

- §4 不包含任何 `cmake`、`ctest`、`./build/cpp_harness` 命令。
- §4 包含对 `README.md` 的明确引用。
- §0 item 3 不再硬编码两个 plan 文件名。
- §3 路由表 "说明" 列没有条目超过 2 句话。

---

### U4. Verify no information loss and cross-reference integrity

**Goal:** 确认清理后 AGENTS.md 中没有任何必须在别处能找到却未给出引用的信息，确认 plan 文件对 AGENTS.md 的交叉引用仍然有效。

**Requirements:** R2, R8

**Dependencies:** U1, U2, U3

**Files:**

- Modify: `AGENTS.md`（可能微调，基于验证结果）

**Approach:**

- 逐条对比删除前后的内容，确认：
  - 被删除的 §2.5 迁移表条目在 §3 路由表中都有对应行
  - 被删除的 §4 构建命令在 README.md 中都有等效覆盖
  - 被删除的 §5/§6 冗余条目在 §2 中能找到权威版本
- 用 grep 在 `docs/plans/` 中搜索对 AGENTS.md 的引用，确认清理后引用的实体仍然存在（如 §3 路由表行、§2 架构规则等）

**Test scenarios:**

- Integration: `grep -r "AGENTS.md" docs/plans/` 返回的每一处引用，其引用的实体（如 provider registry 路由条目、session 路由条目）在清理后的 AGENTS.md 中仍可被人类和 agent 定位。

**Verification:**

- 上述 grep 结果中每一项都能定位到清理后 AGENTS.md 中的对应内容。
