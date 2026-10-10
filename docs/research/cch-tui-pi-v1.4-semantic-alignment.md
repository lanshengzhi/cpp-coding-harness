# cch_tui ↔ pi v1.0.4 语义对齐差异调研报告

- **调研基准**：
  - Pike `cch_tui`（`src/tui/`，HEAD `8346e2b75f6e6546dda972d4d9c3c6dfeaf7193e`）
  - pi upstream `packages/tui`（v1.0.4，HEAD `7c10bd4337495ee613f2224843ecdf349b80d1df`）
- **权威原则与裁决约束**：
  - ADR 0066：已支持子集内的用户可见行为与 upstream pi 一致；
  - ADR 0053：upstream pi 源码为比对证据，非 Pike 设计权威；
  - ADR 0035 / ADR 0051：能力范畴划分、底部 dock 固定机制及架构边界；
  - 本报告仅做调研与决策支持，不修改任何生产代码；严谨区分「实测 probe」与「源码推断」。

---

## 1. 执行摘要与优先级总览

本调研针对 `cch_tui` 基础能力模块与 pi `v1.0.4` upstream 之间的语义差异进行了逐项排查，排除了已关闭的历史项（Phase 1 清单中 5 项已在 Pike HEAD 全部闭环），重点聚焦键盘输入解码、编辑器交互、路径/命令补全、图片协议与渲染管线、终端能力检测与基础组件契约。

各差异项按用户可见破坏性与契约偏离程度分为三个优先级：
- **P0（阻断 / 关键输入损坏）**：直接导致按键输入产生乱码、丢失或触发错误动作（如 F1~F12 插入字面量、Kitty 重复字符未抑制、Shift+数字无法输入符号、Super 组合键误当普通文本等）。
- **P1（高优先级 / 严重渲染退化与状态泄露）**：导致 UI 视觉损坏、图片空白或样式残留（如图片在 transcript 滚入 scrollback 后恢复空白、iTerm2 size 缺失、OSC 8 截断超链接泄漏等）。
- **P2（中低优先级 / 边缘行为漂移与体验细节）**：包括 Markdown 缺 GFM 表格/LaTeX 渲染、编辑器光标跨行 sticky column 缺失、CJK 补全分词不兼容、Zed 终端能力检测缺失等。
- **架构裁决项（Product Decisions Required）**：固定底部 Dock 架构与 pi 差异的裁决、`setImageTranscoder` 动态扩展 Hook 是否纳入 C++ 核心库范畴。

### 综合差异清单矩阵

