# HANDOFF — DE2i-150 + CV32E40P bring-up

> Tài liệu bàn giao trạng thái dự án để bất kỳ agent / developer mới nào
> cũng có thể tiếp tục làm việc ngay. Cập nhật lần cuối: **07/06/2026**.

## 1. Mục tiêu cuối cùng

Triển khai core RISC-V **CV32E40P** (OpenHW Group) lên kit **Terasic DE2i-150**
(Cyclone IV GX `EP4CGX150DF31C7`) để làm nền tảng cho:

- 6 lệnh mở rộng phục vụ AI lượng tử hoá: `MAC`, `Dot4_acc`, `Relu`,
  `Clamp`, `lp.setup`, `p.lw`.
- Kernel INT8 demo cuối cùng để đo cycle-accurate trên kit.

Sau đó sẽ triển khai mô hình AI có sẵn lên kit để đánh giá thực tế. Tích
hợp TFLM và can thiệp để TFLM có thể nhận diện được các lệnh mới đã cấu
hình thêm.

## 1.1 Update quan trọng đến ngày 23/05/2026

Từ milestone LED shift ban đầu, dự án đã chuyển sang bring-up nhóm lệnh
AI/PULP, benchmark UART, và power-analysis flow trên kit thật:

- `COREV_PULP` hiện đã bật `1` trong `rtl/soc/de2i150_cv32e40p_top.v`.
- Firmware hiện không còn là LED shift đơn giản mà là smoke test cho 6
  operation AI mapping qua CORE-V/PULP.
- Đã thêm `firmware/ai_ops.h` để bọc các instruction bằng `.insn`, vì
  toolchain `riscv64-unknown-elf-gcc` hiện tại chưa biết mnemonic CORE-V.
- Đã thêm `FPGA_TIMING_MODE=1` để phá đường timing dài EX-result ->
  ID-forwarding -> ID/EX operand. Với UART TX + benchmark image thời điểm đó,
  Quartus full compile đạt 50 MHz: Fmax `50.08 MHz`, worst setup slack
  `+0.033 ns`, worst hold slack `+0.373 ns`.
- Firmware hiện build bằng `-march=rv32im_zicsr -mabi=ilp32` thay vì
  `rv32imc`, nhằm giữ vùng hardware loop word-aligned trong giai đoạn
  bring-up và cho phép benchmark đọc CSR `mcycle`/`minstret`.
- LEDG0 heartbeat trên kit đã hoạt động. LEDG1 vẫn dùng để báo
  `core_sleep`. LEDR[7:0] dùng làm mã pass/fail/trạng thái ngắn.
- Đã thêm firmware benchmark riêng (`make benchmark`) để đo `mcycle` và
  `minstret` cho ba nhóm hiện tại: `MAC+Clamp`, `Dot4_acc+Clamp`, và
  `Dot4_acc+Clamp+p.lw+lp.setup`. Benchmark in kết quả dạng bảng cố định
  qua UART TX 115200 8N1; LEDR[7:0] chỉ còn báo trạng thái ngắn.
- Kết quả thực tế trên kit đã pass checksum cho cả 3 benchmark:
  `mac_clamp` ~1.07x, `dot4_acc_clamp` ~6.33x, và
  `dot4_plw_lp_clamp` ~13.10x.
- Đã thêm flow Questa RTL simulation để sinh VCD cho Quartus Power Analyzer:
  `sim/run_power_vcd.sh`, `sim/power_tb.sv`, `sim/README.md`. Một scenario
  `dot4_plw_lp_clamp_custom` đã chạy PASS và tạo VCD có signal activity.
- Trong Quartus Power Analyzer, khi add VCD, `Entity` phải là top-level đã
  fit của Quartus: `de2i150_cv32e40p_top`, không phải testbench scope
  `tb_power.u_dut`.

Trạng thái cần nhớ: bản `COREV_PULP=1` hiện đã ổn định để lấy số liệu
performance/resource/power. Chưa nên tự viết lại subset custom trừ khi có
thời gian verify kỹ, đặc biệt với `p.lw` và `lp.setup`.

## 1.2 Update đồng bộ gem5 đến ngày 02/06/2026

Gem5 repo liên quan: `/home/duydonv/gem5/tests/gem5/riscv_ai_ext`.

- Semantics gem5 đã được căn lại theo board implementation hiện tại:
  register-bound `clamp`, `p.lw` load-before-post-increment, và `lp.setup`
  dùng raw CORE-V immediate `body_words + 1`.
