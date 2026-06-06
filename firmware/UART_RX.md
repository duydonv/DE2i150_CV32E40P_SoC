# UART RX smoke test

This mode verifies the board UART receive path before it is used for runtime
model inputs or a loader. It is intentionally separate from TFLM.

The same frame envelope is reused by `tflm_tiny_uart` for runtime inference.
That firmware should be driven as a request/response protocol: send one frame,
wait for one `OK`/`ERR` line, then send the next frame.

## MMIO

UART stays at the existing MMIO base:

| Address | Access | Meaning |
|---|---|---|
| `0x0200_0000` | read | UART status |
| `0x0200_0000` | write | clear sticky RX error bits written as `1` |
| `0x0200_0004` | write | UART TX byte |
| `0x0200_0008` | read | UART RX byte, pops one byte when RX is valid |

Status bits:

| Bit | Name | Meaning |
|---:|---|---|
| 0 | `TX_READY` | TX shifter can accept one byte |
| 1 | `RX_VALID` | RX FIFO has at least one byte |
| 2 | `RX_OVERRUN` | RX byte arrived while the 16-byte FIFO was full |
| 3 | `RX_FRAME_ERROR` | RX stop bit was not high |

## Firmware

Build the RX smoke firmware:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make rx_smoke
```

Then re-run Quartus compilation so `$readmemh` picks up the new
`firmware_byte{0..3}.hex` files, program the `.sof`, and connect a real
RS-232 adapter to the DE2i-150 DB9 connector.

LED status while running:

| LEDR[7:0] | Meaning |
|---|---|
| `0x70` | init |
| `0x71` | waiting for frame sync |
| `0x72` | receiving/checking a frame |
| `0xa5` | last frame passed |
| `0xef` | last frame failed |

## Frame Protocol

Host-to-board frame:

```text
55 aa cmd len_lo len_hi payload checksum_le32
```

The initial command is `cmd = 0x01`. Maximum payload length is 512 bytes.
Checksum is FNV-1a over:

```text
cmd, len_lo, len_hi, payload bytes
```

The board prints one line per valid sync:

```text
OK seq=1 cmd=0x01 len=64 checksum=0x........ cycles=.... status=0x00000001 first=0x.. last=0x..
```

or:

```text
ERR seq=1 code=0x........ expected=0x........ received=0x........ status=0x........
```

`status=0x00000001` is the normal idle success status: TX is ready and no RX
error bit is set. Bit 1 may be set if the host has already queued bytes for
the next frame.

## Host Script

Run the scripted test:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
python3 rx_smoke_runner.py /dev/ttyUSB0
```

The default sends payload lengths:

```text
0, 1, 16, 64, 255, 512
```

For a longer soak test:

```bash
python3 rx_smoke_runner.py /dev/ttyUSB0 --repeat 20 --lengths 1,16,64,255,512
```

The script requires `pyserial`. If it is missing, install it in the host Python
environment before running the script.

## Manual Picocom Test

Open the serial terminal:

```bash
sudo picocom -b 115200 /dev/ttyUSB0
```

After the board prints `Ready`, use picocom's hex-write command:

```text
Ctrl-A Ctrl-W
```

Then paste one of these frames at the `*** hex:` prompt and press Enter:

```text
# empty payload
55 aa 01 00 00 6c 44 ca 0b

# payload: "A"
55 aa 01 01 00 41 fc 02 50 f9

# payload: "ABCD"
55 aa 01 04 00 41 42 43 44 d4 87 d3 f2

# payload: "hello"
55 aa 01 05 00 68 65 6c 6c 6f 9f 2d 5a f8

# bad checksum, expected to print ERR
55 aa 01 01 00 41 00 00 00 00
```

Use `Ctrl-A Ctrl-X` to exit picocom.

## TFLM Tiny UART Commands

`tflm_tiny_uart` uses the same envelope but different command IDs:

| Command | Payload | Meaning |
|---:|---|---|
| `0x10` | 0 bytes | ping |
| `0x11` | 64 bytes | run one small-model input sample |

Build and test:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
make tflm_tiny_uart CROSS=/home/duydonv/tools/xpack-riscv-none-elf-gcc/xpack-riscv-none-elf-gcc-14.2.0-3/bin/riscv-none-elf-
cd ..
/home/duydonv/altera_lite/25.1std/quartus/bin/quartus_sh --flow compile de2i150_cv32e40p_top
```

After programming the `.sof`:

```bash
cd /home/duydonv/de2i150_cv32e40p_soc/firmware
python3 tflm_tiny_uart_runner.py /dev/ttyUSB0
```

Expected inference responses contain `pass=yes`, equal `ref_cls`/`opt_cls`,
and `mismatches=0`.

Current board result: one ping plus 8 inference frames pass, expected classes
`0 0 1 1 2 2 3 3`, `mismatches=0`, `rx_status=0x00000001`, and about `5.61x`
small-model speedup.

LED status while running `tflm_tiny_uart`:

| LEDR[7:0] | Meaning |
|---|---|
| `0x80` | init |
| `0x81` | TFLM setup |
| `0x82` | ready/waiting for the next RX frame |
| `0x83` | receiving/checking a frame |
| `0x84` | running TFLM reference inference |
| `0x85` | running optimized inference |
| `0xa5` | last handled frame passed |
| `0xef` | last handled frame failed, or setup failed |

After a successful runner pass, the firmware returns to `0x82` because it is
idle and waiting for another frame.
