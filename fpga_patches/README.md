# CV32E40P — Patches for Quartus Prime Lite / Standard (SystemVerilog-2005)

Bối cảnh: CV32E40P được viết ở mức **SystemVerilog-2012/2017**, còn Quartus
Prime Lite / Standard Edition (bản 25.1 SC Lite của kit DE2i-150) chỉ parse
tới **SystemVerilog-2005**. Nếu add file gốc thẳng vào Quartus sẽ ra 29 lỗi
Verilog-HDL (`import` trong module header, `for (genvar i ...)`, `case ... inside`,
`x inside {ranges}`, v.v.).

Khi copy core vào `rtl/core/` mình đã patch trực tiếp lên bản copy của dự án
(upstream `/home/duydonv/cv32e40p/` **không bị sửa**). Dưới đây là danh sách
chính xác các chỗ đã sửa để ai đó sau này re-pull core có thể áp lại trong
vài phút.

Cả bộ đã được verify bằng:

- `vlog -sv` của Questa Altera Starter FPGA 2025.2 (0 error)
- `quartus_map` của Quartus Prime 25.1 SC Lite (0 error)

## Danh sách file bị patch

### 1. `rtl/core/cv32e40p_id_stage.sv`

**Chỗ 1** — module-header có 2 `import` liên tiếp (SV-2012 cho phép, SV-2005 thì không):

```verilog
// Trước
module cv32e40p_id_stage
  import cv32e40p_pkg::*;
  import cv32e40p_apu_core_pkg::*;
#(
  ...

// Sau: chỉ giữ 1 import trong header, re-import cái thứ 2 trong body
module cv32e40p_id_stage
  import cv32e40p_pkg::*;
#(
  ...
)(
  ...
);
  import cv32e40p_apu_core_pkg::*;   // moved here
```

**Chỗ 2** — inline `for (genvar i = 0; ...)` (SV-2012):

```verilog
// Trước (~line 901, trong nhánh APU=0 của generate)
end else begin : gen_no_apu
  for (genvar i = 0; i < APU_NARGS_CPU; i++) begin : gen_apu_tie_off
    assign apu_operands[i] = '0;
  end

// Sau: trực tiếp tie packed-array về 0 (APU=0 nên không cần vòng lặp)
end else begin : gen_no_apu
  assign apu_operands = '0;
```

### 2. `rtl/core/cv32e40p_ex_stage.sv`

Giống file `id_stage.sv`, chuyển 2nd `import cv32e40p_apu_core_pkg::*;`
từ module header vào ngay sau `);` của port list.

### 3. `rtl/core/cv32e40p_decoder.sv`

**Chỗ 1** — 3 `import` trong header (chỉ giữ 1, 2 cái còn lại đưa vào body):

```verilog
module cv32e40p_decoder
  import cv32e40p_pkg::*;
#(
  ...
)(
  ...
);
  import cv32e40p_apu_core_pkg::*;
  import cv32e40p_fpu_pkg::*;
```

**Chỗ 2–10** — 9 lần `unique case (X) inside` trở thành `casez (X)`:

```verilog
// Trước                              // Sau
unique case (expr) inside       ->    casez (expr)
```

Các case trong nhánh FPU của decoder có case-label `?` wildcard nên `casez`
vẫn match đúng; không cần `inside` cho ngữ nghĩa.

Với riêng block có range label `[3'b000:3'b100]`, mình liệt kê thủ công:

```verilog
casez (frm_i)
  3'b000, 3'b001, 3'b010, 3'b011, 3'b100 : ;
  default                                : illegal_insn_o = 1'b1;
endcase
```

**Chỗ 11–16** — 6 lần `x inside {[lo:hi], ...}` trong `if (...)` được viết
lại bằng phép so sánh hiển thị:

```verilog
// Trước
if (!(instr_rdata_i[14:12] inside {[3'b000:3'b010], [3'b100:3'b110]})) ...

// Sau
if (!((instr_rdata_i[14:12] <= 3'b010) ||
      (instr_rdata_i[14:12] >= 3'b100 && instr_rdata_i[14:12] <= 3'b110))) ...
```

```verilog
// Trước
if (!(instr_rdata_i[14:12] inside {[3'b000:3'b010]})) illegal_insn_o = 1'b1;

// Sau
if (instr_rdata_i[14:12] > 3'b010) illegal_insn_o = 1'b1;
```

