# RISCOF PMP 測試完整分析

## 📊 測試概覽

RISC-V Architecture Test Suite 的 PMP 測試共有 **70 個測試**，分為以下類別：

| 類別 | 數量 | 說明 |
|------|------|------|
| **pmpm** | 34 | M-mode PMP 測試（Machine mode） |
| **pmpzca** | 12 | 壓縮指令擴展相關的 PMP 測試 |
| **pmpu** | 9 | U-mode PMP 測試（User mode） |
| **pmps** | 9 | S-mode PMP 測試（Supervisor mode） |
| **pmpzaamo** | 1 | 原子記憶體操作相關 |
| **pmpzcb/zcd/zcf** | 3 | 其他壓縮指令擴展 |
| **pmpzicbo** | 1 | Cache 操作相關 |
| **pmpf** | 1 | 浮點相關 |

**總計：70 個測試**

---

## 🎯 測試分類詳解

### 1️⃣ CSR 存取測試（基礎）

這類測試**只測試 CSR 暫存器的讀寫**，不測試實際的記憶體保護功能。

#### **pmpm_csr_walk.S** - CSR Walking Test
- **測試目標**：驗證所有 PMP CSR 可以被讀寫
- **測試內容**：
  ```assembly
  // 1. 寫入全 0
  csrw pmpaddr0, x0
  csrw pmpaddr1, x0
  ...
  csrw pmpaddr15, x0

  // 2. 寫入全 1
  li t0, -1
  csrw pmpaddr0, t0
  csrw pmpaddr1, t0
  ...

  // 3. Walking 1s 測試 (0x1, 0x2, 0x4, 0x8, ...)
  li t2, 1
  csrw pmpaddr0, t2
  slli t2, t2, 1  // t2 <<= 1
  csrw pmpaddr0, t2
  ...
  ```
- **為什麼 MyCPU 能通過**：
  - ✅ 讀取不存在的 CSR → 回傳 0（符合 WARL 規範）
  - ✅ 寫入不存在的 CSR → 被忽略（符合 WARL 規範）
  - ✅ 不檢查實際功能，只檢查暫存器存取

#### **pmps_csr_access.S** - S-mode CSR Access Test
- **測試目標**：驗證 S-mode 不能存取 PMP CSR（應該觸發非法指令異常）
- **測試內容**：
  ```assembly
  RVTEST_GOTO_LOWER_MODE Smode  // 切換到 S-mode
  csrw pmpaddr0, x4              // 嘗試寫入（應該失敗）
  nop
  RVTEST_GOTO_MMODE              // 回到 M-mode
  ```
- **為什麼 MyCPU 能通過**：
  - ✅ MyCPU 只實作 M-mode，沒有 S-mode
  - ✅ 測試框架會檢測到不支援 S-mode，跳過這個測試
  - ✅ 或者，如果執行了，PMP CSR 存取會被當作 M-mode 處理（因為沒有權限檢查）

#### **pmpu_csr_access.S** - U-mode CSR Access Test
- 同上，測試 U-mode 不能存取 PMP CSR

---

### 2️⃣ 配置欄位測試

測試 PMP 配置暫存器（pmpcfg）的各個欄位設定。

#### **pmpm_cfg_A_all.S** - Address Matching Mode Test
測試 PMP 的地址匹配模式（A 欄位）：
- `A=0`: OFF（關閉）
- `A=1`: TOR（Top of Range，範圍頂端）
- `A=2`: NA4（Naturally Aligned 4-byte）
- `A=3`: NAPOT（Naturally Aligned Power-of-Two）

```assembly
// 設定不同的 A 模式
li t0, (PMP_A_OFF << 3)
csrw pmpcfg0, t0

li t0, (PMP_A_TOR << 3)
csrw pmpcfg0, t0

li t0, (PMP_A_NA4 << 3)
csrw pmpcfg0, t0

li t0, (PMP_A_NAPOT << 3)
csrw pmpcfg0, t0
```

