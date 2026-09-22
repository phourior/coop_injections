# SC2 大厅地图信息扫描分析

> **当前实现验证版本**: Base97579  
> **映像基址**: `0x140000000`

---

## 一、当前数据流

大厅信息现在只有一个采集入口：`OnGameLobbyUpdate` Hook。原函数传入的 `a1`
是当前 `GameLobby*`，Hook 在原函数执行完成后直接读取该对象并发布快照：

```text
OnGameLobbyUpdate(a1 = GameLobby*)
  -> SafeCollectData(a1)
  -> g_rawLobby（SRWLOCK 保护）
  -> SnapshotLobbyData()
  -> GUI 左侧地图名

SafeCollectData(a1)
  -> UpdateFullMapVisionMapState(mapPath, mapDesc)
  -> 全图视野地图匹配与补丁状态
```

`GameLobby` 中当前使用的字段：

| 偏移 | 当前用途 | 说明 |
|------|----------|------|
| `+0x08` | `mapPath` | 实际地图标题或路径，例如 `[MM] 往曰神庙Temple of the Past - Protoss` |
| `+0x18` | `mapDesc` | 部分地图为显示标识，部分自定义地图为完整任务描述 |
| `+0x48` | `gameParam1` | 大厅参数 |
| `+0x4C` | `gameParam2` | 大厅参数 |
| `+0x1C10` | `playerIds[16]` | 玩家槽数据；部分 `[MM]` 大厅内也可能全部为零 |
| `+0x1EC8` | `lobbyFlags` | 大厅内非零；离开 Lobby 或开始进入游戏时都可能归零，不能单独判定离场 |

GUI 左侧只显示 `mapPath`。不能优先显示 `mapDesc`，因为“往曰神庙”等自定义
地图的 `mapDesc` 是“萨古拉斯星球上的萨尔那加神庙正在遭受攻击……”这类任务
描述，而不是房间或地图标题。

---

## 二、`CBattleNet` 全局指针定位

`ScanCBattleNetGlobal()` 仍然保留，但不会读取地图字符串。它的用途是为
`ReverseScanLobbyPointer()` 提供 `CBattleNet*`，从而反向发现当前大厅字段并
支持离场检测。

### 2.1 扫描原理

`CBattleNet::Initialize()` 会把构造好的 `CBattleNet*` 存入全局变量，同时以
`LEA R8,[rip+disp32]` 引用字符串 `"CBattleNet::Initialize()"`。

扫描步骤：

1. 在模块内存中搜索 `"CBattleNet::Initialize()\0"`。
2. 找到引用该字符串的 `LEA R8,[RIP+disp32]`，字节前缀为 `4C 8D 05`。
3. 在该指令之前 `0x100` 字节内寻找最后一条
   `MOV [RIP+disp32],RSI`，字节前缀为 `48 89 35`。
4. 计算全局槽地址：`globalSlot = instructionEnd + disp32`。
5. 解引用全局槽得到 `CBattleNet*`。

扫描结果在 `ScanCBattleNetGlobal()` 的函数内静态缓存，避免渲染期间重复扫描
模块镜像。

### 2.2 Base96516 验证数据

| 项目 | 虚拟地址 | RVA | 原始字节 |
|------|----------|-----|----------|
| `"CBattleNet::Initialize()"` | `0x142D4E510` | `0x2D4E510` | - |
| `LEA R8,[RIP+0xCF391C]` | `0x14205ABED` | `0x205ABED` | `4C 8D 05 1C 39 CF 00` |
| 保存 `CBattleNet*` 的 `MOV` | `0x14205AB9B` | `0x205AB9B` | `48 89 35 76 C4 E3 03` |
| `g_CBattleNet` 全局槽 | `0x145E97018` | `0x5E97018` | - |

计算校验：

```text
0x14205ABED + 7 + 0x00CF391C = 0x142D4E510
0x14205AB9B + 7 + 0x03E3C476 = 0x145E97018
```

---

## 三、当前大厅字段反向扫描

### 3.1 为什么从 Hook 反查

`OnGameLobbyUpdate` 的 `a1` 是事件实际使用的 `GameLobby*`，比从类成员布局正向
猜测当前大厅对象更可靠。`ReverseScanLobbyPointer()` 以它为已知目标：

1. 通过 `ScanCBattleNetGlobal()` 得到全局槽。
2. 解引用得到 `CBattleNet*`。
3. 按 8 字节对齐扫描 `CBattleNet` 前 `0xAA0` 字节。
4. 寻找满足 `(memberValue & ~1) == gameLobby` 的直接字段。
5. 唯一直接命中时记录该字段的运行时地址，供日志和后续版本分析使用。

Base97579 的运行时记录中，直接字段位于 `CBattleNet+0x60`。代码没有硬编码
这个偏移，而是每次 DLL 生命周期由 Hook 提供的真实对象动态确认。这样客户端
更新导致类布局变化时，不会继续盲读旧位置。

扫描还会记录间接命中，供 IDA 交叉引用分析；间接命中可能来自静态节点或其他
对象内部引用，不参与当前大厅字段的选择。

### 3.2 日志

扫描结果写入 DLL 目录下的 `lobby_offsets.log`。目录不可写时回退到：

```text
%TEMP%\Dll1_lobby_offsets.log
```

关键日志示例：

```text
[LOBBY-SCAN] DIRECT field=CBattleNet+0x60 raw=0x...
[LOBBY-SCAN] current lobby field=0x...
[LOBBY-SCAN] complete: direct=1 indirect=...
```

