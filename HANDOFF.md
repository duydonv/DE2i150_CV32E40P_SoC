# HANDOFF — DE2i-150 + CV32E40P bring-up

> Tài liệu bàn giao trạng thái dự án để bất kỳ agent / developer mới nào
> cũng có thể tiếp tục làm việc ngay. Cập nhật lần cuối: **17/05/2026**.

## 1. Mục tiêu cuối cùng

Triển khai core RISC-V **CV32E40P** (OpenHW Group) lên kit **Terasic DE2i-150**
(Cyclone IV GX `EP4CGX150DF31C7`) để làm nền tảng cho:

- 6 lệnh mở rộng phục vụ AI lượng tử hoá: `MAC`, `Dot4_acc`, `Relu`,
  `Clamp`, `lp.setup`, `p.lw`.
- Kernel INT8 demo cuối cùng để đo cycle-accurate trên kit.

Sau đó sẽ triển khai mô hình AI có sẵn lên kit để đánh giá thực tế. Tích
hợp TFLM và can thiệp để TFLM có thể nhận diện được các lệnh mới đã cấu
hình thêm.

## 1.1 Update quan trọng ngày 17/05/2026

Từ milestone LED shift ban đầu, dự án đã chuyển sang bring-up nhóm lệnh
AI/PULP trên kit thật:

- `COREV_PULP` hiện đã bật `1` trong `rtl/soc/de2i150_cv32e40p_top.v`.
- Firmware hiện không còn là LED shift đơn giản mà là smoke test cho 6
  operation AI mapping qua CORE-V/PULP.
- Đã thêm `firmware/ai_ops.h` để bọc các instruction bằng `.insn`, vì
  toolchain `riscv64-unknown-elf-gcc` hiện tại chưa biết mnemonic CORE-V.
- Đã thêm `FPGA_TIMING_MODE=1` để phá đường timing dài EX-result ->
  ID-forwarding -> ID/EX operand. Sau sửa này Quartus full compile đạt
  50 MHz: worst setup slack `+0.372 ns`, worst hold slack `+0.374 ns`.
- Firmware smoke test hiện build bằng `-march=rv32im -mabi=ilp32` thay vì
  `rv32imc`, nhằm giữ vùng hardware loop word-aligned trong giai đoạn
  bring-up.
- LEDG0 heartbeat trên kit đã hoạt động. LEDG1 vẫn dùng để báo
  `core_sleep`. LEDR[7:0] dùng làm mã pass/fail của smoke test.

Trạng thái cần nhớ: hướng hiện tại là ưu tiên chạy đúng trên kit trước,
sau đó mới chuẩn hoá lại gem5, toolchain, benchmark và tối ưu tài nguyên.

## 2. Trạng thái hiện tại — đã hoàn thành