| # | 差异项简述 | 严重级 | 语义分类 | 处置建议 | 估计成本 |
|---|------------|--------|----------|----------|----------|
| 1 | F1–F12 功能键在 Editor 中被判定为可打印字符并插入字面量字符串 | **P0** | 契约漂移 | 立即对齐（修复判定） | S (极小) |
| 2 | Kitty 键盘协议 CSI-u 下缺少后续 Raw 字符的重复抑制逻辑 | **P0** | 契约漂移 | 立即对齐（引入 pending 抑制） | M (中等) |
| 3 | Kitty 协议下 Shift+数字（如 `CSI 49:33;2u`）未采用 shiftedKey 导致符号变数字 | **P0** | 契约漂移 | 立即对齐（优先 shiftedKey） | S (极小) |
| 4 | 解码器缺少 Super (Win/Cmd) 修饰键解析与支持 | **P0** | 契约漂移 | 补充支持或显式拦截过滤 | S (极小) |
| 5 | Kitty 协议激活状态下，孤立 `\n` 误当 Enter 提交而非换行 | **P0** | 契约漂移 | 对齐协议感知分支 | S (极小) |
| 6 | 西里尔等非拉丁键盘布局下 CSI-u 的 base_layout_key 回退机制在字母场景误用 | **P0** | 契约漂移 | 对齐 Latin/Symbol 阻断条件 | S (极小) |
| 7 | 图片在 RenderPipeline 渲染管线中后置写出，卷入 scrollback 的图片行无法恢复 | **P1** | 结构性差异 | 决策记录 / 评估 inline placement | L (较大) |
| 8 | iTerm2 图片协议编码缺少 `size=${byteLength}` 字段 | **P1** | 契约漂移 | 立即对齐协议序列 | S (极小) |
| 9 | `truncate_text` 在截断包含 OSC 8 超链接时缺少闭合序列，导致样式向外泄漏 | **P1** | 契约漂移 | 立即对齐 OSC 8 关闭逻辑 | S (极小) |
| 10 | Box 容器缓存采用全量子组件 revision，缺少逐行纯文本快速比对路径 | **P1** | 契约漂移 / 性能 | 评估对齐 pi 快速缓存比对 | M (中等) |
| 11 | 路径与命令补全缺乏 Unicode / CJK 标点空白边界支持 | **P2** | 契约漂移 | 对齐 CJK 标点分隔符正则 | M (中等) |
| 12 | 带有前置引号的路径补全（如 `@"`）被拒绝触发 | **P2** | 契约漂移 | 对齐 wrapper 回跳逻辑 | S (极小) |
| 13 | 文件补全缺少 base-directory-first 深度启发式策略 | **P2** | 契约漂移 | 对齐两阶段探测排序 | M (中等) |
| 14 | Editor 垂直光标移动缺失 `preferredVisualCol`（Sticky Column）特性 | **P2** | 契约漂移 | 对齐视觉列记忆算法 | M (中等) |
| 15 | 默认未绑定 `tui.editor.historyPrevious` / `historyNext` 独立动作 | **P2** | 契约漂移 | 补齐定义与空键位绑定 | S (极小) |
| 16 | Markdown 渲染缺 GFM 表格支持与 LaTeX 转换 | **P2** | 已记录有意分歧 / Deferred | 维持分歧 / 记录现状 | — |
| 17 | Unicode 宽度计算缺 Myanmar 与 Indic 辅音 marks 补全 | **P2** | 契约漂移 | 补充标记码点宽度补正 | S (极小) |
| 18 | 终端能力检测遗漏 Zed 编辑器及 `PI_*` 环境变量手动覆盖 | **P2** | 契约漂移 | 对齐环境判断与覆盖变量 | S (极小) |
| 19 | Text 与 Loader 窄宽度填充溢出抛出 Validation 错误而非弹性容错 | **P2** | 契约漂移 | 对齐 padding 动态收缩与软折行 | S (极小) |
| 20 | Overlay 布局计算模型：比例与全屏锚定 vs 相对区域锚定 | **P2** | 架构差异 | 记录有意分歧 | — |
| 21 | 固定底部 Dock 架构与 pi Transcript 行内混排差异 | **裁决项** | 有意产品分歧 | 维持 Pike 独立架构 (ADR 0051) | — |
| 22 | `setImageTranscoder` 动态转码回调机制 | **裁决项** | Deferred 范围 | 评估是否在 C++ 导出此 Hook | S (极小) |

---

## 2. P0 级差异：输入解码与键盘交互阻断

### #1 F1–F12 功能键在 Editor 中被判定为可打印字符并插入字面量

- **差异描述**：
  在终端未开启 Kitty 增强协议或解析 legacy 转义序列时，F1–F12 键被解析为 `KeyEvent{.key = "f1"}` 等。在输入分发进入 `Editor` 时，`detail::is_printable` 仅排除了 `enter`, `tab`, `escape`, `backspace`, `delete`, `insert`, `clear`, `home`, `end`, `pageUp`, `pageDown` 及方向键，但未排除 `f1`–`f12`。随后 `detail::printable_text` 直接返回其 `event.key`（如字符串 `"f1"`），导致按下功能键直接在输入框中打出 `"f1"`, `"f2"` 字面量。
- **证据链**：
  - Pike 侧：`src/tui/InteractionUtils.hpp:21-28`（`is_printable` 函数排除项不含 `f1`~`f12`）；`src/tui/InteractionUtils.hpp:35`（`printable_text` 直接回退返回 `event.key`）；`src/tui/Editor.cpp:1238-1241`（输入处理最后未被 keybindings 消费时调用 `insert_character(detail::printable_text(*event))`）。
  - pi 侧：`../pi/packages/tui/src/components/editor.ts:695-1000`（仅当 `decodePrintableKey(data)` 返回有效非空字符且非控制键时才调用 `insertText`；F 键不属于 printable）。
- **判定性质**：契约漂移（严重用户可见 Bug）。
- **建议与成本**：立即对齐。在 `InteractionUtils.hpp` 的 `is_printable` 中显式排除 `f1`~`f12`，或要求 `key.size() == 1`（UTF-8 字符）才允许视为可打印字符。成本：**S (极小)**。