- Gem5 vẫn giữ prototype custom-0 encoding; SoC/firmware vẫn dùng CORE-V/PULP
  `.insn` mapping thật trên CV32E40P. Vì vậy so sánh cần dựa trên semantics,
  không dựa trên opcode bit pattern.
- Gem5 đã có chế độ sensitivity `--no-cache --memory fast-ram` dùng
  `SingleChannelSimpleMemory(latency=1ns, bandwidth=256GiB/s)` để gần hơn với
  kit-local RAM so với NoCache + DDR3 mặc định.
- O3/FDP hardware-loop path trong gem5 đã cải thiện nhưng chưa phải mô hình
  cycle-for-cycle của CV32E40P. Cached O3 vẫn còn residual execute-time repair
  trên microbenchmark; thử ép setup-time squash/re-fetch làm
  `nonControlRedirects=0` nhưng fast-ram bị regress nên đã revert.
- Khi viết báo cáo/bàn giao, ưu tiên số liệu board UART benchmark là kết quả
  thực nghiệm chính. Gem5 nên được trình bày như smoke/regression và
  modeling/sensitivity support.

## 1.3 Update tiny AI và TFLM đến ngày 03/06/2026

- Đã thêm `firmware/tiny_ai.c` làm bước đệm trước TFLM: chạy MLP INT8 cực nhỏ
  với baseline C và bản dùng CORE-V/PULP wrappers. Model/data được export từ
  `firmware/export_tiny_ai_model.py` sang `firmware/tiny_ai_model.h`.
- Kết quả UART mới nhất trên kit cho `tiny_ai`: model `8x8 quadrant MLP int8
  64 -> 16 -> 4`, 8 mẫu, 8704 INT8 MACs, speedup `7.09x`, checksum
  `0x6c75e8cf`, accuracy `8/8`.
- Đã thêm flow TensorFlow Lite Micro generated tree tại
  `third_party/tflm_tree` và firmware `firmware/tflm_hello.cc` để chạy
  official `hello_world` int8 model với một op `FullyConnected`. Tree này là
  generated dependency local và bị ignore bởi git; tái tạo bằng
  `firmware/generate_tflm_tree.sh`.
- TFLM cần C++17 bare-metal toolchain có libstdc++ headers. Ubuntu
  `riscv64-unknown-elf-g++` trên máy này không đủ; build TFLM dùng xPack GNU
  RISC-V Embedded GCC 14.2.0. Toolchain đã được đặt bền vững dưới
  `/home/duydonv/tools/xpack-riscv-none-elf-gcc/...`.
- Upstream TFLM source clone đã được đặt ngoài repo SoC tại
  `/home/duydonv/src/tflite-micro`, worktree sạch ở commit `ac1fae3`. Clone
  này chỉ cần khi regenerate/update `third_party/tflm_tree`.
- Để TFLM fit, local BRAM/linker/splitter đã tăng từ 32 KB lên 128 KB:
  `BRAM_AW_WORDS=15`, `sections.lds LENGTH=0x20000`,
  `split_hex.py WORDS=32768`.
- `tflm_hello` đã link được trên host với size
  `text=46484 data=292 bss=4444 dec=51220`, và Quartus full compile đã pass
  với 128 KB BRAM.
- `tflm_hello` đã chạy pass trên kit: cycles `45149`, instret `32117`,
  cycles/invoke `11287`, checksum `0x3a357ded`, status `0x00000000`,
  outputs int8 `89 125 93 4`; xem `firmware/TFLM.md`.
- Đã bắt đầu mốc kế tiếp `tflm_tiny_ai`: cùng tiny MLP 8x8 của `tiny_ai.c`
  nhưng đóng gói thành TFLM flatbuffer và chạy bằng reference
  `FullyConnected`. Model được sinh bởi
  `firmware/generate_tflm_tiny_ai_model.cc` từ `tiny_ai_model.h`, không cần
  TensorFlow Python/`flatc`. Đây là bước đúng-trước-nhanh-sau, chưa dùng
  CORE-V/PULP optimized kernel.
- UART RX hiện đã có peripheral/MMIO và smoke firmware riêng (`rx_smoke`) để
  kiểm tra frame/checksum độc lập với TFLM. Đã thêm `tflm_tiny_uart` để
  đưa RX vào TFLM small model theo request/response, không dùng TX report lặp.
  Mode này đã board-test pass; runtime input streaming cho model FC lớn nên
  dùng lại protocol đó.
