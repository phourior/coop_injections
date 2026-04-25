# SC2 大厅地图信息特征码扫描分析

> **分析版本**: Base96516 (`SC2_x64.exe`)  
> **IDA 数据库**: `D:\StarCraft II\Versions\Base96516\SC2_x64.exe`  
> **MD5**: `fc18e16d19aaab7f17ee27970217f0c4`  
> **SHA256**: `e897c6fda71662e9d762d085cd57ab553763ac73f664741668cca92abc25b413`  
> **CRC32**: `0x98a9b38f`  
> **映像基址**: `0x140000000`  
> **映像大小**: `0x8202000`

---

## 一、目标：读取大厅地图信息

游戏在内存中维护一个 `CBattleNet` 单例，通过其内部事件链节点可访问当前大厅的地图路径和显示名：

```
CBattleNet* g_CBattleNet          ← 全局指针（静态地址，每版本变化）
  +0x5A8 → EventChainNode*        ← 事件链头节点
    +0x08 & ~1ULL → GameLobby*    ← 最低位为内部标志，需屏蔽
      +0x08 → BSN::String          ← 地图路径（如 "Maps/Ladder/..."）
      +0x18 → BSN::String          ← 地图显示名（如 "Lost Temple"）
```

---

## 二、全局指针定位

### 2.1 硬编码地址（仅 Base96516 有效）

| 项目 | 地址 |
|------|------|
| `g_CBattleNet` 虚拟地址 | `0x145E97018` |
| `g_CBattleNet` RVA | `0x5E97018` |

### 2.2 特征码扫描方法（版本无关）

**原理**：`CBattleNet::Initialize()` 函数会将构造好的 `CBattleNet*` 存入全局变量，且同一函数中会以 `LEA R8,[rip+disp32]` 引用函数名字符串 `"CBattleNet::Initialize()"` 用于日志/断言。

**扫描步骤**：

1. 在模块内存中搜索字节串 `"CBattleNet::Initialize()\0"`（25 字节含终止符）
2. 找到 `LEA R8,[RIP+disp32]`（字节前缀 `4C 8D 05`）引用该字符串的指令
3. 在该 `LEA` 指令之前 `0x100` 字节范围内，取**最后一条** `MOV [RIP+disp32],RSI`（字节前缀 `48 89 35`）
4. 从该 `MOV` 指令提取 RIP 相对偏移：`globalAddr = (instrVA + 7) + disp32`

**Base96516 验证数据**：

| 指令 | 虚拟地址 | RVA | 原始字节 |
|------|----------|-----|---------|
| `"CBattleNet::Initialize()"` 字符串 | `0x142D4E510` | `0x2D4E510` | — |
| `LEA R8,[RIP+0xCF391C]` | `0x14205ABED` | `0x205ABED` | `4C 8D 05 1C 39 CF 00` |
| `MOV [RIP+0x3E3C476],RSI`（存储 CBattleNet*） | `0x14205AB9B` | `0x205AB9B` | `48 89 35 76 C4 E3 03` |
| `MOV [RIP+0x3E3C436],RBX`（失败路径，清空指针） | `0x14205ABDB` | `0x205ABDB` | `48 89 1D 36 C4 E3 03` |

- `LEA` 计算：`0x14205ABED + 7 + 0x00CF391C = 0x142D4E510` ✓  
- `MOV` 计算：`0x14205AB9B + 7 + 0x03E3C476 = 0x145E97018` ✓  
- `MOV` → `LEA` 距离：`0x52`（82 字节），中间 19 条指令  
- 扫描窗口 `0x100` 字节足够（距离仅 0x52），且取**最后一条** `48 89 35` 可绕过失败路径的 `48 89 1D`（不同后缀字节，不会干扰）

**所在函数**：`sub_14205A9F0`（即 `CBattleNet::Initialize()`），VA `0x14205A9F0`，RVA `0x205A9F0`。

---

## 三、偏移链分析