如果直接命中不是唯一值，只记录诊断结果，不采用不确定候选。

---

## 四、SC2 字符串读取

`GameLobby+0x08` 和 `GameLobby+0x18` 都按 `BSN::String` 布局读取：

```text
+0x00  DWORD  length
+0x04  DWORD  cap_flags（bit1=1 表示堆分配）
+0x08  内联字符数据或堆指针
```

`ReadSc2StringRaw()` 使用 `SafeMemcpy()` 和 SEH 保护：

```cpp
if (capFlags & 2)
    dataAddress = ReadMemory<uintptr_t>(stringObject + 0x08);
else
    dataAddress = stringObject + 0x08;
```

读取长度必须非零且小于目标缓冲区容量，复制完成后由 DLL 补写终止 `NUL`。
原始对象的 `length` 在个别自定义地图中可能大于实际标题长度并包含尾部无效数据，
但有效标题位于首个 `NUL` 之前，当前地图匹配和 GUI 显示均按 C 字符串边界处理。

---

## 五、大堂快照与全图视野

SC2 离开 Lobby 后可能继续保留原 `GameLobby*` 和地图标题，因此不能仅用指针
是否变化判断离场。当前实测结果：

| 状态 | `GameLobby*` | 标题 | `playerIds` | `lobbyFlags` |
|------|--------------|------|-------------|--------------|
| `[MM]` Lobby 内 | 可能保持不变 | 有效 | 可能全零 | 非零，观测值 `0x63` |
| 离开 Lobby 后 | 仍可能保持不变 | 旧值仍在 | 全零 | `0` |
| 开始进入游戏 | 仍可能保持不变 | 旧值仍在 | 可能全零 | 也可能为 `0` |

由于 `lobbyFlags == 0` 无法区分“离开 Lobby”和“开始游戏”，当前实现不再主动
轮询或清空已发布快照。GUI 左侧保留最近一次 `OnGameLobbyUpdate` 发布的房间名，
全图视野也保留当前地图状态，避免读图时被误恢复。

后续 `OnGameLobbyUpdate` 发布新地图时会替换快照，并重新驱动全图视野地图状态。
按 END 卸载 DLL 时，`CleanupHooks()` 仍会恢复直接代码补丁并清理 Hook。

---

## 六、版本升级验证清单

### 6.1 Hook 与字段

- [ ] `ON_GAME_LOBBY_UPDATE_PATTERN` 在新镜像中唯一命中函数入口。
- [ ] Hook 的 `a1` 仍是有效 `GameLobby*`。
- [ ] `GameLobby+0x08` 仍保存地图标题或路径。
- [ ] `GameLobby+0x18` 的实际语义重新抽样，不假定它总是显示名。
- [ ] `GameLobby+0x1EC8` 在 Lobby 内非零；离场和开始游戏归零均只作为诊断。
- [ ] 自定义地图内 `playerIds` 是否填充只作为诊断信息。

### 6.2 全局扫描与反向扫描

- [ ] `"CBattleNet::Initialize()"` 字符串仍然存在。
- [ ] `LEA R8,[rip+...]` 和保存全局对象的 `MOV [rip+...],RSI` 仍符合扫描模板。
- [ ] 反向扫描恰好得到一个直接 `GameLobby*` 字段。
- [ ] `lobby_offsets.log` 中出现 `current lobby field`。
- [ ] 开始游戏后 GUI 房间名和全图补丁状态保持不变。

若直接字段无法唯一解析，应重新分析 IDA xref，不能把某次运行中观察到的偏移
直接固化到代码。

---

## 七、稳定性评估

| 特征或字段 | 稳定性 | 处理方式 |
|------------|--------|----------|
| `"CBattleNet::Initialize()"` | 高 | 特征码定位全局槽 |
| `OnGameLobbyUpdate` 函数特征 | 中 | 唯一 pattern 命中，否则不安装 Hook |
| `CBattleNet` 当前大厅字段 | 中 | 从 Hook 的真实 `GameLobby*` 动态反查 |
| `GameLobby+0x08/+0x18` | 中 | 每版本抽样验证字段语义 |
| `GameLobby+0x1EC8` | 中 | 仅作诊断，不用于区分离场和开始游戏 |
| `BSN::String` 布局 | 高 | 长度校验、SEH 安全读取 |

---

## 八、相关代码文件

| 文件 | 功能 |
|------|------|
| `Dll1/core/game_data.cpp` | `ScanCBattleNetGlobal()` 特征扫描和扫描预热 |
| `Dll1/core/game_data.h` | 游戏数据扫描接口 |
| `Dll1/core/memory.h` | `ReadMemory<T>()`、`SafeMemcpy()` 和 `PatternScan()` |
| `Dll1/hooks/game_hook.cpp` | 大厅 Hook、快照与反向字段扫描 |
| `Dll1/hooks/game_hook_leader_panel.cpp` | 左上角统计面板可见性与热键过滤 |
| `Dll1/hooks/game_hook_vision.cpp` | 全图视野状态机与模式调度 |
| `Dll1/hooks/game_hook_observer_vision.cpp` | 观察者视野 / Fog Hook 与增强视野底层实现 |
| `Dll1/hooks/game_hook.h` | `RawLobbyData`、`LobbyData` 和公开状态接口 |
| `Dll1/render/menu.cpp` | 从 `SnapshotLobbyData()` 显示 `mapPath` |
| `Dll1/render/overlay.cpp` | 提供渲染帧和 150 ms 游戏数据缓存调度入口 |