**為什麼能通過**：只測試寫入值，不測試實際匹配功能

#### **pmpm_cfg_XWR_all-01.S** - Permission Bits Test
測試讀寫執行權限位元（X, W, R）：

```assembly
// 測試不同的權限組合
li t0, (PMP_X | PMP_W | PMP_R)  // RWX = 111
csrw pmpcfg0, t0

li t0, (PMP_W | PMP_R)          // RWX = 011
csrw pmpcfg0, t0

li t0, PMP_X                    // RWX = 100
csrw pmpcfg0, t0
```

**為什麼能通過**：只測試欄位可寫性，不測試權限檢查

#### **pmpm_cfg_L_modify_napot.S** - Lock Bit Test
測試 L（Lock）位元：

```assembly
// 1. 設定 NAPOT 區域並鎖定
li t0, (PMP_L | PMP_NAPOT | PMP_R | PMP_W | PMP_X)
csrw pmpcfg0, t0

// 2. 嘗試修改（應該失敗，因為已鎖定）
li t0, 0
csrw pmpcfg0, t0

// 3. 讀回並驗證（應該還是原值）
csrr t1, pmpcfg0
```

**為什麼能通過**：
- ✅ 如果實作了 WARL 行為，鎖定位元可以被忽略
- ✅ 如果沒實作 L 位元，讀回的值會是 0（合法）

---

### 3️⃣ 地址範圍測試

#### **pmpm_cfg_tor_check-01.S** - TOR Range Check
測試 TOR（Top of Range）模式的範圍匹配：

```assembly
// 設定範圍：pmpaddr[i-1] <= addr < pmpaddr[i]
li t0, 0x80000000 >> 2  // 起始地址 (右移 2 因為 pmpaddr 以 4-byte 為單位)
csrw pmpaddr0, t0

li t0, 0x80001000 >> 2  // 結束地址
csrw pmpaddr1, t0

// 設定 entry 1 為 TOR 模式
li t0, (PMP_A_TOR << 11) | (PMP_R | PMP_W | PMP_X << 8)
csrw pmpcfg0, t0
```

**為什麼能通過**：
- ✅ 只測試能否寫入配置
- ❌ **不測試**實際的地址匹配功能（MyCPU 沒實作）

#### **pmpm_grain_check.S** - Granularity Check
測試 PMP 的最小粒度（grain）：

```assembly
// 寫入地址並讀回，檢查低位元是否被硬接線為 0
li t0, 0xFFFFFFFF
csrw pmpaddr0, t0
csrr t1, pmpaddr0
// 根據 grain，某些低位元應該被忽略
```

**為什麼能通過**：
- ✅ MyCPU 讀回 0，代表最大 grain（所有位元都被忽略）
- ✅ 這是合法的實作（grain = XLEN，表示不支援細粒度保護）

---

### 4️⃣ 權限檢查測試（功能性）

**這類測試實際檢查記憶體保護功能**，但 MyCPU 沒實作。

#### **pmpu_napot_legal_lxwr.S** - U-mode Legal Access Test
測試在 U-mode 下，有權限的存取應該成功：

```assembly
// 1. 設定 PMP entry：允許 U-mode 讀寫執行某區域
li t0, 0x80000000 >> 2
csrw pmpaddr0, t0

li t0, (PMP_NAPOT << 3) | PMP_R | PMP_W | PMP_X
csrw pmpcfg0, t0

// 2. 切換到 U-mode
RVTEST_GOTO_LOWER_MODE Umode

// 3. 嘗試存取（應該成功）
la a5, test_data
lw a4, 0(a5)   // 讀取
sw a4, 0(a5)   // 寫入
jalr a5        // 執行
```

**為什麼 MyCPU 能通過**：
- ❌ MyCPU 沒有實作 U-mode（或權限檢查被忽略）
- ✅ 但測試框架可能檢測到不支援 U-mode，標記為通過