| Hạng mục | Trạng thái | Verified ở đâu |
|---|---|---|
| Cấu trúc project (rtl/core, rtl/fpga, rtl/soc, firmware) | ✅ | `tree de2i150_cv32e40p_soc/` |
| Copy core SV từ upstream `/home/duydonv/cv32e40p/rtl/` | ✅ | 28 file `.sv` + 3 package |
| FPGA-friendly clock gate (thay `bhv/cv32e40p_sim_clock_gate.sv`) | ✅ | `rtl/fpga/cv32e40p_clock_gate.sv` |
| OBI-to-native bus shim + 32 KB BRAM (4 byte-lane M9K) | ✅ | `rtl/soc/de2i150_cv32e40p_top.v` |
| Reset synchronizer | ✅ | Cùng file trên |
| MMIO LED tại `0x0300_0000` | ✅ | Cùng file trên |
| Heartbeat trên `LEDG0`, `core_sleep` ra `LEDG1` | ✅ | Cùng file trên |
| Firmware bare-metal LED shift (rv32imc) | ✅ | `firmware/{main.c, start.s, sections.lds, Makefile}` |
| Enable `COREV_PULP=1` cho hwloop/post-increment/CORE-V ALU/MUL | ✅ | `rtl/soc/de2i150_cv32e40p_top.v` |
| Smoke firmware cho 6 AI ops qua CORE-V/PULP `.insn` | ✅ | `firmware/main.c`, `firmware/ai_ops.h` |
| `FPGA_TIMING_MODE=1` để đóng timing 50 MHz | ✅ | `rtl/core/*`, Quartus STA |
| Splitter `firmware.bin` → 4 byte-lane hex | ✅ | `firmware/split_hex.py` |
| 8 patch SV-2012→SV-2005 cho Quartus Lite | ✅ | `fpga_patches/README.md` |
| Verify patch bằng `vlog -sv` (Questa) | ✅ | 0 error |
| Verify patch bằng `quartus_map` | ✅ | 0 error, 85 warning (đều benign) |
| Audit logic của patch (diff vs upstream) | ✅ | Cosmetic + sim-only + set-equivalent |
| Đối chiếu với cộng đồng (Issue OpenHW #1050) | ✅ | Trùng 5/5 nhóm fix; OpenHW từ chối merge upstream |
| So sánh với `sv2v` (alt route) | ✅ | sv2v cũng chạy được, ±0.6% LE; chọn manual patch để debug Signal Tap dễ hơn |
| SDC clock + false_path | ✅ | `de2i150_cv32e40p_top.sdc` |
| Pin assignment | ✅ | `PIN_ASSIGNMENTS.md` (đã verify với user manual DE2i-150) |
| **Nạp `.sof` lên kit, LED shift đúng** | ✅ | Ngày 26/04/2026 — bring-up milestone đầu tiên đạt |
| LEDG0 heartbeat sau khi bật PULP/timing mode | ✅ | Ngày 17/05/2026 — user xác nhận LEDG nháy đúng |

## 3. Các quyết định kỹ thuật đã chốt — KHÔNG được thay đổi nếu không thảo luận

### 3.1 Tại sao chọn CV32E40P thay vì PicoRV32

PicoRV32 dùng PCPI cho custom instruction nhưng **không hỗ trợ lệnh có
nhánh / điều khiển luồng** (`lp.setup`) hay lệnh **đọc và sửa state khác**
(`p.lw` cần update register). CV32E40P có sẵn `COREV_PULP=1` cung cấp:

- `lp.setup`/`lp.count` (hardware loop) qua `cv32e40p_hwloop_regs.sv`
- `p.lw` (post-increment load) trong `cv32e40p_load_store_unit.sv`
- `cv.mac`, `cv.sdotsp.b`, `cv.max`, `cv.clip`/`cv.clipu` đều ở
  `cv32e40p_alu.sv` / `cv32e40p_mult.sv`

Vì vậy trong giai đoạn hiện tại, cả 6 operation đều được map lên CORE-V/PULP
có sẵn. Phần "custom RTL riêng" chỉ nên quay lại nếu sau khi benchmark có
lý do rõ ràng: cần semantics khác, cần giảm tài nguyên bằng subset, hoặc
cần thêm quantization scaling đặc thù không có trong PULP.

### 3.2 Tại sao dùng manual SV→SV2005 patch thay vì sv2v

Đã so sánh thực tế (xem chat earlier), kết luận:

- Manual patch: 7744 LE, 85 warning, debug Signal Tap cleanly với tên
  signal nguyên gốc.
- sv2v: 7789 LE (+0.6%), 325 warning, signal bị flatten thành slice
  (`hwlp_counter_n[k*32+:32]`) — khó trace.

Vì có khả năng sau này phải **edit decoder/alu/mult** để tối ưu subset hoặc
thêm semantics riêng, manual patch ưu việt hơn cho dev loop. sv2v vẫn là
"plan B" nếu sau này pull upstream.

### 3.3 Tại sao SoC top viết bằng Verilog-2001 (không SV)

`rtl/soc/de2i150_cv32e40p_top.v` cố tình giữ Verilog-2001 để tránh tương
tác với SV-2005 limitations của Quartus Lite. Core SV được include qua
`SYSTEMVERILOG_FILE` trong QSF.

### 3.4 Tham số core hiện tại

```verilog
cv32e40p_top #(
    .COREV_PULP      (1),     // bật CORE-V/PULP: hwloop, post-inc load, cv.*
    .COREV_CLUSTER   (0),
    .FPU             (0),     // Tắt → không cần fp_wrapper, fpnew vendor
    .FPU_ADDMUL_LAT  (0),
    .FPU_OTHERS_LAT  (0),
    .ZFINX           (0),
    .NUM_MHPMCOUNTERS(1),
    .FPGA_TIMING_MODE(1)      // local FPGA timing patch
)
```

| Param | Giá trị | Lý do |
|---|---|---|
| `COREV_PULP` | 1 | Cần `cv.mac`, `cv.sdotsp.b`, `cv.max`, `cv.clipur`, `cv.lw` post-increment và `cv.setup` |
| `FPU` | 0 | Tránh fpnew vendor (~3000 LE) cho bring-up |
| `FPGA_TIMING_MODE` | 1 | Thêm bubble cho producer-consumer trực tiếp để đóng timing 50 MHz trên Cyclone IV |
| `BOOT_ADDR` | `0x0000_0000` | Match `_start` trong `sections.lds` |
| `MTVEC_ADDR` | `0x0000_0040` | Vector table 64 B sau boot |
| BRAM size | 32 KB | 4 × M9K bank, mỗi bank 8-bit × 8K |
| LED MMIO | `0x0300_0000` | Tránh đụng BRAM range `0x0` – `0x7FFF` |

### 3.5 Toolchain

- **Quartus Prime 25.1 SC Lite Edition** (build 1129) — **không có** option
  SV-2012, đó là lý do phải patch.
- **Questa Altera Starter FPGA Edition 2025.2** — dùng cho RTL sim, có
  license active.
- **`riscv64-unknown-elf-gcc`** hiện build smoke firmware với
  `-march=rv32im -mabi=ilp32`. Lý do: giữ hardware-loop body word-aligned
  trong bring-up, tránh compiler emit compressed instruction quanh
  `lp.setup`.
- Sau này vẫn nên chuyển sang `riscv32-corev-elf-gcc` hoặc CORE-V-aware
  toolchain để dùng mnemonic/builtin `cv.*`, `lp.*`, `p.lw` thay vì `.insn`.

### 3.6 Mapping 6 operation AI hiện tại

Mục tiêu thực dụng đã chốt: trên kit thật không bắt buộc giữ nguyên opcode
prototype trong gem5. Miễn là chức năng cuối cùng tương đương và phù hợp
với datapath CV32E40P thì ưu tiên tái dùng CORE-V/PULP có sẵn để giảm rủi
ro sửa core.

| Operation ở mức firmware | Mapping hiện tại | Ghi chú |
|---|---|---|
| `mac(acc, a, b)` | `cv.mac` qua `.insn r 0x2b, 3, 0x48` | Dùng MAC 32-bit có sẵn trong multiplier path |
| `dot4_acc(acc, a, b)` | `cv.sdotsp.b` qua `.insn r 0x7b, 1, 0x54` | Dot product 4 lane signed int8 + accumulate |
| `relu(x)` | `cv.max x, x, x0` qua `.insn r 0x2b, 3, 0x2d` | Tương đương max(x, 0) |
| `clamp(x, ub)` | `cv.clipur` qua `.insn r 0x2b, 3, 0x3b` | Clamp về `[0, ub]`, phù hợp hơn dạng AI quantization |
| `p.lw` | `cv.lw rd, (rs1), imm` qua custom-0 `.insn i 0x0b, 2` | Hardware đọc tại `rs1`, sau đó `rs1 += sext(imm)` |
| `lp.setup` | `cv.setup L0, rs1, uimmL` qua `.insn i 0x2b, 4` | Hardware loop L0, count từ `rs1` |

Hai điểm sai khác đã chốt so với gem5 prototype ban đầu:

- `clamp`: gem5 trước đó cho upper bound bất kỳ. Trên core hiện tại dùng
  `cv.clipur` dạng register upper bound, không phải kiểu `2^n - 1` immediate.
  Hướng này phù hợp hơn cho bài toán AI thực tế vì activation quantization
  thường cần clamp theo range runtime như `[0, 255]`, `[0, 127]`, hoặc scale
  phụ thuộc tensor.
- `p.lw`: gem5 prototype đọc `Mem[rs1 + imm]` rồi post-increment cố định.
  CORE-V `cv.lw` đọc `Mem[rs1]` rồi cộng immediate vào base register. Hướng
  hardware hiện tại vẫn phổ biến cho stream access; nếu cần offset so với
  baseline thì pre-bias pointer một lần trước vòng lặp.

### 3.7 Các lỗi bring-up đã gặp và kết luận

| Lỗi / hiện tượng | Nguyên nhân | Cách xử lý |
|---|---|---|
| Fmax tụt khoảng 37.6 MHz khi bật PULP/custom path | Đường tổ hợp dài EX-result -> ID forwarding -> ID/EX operand trên FPGA Cyclone IV | Thêm `FPGA_TIMING_MODE=1`: stall một chu kỳ cho producer-consumer trực tiếp và tắt EX->ID bypass trong mode này |
| LEDR `0xE3` ở test ReLU | Mapping/test ban đầu chưa ổn định khi bring-up nhóm lệnh | Đã chuyển `relu` sang mapping rõ ràng bằng `cv.max(x, 0)` và mở rộng smoke test |
| LEDR `0xE6` ở `lp_setup` | Có 2 rủi ro: raw immediate của `.insn` bị hiểu nhầm theo byte, và firmware `rv32imc` có thể đặt loop body ở halfword address | Raw imm đúng là `body_words + 1`; firmware smoke build `rv32im`, block asm dùng `.option norvc` + `.balign 4` |
| Sửa `ai_ops.h` nhưng firmware không rebuild | Makefile trước đó không khai báo dependency header | Đã thêm `ai_ops.h` vào dependency của `firmware.elf` |

Với `lp.setup`, ghi nhớ công thức quan trọng:

```text
RTL: hwlp_end = pc_of_setup + (raw_imm << 2)
Controller compare: hwlp_end == last_body_pc + 4
=> raw_imm = number_of_body_words + 1
```

Ví dụ body 3 lệnh 32-bit = 12 byte thì `.insn` phải encode raw immediate
`4`, không phải `12`.

### 3.8 Smoke test firmware hiện tại

`firmware/main.c` trả mã lỗi trên `LEDR[7:0]`:

| LEDR | Ý nghĩa |
|---|---|
| `0xE1` | `mac` fail |
| `0xE2` | `dot4_acc` fail |
| `0xE3` | `relu` fail |
| `0xE4` | `clamp` fail |
| `0xE5` | `p.lw` fail |
| `0xE6` | `lp.setup` fail |
| `0xA5` | Tất cả smoke test pass |

Vòng `main()` hiện chớp giữa `status` và `0x00` với delay dài hơn bản đầu,
để nhìn LEDR rõ hơn trên kit. `LEDG0` là heartbeat độc lập ở SoC top nên
LEDG0 nháy chỉ chứng minh clock/reset/SoC sống; kết quả instruction smoke
test vẫn nằm ở LEDR.

## 4. Cấu trúc thư mục project

```
de2i150_cv32e40p_soc/
├── rtl/
│   ├── core/                                <- 28 file .sv ĐÃ PATCH SV-2005
│   │   ├── include/
│   │   │   ├── cv32e40p_pkg.sv              <- ⭐ enum opcode, ALU op (tra cứu khi xem waveform)
│   │   │   ├── cv32e40p_apu_core_pkg.sv
│   │   │   └── cv32e40p_fpu_pkg.sv          <- ĐÃ PATCH (enum width)
│   │   ├── cv32e40p_top.sv                  <- entry point
│   │   ├── cv32e40p_core.sv
│   │   ├── cv32e40p_id_stage.sv             <- ĐÃ PATCH (multi-import + inline genvar)
│   │   ├── cv32e40p_ex_stage.sv             <- ĐÃ PATCH (multi-import)
│   │   ├── cv32e40p_decoder.sv              <- ⭐ ĐÃ PATCH (3-import + inside), PULP decode đang dùng
│   │   ├── cv32e40p_alu.sv                  <- ⭐ ReLU/Clamp map qua ALU PULP có sẵn
│   │   ├── cv32e40p_mult.sv                 <- ⭐ MAC/Dot4 map qua multiplier PULP có sẵn
│   │   ├── cv32e40p_load_store_unit.sv      <- p.lw đã có sẵn
│   │   ├── cv32e40p_hwloop_regs.sv          <- ĐÃ PATCH (bare for-generate) — lp.setup state
│   │   ├── cv32e40p_cs_registers.sv         <- ĐÃ PATCH (bare if-generate × 3)
│   │   ├── cv32e40p_apu_disp.sv             <- ĐÃ PATCH (inline genvar × 2)
│   │   ├── cv32e40p_register_file_ff.sv     <- ĐÃ PATCH (unnamed for-generate)
│   │   └── ... 17 file khác (controller, prefetch, fifo, obi_interface, ...)
│   ├── fpga/
│   │   └── cv32e40p_clock_gate.sv           <- TỰ VIẾT — pass-through cho FPGA
│   └── soc/
│       └── de2i150_cv32e40p_top.v           <- TỰ VIẾT — Verilog-2001, OBI shim + BRAM + MMIO
├── firmware/
│   ├── ai_ops.h                             <- CORE-V/PULP .insn wrappers
│   ├── main.c                               <- AI/PULP smoke test, LED pass/fail
│   ├── start.s                              <- crt0 RV32 (set sp, jump main)
│   ├── sections.lds                         <- linker (32 KB RAM at 0x0)
│   ├── split_hex.py                         <- bin → 4 byte-lane hex
│   ├── Makefile                             <- riscv64-unknown-elf-gcc
│   ├── firmware.elf / .bin / .hex           <- output sau make
│   └── firmware_byte{0..3}.hex              <- ⭐ feed vào $readmemh trong SoC top
├── de2i150_cv32e40p_top.sdc                 <- clock 50 MHz + false_path
├── de2i150_cv32e40p_top.qpf / .qsf          <- Quartus generated; KHÔNG sửa tay
├── fpga_patches/
│   └── README.md                            <- ⭐ Chi tiết 8 patch để re-apply khi pull upstream
├── PIN_ASSIGNMENTS.md                       <- pin map đã verify với user manual
├── README.md                                <- hướng dẫn build + bring-up
└── HANDOFF.md                               <- file này
```

## 5. Roadmap — đầu việc kế tiếp

### Phase 1 — Bring-up baseline (✅ XONG)

1. ~~Setup project, copy core, viết SoC top, firmware LED shift.~~
2. ~~Patch SV-2005 cho Quartus Lite.~~
3. ~~Compile thành công + nạp .sof + LED shift trên kit.~~

### Phase 2 — Đo tài nguyên & enable PULP extension (✅ cơ bản đã xong)

1. **Đo baseline resource** với config hiện tại (`COREV_PULP=0, FPU=0`):
   - Logic cells: 7744
   - RAM segments: 64
   - DSP: 8
   - Lưu lại để so sánh trước/sau khi enable PULP và thêm custom inst.
2. **Switch `COREV_PULP=0 → 1`** trong `rtl/soc/de2i150_cv32e40p_top.v`:
   ```verilog
   .COREV_PULP(1),
   ```
3. **Re-synthesize**: đã gặp timing fail ở 50 MHz, sau đó thêm
   `FPGA_TIMING_MODE=1` và compile đạt timing:
   - Worst setup slack: `+0.372 ns`
   - Worst hold slack: `+0.374 ns`
   - Timing Analyzer báo fully constrained setup/hold.
4. **Verify bằng smoke firmware**: đã có test nhỏ cho cả 6 operation trong
   `firmware/main.c`, dùng wrapper `.insn` ở `firmware/ai_ops.h`.
5. **Việc còn mở của Phase 2**:
   - Chốt kết quả LEDR sau image mới nhất trên kit và ghi lại pass/fail.
   - Lưu resource report chính xác sau `COREV_PULP=1` + `FPGA_TIMING_MODE=1`
     để so với baseline `COREV_PULP=0`.
   - Cân nhắc dùng Signal Tap hoặc RTL simulation nếu còn lỗi LEDR nào.

### Phase 3 — Benchmark, chuẩn hoá toolchain/gem5, và cân nhắc custom RTL

Hiện tại 6 operation đã được map lên CORE-V/PULP có sẵn. Chưa cần vội tự
thêm opcode riêng nếu mục tiêu trước mắt là chạy model thực tế và đo lợi ích.

Đầu việc tiếp theo nên đi theo thứ tự:

1. **Viết benchmark hiệu năng nhỏ**:
   - Baseline C thuần.
   - Bản dùng wrapper `ai_ops.h`.
   - Đo `mcycle`/`minstret` quanh kernel.
   - Kernel đầu tiên nên là dot product INT8 hoặc dense layer rất nhỏ để dễ
     kiểm tra đúng/sai trên LED/UART.
2. **Tách smoke test và benchmark**:
   - `main.c` hiện đang là smoke test.
   - Nên tạo thêm firmware benchmark riêng hoặc dùng `#define` chọn mode,
     tránh benchmark bị lẫn delay LED.
3. **Chuẩn hoá lại gem5 sau khi kit chạy ổn**:
   - `clamp` nên đồng bộ về register upper-bound `[0, ub]`.
   - `p.lw` nên quyết định theo semantics CORE-V hardware: load tại base,
     sau đó post-increment bằng immediate.
   - Thống kê gem5 O3 có thể khác hardware pipeline; trước mắt chỉ cần
     functional match, performance model sẽ chỉnh sau.
4. **Cài CORE-V-aware toolchain**:
   - Mục tiêu là dùng mnemonic hoặc builtin thay `.insn` thủ công.
   - Khi đổi toolchain phải objdump kiểm tra encoding không đổi.
5. **Cân nhắc tắt PULP và tự build subset riêng**:
   - Lý do cân nhắc: giảm tài nguyên và giảm phần logic không dùng.
   - Chỉ nên làm sau khi đã có số liệu resource/performance của bản
     `COREV_PULP=1`, vì bản PULP hiện đang giúp bring-up nhanh và ít rủi ro.
   - Nếu đi hướng subset riêng, cần tự giữ lại ít nhất: 4 ALU/MUL ops,
     post-increment load, và hardware loop hoặc một cơ chế thay thế.

### Phase 3b — Nếu quyết định tự build custom subset thay PULP

Đây là hướng tối ưu tài nguyên, không phải hướng ưu tiên ngay lập tức.

| Nhóm | Việc phải làm nếu tắt `COREV_PULP` |
|---|---|
| `mac`, `dot4_acc`, `relu`, `clamp` | Thêm decode riêng và nối vào ALU/MUL path tối thiểu |
| `p.lw` | Tự giữ lại hoặc viết lại post-increment LSU path, gồm writeback thứ hai về base register |
| `lp.setup` | Tự giữ lại hardware loop regs/controller path hoặc thiết kế cơ chế loop riêng |
| Toolchain | Vẫn cần `.insn`/builtin và objdump kiểm tra encoding |
| Verification | Cần smoke test + waveform vì tắt PULP dễ làm mất các đường điều khiển đặc thù |

Rủi ro lớn nhất khi tự subset là `lp.setup` và `p.lw`, vì đây không còn là
lệnh ALU thuần mà đụng đến control-flow, PC, LSU và register writeback.

### Phase 4 — UART + LCD (optional, copy từ PicoRV32 project)

Lấy MMIO module + pin map từ `/home/duydonv/de2i_150_test/`:

- UART (115200 8N1) trên `/dev/ttyUSB0` — heartbeat + echo
- LCD 1602 hiển thị string nhận được từ UART RX

### Phase 5 — INT8 kernel demo + profiling

- Chạy 1 kernel matmul/convolution INT8 trên baseline C và bản dùng
  CORE-V/PULP wrappers.
- Đếm cycle qua `mcycle` CSR.
- So sánh số liệu để chứng minh giá trị của extension trước khi quyết định
  có cần tự build subset/custom RTL hay không.

## 6. Cách verify mọi thứ vẫn work khi resume

Mỗi khi quay lại dự án sau thời gian dài, chạy 3 lệnh sau để confirm:

```bash
# 1. Kiểm tra patch còn nguyên
cd /home/duydonv/de2i150_cv32e40p_soc
grep -c "Quartus Lite" rtl/core/cv32e40p_*.sv rtl/core/include/cv32e40p_fpu_pkg.sv
# Phải ra ≥8 hit (mỗi file patch có ít nhất 1 comment đánh dấu)

# 2. Build firmware
cd firmware && make clean && make
ls firmware_byte*.hex      # Phải có 4 file

# 3. Synth thử hoặc full compile khi cần .sof mới
cd ..
/home/duydonv/altera_lite/25.1std/quartus/bin/quartus_map \
  --read_settings_files=on --write_settings_files=off \
  de2i150_cv32e40p_top -c de2i150_cv32e40p_top 2>&1 | grep "successful"
# Phải ra: "Quartus Prime Analysis & Synthesis was successful. 0 errors, ..."

# Full compile để sinh .sof mới và kiểm timing:
/home/duydonv/altera_lite/25.1std/quartus/bin/quartus_sh \
  --flow compile de2i150_cv32e40p_top
# Timing hiện kỳ vọng ở 50 MHz: setup slack dương, hold slack dương.
```

Nếu cả 3 pass thì project state intact.

## 7. Các file tài liệu khác trong dự án

| File | Mục đích |
|---|---|
| `README.md` | Hướng dẫn dùng dự án (build, nạp .sof, expected behaviour) |
| `PIN_ASSIGNMENTS.md` | Pin map FPGA — đã verify với user manual |
| `fpga_patches/README.md` | ⭐ Chi tiết 8 patch SV-2005 với before/after, để re-apply khi pull upstream |
| `HANDOFF.md` (file này) | Bàn giao trạng thái dự án |

## 8. Pitfalls đã gặp & cách tránh

| Pitfall | Triệu chứng | Cách xử lý |
|---|---|---|
| Quartus Lite không hỗ trợ SV-2012 | 29 lỗi `10170 syntax error` khi synth | Đã patch — xem `fpga_patches/README.md`. **Không** copy đè file core từ upstream |
| `cv32e40p_sim_clock_gate.sv` infer latch trên clock | Quartus warn không route global clock được | Đã thay bằng `rtl/fpga/cv32e40p_clock_gate.sv` pass-through |
| OBI bus không nối thẳng được BRAM | Core hang ở trap | OBI shim trong `de2i150_cv32e40p_top.v` (`gnt=req`, `rvalid` 1-cycle delay) |
| `$readmemh` chỉ load lúc elaborate | Edit firmware xong CPU vẫn chạy code cũ | Phải **re-run `make`** rồi **re-compile Quartus** (không có loader runtime) |
| LEDR[8-17] sáng mờ nếu không assign pin | Quartus place fail hoặc nhiễu | Đã assign hết 18 LEDR trong `PIN_ASSIGNMENTS.md`, RTL tie low cho [17:8] |
| Quartus warning 85 cái sau patch | Tưởng có lỗi | Đều là benign: unused signal khi FPU=0, inferred RAM, truncation. **Không cần fix** |
| Bật PULP làm Fmax dưới 50 MHz | Quartus timing fail dù logic function đúng | Giữ `FPGA_TIMING_MODE=1`; nếu sửa forwarding/controller phải re-run full compile và check STA |
| `lp.setup` bằng `.insn` dùng immediate sai | LEDR `0xE6`, vòng lặp chạy sai số lần hoặc không loop | Raw immediate = `body_words + 1`; dùng `.option norvc` + `.balign 4` trong smoke test |

## 9. Tham chiếu nhanh các artifact bên ngoài project

| Đường dẫn | Vai trò |
|---|---|
| `/home/duydonv/cv32e40p/` | Upstream CV32E40P (KHÔNG sửa) |
| `/home/duydonv/cv32e40p/rtl/` | Source SV gốc — copy từ đây vào `rtl/core/` |
| `/home/duydonv/cv32e40p/example_tb/core/hwlp_test/` | ⭐ Reference cho test `lp.setup` |
| `/home/duydonv/cv32e40p/docs/source/` | TRM (instruction_set_extensions.rst, corev_hw_loop.rst, fpu.rst…) |
| `/home/duydonv/picorv32/` | PicoRV32 upstream (đã chuyển project, archive) |
| `/home/duydonv/de2i_150_test/` | Project PicoRV32 cũ — mượn UART/LCD module + pin map |
| `/home/duydonv/altera_lite/25.1std/quartus/bin/` | Quartus binaries |
| `/home/duydonv/altera_lite/25.1std/questa_fse/bin/` | Questa binaries (vlog, vsim, vlib) |

## 10. OpenHW community context

- Issue tham chiếu: https://github.com/openhwgroup/cv32e40p/issues/1050
  (cùng vấn đề Quartus 25.1 SC Lite, OpenHW từ chối merge fix upstream).
- Vì vậy patch của dự án này phải **giữ tại local**, không hy vọng pull
  about upstream merge để loại bỏ.
- Nếu sau này upstream thay đổi nhiều, có thể cân nhắc chuyển sang `sv2v`
  workflow (tham khảo so sánh trong chat history).