### #2 Kitty 重复字符抑制机制缺失导致字符双击输入

- **证据链**：
  - pi 侧：`../pi/packages/tui/src/stdin-buffer.ts:186-192` 定义 `parseUnmodifiedKittyPrintableCodepoint`，正则识别 `^\x1b\[(\d+)(?::\d*)?(?::\d+)?u$`（无修饰且码点 $\ge 32$）；`stdin-buffer.ts:399-408` 在 `emitDataSequence` 中暂存 `pendingKittyPrintableCodepoint`，若随后紧跟一个由终端冗余上报的纯 raw ASCII 字符，且码点一致，则将其彻底抑制忽略。
  - Pike 侧：`src/tui/InputDecoder.cpp:719-840`（`TerminalStreamDecoder::process_bytes`）。Pike 直接在数据流中并行匹配各个 token，一旦匹配到 Kitty CSI-u 产生一个 `KeyEvent`，随后到来的原始字符又被作为单字节输入解析为另一个独立的 `KeyEvent`，导致在支持 Kitty 协议但有回退行为的终端上（如某些版本的 Ghostty/Foot/Kitty 配置）用户按一次键打出两个字符。
- **判定性质**：契约漂移（输入可靠性缺陷）。
- **建议与成本**：立即对齐。在 `TerminalStreamDecoder` 中引入类似 pi 的 `pending_kitty_printable_codepoint` 状态变量，在下一字节到来时核验并吸收抑制。成本：**M (中等)**。

### #3 Kitty 协议下 Shift+数字未优先提取 `shiftedKey`

- **差异描述**：
  当用户按下 `Shift+1`（英文键盘下应产生感叹号 `!`）且终端以 Kitty 格式输出扩展参数（如 `\x1b[49:33;2u`，其中 49 为 `'1'`，33 为 `'!'`，修饰键 2 表示 Shift）时，pi 解析为 `'!'`，而 Pike 解析为带 shift 的 `'1'`，导致编辑器插入了 `'1'` 而非 `'!'`。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/keys.ts:1350-1385`（`decodeKittyPrintable`）：
    ```ts
    let effectiveCodepoint = codepoint;
    if (modifier & MODIFIERS.shift && typeof shiftedKey === "number") {
        effectiveCodepoint = shiftedKey;
    }
    return String.fromCodePoint(effectiveCodepoint);
    ```
  - Pike 侧：`src/tui/InputDecoder.cpp:101-148`：`key_for_codepoint` 仅接收原始 `codepoint` 与 `modifiers.shift`；`make_key_event` 与 `parse_kitty_csi_u` 虽然解析了 key_parts[1]（shiftedKey），但将其直接丢弃，仅解析了 base_layout_key，导致字符构建始终使用原始数字码点 49。
- **判定性质**：契约漂移。
- **建议与成本**：立即对齐。在 `make_key_event` 中当 `modifiers.shift` 生效且存在 `shifted_key` 时，优先使用 `shifted_key` 映射字符。成本：**S (极小)**。

### #4 解码器缺失 Super (Win/Cmd) 修饰键解析与支持

- **差异描述**：
  pi 完整支持 `super`（掩码位 8），识别如 `super+k`, `super+enter` 等，并在 `decodeKittyPrintable` 中显式拦截含有 `super` 位的序列避免误当文本插入；Pike 的 `parse_modifiers` 仅校验了 1, 2, 4 位（`Shift`, `Alt`, `Ctrl`），任何含有 Super 的协议值都会被 `parse_modifiers` 判定为非法并返回 `std::nullopt`，进而回退至 raw 字符处理或彻底吞掉。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/keys.ts:142`（`type ModifierName = "ctrl" | "shift" | "alt" | "super"`）；`keys.ts:296`（`super: 8`）；`keys.ts:779`（`supportedModifierMask` 包含 `super`）；`keys.ts:1364`。
  - Pike 侧：`src/tui/InputDecoder.cpp:62-67`：
    ```cpp
    std::optional<ParsedModifiers> parse_modifiers(unsigned int protocol_value) {
        if (protocol_value == 0) return std::nullopt;
        const auto modifier = (protocol_value - 1) & ~kLockModifiers;
        if ((modifier & ~(kShiftModifier | kAltModifier | kCtrlModifier)) != 0) return std::nullopt;
        ...
    }
    ```
    `src/tui/include/cch/tui/Keys.hpp:18-28`：`KeyEvent` 结构体仅有 `ctrl`, `shift`, `alt` 三个布尔值，完全没有 `super`。