- Đã chuẩn bị artifact host cho mốc MNIST FC lớn hơn `784 -> 32 -> 10` tại
  `firmware/mnist_fc/`: script train/quantize/verify TensorFlow, model
  full-INT8 `.tflite`, C byte array, metadata và 32 test vectors. Kết quả
  verify host: float accuracy `96.30%`, INT8 accuracy `96.28%` trên 10,000
  mẫu MNIST test, model size `28368` byte, checksum INT8 `0x7c33a8dc`.
  Firmware `tflm_mnist_fc` đã chạy ref-vs-opt pass trên board: `tflm_ref` và
  `pulp_opt` đều dùng cùng flatbuffer model, checksum `0x00cb95fc`, expected
  class `32/32`, score mismatches `0`. Bản này đã sửa lỗi đọc constant tensor
  của TFLM Micro bằng cách lấy weight/bias trực tiếp từ flatbuffer buffer.
  Sau khi thêm fast path FC1x4 aligned `cv.lw` + `cv.setup` quanh
  `cv.sdotsp.b`, board UART mới nhất đạt validated speedup `12.39x`: ref
  `11172961` cycles, opt `901789` cycles, checksum match,
  `Overall pass: yes`. Firmware cũng in thêm inference-only timing: ref
  `10817077` cycles, opt `874897` cycles, speedup `12.36x`. Clamp vẫn giữ
  scalar signed TFLite-compatible path, chưa map sang `cv.clipur`.

## 2. Trạng thái hiện tại — đã hoàn thành

