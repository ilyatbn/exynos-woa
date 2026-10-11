# M17 - MAX77705 MUIC COM switch (D+/D- routing) research

Hypothesis: MAX77705 internal USB-C D+/D- switch not routed to AP USB PHY under our firmware.

## Progress log

### F1 (2026-10-11) Sources fetched
LineageOS android_kernel_samsung_exynos9820 lineage-18.1, raw base
`https://raw.githubusercontent.com/LineageOS/android_kernel_samsung_exynos9820/lineage-18.1/`
- include/linux/ccic/max77705.h:505-536 - mailbox: OPCODE_WRITE 0x21, OPCODE_WRITE_END 0x41,
  OPCODE_READ 0x51, OPCODE_SIZE 1, OPCODE_DATA_LENGTH 32. Opcodes BCCTRL1_R 0x01/W 0x02,
  BCCTRL2_R 0x03/W 0x04, CTRL1_R 0x05 / CTRL1_W 0x06, CCCTRL1_R 0x0B/W 0x0C ...
- include/linux/mfd/max77705-private.h:160ff - "Slave addr = 0x4A: USBC" (8-bit; = our 7-bit 0x25).
  AP_DATAOUT0..32 = 0x21..0x41, AP_DATAIN0.. = 0x51.. ; status regs UIC_INT 0x02, USBC_STATUS1 0x06,
  USBC_STATUS2 0x07, BC_STATUS 0x08, UIC_INT_M 0x0e.

### F2 COM switch = opcode-only (CONTROL1, via USBC firmware), no direct register
include/linux/muic/max77705-muic.h:62-69 COMMAND_CONTROL1_READ 0x05 / _WRITE 0x06.
max77705-muic.h:267-275 + 333-379: CONTROL1 byte = NoBCComp[7] | RCPS[6] | COMP2SW(D+)[5:3] | COMN1SW(D-)[2:0].
Field values: 001 USB(AP), 010 audio, 011 UART(AP), 100 USB_CP, 101 UART_CP, 111 open
(header comment says 000 open, enum uses 7). NoBCComp 0 = "compare with BC1.2", 1 = "ignore BC1.2, manual control".
Computed bytes: COM_USB = 0x09, COM_OPEN = 0x3F, COM_UART = 0x9B, COM_USB_CP = 0xA4, COM_UART_CP = 0xAD.

### F3 kernel write path
drivers/muic/max77705-muic.c:315-338 max77705_switch_path(): opcode 0x06, write_length 1, data[0]=reg_val,
read_length 0 -> max77705_usbc_opcode_write (queued).
drivers/ccic/max77705_usbc.c:1706-1745 max77705_i2c_opcode_write: bulk write to slave USBC reg 0x21
of [opcode, data...] then single write 0x41 <- 0x00 (end marker) if length < 32.
max77705_usbc.c:1757-1785 opcode_read: bulk read from 0x51, length+1 bytes ([0] = opcode echo, [1..] = data).
com_to_usb_ap (muic.c:352-365) writes COM_USB.