- **判定性质**：契约漂移。
- **建议与成本**：对齐。在 `KeyEvent` 中补充 `bool super{false};`，并在 `parse_modifiers` 与 `Keybindings` 解析中加入 Super 支持；若暂不向用户暴露 Super 快捷键，也必须显式拦截含有 Super 标志的按键，禁止其作为普通文本输入。成本：**S (极小)**。

### #5 Kitty 协议激活状态下，孤立 `\n` 误当 Enter 提交

- **差异描述**：
  在现代终端（如 Ghostty 等）配置了 `keybind = shift+enter=text:\n`，或 Kitty 协议处于激活状态时：pi 能够识别此时孤立的 `\n` 是用户映射的 `shift+enter`（即插入新行），而 `\r` 才是正常的提交 `enter`；Pike 侧无论协议状态为何，统一在 `InputDecoder.cpp:308` 判定 `if (sequence == "\r" || sequence == "\n") return *parse_key_id("enter");`，导致 Ghostty/Kitty 下按下 Shift+Enter 直接触发消息提交而非换行。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/keys.ts:1264-1267`：
    ```ts
    if (_kittyProtocolActive) {
        if (data === "\x1b\r" || data === "\n") return "shift+enter";
    }
    if (data === "\r" || (!_kittyProtocolActive && data === "\n") || data === "\x1bOM") return "enter";
    ```
  - Pike 侧：`src/tui/InputDecoder.cpp:308` 无条件将 `\n` 视为 `enter`；`TerminalStreamDecoder` 与 `InputDecoder` 未感知终端是否激活了 Kitty Keyboard 协议。
- **判定性质**：契约漂移。
- **建议与成本**：对齐。将终端 Kitty 协议激活状态通过 Terminal / Tui 传入 InputDecoder，当协议生效时，将 `\n` 与 `\x1b\r` 解码为 `shift+enter`。成本：**S (极小)**。

### #6 西里尔等非拉丁键盘布局下 CSI-u 的 base_layout_key 回退机制在字母场景误用

- **差异描述**：
  在非拉丁键盘（如俄语西里尔布局）下，终端上报 `CSI 1089::99;5u`（表示按下西里尔字母 С，物理键为 c，修饰键 Ctrl）。pi 的规则是：仅当码点不是已知的拉丁字母/数字/常用符号时，才允许回退到 `baseLayoutKey`（使 Ctrl+С 触发 Ctrl+c）。但对于字母本身的重映射（如 Dvorak），必须以 codepoint 优先，避免误判。Pike 目前在 `InputDecoder.cpp:133-138` 虽然有 `authoritative` 判断，但其对于 UTF-8 多字节字符的判定条件直接认为 `authoritative = false`，导致非拉丁字母总是被 `base_layout_key` 覆盖，如果终端上报了 base 键，可能导致文本录入异常替换。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/keys.ts:686-695` 与 `keys.ts:1220-1230`。
  - Pike 侧：`src/tui/InputDecoder.cpp:133-138`；`tests/tui/TuiTest.cpp:609`（已包含 `\x1b[1089::99;5u` 测试，确认了快捷键 Ctrl+C 的命中，但在纯文字插入通道上的边界需确保与 pi 完全对称）。
- **判定性质**：契约对齐微调。
- **建议与成本**：保持测试覆盖，核验在非修饰键（无 Ctrl/Alt）纯输入时，西里尔字符不会被回退成英文字符。成本：**S (极小)**。

---

## 3. P1 级差异：渲染管线、图片恢复与样式泄漏

### #7 图片在 RenderPipeline 中后置写出，卷入 scrollback 的图片行无法恢复（结构性差异）