| Hạng mục | Trạng thái | Verified ở đâu |
|---|---|---|
| Cấu trúc project (rtl/core, rtl/fpga, rtl/soc, firmware) | ✅ | `tree de2i150_cv32e40p_soc/` |
| Copy core SV từ upstream `/home/duydonv/cv32e40p/rtl/` | ✅ | 28 file `.sv` + 3 package |
| FPGA-friendly clock gate (thay `bhv/cv32e40p_sim_clock_gate.sv`) | ✅ | `rtl/fpga/cv32e40p_clock_gate.sv` |
| OBI-to-native bus shim + 128 KB BRAM (4 byte-lane M9K) | ✅ | `rtl/soc/de2i150_cv32e40p_top.v` |
| Reset synchronizer | ✅ | Cùng file trên |
| MMIO LED tại `0x0300_0000` | ✅ | Cùng file trên |
| UART TX MMIO tại `0x0200_0000/0x0200_0004` | ✅ | `rtl/soc/de2i150_cv32e40p_top.v`, `firmware/perf.h` |
| UART RX MMIO + smoke test | ✅ | RX data `0x0200_0008`, status bits `RX_VALID/RX_OVERRUN/RX_FRAME_ERROR`, `firmware/rx_smoke.c`, `firmware/UART_RX.md`; runner/manual `picocom` pass |
| Heartbeat trên `LEDG0`, `core_sleep` ra `LEDG1` | ✅ | Cùng file trên |
| Firmware bare-metal LED shift (rv32imc) | ✅ | `firmware/{main.c, start.s, sections.lds, Makefile}` |
| Enable `COREV_PULP=1` cho hwloop/post-increment/CORE-V ALU/MUL | ✅ | `rtl/soc/de2i150_cv32e40p_top.v` |
| Smoke firmware cho 6 AI ops qua CORE-V/PULP `.insn` | ✅ | `firmware/main.c`, `firmware/ai_ops.h` |
| Benchmark firmware baseline vs CORE-V/PULP | ✅ | `firmware/benchmark.c`, `firmware/perf.h`, `firmware/BENCHMARKS.md` |
| 3-row UART benchmark table trên kit | ✅ | `mac_clamp`, `dot4_acc_clamp`, `dot4_plw_lp_clamp` đều pass checksum |
| Power VCD flow cho Quartus Power Analyzer | ✅ | `sim/run_power_vcd.sh`, `sim/power_tb.sv`, `sim/README.md` |
| Đồng bộ semantics với gem5 prototype | ✅ | register-bound clamp, CORE-V `p.lw`, raw-imm `lp.setup`; xem gem5 `riscv_ai_ext/HANDOFF.md` |
| Tiny INT8 MLP pre-TFLM | ✅ | `firmware/tiny_ai.c`, `firmware/TINY_AI.md`; board UART speedup 7.09x |
| TFLM `hello_world` int8 build | ✅ host-build | `firmware/tflm_hello.cc`, `firmware/TFLM.md`, local ignored `third_party/tflm_tree` |
| Full compile 128 KB TFLM image | ✅ | `.sof` sinh tại `output_files/de2i150_cv32e40p_top.sof`; setup slack +0.337 ns |
| TFLM `hello_world` board run | ✅ | UART checksum `0x3a357ded`, outputs `89 125 93 4`, pass yes |
| TFLM tiny MLP reference build/full compile | ✅ | `firmware/tflm_tiny_ai.cc`, model 2288 B, firmware `dec=55916`, Quartus 0 errors |
| TFLM tiny MLP reference board run | ✅ | UART checksum `0xc5f79430`, cycles `167327`, accuracy `8/8`, pass yes |
| TFLM tiny MLP ref-vs-opt firmware build | ✅ | `pulp_opt` dùng `cv.sdotsp.b`, `cv.lw`, `cv.setup`, `cv.clipur`; firmware `dec=58788` |
| TFLM tiny MLP ref-vs-opt full compile | ✅ | `.sof` mới tại `output_files/de2i150_cv32e40p_top.sof`; setup slack +0.337 ns |
| TFLM tiny MLP ref-vs-opt board run | ✅ | ref `167507` cycles, opt `29620` cycles, speedup `5.66x`, checksum match |
| TFLM tiny MLP UART runtime-input firmware | ✅ board-pass | `firmware/tflm_tiny_uart.cc`, `firmware/tflm_tiny_uart_runner.py`; ping + 8 framed samples pass, speedup ~`5.61x` |
| MNIST FC `784->32->10` INT8 host artifacts | ✅ host-verified | `firmware/mnist_fc/`; INT8 `.tflite` accuracy `96.28%`, model `28368` B, checksum `0x7c33a8dc` |
| MNIST FC TFLM reference board run | ✅ board-pass | ref-only UART checksum `0x00cb95fc`, expected-class `32/32`, score mismatches `0`, cycles `11171144` |
| MNIST FC ref-vs-opt firmware | ✅ board-pass | `firmware/tflm_mnist_fc.cc`; validated ref `11172961` cycles, opt `901789` cycles, speedup `12.39x`; inference-only ref `10817077`, opt `874897`, speedup `12.36x`; checksum `0x00cb95fc`, score mismatches `0`, firmware `dec=121732`, `.sof` checksum `0x022DD42A` |
| `FPGA_TIMING_MODE=1` để đóng timing 50 MHz | ✅ | `rtl/core/*`, Quartus STA |
| Splitter `firmware.bin` → 4 byte-lane hex | ✅ | `firmware/split_hex.py` |
| 8 patch SV-2012→SV-2005 cho Quartus Lite | ✅ | `fpga_patches/README.md` |
| Verify patch bằng `vlog -sv` (Questa) | ✅ | 0 error |
| Verify patch bằng `quartus_map` | ✅ | 0 error, 77 warning sau khi thêm UART TX (đều benign) |
| Full compile UART benchmark image trước TFLM | ✅ | 0 error, 84 warning; setup `+0.033 ns`, hold `+0.373 ns` |
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
| BRAM size | 128 KB | 4 byte-lane bank, mỗi bank 8-bit × 32K |
| LED MMIO | `0x0300_0000` | Tránh đụng BRAM range `0x0` – `0x1FFFF` |

### 3.5 Toolchain

- **Quartus Prime 25.1 SC Lite Edition** (build 1129) — **không có** option
  SV-2012, đó là lý do phải patch.
- **Questa Altera Starter FPGA Edition 2025.2** — dùng cho RTL sim, có
  license active.
- **`riscv64-unknown-elf-gcc`** hiện build firmware với
  `-march=rv32im_zicsr -mabi=ilp32`. Lý do: giữ hardware-loop body
  word-aligned trong bring-up, tránh compiler emit compressed instruction
  quanh `lp.setup`, và vẫn cho phép benchmark đọc CSR performance counter.
- **TFLM dùng C++17** nên cần `g++` bare-metal có libstdc++ headers. Toolchain
  Ubuntu `riscv64-unknown-elf-g++` trên máy này thiếu C++ standard headers;
  build `tflm_hello` hiện dùng xPack. Đường dẫn `/tmp/...` chỉ là bring-up
  tạm; đường dẫn lâu dài đã xác minh là
  `/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-`.
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

