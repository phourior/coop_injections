# AGENTS.md

## 1. 项目概览

这是一个用于 Windows 平台下对 `SC2_x64.exe` 进行运行时功能扩展、调试与分析的 C++ 项目。  
项目包含两个主要组件：

- `Dll1`：注入到目标进程中的 DLL，负责 Hook、内存读取、功能修改和 ImGui 渲染。
- `ConsoleApplication1`：用于加载或注入 DLL 的控制台程序。

项目还包含：

- `docs/`：逆向分析、地址扫描、功能说明文档。
- `tools/`：用于 PE、内存快照、差异分析的辅助脚本。
- `py/`：实验性 Python 脚本。

本项目仅用于开发者明确授权的环境、测试用途和兼容性研究。

---

## 2. 环境要求

### 操作系统

- Windows 10 / Windows 11
- x64 环境

### 开发工具

- Visual Studio 2022 或更新版本
- MSVC C++ 工具链
- Windows SDK
- C++20

### 推荐工具

- Visual Studio
- VS Code
- CMake（如果后续迁移构建系统）
- Git
- Python 3（运行 `tools/` 或 `py/` 中的脚本）

---

## 3. 项目构建

### 使用 Visual Studio

打开：

`Dll1.slnx`

推荐配置：

- Configuration：`Debug`
- Platform：`x64`

分别构建：

- `Dll1`
- `ConsoleApplication1`

构建输出通常位于：

- `x64/Debug/`
- 或项目内部的 `x64/Debug_*` 目录

### 使用 MSBuild

示例：

```powershell
msbuild Dll1.slnx /p:Configuration=Debug /p:Platform=x64
```

如果只构建 DLL：

```powershell
msbuild Dll1/Dll1.vcxproj /p:Configuration=Debug /p:Platform=x64
```

---

## 4. 代码结构

### `Dll1/core`

核心基础设施：

- `globals.*`：全局状态
- `memory.h`：内存访问工具
- `game_data.*`：游戏数据结构和读取逻辑
- `log.h`：日志工具

### `Dll1/hooks`

Hook 与功能逻辑：

- `d3d9_hook.*`：Direct3D9 Hook
- `game_hook.*`：主要游戏 Hook
- `game_hook_experience.cpp`：经验相关功能
- `game_hook_mastery.cpp`：精通相关功能
- `game_hook_vision.cpp`：视野功能
- `game_hook_observer_vision.cpp`：观察者模式视野
- `game_hook_leader_panel.cpp`：统计面板
- `game_hook_nnet.cpp`：网络相关 Hook

### `Dll1/render`

渲染和 UI：

- `menu.*`：ImGui 菜单
- `overlay.*`：Overlay 绘制

### `Dll1/ext`

第三方依赖：

- ImGui
- MinHook
- stb_image

---

## 5. 代码规范

### 语言标准

- 使用 C++20
- 优先使用现代 C++ 特性
- 避免不必要的裸指针
- 尽量使用 RAII 管理资源
- Windows API 句柄必须明确释放

### 命名约定

#### 全局变量

```cpp
g_variableName
```

#### 静态变量

```cpp
s_variableName
```

#### 常量

```cpp
kConstantName
```

或：

```cpp
UPPER_CASE_CONSTANT
```

#### 函数

```cpp
PascalCaseFunction()
```

#### 类型

```cpp
PascalCaseType
```

---

## 6. Hook 开发规则

### 使用 MinHook

所有 Hook 必须遵守以下生命周期：

```text
MH_CreateHook
MH_EnableHook
MH_DisableHook
MH_RemoveHook
```

如果启用了多个 Hook，应优先使用：

```cpp
MH_QueueEnableHook
MH_QueueDisableHook
MH_ApplyQueued
```

避免逐个暂停所有线程。

### Hook 回调要求

Hook 回调必须：

- 尽量短小
- 避免阻塞
- 避免动态内存分配
- 避免文件 IO
- 避免锁竞争
- 避免调用复杂 STL

### Hook 生命周期

卸载 DLL 时必须：

1. 设置卸载标志
2. 禁止新的 Hook 逻辑执行
3. 等待正在执行的 Hook 回调结束
4. 禁用 Hook
5. 恢复原始游戏状态
6. 卸载 DLL

禁止在 Hook 回调仍运行时释放 DLL。

---

## 7. 内存访问规则

禁止直接假设地址始终有效。

推荐使用：

```cpp
SafeMemcpy
ReadMemory
PatternScan
```

访问游戏对象前必须验证：