### 4. `rtl/core/cv32e40p_cs_registers.sv`

Quartus Lite (SV-2005) đòi **bắt buộc** có `generate` / `endgenerate` bao
quanh `if`-generate hay `for`-generate ở module scope. Ba block `if (...)
begin : gen_...` được bọc lại:

```verilog
generate
if (PULP_SECURE == 1) begin : gen_pulp_secure_read_logic
  ...
end else begin : gen_no_pulp_secure_read_logic
  ...
end
endgenerate
```

Áp dụng cho 3 block: `gen_pulp_secure_read_logic`, `gen_pulp_secure_write_logic`,
`gen_trigger_regs`.

### 5. `rtl/core/cv32e40p_apu_disp.sv`

Hai vòng for-generate dùng inline `genvar`:

```verilog
// Trước
generate
  for (genvar i = 0; i < 3; i++) begin : gen_read_deps ...
endgenerate
generate
  for (genvar i = 0; i < 2; i++) begin : gen_write_deps ...
endgenerate

// Sau: declare genvar ra ngoài, đặt tên khác nhau để khỏi đụng
genvar gvr;
generate
  for (gvr = 0; gvr < 3; gvr = gvr + 1) begin : gen_read_deps ...
endgenerate

genvar gvw;
generate
  for (gvw = 0; gvw < 2; gvw = gvw + 1) begin : gen_write_deps ...
endgenerate
```

### 6. `rtl/core/cv32e40p_hwloop_regs.sv`

Vòng `for` ở module scope không có `generate` wrapper, lại không có named begin:

```verilog
// Trước
genvar k;
for (k = 0; k < N_REGS; k++) begin
  assign hwlp_counter_n[k] = hwlp_counter_q[k] - 1;
end

// Sau
genvar k;
generate
  for (k = 0; k < N_REGS; k = k + 1) begin : gen_hwlp_counter_dec
    assign hwlp_counter_n[k] = hwlp_counter_q[k] - 1;
  end
endgenerate
```

### 7. `rtl/core/cv32e40p_register_file_ff.sv`

Block `for`-generate thiếu tên (lỗi "this block requires a name" của Quartus):

```verilog
// Trước (~line 142, nhánh FPU=1)
for (l = 0; l < NUM_FP_WORDS; l++) begin
  always_ff @(posedge clk, negedge rst_n) begin : fp_regs ...

// Sau
for (l = 0; l < NUM_FP_WORDS; l = l + 1) begin : gen_fp_word
  always_ff @(posedge clk, negedge rst_n) begin : fp_regs ...
```

### 8. `rtl/core/include/cv32e40p_fpu_pkg.sv`

Enum literal thiếu width so với `enum logic [FP_FORMAT_BITS-1:0]` (3 bit).
Quartus Lite strict-check width này, mặc dù SV spec cho phép unsized `'d0`:

```verilog
// Trước
typedef enum logic [FP_FORMAT_BITS-1:0] {
  FP32    = 'd0,     // '32-bit -> 3-bit mismatch
  ...

// Sau
typedef enum logic [FP_FORMAT_BITS-1:0] {
  FP32    = 3'd0,    // explicit width
  ...
```

## Cách re-apply khi pull lại core

Tất cả patches trên đều là sửa cơ học trên file copy. Nếu sau này bạn
`cp -R /home/duydonv/cv32e40p/rtl/*  rtl/core/` lần nữa, chỉ cần mở 8 file
bên trên và chép lại các đoạn "Sau" là xong. Mình khuyến nghị giữ
patch này như **tài liệu** chứ không tự sinh diff, vì core có thể update
và diff có thể lệch ngữ cảnh.

## Tại sao không đổi setting Quartus là xong?

Quartus Prime 25.1 SC Lite **không** có tuỳ chọn `SYSTEMVERILOG_INPUT_VERSION`
tới SV-2012 — chỉ có Verilog-1995, Verilog-2001, SystemVerilog (2005).
Nếu muốn full SV-2012/17 bạn phải lên **Pro Edition** hoặc Standard Edition
phiên bản cao. Vì yêu cầu ban đầu là dùng đúng bản user đã cài (25.1 Lite),
patch là con đường nhanh và không tốn license.