Hai điểm sai khác đã chốt so với gem5 prototype ban đầu, và gem5 hiện đã được
cập nhật để match các semantics này:

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
| LEDR `0xE6` ở `lp_setup` | Có 2 rủi ro: raw immediate của `.insn` bị hiểu nhầm theo byte, và firmware `rv32imc` có thể đặt loop body ở halfword address | Raw imm đúng là `body_words + 1`; firmware build `rv32im_zicsr`, block asm dùng `.option norvc` + `.balign 4` |
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
│   ├── benchmark.c                          <- 3-row UART benchmark + POWER_SIM mode
│   ├── tiny_ai.c                            <- tiny INT8 MLP baseline/custom
│   ├── tiny_ai_model.h                      <- generated tiny model/data
│   ├── export_tiny_ai_model.py              <- export script for tiny model
│   ├── tflm_hello.cc                        <- first TFLM hello_world firmware
│   ├── tflm_tiny_ai.cc                      <- tiny MLP TFLM reference firmware
│   ├── generate_tflm_tiny_ai_model.cc       <- host generator for tiny MLP flatbuffer
│   ├── tflm_tiny_ai_model_data.{cc,h}       <- generated embedded tiny MLP model
│   ├── tflm_port.cc                         <- TFLM target hooks
│   ├── tflm_kernel_util_shim.cc             <- compatibility shim for generated tree
│   ├── tflm_sources_minimal.txt             <- minimal TFLM source list
│   ├── generate_tflm_tree.sh                <- regenerate ignored TFLM tree
│   ├── BENCHMARKS.md                         <- ⭐ Benchmark groups, ops count, UART/LED output
│   ├── TINY_AI.md                           <- ⭐ Tiny MLP notes and board result
│   ├── TFLM.md                              <- ⭐ TFLM bring-up notes
│   ├── perf.h                               <- CSR, UART, LED helpers
│   ├── start.s                              <- crt0 RV32 (set sp, jump main)
│   ├── sections.lds                         <- linker (128 KB RAM at 0x0)
│   ├── split_hex.py                         <- bin → 4 byte-lane hex
│   ├── Makefile                             <- riscv64-unknown-elf-gcc
│   ├── firmware.elf / .bin / .hex           <- output sau make
│   └── firmware_byte{0..3}.hex              <- ⭐ feed vào $readmemh trong SoC top
├── third_party/
│   └── tflm_tree/                           <- generated TensorFlow Lite Micro tree, ignored by git
├── sim/
│   ├── power_tb.sv                           <- Questa testbench, dump VCD theo LED marker
│   ├── run_power_vcd.sh                      <- build/run 6 power scenarios, auto-restore firmware
│   └── README.md                             <- ⭐ Hướng dẫn VCD + Quartus Power Analyzer
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
   `FPGA_TIMING_MODE=1` và compile đạt timing. Image 128 KB/TFLM hiện
   cần `PLACEMENT_EFFORT_MULTIPLIER=4.0` để đóng chậm 85C:
   - Worst setup slack: `+0.337 ns`
   - Worst hold slack: `+0.374 ns`
4. **Verify bằng smoke firmware**: đã có test nhỏ cho cả 6 operation trong
   `firmware/main.c`, dùng wrapper `.insn` ở `firmware/ai_ops.h`.
5. **Trạng thái Phase 2 sau benchmark UART**:
   - Resource current PULP+UART+128KB BRAM/TFLM image: khoảng 13,109 LE,
     2,614 regs, 2,097,152 memory bits, 16 embedded 9-bit multiplier elements.
   - Baseline cũ `COREV_PULP=0, FPU=0`: khoảng 7,744 logic cells, 64 RAM
     segments, 8 DSP.
   - Tự subset nhỏ hơn có thể tiết kiệm tài nguyên, nhưng hiện chưa ưu tiên
     vì bản full PULP đã verify tốt trên kit.

### Phase 3 — Benchmark, chuẩn hoá toolchain/gem5, và cân nhắc custom RTL

Hiện tại 6 operation đã được map lên CORE-V/PULP có sẵn. Chưa cần vội tự
thêm opcode riêng nếu mục tiêu trước mắt là chạy model thực tế và đo lợi ích.

Đầu việc tiếp theo nên đi theo thứ tự:

1. **Viết benchmark hiệu năng nhỏ**:
   - ✅ Baseline C thuần.
   - ✅ Bản dùng wrapper `ai_ops.h`.
   - ✅ Đo `mcycle`/`minstret` quanh kernel.
   - ✅ Ba kernel hiện tại:
     `MAC+Clamp`, `Dot4_acc+Clamp`, `Dot4_acc+Clamp+p.lw+lp.setup`.