### 3.1 `+0x5A8` — 事件链节点

**确认方式**：

- `CBattleNet` 构造函数 `sub_140512750` 对 `[rdi+0x5A8]` 赋零初始化
- 子函数 `sub_14051F820` 调用 `sub_1404FE6F0(a1 + 1448)`（十进制 1448 = 十六进制 0x5A8）
- `sub_1404FE6F0` 是事件链节点构造函数，依次将 `*(_OWORD*)a1`、`*(a1+8)`、`*(a1+16)` 等清零

**结论**：`CBattleNet` 在偏移 `0x5A8` 处嵌入一个 `EventChainNode`（侵入式链表节点），节点大小至少 32 字节。

### 3.2 `+0x08 & ~1ULL` — GameLobby 指针（含标志位）

**结构**：`EventChainNode+0x08` 存储 `GameLobby*`，但最低位 (bit0) 被用作内部状态标志（SC2 侵入式指针/链表惯用模式）。读取时须屏蔽：

```cpp
uintptr_t raw = ReadMemory<uintptr_t>(pEvtNode + 0x08);
uintptr_t pLobby = raw & ~1ULL;
```

**确认方式**：

搜索 `.text` 段中 `AND reg, 0FFFFFFFFFFFFFFFEh`（即 `AND ~1`）模式，发现大量函数（`sub_14010be40`、`sub_14010bee0`、`sub_14010c270` 等）使用如下惯用序列：

```asm
mov  rXX, [rYY]          ; 读取链表节点中的指针
and  rXX, 0FFFFFFFFFFFFFFFEh  ; 屏蔽最低位
jz   short null_check    ; 判空
cmp  [rXX+18h], rZZ      ; 访问目标对象字段
```

这是 SC2 中广泛使用的侵入式链表/事件链遍历范式。

### 3.3 `GameLobby+0x08` 和 `+0x18` — 字符串字段

两个字段均为 `BSN::String` 对象（SC2 内部字符串类），内存布局：

```
+0x00  DWORD  length
+0x04  DWORD  cap_flags   (bit1=1 表示堆分配)
+0x08  char[] 内联缓冲区 (len≤容量阈值时有效) 或堆指针 (len>阈值时)
```

读取函数 `ReadSc2String`（`memory.h`）已实现：

```cpp
if (cap_flags & 2)
    dataAddr = ReadMemory<uintptr_t>(strObj + 0x08);  // 堆分配
else
    dataAddr = strObj + 0x08;                          // 内联
```

| 偏移 | 含义 | 示例值 |
|------|------|--------|
| `GameLobby+0x08` | 地图路径/句柄 | `"Maps/Ladder/Equilibrium513.SC2Map"` |
| `GameLobby+0x18` | 地图显示名 | `"Equilibrium"` |

**偏移间距**：两字段相差 `0x10`（16 字节），正好是一个 `BSN::String` 对象的固定头部大小（`length`+`cap_flags`+8字节指针/内联缓冲区起始）。

---

## 四、现有实现（game_data.cpp）

### 4.1 `ScanCBattleNetGlobal()`

已按上述算法实现，关键逻辑：

```cpp
// 步骤1：找字符串
static const char kStr[] = "CBattleNet::Initialize()";
// 步骤2：找 LEA R8,[rip+disp32]  (4C 8D 05 ...)
if (base[i] != 0x4C || base[i+1] != 0x8D || base[i+2] != 0x05) continue;
int32_t disp = *reinterpret_cast<const int32_t*>(base + i + 3);
if ((base + i + 7) + disp != strAddr) continue;
// 步骤3：在前 0x100 字节找最后一条 MOV [RIP+disp32],RSI (48 89 35 ...)
for (j = lo; j + 7 <= i; ++j)
    if (base[j]==0x48 && base[j+1]==0x89 && base[j+2]==0x35)
        movIdx = j;  // 取最后一个
// 步骤4：计算全局地址
int32_t ripDisp = *reinterpret_cast<const int32_t*>(base + movIdx + 3);
return (base + movIdx + 7) + ripDisp;
```