- **差异描述**：
  - pi 方案：在主屏幕逐行写出（`tui-main-screen.ts`）时，图片作为行内转义序列直接内联拼接在对应的终端行中输出。当历史会话向上滚动进入终端回滚缓冲区（scrollback）时，终端原生保留了该行的图片控制序列，向上翻页查看历史时图片正常显示。
  - Pike 方案：`RenderPipeline` 采用分离架构，先将所有文本行一次性写出到终端，然后在末尾遍历调用 `terminal_.place_image(...)`（`src/tui/RenderPipeline.cpp:787`）。在 `ProcessTerminal.cpp:1600+` 中，如果计算出图片所在的顶部行号小于当前 dock 上方的可视视口（即已经卷入 scrollback），Pike 会直接跳过放置该图片，避免光标越界。这导致在长会话 transcript 滚动后，曾经渲染过的图片在终端滚动历史中变成大片永久空白，且无法恢复。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/tui-main-screen.ts:100-150`（内联写出 Kitty/iTerm 图片数据）。
  - Pike 侧：`src/tui/RenderPipeline.cpp:780-790`；`src/tui/ProcessTerminal.cpp:1580-1630`（跳过 row 越界检查与后置 cursor 放置）。
- **判定性质**：结构性设计分歧（ADR 0051 底部 dock 与统一虚拟终端机制带来的副作用）。
- **建议与成本**：
  - 短期：记录为**已记录分歧**。由于 Pike 采用固定底部 dock（editor 与 transcript 滚动解耦），终端硬件光标在 transcript 区域与 dock 区域来回跳跃，行内写出图片容易污染 dock 区域的局部刷新。
  - 长期：若要彻底解决 scrollback 图片白块，需重构 `RenderPipeline` 在刷新 transcript 行时内联发射图片序列。成本：**L (较大)**。

### #8 iTerm2 图片协议编码缺少 `size=${byteLength}` 字段

- **差异描述**：
  pi upstream 在生成 iTerm2 协议序列时，包含 `size=<base64字节数>` 参数。缺少该参数在某些终端（如 WezTerm 的 iTerm2 兼容模式或原生 iTerm2）接收大图片时可能触发格式解析失败或渲染丢帧。Pike 的实现自称对齐旧基线 `83114817`，未包含该字段。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/terminal-image.ts:297-303`：
    ```ts
    const size = Buffer.byteLength(base64Data, "base64");
    return `\x1b]1337;File=inline=1;width=${width};height=${height};size=${size}:${base64Data}\x07`;
    ```
  - Pike 侧：`src/tui/TerminalImage.cpp:150-180`（`encode_iterm2` 函数拼接 `File=inline=1;width=...;height=...:`，缺少 `size=`）。
- **判定性质**：契约漂移。
- **建议与成本**：立即对齐。解码 base64 长度并添加 `size=` 参数。成本：**S (极小)**。

### #9 `truncate_text` 截断带 OSC 8 超链接时缺少闭合，导致样式向外泄漏

- **差异描述**：
  当一行文本中含有打开的 OSC 8 超链接（`\x1b]8;;url\x07`）并被 `truncate_text` 截断时，pi 的 `finalizeTruncatedResult` 会在截断点之后、省略号之前显式追加活动的 OSC 8 闭合序列 `\x1b]8;;\x07`。而 Pike 的 `truncate_text` 仅追加了 SGR 颜色属性重置 `\x1b[0m`（`detail::kSgrReset`），未闭合超链接。这导致后续整行甚至终端后续输出的文本全部被终端识别为可点击超链接。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/utils.ts:155-170`：
    ```ts
    function finalizeTruncatedResult(...): string {
        const reset = "\x1b[0m";
        const hyperlinkClose = getActiveOsc8Close(prefix);
        ...
        return `${prefix}${hyperlinkClose}${reset}${ellipsis}${reset}`;
    }
    ```
  - Pike 侧：`src/tui/Utils.cpp:405-415`：
    ```cpp
    result += detail::kSgrReset;
    result += ellipsis;
    if (!ellipsis.empty()) result += detail::kSgrReset;
    ```
    仅有 `kSgrReset`，完全没有 `kOsc8LinkClose`。
- **判定性质**：契约漂移（严重渲染破坏）。
- **建议与成本**：立即对齐。在截断结尾判定是否存在活动超链接，若有则追加 `detail::kOsc8LinkClose`。成本：**S (极小)**。

### #10 Box 容器缓存缺少逐行纯文本快速比对路径

- **差异描述**：
  pi 的 `Box` 组件在每次 `render` 时，先以未填充形式收集所有子组件的行字符串，如果这些子组件返回的行内容与上次完全一致（内存字符串比对），则直接复用上次渲染结果，避免反复进行 ANSI 分词与行填充；Pike 的 `Box` 依赖内部 `children_revision_`，但当子组件内部发生局部更新而父 Box 未收到通知，或者相反子组件虽然刷新了但文本无变化时，Pike 每次必须遍历运行 `detail::prepare_rendered_line` 与分词器，开销较大且容易因为 revision 计数不一致出现缓存陈旧。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/components/box.ts:50-130`。
  - Pike 侧：`src/tui/Container.cpp:96-160`。