2. **Tách smoke test và benchmark**:
   - ✅ `main.c` giữ smoke test.
   - ✅ `benchmark.c` là firmware benchmark riêng, chọn bằng `make benchmark`.
3. **Kết quả benchmark thực tế mới nhất đã quan sát trên UART**:
   - `mac_clamp`: baseline 281,412 cycles, custom 263,212 cycles,
     speedup 1.07x, checksum `0x06535320`.
   - `dot4_acc_clamp`: baseline 496,830 cycles, custom 78,516 cycles,
     speedup 6.33x, checksum `0x9587070e`.
   - `dot4_plw_lp_clamp`: baseline 496,830 cycles, custom 37,938 cycles,
     speedup 13.10x, checksum `0x9587070e`.
4. **Chuẩn hoá lại gem5 sau khi kit chạy ổn**:
   - `clamp` nên đồng bộ về register upper-bound `[0, ub]`.
   - `p.lw` nên quyết định theo semantics CORE-V hardware: load tại base,
     sau đó post-increment bằng immediate.
   - Thống kê gem5 O3 có thể khác hardware pipeline; trước mắt chỉ cần
     functional match, performance model sẽ chỉnh sau.
5. **Cài CORE-V-aware toolchain**:
   - Mục tiêu là dùng mnemonic hoặc builtin thay `.insn` thủ công.
   - Khi đổi toolchain phải objdump kiểm tra encoding không đổi.
6. **Cân nhắc tắt PULP và tự build subset riêng**:
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

### Phase 4 — UART, Power Analyzer, và LCD

UART TX đã được thêm trực tiếp vào SoC wrapper, dùng pin map từ
`/home/duydonv/de2i_150_test/`:

- UART TX 115200 8N1 qua DB9 RS-232 để in benchmark/TFLM reports.
- UART RX 115200 8N1 đã có MMIO `0x0200_0008` + FIFO 16 byte + sticky
  overrun/frame-error status. `make rx_smoke` và `rx_smoke_runner.py` đã pass;
  manual `picocom` hex-write frame cũng pass.
- `tflm_tiny_uart` dùng lại frame RX để chạy một sample nhỏ qua TFLM ref và
  `pulp_opt`, trả một dòng `OK/ERR`, tránh xung đột với report TX lặp.
- Runtime input streaming/loader cho model FC lớn nên dùng lại protocol RX này
  sau khi small-model UART path pass trên board.
- LCD 1602 chỉ thêm nếu cần demo độc lập không dùng laptop.

Power-analysis flow hiện tại:

- `sim/run_power_vcd.sh --list` liệt kê 6 scenario:
  baseline/custom cho 3 benchmark.
- Chạy không tham số sẽ build firmware POWER_SIM cho từng scenario, chạy
  Questa, tạo từng VCD trong `sim/vcd/`, rồi restore lại firmware benchmark
  UART bình thường.
- VCD có thể rất lớn; nên chạy từng scenario nếu thiếu dung lượng.
- Khi add VCD vào Quartus Power Analyzer, chọn `VCD file`, Entity là
  `de2i150_cv32e40p_top`.
- So sánh báo cáo nên dùng cả average power và energy:
  `energy = average_power * cycles / 50_000_000`.

### Phase 5 — Tiny AI pre-TFLM + TFLM bring-up

- ✅ Chạy `tiny_ai` với exported 8x8 quadrant MLP INT8:
  baseline C vs CORE-V/PULP wrappers, checksum match, accuracy `8/8`,
  speedup `7.09x` trên kit.
- ✅ Tăng local BRAM/linker/splitter lên 128 KB để chứa runtime TFLM nhỏ.
- ✅ Vendor generated TensorFlow Lite Micro tree và build được
  `tflm_hello` trên host với xPack C++ toolchain.
- ✅ Re-run Quartus full compile với 128 KB BRAM + TFLM hex:
  `output_files/de2i150_cv32e40p_top.sof`, 0 errors, 84 warnings,
  slow-85C setup slack `+0.337 ns`, hold slack `+0.374 ns`.
- ✅ Nạp `.sof` và capture UART output của `tflm_hello`: checksum
  `0x3a357ded`, outputs int8 `89 125 93 4`, status `0x00000000`, pass yes.
- ✅ Đặt xPack toolchain vào `/home/duydonv/tools` và upstream TFLM clone vào
  `/home/duydonv/src/tflite-micro` để tránh phụ thuộc `/tmp`.
