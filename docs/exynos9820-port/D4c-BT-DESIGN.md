# D4c — Bluetooth on the S10e: design

The BT function of the same **BCM4375** the WLAN work (M12/D4b) drives over PCIe,
reached instead through a dedicated **UART**: H4 framing + Broadcom patchram, exposed
to Windows as a **BthxDDI transport** so the in-box stack (`bthport.sys`) binds it.
Research record: [`notes/bt-driver.md`](../../notes/bt-driver.md) (B1-B4). Constants:
`Exynos9820.h` "Bluetooth" block. This file is the plan of record for the code.

## 1. Definition of done + rung ladder

Nothing is Ready without device validation (ground rule 7). Rungs:

| Rung | Observation | Where |
|---|---|---|
| R1 | BT UART echoes: TX FIFO -> RX FIFO at 115200 (loopback or bus scope-free self-test via RTS/CTS wrap if needed) | UEFI |
| R2 | ROM HCI answers: `HCI_READ_LOCAL_VERSION (0x1001)` gets a Command Complete over the UART | UEFI |
| R3 | patchram-up: 0xFC2E -> .hcd replay -> 0xFC4E -> version read again shows the **patched** LMP subver | UEFI or Windows |
| R4 | Windows enumerates `ACPI\SAMS9826`, driver starts, bthport binds the PDO, radio shows in Settings | Windows |
| R5 | LE scan finds devices; pair + connect | Windows |
| R6 | (later) S3 wake via hostwake, baud switch, SCO/PCM audio | Windows |

The Windows half is only *safe to trust* after R1-R3 are seen on the device; the UEFI
half exists to make those observations cheap.

## 2. Hardware contract (all VERIFIED — reads in notes B3)

| Item | Value |
|---|---|
| UART | `uart@10840000` (uart1, PERIC1), s3c/Exynos register map, fifo 256, USI v2 quirks at +0xC4/+0xC8 |
| USI mode sel | `0x10821000` (4 B) = **1** for UART |
| UART IRQ | GSIV **440** (DT cell = SPI 408; +32 — corrected 2026-10-13, was 408) |
| CMU_PERIC1 ids | gate **0x22a** (`gate_uart_clk1`), ipclk **0x23c** (`ipclk_uart1`) |
| Pins | gpq0-0..3 function 2 (ALIVE block); RTS also as GPIO (gpq0-2 fn 1); bt_wake gpg2-0 (PERIC0); bt_hostwake gpa2-3 (ALIVE, EINT19, GSIV 56) |
| bt_en | PCAL6524 expander @0x22 on hsi2c@10930000, pin 7; expander reset gpg4-0 — **variant-dependent, Q1 open** |
| Patch files | `/vendor/firmware/bcm4375B1_semco.hcd` (and siblings) = raw HCI command stream, 1260x `0xFC4C` + `0xFC4E` |
| Module | SEMCO E43 (BCM4375 B1 baseline strings); 37.4 MHz XTAL on the chip side |

## 3. Two halves — who owns what

**UEFI half (`Beyond0LteBtUartDxe`, small):** the same role split M12 settled for
WLAN — firmware latches the *role* (clocks, pad mux, USI mode), the OS gets the
*device*. Steps: CMU_PERIC1 QCH + gate/ipclk ungates (ids above; same gate-math
pattern as CMU_FSYS1 in `notes/m12-wifi-chip.md`), write USI mode=1, mux gpq0-0..3
fn2 + gpg2-0 out + gpa2-3 in, release USI reset (USI_CON 1->0), USI_OPTION CLKREQ_ON.
Then rungs R1/R2 print `[BT] R2: ver <…>` on the debug console. **No production
dependency**: if this DXE is dropped, Windows must redo those steps — the Windows
driver therefore re-asserts USI_CON/OPTION (they live inside the UART window it owns)
but not CMU/pinctrl (not in its `_CRS` — role-latch, same contract as WLAN/XHC0).

