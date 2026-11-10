# Pike

<p align="center">
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

基于现代 C++23 构建的实验性高性能 Coding Agent Runtime，提供原生终端界面（Native TUI）、一键式 CLI 接口及符合 C++ 惯用法的接口抽象。专为高性能、严格架构契约守卫与高确定性构建设计。

> **提示**
> Pike 是用于学习与前沿实验的原型运行时，并非生产级沙箱：提示词、输入文件以及工具执行输出可能会通过本地网关发送给配置的模型提供商。

---

## 快速上手

### 1. 启动本地模型网关
Pike 通过本地 [Magpie](docs/usage.md#models-and-authentication) 网关（运行于 `http://127.0.0.1:3425`）统一进行上游模型路由、模型目录发现与凭据管理。在启动 Pike 前请先运行：

```bash
magpie serve
```

### 2. 启动交互式原生终端界面 (Native TUI)
在终端中直接打开交互式编码助手：

```bash
build/release/pike
```

### 3. CLI 快速调用与多模态输入
支持通过命令行直接传参，或通过标准管道（stdin）输入上下文：

```bash
# 直接在标准输出中打印单次结果
build/release/pike --print "解释当前项目的代码结构"

# 通过管道传递上下文
cat error.log | build/release/pike --print "诊断该错误日志"

# 多模态与文件输入（基于文件内容自动检测 PNG/JPEG/GIF/WebP 图像格式）
build/release/pike @screenshot.png @main.cpp "结合实际代码审查此 UI 异常"

# 恢复历史会话或在已有上下文中继续
build/release/pike --continue "继续刚才的任务"
build/release/pike --resume
```

---

## 核心特性

- **原生终端 UI (Native TUI)**：极速全键盘操作体验，内置斜杠命令（`/model`, `/tree`, `/fork`, `/compact`, `/reload`）、会话导航，并支持自定义主题与键位映射。
- **内容自感知多模态输入**：文件路径前加 `@` 即可自动引入（如 `@spec.pdf`, `@diagram.webp`）。基于字节签名自动识别图片格式并安全缩放，文本文件自动规整注入提示词上下文。
- **会话树与时间旅行 (Session Trees)**：支持树状会话历史与非破坏性分叉（`/fork`）、历史分支可视化（`/tree`）及会话保存与恢复（`--resume`, `--continue`, `--session`）。
- **内置 User Bash 执行**：可在编辑器提示输入行直接运行 Shell 命令（`!cmd` 会将命令输出记录入对话历史；`!!cmd` 仅执行但不污染后续 LLM 上下文）。
- **通过 Magpie 本地网关进行模型路由**：通过 `--model <id>` 动态发现本地模型目录，由本地网关统一处理上游负载均衡、故障转移与轮换，避免在本地会话中明文落盘云端 API 凭据。
- **Fail-Closed 架构与 C++23 核心**：严格全库无异常策略（`-fno-exceptions`）、零开销抽象，并在编译构建期通过对齐架构门禁（Parity Architecture Gate）确保模块间边界与契约。

---

## 环境依赖

官方支持平台为 **原生 Linux x86-64 (glibc)**：

- **编译器**：GCC 16.x（Clang 22.x 用于 Debug 一致性校验）
- **构建工具**：CMake 4.4+, Ninja 1.11+
- **包管理器**：锁定版本的 vcpkg 清单（通过 `scripts/bootstrap.sh` 自动安装与锁定，不支持系统级依赖包构建）

更多工具链决策可参见 [ADR 0038](docs/adr/0038-reproducible-build-toolchain-floor-gcc-16-cmake-4-single-vcpkg-path.md) 与 [ADR 0039](docs/adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md)。

---

## 编译与测试

### Release 版本构建与测试

`bootstrap.sh` 脚本会自动检查宿主环境工具、在 `.deps/vcpkg` 下建立锁定版本的 vcpkg 二进制工具。随后由 CMake 驱动配置、构建与测试：

```bash
# 1. 准备依赖与工具链环境
scripts/bootstrap.sh

# 2. 构建 Release 版本
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

# 3. 运行完整测试套件
ctest --preset vcpkg-release
```

编译出的可执行文件位于 `build/release/pike`。

### 验证构建

```bash
build/release/pike --version
build/release/pike --help
```

*注：默认测试集具备完全确定性，使用 Mock Fake Provider，不发起任何外网请求。*

### 安装

将独立运行时安装至指定前缀路径：

```bash
cmake --install build/release --prefix ~/.local
~/.local/bin/pike --version
```

---

## 日常开发循环

常规开发使用默认的 `vcpkg` Debug Preset（生成在 `build/` 目录）：

```bash
cmake --preset vcpkg               # 每个新检出只需运行一次
cmake --build --preset vcpkg       # 增量编译
ctest --preset vcpkg               # 运行全量离线测试套件
```

### 聚焦模块测试 (Focused Shard Testing)
在特性迭代阶段，可以只编译目标模块并跳过全局架构门禁以大幅提升速度：

```bash
cmake --build --preset vcpkg --target cch_tests_coding_agent
ctest --preset vcpkg -LE architecture -L coding_agent
```

### 内存与线程安全检测 (Sanitizers)

Sanitizer 构建运行于隔离的伪提供者（Fake Provider）与临时环境中：

```bash
# AddressSanitizer 与 UndefinedBehaviorSanitizer (ASan/UBSan)
cmake --preset vcpkg-asan-ubsan && cmake --build --preset vcpkg-asan-ubsan
ctest --preset vcpkg-asan-ubsan

# ThreadSanitizer (TSan) 异步并发与关闭场景测试
cmake --preset vcpkg-tsan && cmake --build --preset vcpkg-tsan
scripts/ci/tsan-scenarios.sh
```

---

## 核心文档索引

- 📖 [用户使用指南 (Usage Guide)](docs/usage.md) — 完整的 CLI 参数、会话机制、配置项、Native TUI 斜杠命令与 User Bash 用法说明。
- ⌨️ [快捷键指南 (Keybindings)](docs/keybindings.md) — 原生 TUI 的键位映射语法、操作分派与自定义规则。
- 🔄 [Agent 生命周期 (Agent Lifecycle)](docs/agent-lifecycle.md) — 事件循环顺序、执行阶段与任务取消流转机制。
- 🏗️ [架构设计 (Architecture Guide)](docs/agents/architecture.md) — Owner 模块包划分、接口边界与设计决策 (ADRs)。
- 🧪 [测试验证与发布指南 (Validation Guide)](docs/agents/validation.md) — 聚焦/全量/全新三级测试分级、严格无异常策略及 CI 发布制品审计门禁。
- 📚 [术语表 (Glossary)](GLOSSARY.md) — 项目规范定义与专业术语表。