### 4.2 `ReadLobbyInfo()`

```cpp
LobbyInfo info{};
static uintptr_t sCBattleNetGlobal = ScanCBattleNetGlobal();
if (!sCBattleNetGlobal) return info;

info.pBattleNet = ReadMemory<uintptr_t>(sCBattleNetGlobal);   // CBattleNet*
info.pEvtNode   = ReadMemory<uintptr_t>(info.pBattleNet + 0x5A8); // 事件链节点
uintptr_t raw   = ReadMemory<uintptr_t>(info.pEvtNode + 0x08);
info.pLobby     = raw & ~1ULL;                                 // GameLobby*

info.mapPath    = ReadSc2String(info.pLobby + 0x08);
info.mapName    = ReadSc2String(info.pLobby + 0x18);
info.valid      = true;
```

---

## 五、新版本验证清单

升级到新版本后，在 IDA Pro 中执行以下检查：

### 5.1 特征码扫描验证

- [ ] 搜索字符串 `"CBattleNet::Initialize()"` — 必须仍然存在且唯一
- [ ] 确认字符串仍被 `LEA R8,[rip+...]`（`4C 8D 05`）引用
- [ ] 确认同函数内仍存在 `MOV [rip+...],RSI`（`48 89 35`）
- [ ] 计算新的全局地址 RVA，与代码读取结果对比

### 5.2 偏移链验证

- [ ] 在 `CBattleNet` 构造函数中确认事件链节点仍在 `+0x5A8`（十进制 1448）
  - 查找 `sub_1404FE6F0(a1 + 1448)` 或类似调用
- [ ] 确认 `EventChainNode+0x08` 仍存储 `GameLobby*`（含最低位标志）
  - 查找访问 `[node+8]` 后紧跟 `AND reg, FFFFFFFFFFFFFFFEh` 的代码
- [ ] 确认 `GameLobby+0x08` 和 `+0x18` 仍为两个相邻字符串字段
  - 在已知 GameLobby 地址处检查两字段的字符串长度和内容是否合理

### 5.3 若偏移发生变化

若 `+0x5A8` / `+0x08` / `+0x08` / `+0x18` 中任一偏移变化：

1. 找到 `CBattleNet::Initialize()` 函数（通过字符串 xref 已定位）
2. 搜索 `sub_1404FE6F0(a1 + ???)` 调用，提取新的事件链偏移
3. 在网络大厅处理函数中（搜索 `"S2GameLobby"` 或 `"mapFile"` 字符串）查找字段访问偏移
4. 更新 `ReadLobbyInfo()` 中的三处硬编码偏移

---

## 六、稳定性评估

| 偏移/特征 | 稳定性 | 理由 |
|-----------|--------|------|
| 字符串 `"CBattleNet::Initialize()"` | **高** | 名称字符串用于日志，不轻易改变 |
| `LEA R8,[rip+str]` + `MOV [rip+g],RSI` 模式 | **高** | 编译器生成的标准全局初始化模式 |
| `CBattleNet` 偏移 `+0x5A8` | **中** | 类成员布局版本间可能变化 |
| `EventChainNode+0x08` 指针位置 | **中** | 侵入式链表节点标准布局 |
| `GameLobby+0x08/+0x18` 字符串字段 | **中** | Protobuf 消息字段布局相对稳定 |
| `BSN::String` 内存布局 | **高** | SC2 内部容器类型，基本不变 |

---

## 七、相关代码文件

| 文件 | 功能 |
|------|------|
| `Dll1/core/game_data.cpp` | `ScanCBattleNetGlobal()` + `ReadLobbyInfo()` 实现 |
| `Dll1/core/game_data.h` | `LobbyInfo` 结构体定义 |
| `Dll1/core/memory.h` | `ReadMemory<T>()` + `ReadSc2String()` + `PatternScan()` 工具函数 |