**Windows half (`drivers/Exynos9820Bt`):** KMDF function driver, modeled on the WDK
`bluetooth/serialhcibus` sample (MS-PL) with `worproject/cywbtserialbus` (MS-PL) as
the Broadcom-specific prior art. **Decision: direct MMIO UART, no Windows serial
stack** — the Samsung UART is not 16550-compatible and no in-box function driver
exists for it; cywbtserialbus's `SERIAL_BAUD_RATE`-based lower edge is not available
to us. The driver maps the UART window from `_CRS` descriptor 0 and drives ULCON/
UCON/UFCON/UBRDIV/FIFO registers itself (register semantics: our own
`Exynos9820SerialPortLib` + vendor `serial_s3c.h`).

## 4. Windows driver layering

1. **Bind/resources** — `ACPI\SAMS9826`; `_CRS` contract: desc 0 = UART MEM,
   desc 1 = GpioInt (hostwake), Interrupt entry = UART GSIV 440.
2. **UART engine** — init at 115200 8N1; RX via ISR + ring; TX via FIFO-drain ISR.
   Flow control: UFCON RTS/CTS enable is a **rung** (which of gpq0-1..3 are the
   USI HW flow pins is UNVERIFIED; H4 requires it eventually — hci_bcm refuses
   without it).
3. **H4 framing** — type byte 01/02/04 + HCI lengths (command/ACL/event). This is
   the whole transport protocol; no H5/retry in slice 1.
4. **BCM bring-up** — power assumed on (Q1); read local version; if RAM patch
   needed: 0xFC2E, 50 ms, replay the `.hcd` (file in the driver package, copied
   from the device — never committed, same blob policy as M12), 250 ms; optional
   baud switch behind a registry knob (`BaudRate`, default **off/115200**, Q2):
   0xFC45 clock + 0xFC18 baud then re-clock our own UART.
5. **BthxDDI PDO** — one HCI function; `IOCTL_BTHX_SET/GET_VERSION`,
   `QUERY_CAPABILITIES` (LE+ACL, no SCO in slice 1), `READ_HCI`/`WRITE_HCI`
   pumping the H4 engine; hostwake GpioInt arms the RX pump.

## 5. License posture

- `serialhcibus` + `cywbtserialbus`: **MS-PL** — code may be adopted with notices;
  our skeleton starts from the sample's structure, keeping copyright headers where
  code is taken.
- `hci_bcm.c` / `btbcm.c` (GPL-2.0): **behavioral reference only** — opcodes and
  sequences are facts, no code or text copied.
- `bcm43xx.c` (GPL-2.0): reference for GPIO roles and power timing only.
- Patch blobs: device-copied, untracked (M12 policy).

## 6. Risks / open questions (each names its closing read)

- **Q1** bt_en line owner on our firmware (PCAL6524 pin 7 vs gpm8 vs nobody) —
  live `/proc/device-tree/bluetooth/gpios` + active-overlay id (TWRP root read).
  Slice 1 assumes the chip is already powered; R2 answers immediately either way.
- **Q2** oper baud for this module — first runs stay at 115200; 3 Mbaud switch is a
  registry-gated experiment (0xFC45+0xFC18).
- **Q3** does the ROM answer HCI pre-patch — rung R2 decides; if not, patchram is
  mandatory before any HCI (minidriver flow already in the .hcd).
- **Q4** flow-control pin mapping on gpq0-1..3 — UNVERIFIED; R1 loopback + vendor
  `s3c2410` driver read closes it. Without it, H4 is lossy at any real load.
- **Q5** pad retention: `pin-con-pdn=3` on gpg2-0 says the pad changes state in
  sleep; S3 resume re-init is a rung, not assumed.
- **Q6** bthport binding to a non-USB transport on Win11 ARM64 — serialhcibus is
  the documented path (R4 is the observation).