- **判定性质**：性能契约优化 / 隐式状态同步差异。
- **建议与成本**：评估优化。成本：**M (中等)**。

---

## 4. P2 级差异：编辑体验、补全策略与边缘排版

### #11 补全缺乏 Unicode / CJK 标点与空白边界支持

- **差异描述**：
  在中文或日文输入环境下，用户输入中文字符后紧跟 `@` 或 `/`，或者在中文逗号、句号后开始输入文件路径。pi 明确引入了 `cjkBreakRegex` 与 `cjkPunctuationRegex`，允许在中文字符、全角标点后正确识别 token 边界；Pike 的 `kPathDelimiters` 仅包含 ASCII 的 `" \t\"'="`，导致在中文文本后无法激活文件或技能补全。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/utils.ts:54-63`；`../pi/packages/tui/src/autocomplete.ts:8-10`。
  - Pike 侧：`src/tui/Autocomplete.cpp:37`；`Autocomplete.cpp:123`。
- **判定性质**：契约漂移（国际化体验缺陷）。
- **建议与成本**：对齐。将 CJK 标点集合加入 token 分隔符判定。成本：**M (中等)**。

### #12 引号 Wrapper 后 `@"` 补全被拒绝

- **差异描述**：
  当用户输入形如 `(@"path/to/file"` 或 `@'path'` 时，pi 的 `autocomplete.ts` 会回跳识别括号、方括号等成对符号并剥离 wrapper；Pike 的 `extract_quoted_prefix` 在处理前置字符时，如果发现 `@` 前面是 `(` 等非空白符号，直接拒绝触发。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/autocomplete.ts:10-40`（`PATH_WRAPPERS` 映射 `(`, `[`, `{`, `<`）。
  - Pike 侧：`src/tui/Autocomplete.cpp:127-137`。
- **判定性质**：契约漂移。
- **建议与成本**：对齐。在提取前缀时回跳 strip 括号等 wrapper。成本：**S (极小)**。

### #13 文件补全缺少 Base-Directory-First 策略

- **差异描述**：
  在大型工程目录下触发路径补全时，pi 采取两阶段策略：先以 `maxDepth: 1` 快速读取当前层级目录的前 100 项，再递归扫描全项目 100 项，合并去重后进行 fuzzy 打分。这确保了用户输入 `./` 时当前目录下的近邻文件永远优先展示；Pike 直接单次调用 `walk_directory_with_fd` 扫描 100 项直接排序，如果深层匹配得分接近，容易把深层文件顶在前面。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/autocomplete.ts:getBaseDirSuggestions`。
  - Pike 侧：`src/tui/Autocomplete.cpp:491+`。
- **判定性质**：体验契约漂移。
- **建议与成本**：对齐。成本：**M (中等)**。

### #14 Editor 垂直光标移动缺失 `preferredVisualCol`（Sticky Column）

- **差异描述**：
  当用户在长行末尾按方向键向下移动到短行，再向下移动到长行时：标准编辑器交互（包括 pi）会记住初始的视觉列 `preferredVisualCol`，在进入后续长行时自动恢复到原本的列位置；Pike 目前直接将光标 clamp 到了短行末尾（`std::min(to.end, to.start + offset)`），一旦经过一行短文本，光标就永久丧失了原本在行尾的列记忆。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/components/editor.ts:357`, `:1575-1615`（`preferredVisualCol` 算法，含 7 种状态分支）。
  - Pike 侧：`src/tui/Editor.cpp:765-780`（`move_vertical` 仅计算当前列 clamp）。
- **判定性质**：契约漂移（常见编辑器行为差异）。
- **建议与成本**：对齐。在 `Editor::Impl` 中引入 `preferred_visual_col`。成本：**M (中等)**。

### #15 键位映射缺失 `tui.editor.historyPrevious` / `historyNext` 独立定义

- **差异描述**：
  pi 将历史记录浏览独立暴露为 keybinding action（虽然默认按键为空列表，但允许用户自行绑定如 `ctrl+p`, `ctrl+n`）；Pike 没有这个 action，而是将历史记录浏览硬编码死在 `cursorUp` / `cursorDown` 且必须位于第一行/最后一行时才触发。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/keybindings.ts:11-12`, `:74-81`。
  - Pike 侧：`src/tui/Keybindings.cpp:210-240`；`src/tui/Editor.cpp:1193-1215`。
- **判定性质**：契约漂移。
- **建议与成本**：在 `Keybindings.cpp` 中补齐动作定义，允许用户按需配置独立历史快捷键。成本：**S (极小)**。

### #16 Markdown 渲染缺失 GFM 表格与 LaTeX 显示

- **差异描述**：
  pi 支持 Markdown 中的 GFM 表格自动排版（带边框、列宽感知、自动换行），并内置了 `renderLatex` 解析数学公式为 Unicode 上标/下标；Pike 使用 md4c 库，在解析阶段直接将 `MD_BLOCK_TABLE`, `MD_BLOCK_TR`, `MD_BLOCK_TD` 等全部映射为了普通 `BlockKind::Paragraph`（`src/tui/Markdown.cpp:168-175`），并将 LaTeX 公式当做普通行内文本/代码块输出。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/components/markdown.ts:560-562`, `:834-1021`；`../pi/packages/tui/src/latex.ts`。
  - Pike 侧：`src/tui/Markdown.cpp:168-175`, `:190-192`, `:322`（`MD_FLAG_TABLES` 标志未开启）。
- **判定性质**：已记录有意分歧（ADR 0035 记录 Markdown 采用 C++ tolerant parser，暂不实现复杂表格排版）。
- **建议与成本**：维持现状，暂不对齐。成本：**L (较大)**。

### #17 Unicode 宽度计算缺失 Myanmar 与 Indic Spacing Marks 修正

- **差异描述**：
  pi 在 `utils.ts:210-250` 中专门处理了终端渲染缅甸语（Myanmar）和印度语系复合元音附加符号（Spacing Marks）时的列宽溢出问题，给出了 `terminalSpacingMarkRegex`；Pike 基于 `utf8proc`，虽然处理了 Emoji 与宽字符，但对缅甸语等特定语系的标记符号未进行 terminal-spacing 累加，在渲染包含此类文字的文本时会导致边框或对其计算偏小 1 列。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/utils.ts:35-45`, `:210-250`；`packages/tui/test/truncate-to-width.test.ts:100`。
  - Pike 侧：`src/tui/UnicodeWidth.cpp:250-290`。
- **判定性质**：契约漂移。
- **建议与成本**：对齐。在 `UnicodeWidth.cpp` 的码点遍历循环中补充对应 Unicode 范围校验。成本：**S (极小)**。

### #18 终端能力检测遗漏 Zed 编辑器及 `PI_*` 环境变量手动覆盖

- **差异描述**：
  pi 在 `terminal-image.ts:114` 识别 `termProgram === "zed"` 支持 trueColor 与 hyperlinks；并在 `139-160` 允许通过 `PI_HYPERLINKS`, `PI_IMAGE_PROTOCOL`, `PI_TRUE_COLOR` 强制覆盖检测结果；Pike 遗漏了 Zed，且没有任何环境变量允许用户在检测失败时手动覆盖终端能力。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/terminal-image.ts:114`, `:139-160`。
  - Pike 侧：`src/tui/TerminalImage.cpp:361-366`；`src/tui/ProcessTerminal.cpp:163-168`。
- **判定性质**：契约漂移。
- **建议与成本**：立即对齐。将 Zed 写入已知终端列表，并引入环境变量覆盖检测。成本：**S (极小)**。

### #19 Text 与 Loader 窄宽度填充溢出抛出 Validation 错误

- **差异描述**：
  当终端视口缩到极窄（例如宽度仅 1~2 列，小于 `paddingX * 2`）时，pi 会动态缩小 padding（`Math.min(paddingX, Math.floor((width - 1) / 2))`）以尽力渲染；Pike 的 `Text::render` 与 `Box::render` 直接返回 `support::ErrorCode::Validation` 错误（"width is too small for padding"），在窗口快速拖拽缩小时可能触发局部崩溃或不可逆的错误挂起。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/components/text.ts:50-55`。
  - Pike 侧：`src/tui/Text.cpp:70-75`；`src/tui/Container.cpp:103-107`。
- **判定性质**：契约漂移（鲁棒性问题）。
- **建议与成本**：对齐。将硬报错改为自动将 padding clamp 到 `(width - 1) / 2`。成本：**S (极小)**。

### #20 Overlay 布局计算模型差异

- **差异描述**：
  pi 的 Overlay 尺寸计算支持基于终端全屏尺寸百分比解析（`parseSizeValue`），并在此基础上施加 margins；Pike 采用基于 `OverlayPosition` 枚举（居中、左上、右下等）与锚定组件相对偏移模型，二者配置 API 与边界处理模型不完全同构。
- **证据链**：
  - pi 侧：`../pi/packages/tui/src/tui.ts:228`, `:1204-1250`。
  - Pike 侧：`src/tui/Overlay.cpp`；`src/tui/OverlayCompositor.cpp:341-354`。
- **判定性质**：架构差异（ADR 0035 已界定）。
- **建议与成本**：维持现状，记录为有意设计分歧。成本：—。

---

## 5. 产品与架构裁决项（Decisions Required）

### 裁决项 A：固定底部 Dock 架构与 pi 的行为差异

- **背景与分歧**：
  - upstream pi 的设计中，Editor 仅仅是 TUI 组件树中的一个普通组件，位于 transcript 之后，在普通模式下随着历史输出一起向下流动。
  - Pike 依据 **ADR 0051** 与 PR #607（issue #597/#598）明确作出了重大的架构分歧决策：**实行固定底部 Dock（Fixed Bottom Dock）**。Editor 始终固定在终端物理底部的若干行，拥有独立的硬件滚动视口（`set_scroll_margins`）与光标约束。
- **张力所在**：
  ADR 0066 提出「在支持的子集内用户可见行为与 pi 保持严格一致」；但固定底部 Dock 会直接改变用户对于多行编辑、滚屏交互、图片位置的视觉心智。
- **裁决建议**：
  **确认 ADR 0051 的效力优先于 ADR 0066 的字面一致性**。固定底部 Dock 是 Pike 解决 CLI 交互闪烁与长会话稳定性的重大特性创新，属于**有意产品分歧**，不应退回 pi 的混排流动模型。需在 ADR 0066 或后续更新中明确将「固定底部 Dock」作为顶层豁免项记录。

### 裁决项 B：`setImageTranscoder` 动态转码 Hook 是否纳入 C++ 基础库

- **背景与分歧**：
  pi 在 v1.0.1 引入了 `setImageTranscoder`（`terminal-image.ts`），允许上层扩展传入自定义同步函数，将 WebP、AVIF 等特殊图片实时转码为 PNG base64。Pike 目前为静态编译，依赖系统 ImageMagick/STB，没有暴露运行时的图片转码拦截器。
- **裁决建议**：
  **标记为 Deferred**。Pike 当前无动态插件体系，无需暴露转码 Hook。

---

## 6. ADR 0035 历史描述更新建议

经本调研实测核验，ADR 0035 中存在个别与当前代码库实际实现不相符或过时的历史陈述，建议发起小型 PR 进行文档勘误：

1. **TruncatedText 省略号描述**：
   - ADR 0035 决策记录称：*“Keep TruncatedText's '...' default: rejected — pi's TruncatedText hard-cuts with no ellipsis; the strict-alignment directive removes the divergent default”*。
   - 事实上，pi upstream 的 `TruncatedText`（`packages/tui/src/components/truncated-text.ts:46`）调用 `truncateToWidth(singleLineText, availableWidth)`，而 `truncateToWidth` 的函数签名默认参数正是 `ellipsis: string = "..."`！因此 pi 的 TruncatedText 默认是有省略号的。
   - 反观 Pike 的 `src/tui/TruncatedText.cpp:52` 明确传入了空字符串 `truncate_text(single_line, available_width, "")`，执行了硬截断。
   - **建议**：修正 ADR 0035 的陈述，明确 Pike 的 TruncatedText 选择硬切（hard-cut）是有意优化，或是将代码调整为与 pi 一致带省略号。
2. **`cch_tui` 与 `pi-tui` baseline pin**：
   - 文档中反复出现的 `83114817` baseline 与当前实际对齐的 `v1.0.4`（`7c10bd4`）已存在跨度，建议在相关 ADR 附录中明确标注本文档更新的基准演进记录。