### F4 attach behaviour
muic.c vps table 187-193: USB (ADC open, VBUS high, CHGTYP_USB=1) -> COM_USB; CDP, DCD timeout, OTG -> COM_USB;
TA/DCP/AFC -> COM_OPEN.  handle_attach muic.c ~1405-1408: USB/CDP/JIG_USB -> max77705_muic_attach_usb_path ->
com_to_usb_ap (explicit write of 0x09). TA -> write_vps_regs (which no longer writes: #if 0 at 437-443).
Probe (muic.c:2431) only sets the sw state variable switch_val = COM_OPEN (no I2C write there).
**Shutdown (muic.c:2592) calls com_to_open() -> CONTROL1 = 0x3F.** So after a Linux/TWRP reboot the switch
may be left OPEN unless something (sboot / USBC FW reset) re-routes it. KEY FOR US.

### F5 opcode handshake
max77705.h (ccic) :81-109: REG_UIC_INT_M 0x0E (init value 0x04), UIC_INT (0x02) bit7 APCmdResI = "AP command response",
bit5 VBUSDetI, bit3 DCDTmoI, bit1 CHGTypI. REG_USBC_STATUS2 0x07 = SYSMsg. BC_STATUS 0x08: [7] VBUSDet,
[5:3] PrChgTyp, [2] DCDTmo, [1:0] ChgTyp (0 none, 1 SDP, 2 CDP, 3 DCP).
Kernel sequencing (max77705_usbc.c:2133ff cmd_run, :2802 apcmd_irq, :1813 irq_execute): write [op,data] @0x21 +
0x00 @0x41; FW raises APCmdResI; IRQ handler bulk-reads 0x51 (len+1), data[0] must echo the opcode;
only then the next queued opcode is sent. For CTRL1_W read_length=0 (response = echo only).
max77705.c:543-563 (mfd, jig_on) does the same raw sequence without any wait: proof a bare write works.

### F6 BC1.2 controls
muic.h:243-265: BC_CTRL1 (op 0x01 R / 0x02 W): [7] DCDCpl, [6] UIDEN, [5] NoAutoIBUS, [4] 3ADCPDet,
[1] CHGDetMan, [0] CHGDetEn.  BC_CTRL2 (0x03/0x04): [5] DNMonEn, [4] DPDNMan, [3:2] DPDrv, [1:0] DNDrv.
max77705_muic_enable_chgdet (muic.c:530) sets CHGDetEn|CHGDetMan (manual re-run of detection);
dp_reset (muic.c:515) sets DPDNMan=1, DPDrv=00 (D+ to GND, used on DCD timeout before SYSTEM_RUNNING).
No "CHGDETRUN" in this part; detection result arrives as CHGTypI with BC_STATUS.ChgTyp.

### F7 confirmation + read-back
max77705_usbc.c:1436-1475 (fw_update sysfs debug hook, Maxim's own test): OPCODE_CTRL1_R (0x05) read_length 1;
OPCODE_CTRL1_W (0x06) write_data[0] = **0x09** -> confirms 0x09 = USB-to-AP (D+ and D- both 001).
So CONTROL1 is readable: write 0x05 @0x21, 0x00 @0x41, wait APCmdResI, read 2 bytes @0x51 -> [0x05, CONTROL1].
max77705_usbc.c:1511 init_opcode at probe only: disable auto VBUS, (audio), alt mode. No CTRL1 write at usbc probe.
muic.c:2193-2200 init_detect at probe: bulk-reads USBC_STATUS1..(5 bytes from 0x06) and runs detect_dev ->
if BC_STATUS.ChgTyp=SDP(1) & VBUSDet -> handle_attach -> com_to_usb_ap (write 0x09). So under TWRP the
kernel itself explicitly wrote 0x09 (attached_dev=USB). The switch is driver-set, not FW-automatic
(INFERRED: FW may also auto-route when NoBCComp=0, header says "0: Compare with BC1.2").
max77705_usbc.c:2460ff uic_op_send_work (debug/test list): BCCTRL1_W 0x20 (NoAutoIBUS), BCCTRL2_W 0x10 (DPDNMan).

### F8 shutdown/reboot path (strong support for the hypothesis)
max77705_usbc.c:3689-3730 max77705_usbc_shutdown (platform .shutdown, runs on every kernel reboot):
max77705_muic_shutdown -> com_to_open (CONTROL1=0x3F, muic.c:2592) ; mask all USBC INT_M = 0xFF;
then (unless water/RID) max77705_reset_ic = write USBC reg 0x80 <- 0x0F, msleep 100 (usbc.c:2520-2524);
INT_M again 0xFF; read UIC_INT to clear. So after TWRP/Android reboot the CCIC FW is reset with all
interrupts masked, and the COM switch is OPEN or whatever FW default is after that reset (UNKNOWN - the
CTRL1 read-back below closes it). Linux re-routes on the next boot only because muic init_detect
(muic.c:2193) writes 0x09 on SDP. Our UEFI never writes CTRL1 -> host likely never sees D+ pull-up.
max77705_usbc.c:2550-2600 on SYSMSG BOOT_POR / BOOT_WDT kernel does reset_ic, UIC_INT_M=0x04, CC 0x20, PD 0x00,
VDM 0xF0, read UIC_INT to clear, init_opcode - no CTRL1 write (switch only from muic attach).
Bulk I/O = i2c_smbus_{write,read}_i2c_block_data (max77705.c:132-145, 221-235): one transaction
[S addr|W, 0x21, op, data.. P]; [S W 0x41 0x00 P]; read [S W 0x51 Sr R n bytes P].
Our Pmic.c has no opcode helper yet; Hsi2cXfer(addr, FALSE, buf, n, TRUE) already supports n-byte writes.

### F9 recommended UEFI sequence (slave 0x25)
0. Optional: read UIC_INT (0x02) once to clear stale bits; log BC_STATUS (0x08), USBC_STATUS1/2 (0x06/0x07), UIC_INT_M (0x0E).
(a) Read: write [0x21, 0x05]; write [0x41, 0x00]; poll UIC_INT 0x02 bit7 (APCmdResI) <=100 ms (INFERRED: bit latches
    even when masked - kernel reads UIC_INT after masking 0xFF at shutdown); read 2 bytes from 0x51 -> expect [0x05, CTRL1].
    Decode: 0x09 USB-AP, 0x3F open, 0x9B UART-AP, 0xA4 USB-CP.
(b) Force: write [0x21, 0x06, 0x09]; write [0x41, 0x00]; poll APCmdResI; read 0x51 expect 0x06 echo; re-run (a) to verify 0x09.
Do it before DWC3 RUN_STOP; and ideally after BC_STATUS.ChgTyp != 0 (detection done) - INFERRED ordering.
Status: research only, nothing device-validated.