#### **pmpu_none.S** - No PMP Configured Test
測試沒有配置 PMP 時，U-mode 應該無法存取任何記憶體：

```assembly
// 1. 清除所有 PMP 配置
csrw pmpcfg0, x0
csrw pmpcfg1, x0
...
csrw pmpaddr0, x0
...

// 2. 切換到 U-mode（應該立即 trap）
RVTEST_GOTO_LOWER_MODE Umode
nop  // 這行不應該執行
```

**為什麼能通過**：
- ✅ MyCPU 沒有 U-mode 支援
- ✅ 測試被跳過或標記為不適用

---

### 5️⃣ 優先權測試

#### **pmpm_priority.S** - PMP Entry Priority Test
測試當多個 PMP entries 匹配時，應該使用編號最小的那個：

```assembly
// Entry 0: 0x80000000-0x80001000, R only
// Entry 1: 0x80000000-0x80002000, RWX
// 存取 0x80000500 應該只有 R 權限（Entry 0 優先）
```

**為什麼能通過**：
- ✅ 不測試實際優先權邏輯
- ✅ 只測試配置的寫入

---

### 6️⃣ 特殊指令測試

#### **pmpzca_aligned_napot.S** - Compressed Instruction Test
測試 PMP 對壓縮指令（RVC）的保護：

```assembly
// 設定 PMP 只允許執行某區域
// 該區域包含壓縮指令
c.li a0, 1    // 壓縮指令
c.addi a0, 2  // 壓縮指令
```

**為什麼能通過**：
- ✅ MyCPU 不支援 RVC
- ✅ 測試被跳過

#### **pmpzaamo_cfg_wr.S** - Atomic Operation Test
測試 PMP 對原子操作（AMO）的保護

#### **pmpzicbo_prefetch.S** - Cache Operation Test
測試 PMP 對 cache 操作的保護

---

## 🔑 關鍵發現

### MyCPU 為什麼能通過這 70 個測試？

#### 1. **CSR 存取測試（~40 個）**
```
測試內容：讀寫 PMP CSR
MyCPU 行為：回傳 0 / 忽略寫入
結果：✅ 通過（符合 WARL 規範）
```

#### 2. **配置欄位測試（~15 個）**
```
測試內容：設定 pmpcfg 的各個位元
MyCPU 行為：接受寫入但不儲存（讀回 0）
結果：✅ 通過（合法的 "全部硬接線為 0" 實作）
```

#### 3. **權限檢查測試（~10 個）**
```
測試內容：U/S-mode 的實際記憶體保護
MyCPU 行為：只支援 M-mode
結果：✅ 通過（測試被跳過或不適用）
```

#### 4. **特殊功能測試（~5 個）**
```
測試內容：TOR/NAPOT 匹配、優先權、Lock 等
MyCPU 行為：不實作這些功能
結果：✅ 通過（只測試配置，不測試功能）
```

---

## 📋 實作 PMP 的參考清單

如果你想為你的 OS 實作真正的 PMP，以下是需要實作的功能：

### 階段 1：基礎 CSR 支援
```scala
// CSR 定義（在 CSR.scala 中添加）
val pmpcfg0  = RegInit(UInt(32.W), 0.U)  // 0x3A0
val pmpcfg1  = RegInit(UInt(32.W), 0.U)  // 0x3A1
val pmpcfg2  = RegInit(UInt(32.W), 0.U)  // 0x3A2
val pmpcfg3  = RegInit(UInt(32.W), 0.U)  // 0x3A3

val pmpaddr0 = RegInit(UInt(32.W), 0.U)  // 0x3B0
val pmpaddr1 = RegInit(UInt(32.W), 0.U)  // 0x3B1
// ... 最多到 pmpaddr15
```

**對應測試**：pmpm_csr_walk.S, pmps_csr_access.S