- ✅ Đã chuyển sang model TFLM tiny MLP tương tự `tiny_ai.c` bằng reference
  kernel trước: firmware `tflm_tiny_ai`, model data sinh từ
  `tiny_ai_model.h`, chỉ dùng builtin `FullyConnected`.
- ✅ Build/link firmware pass với size `text=47048 data=292 bss=8576
  dec=55916`, model flatbuffer `2288` byte.
- ✅ Full Quartus compile pass cho image `tflm_tiny_ai`: `.sof` mới tại
  `output_files/de2i150_cv32e40p_top.sof`, 0 errors, 84 warnings, slow-85C
  setup slack `+0.337 ns`, hold slack `+0.374 ns`.
- ✅ Board UART run cho `tflm_tiny_ai` đã pass trên kit: checksum
  `0xc5f79430`, cycles `167327`, instret `118203`, cycles/sample `20915`,
  cycles/MAC `19.22`, classes `0 0 1 1 2 2 3 3`, sample0 scores `40 0 0 0`,
  accuracy `8/8`, pass yes.
- ✅ Đã thêm ref-vs-opt fixed-sample path vào `tflm_tiny_ai`: `tflm_ref` chạy
  official TFLM `FullyConnected`, `pulp_opt` dùng `cv.sdotsp.b`, `cv.lw`,
  `cv.setup`, `cv.clipur`, và giữ TFLM requantization để target bit-exact
  checksum `0xc5f79430`.
- ✅ Build/link firmware ref-vs-opt pass với size `text=49880 data=292
  bss=8616 dec=58788`.
- ✅ Full Quartus compile pass cho image ref-vs-opt: `.sof` mới tại
  `output_files/de2i150_cv32e40p_top.sof`, 0 errors, 84 warnings, slow-85C
  setup slack `+0.337 ns`, hold slack `+0.374 ns`.
- ✅ Board UART run cho image ref-vs-opt đã pass trên kit: `tflm_ref`
  cycles `167507`, instret `118343`, cycles/MAC `19.24`; `pulp_opt`
  cycles `29620`, instret `20864`, cycles/MAC `3.40`; checksum hai path đều
  `0xc5f79430`; class/score mismatches `0`; accuracy `8/8`; speedup `5.66x`;
  `Overall pass: yes`.
- ✅ UART RX smoke test đã pass độc lập với TFLM: framed payload/checksum qua
  `rx_smoke_runner.py` và manual `picocom` đều nhận `OK`, status
  `0x00000001`.
- ✅ Đã thêm `tflm_tiny_uart` để nhận runtime input cho small TFLM model:
  host-build pass, firmware `text=49536 data=292 bss=8668 dec=58496`; board
  run pass với ping + 8 inference frames, classes `0 0 1 1 2 2 3 3`,
  `mismatches=0`, `rx_status=0x00000001`, speedup khoảng `5.61x`.
- ✅ Đã chuẩn bị host artifact cho model MNIST FC `784 -> 32 -> 10`:
  `firmware/mnist_fc/mnist_fc_int8.tflite` là source of truth, kèm
  `mnist_fc_model_data.{cc,h}`, `mnist_fc_test_vectors.h`, metadata và script
  tái tạo. Graph có 2 op `FULLY_CONNECTED`; input/weight/hidden/output là
  INT8, bias INT32. Host verify INT8 accuracy `96.28%`, checksum
  `0x7c33a8dc`, 32-vector checksum `0x00cb95fc`.
- ✅ Board run reference-only của `tflm_mnist_fc` đã pass theo UART capture:
  checksum `0x00cb95fc`, label matches `31/32`, expected-class `32/32`,
  score mismatches `0`, cycles `11171144`, instret `7711381`.
- ✅ Đã nâng `tflm_mnist_fc` lên ref-vs-opt fixed-vector: `tflm_ref` vẫn chạy
  official TFLM `FullyConnected`; `pulp_opt` đọc weight/bias/scale/zero-point
  từ cùng flatbuffer model, dùng `cv.sdotsp.b` dot4 và per-channel TFLite
  requant. Lỗi setup trước đó (`0xe036`, cả ref/custom đều in 0) do dùng
  `GetTensor()` để lấy constant tensor trong TFLM Micro; bản hiện tại đọc
  weight/bias trực tiếp từ flatbuffer `Buffer`.