- 指针非空
- 地址可读
- 对象字段范围合理
- 模块基址有效

禁止在未知地址直接解引用。

---

## 8. Pattern Scan 规则

所有运行时地址应优先通过特征码定位。

示例：

```cpp
PatternScan("SC2_x64.exe", pattern);
```

禁止：

- 仅使用固定 RVA
- 未验证原始指令字节就修改代码
- 在版本变化后自动继续执行未知地址

如果特征码失败：

- 输出日志
- 禁用该功能
- 不允许崩溃

---

## 9. 代码补丁规则

修改游戏代码前必须：

1. 验证原始字节
2. 保存原始字节
3. 修改页面权限
4. 写入补丁
5. FlushInstructionCache
6. 恢复页面权限

卸载时必须恢复原始字节。

所有补丁必须具备：

```cpp
ApplyPatch()
RestorePatch()
```

---

## 10. 多线程规则

游戏内部存在多个线程：

- 渲染线程
- 游戏逻辑线程
- 网络线程
- UI 线程

Hook 状态必须使用：

- `Interlocked*`
- `SRWLOCK`
- 或原子变量

禁止：

- 在 Hook 中长时间持锁
- 在暂停线程时等待锁
- 在卸载过程中与 Hook 并发修改状态

---

## 11. ImGui 开发规则

ImGui UI 位于：

```text
Dll1/render/menu.cpp
```

UI 只能：

- 修改功能开关
- 显示状态
- 修改配置

UI 不应直接执行复杂 Hook 操作。

应调用功能接口，例如：

```cpp
EnableFullMapVision()
DisableFullMapVision()
```

---

## 12. 功能状态模型

每个功能应区分：

```text
Requested
Applied
Failed
```

示例：

```text
用户启用功能
    ↓
Requested = true
    ↓
等待游戏状态准备
    ↓
Applied = true
```

如果运行环境未准备好：

```text
Requested = true
Applied = false
```

并允许自动重试。

---

## 13. 地图限制

视野功能可通过以下变量控制地图限制：

```cpp
FULL_MAP_VISION_MAP_LIMIT_ENABLED
```

当启用时，仅允许：

```cpp
FULL_MAP_VISION_MAPS
```

中列出的地图。

观察者模式、增强视野和普通全图视野必须统一遵守该限制。

---

## 14. 日志

所有重要操作必须记录日志：

```text
Hook 创建
Hook 启用
Hook 禁用
Patch 应用
Patch 恢复
PatternScan 失败
状态验证失败
```

禁止在高频 Hook 中持续输出日志。

高频日志应使用一次性标记，例如：

```cpp
InterlockedExchange
```

---

## 15. DLL 卸载

卸载流程必须保证：

```text
BeginDllUnload
↓
停止功能
↓
等待 Hook 回调
↓
Disable Hook
↓
恢复 Patch
↓
Shutdown Overlay
↓
FreeLibrary
```

禁止：

- 在 DllMain 中执行复杂清理
- 在 Hook 回调运行时卸载
- 在渲染线程仍使用 ImGui 时释放资源

---

## 16. 测试流程

修改 Hook 或卸载逻辑后必须测试：

```text
启动游戏
↓
注入 DLL
↓
开启功能
↓
关闭功能
↓
END 卸载
↓
重新注入
```

至少重复 5 次。

必须验证：

- 游戏不崩溃
- 游戏画面不卡死
- Hook 正确卸载
- DLL 可以重新注入
- 状态正确恢复

---

## 17. 禁止事项

禁止：

- 使用未经验证的固定地址
- 在 Hook 中 Sleep
- 在 Hook 中执行磁盘 IO
- 忽略指针有效性
- 修改代码但不提供恢复逻辑
- 在 DllMain 中初始化复杂系统
- 未测试重复注入

---

## 18. 文档要求

新增功能时应更新：

```text
docs/
```

文档应包含：

- 功能原理
- Hook 地址来源
- Pattern
- 数据结构
- 风险
- 卸载流程
- 测试结果

---

## 19. Agent 工作规则

AI Agent 修改项目时必须：

1. 先阅读相关代码
2. 理解 Hook 生命周期
3. 保持现有架构
4. 不进行无关重构
5. 修改后必须构建
6. 检查编译错误
7. 更新相关文档
8. 不删除已有功能

如果无法确认行为，应：

- 输出日志
- 保持功能禁用
- 避免猜测

---

## 20. 安全原则

所有功能必须遵守：

```text
可恢复
可禁用
可验证
可卸载
```

任何 Hook 或 Patch 如果无法安全恢复，应默认不启用。