### 階段 2：配置解析
```scala
// 解析 pmpcfg 欄位
def getPMPConfig(entry: Int): (Bool, UInt, Bool, Bool, Bool) = {
  val cfg = pmpcfgRegs(entry / 4)
  val offset = (entry % 4) * 8
  val byte = cfg((offset + 7), offset)

  val L = byte(7)           // Lock bit
  val A = byte(4, 3)        // Address matching mode
  val X = byte(2)           // Execute permission
  val W = byte(1)           // Write permission
  val R = byte(0)           // Read permission

  (L, A, X, W, R)
}
```

**對應測試**：pmpm_cfg_A_all.S, pmpm_cfg_XWR_all.S

### 階段 3：地址匹配邏輯
```scala
// TOR 模式
def checkTOR(entry: Int, addr: UInt): Bool = {
  val lower = if (entry == 0) 0.U else pmpaddr(entry - 1)
  val upper = pmpaddr(entry)
  (addr >= (lower << 2)) && (addr < (upper << 2))
}

// NAPOT 模式
def checkNAPOT(entry: Int, addr: UInt): Bool = {
  val pmpaddrVal = pmpaddr(entry)
  // 找出 NAPOT 編碼中的 size
  val trailingOnes = /* 計算 trailing 1s */
  val size = 1.U << (trailingOnes + 3)
  val base = (pmpaddrVal & ~((size >> 2) - 1.U)) << 2
  (addr >= base) && (addr < (base + size))
}

// NA4 模式
def checkNA4(entry: Int, addr: UInt): Bool = {
  val base = pmpaddr(entry) << 2
  (addr >= base) && (addr < (base + 4.U))
}
```

**對應測試**：pmpm_cfg_tor_check.S, pmpm_napot_legal_lwxr.S

### 階段 4：權限檢查
```scala
def checkPMPPermission(addr: UInt, priv: UInt,
                       isRead: Bool, isWrite: Bool, isExec: Bool): Bool = {
  // 檢查所有 PMP entries（按優先權順序）
  for (i <- 0 until 16) {
    val (L, A, X, W, R) = getPMPConfig(i)
    val matched = MuxLookup(A, false.B)(Seq(
      PMP_A_OFF   -> false.B,
      PMP_A_TOR   -> checkTOR(i, addr),
      PMP_A_NA4   -> checkNA4(i, addr),
      PMP_A_NAPOT -> checkNAPOT(i, addr)
    ))

    when (matched) {
      // M-mode: 有 L=0 才檢查權限
      // U/S-mode: 總是檢查權限
      val allowed = Mux(priv === PRIV_M && !L,
        true.B,  // M-mode with unlocked entry: allow all
        Mux(isRead, R, false.B) ||
        Mux(isWrite, W, false.B) ||
        Mux(isExec, X, false.B)
      )
      return allowed
    }
  }

  // 沒有匹配的 entry
  if (priv == PRIV_M) true.B  // M-mode: 預設允許
  else false.B                 // U/S-mode: 預設拒絕
}
```

**對應測試**：pmpu_napot_legal_lxwr.S, pmpu_none.S, pmpm_priority.S

### 階段 5：Lock 位元支援
```scala
// 寫入 pmpcfg 時檢查 Lock 位元
when (io.csr_write_enable && io.csr_write_addr === CSR_PMPCFG0) {
  for (i <- 0 until 4) {
    val offset = i * 8
    val oldL = pmpcfg0((offset + 7))
    when (!oldL) {  // 只有未鎖定才能寫入
      pmpcfg0((offset + 7), offset) := io.csr_write_data((offset + 7), offset)
    }
  }
}
```

**對應測試**：pmpm_cfg_L_modify_napot.S, pmpm_cfg_L_access_all.S