- ✅ Đã thêm fast path FC1x4 aligned `cv.lw` + `cv.setup` cho MNIST dot4
  kernel. FC1 xử lý 4 hidden output mỗi tile; loop body dùng 1 `cv.lw`
  activation, 4 `cv.lw` weight và 4 `cv.sdotsp.b`. Nếu alignment không phù
  hợp thì fallback về path scalar `dot4_i8()`/`pack4_i8()` cũ. FC2 vẫn dùng
  path scalar aligned `cv.lw` + `cv.setup`. Clamp vẫn giữ scalar signed
  TFLite-compatible path.
- ✅ Board UART run ref-vs-opt của `tflm_mnist_fc` đã pass: ref
  `11172961` cycles / `7687470` instret, opt `901789` cycles / `624902`
  instret cho validated run; checksum hai path đều `0x00cb95fc`, label
  matches `31/32`, expected-class `32/32`, class mismatches `0`, score
  mismatches `0`, speedup `12.39x`, `Overall pass: yes`.
- ✅ Cùng firmware hiện in thêm inference-only timing để phục vụ báo cáo:
  ref `10817077` cycles / `7493516` instret, opt `874897` cycles /
  `608100` instret, speedup `12.36x`. Đây là pass đo riêng quanh
  `Invoke()`/`mnist_fc_infer_opt_one()`, không tính input handling,
  checksum/argmax/mismatch bookkeeping.
- ✅ Build pass với xPack:
  `text=107012 data=292 bss=14428 dec=121732`, `firmware.bin=107308` byte.
  Full Quartus compile pass ngày 07/06/2026: 13,381 LE, 2,793 regs,
  2,097,152 memory bits, 16 DSP; slow-85C setup slack `+0.256 ns`, hold
  `+0.375 ns`. `.sof` checksum trong assembler report: `0x022DD42A`.
- ⏭️ Bước tiếp theo là thêm UART RX frame 784 byte cho MNIST input runtime
  hoặc đánh giá hướng model/quantization unsigned để tận dụng `cv.clipur`.

## 6. Cách verify mọi thứ vẫn work khi resume

Mỗi khi quay lại dự án sau thời gian dài, chạy 3 lệnh sau để confirm:

```bash
# 1. Kiểm tra patch còn nguyên
cd /home/duydonv/de2i150_cv32e40p_soc
grep -c "Quartus Lite" rtl/core/cv32e40p_*.sv rtl/core/include/cv32e40p_fpu_pkg.sv
# Phải ra ≥8 hit (mỗi file patch có ít nhất 1 comment đánh dấu)

# 2. Build firmware smoke/tiny/TFLM
cd firmware && make smoke
make tiny_ai
make tflm_hello CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
make tflm_tiny_ai CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
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
| `firmware/BENCHMARKS.md` | ⭐ Mô tả 3 benchmark, số phép toán, UART table, LED status |
| `sim/README.md` | ⭐ Hướng dẫn tạo VCD bằng Questa và dùng Quartus Power Analyzer |
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
| Power Analyzer báo invalid entity `tb_power.u_dut` | Ô Entity của Power Analyzer chỉ nhận top-level entity đã fit trong Quartus | Dùng Entity `de2i150_cv32e40p_top`; `tb_power` chỉ là testbench của Questa |
| TFLM không compile với Ubuntu `riscv64-unknown-elf-g++` | Lỗi thiếu C++ standard headers như `<utility>` | Dùng xPack `riscv-none-elf-g++` hoặc toolchain bare-metal có libstdc++ |
| TFLM image vượt 32 KB | `split_hex.py` báo firmware larger than memory hoặc link fail | Giữ BRAM/linker/splitter ở 128 KB: `BRAM_AW_WORDS=15`, `LENGTH=0x20000`, `WORDS=32768` |

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
| `/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/` | C++ bare-metal toolchain khuyến nghị để build `tflm_hello`/`tflm_tiny_ai` lâu dài |
| `/home/duydonv/src/tflite-micro/` | Optional upstream TFLM clone, chỉ cần khi regenerate/update `third_party/tflm_tree` |

## 10. OpenHW community context

- Issue tham chiếu: https://github.com/openhwgroup/cv32e40p/issues/1050
  (cùng vấn đề Quartus 25.1 SC Lite, OpenHW từ chối merge fix upstream).
- Vì vậy patch của dự án này phải **giữ tại local**, không hy vọng pull
  about upstream merge để loại bỏ.
- Nếu sau này upstream thay đổi nhiều, có thể cân nhắc chuyển sang `sv2v`
  workflow (tham khảo so sánh trong chat history).