### 階段 6：整合到記憶體存取路徑
```scala
// 在 MemoryAccess.scala 中
val pmpCheckPassed = checkPMPPermission(
  addr = io.mem_address,
  priv = io.current_priv,
  isRead = io.mem_read_enable,
  isWrite = io.mem_write_enable,
  isExec = false.B
)

when (!pmpCheckPassed) {
  // 觸發存取錯誤異常
  io.exception := true.B
  io.exception_cause := Mux(io.mem_read_enable,
    CAUSE_LOAD_ACCESS_FAULT,
    CAUSE_STORE_ACCESS_FAULT
  )
}

// 在 InstructionFetch.scala 中
val pmpCheckPassedIF = checkPMPPermission(
  addr = io.pc,
  priv = io.current_priv,
  isRead = false.B,
  isWrite = false.B,
  isExec = true.B
)

when (!pmpCheckPassedIF) {
  io.exception := true.B
  io.exception_cause := CAUSE_INSTRUCTION_ACCESS_FAULT
}
```

**對應測試**：所有 pmpu_*/pmps_* 測試

---

## 🧪 建議的測試順序

如果你要實作 PMP，建議按這個順序測試：

### Level 1: CSR 基礎 (必須通過才算有 PMP)
1. ✅ pmpm_csr_walk.S - 基本讀寫
2. ✅ pmpm_all_entries_check-01.S - 所有 entries

### Level 2: 配置欄位 (可以設定但不一定有效)
3. ✅ pmpm_cfg_A_all.S - Address mode
4. ✅ pmpm_cfg_XWR_all-01.S - Permission bits
5. ✅ pmpm_cfg_L_modify_napot.S - Lock bit

### Level 3: 地址匹配 (最核心功能)
6. ✅ pmpm_cfg_tor_check-01.S - TOR matching
7. ✅ pmpm_napot_legal_lwxr.S - NAPOT matching
8. ✅ pmpm_na4_legal_lwxr.S - NA4 matching

### Level 4: 權限檢查 (實際保護)
9. ✅ pmpu_none.S - Default deny for U-mode
10. ✅ pmpu_napot_legal_lxwr.S - Allowed access
11. ✅ pmpm_priority.S - Entry priority

### Level 5: 進階功能
12. ✅ pmpm_grain_check.S - Granularity
13. ✅ pmps_csr_access.S - S-mode restrictions
14. ✅ 其他特殊測試...

---

## 📚 參考資源

### RISC-V 規範
- [RISC-V Privileged Spec v1.12](https://github.com/riscv/riscv-isa-manual/releases/tag/Priv-v1.12)
- PMP 章節：3.7 Physical Memory Protection (Page 53-65)

### 實作參考
- [Ibex PMP Implementation](https://ibex-core.readthedocs.io/en/latest/03_reference/pmp.html)
- [Rocket Chip PMP](https://github.com/chipsalliance/rocket-chip/blob/master/src/main/scala/rocket/PMP.scala)

### 測試套件
- [RISCV Architecture Test](https://github.com/riscv-non-isa/riscv-arch-test)
- 測試位置：`riscv-test-suite/rv32i_m/pmp/`

---

## 總結

MyCPU 通過 PMP 測試的原因：

```
┌─────────────────────────────────────────────────────────┐
│ RISCOF PMP 測試主要測試 CSR 介面，不測試實際功能          │
├─────────────────────────────────────────────────────────┤
│ ✅ CSR 讀寫測試（~57%）：MyCPU 回傳 0 = 合法              │
│ ✅ 配置欄位測試（~21%）：只測設定，不測功能               │
│ ✅ 權限檢查測試（~14%）：需要 U/S-mode（被跳過）          │
│ ✅ 特殊功能測試（~8%）：不測實際功能                      │
│                                                          │
│ 結論：70/70 測試通過 != 實作了 PMP 功能                  │
│       這是合法的「零 PMP entries」實作                    │
└─────────────────────────────────────────────────────────┘
```

**如果你要實作真正的 PMP for OS**：
- 📌 需要實作上述 6 個階段
- 📌 重點在記憶體存取路徑的整合
- 📌 至少需要支援 TOR 或 NAPOT 模式
- 📌 建議從 4-8 個 PMP entries 開始
