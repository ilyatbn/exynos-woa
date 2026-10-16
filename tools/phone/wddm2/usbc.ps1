# usbc.ps1 - G18 H1: the max77705 USB-C block snapshot S10eBattery writes every poll (Device Parameters of
# ACPI\SAMS9826\0): UsbcRev (HW | FW << 8), UsbcSt0 (regs 0x06..0x09), UsbcSt1 (0x0A CC_STATUS0, 0x0B CC_STATUS1,
# 0x0C PD_STATUS0, 0x0D PD_STATUS1), plus PollOk/PollFail. Bytes are printed per register.
$k = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SAMS9826\0\Device Parameters'
$p = Get-ItemProperty $k
function B($v, $base) { 0..3 | ForEach-Object { '0x{0:X2}=0x{1:X2}' -f ($base + $_), (($v -shr (8 * $_)) -band 0xFF) } }
"PollOk $($p.PollOk) PollFail $($p.PollFail) UsbcRev 0x{0:X4}" -f $p.UsbcRev
if ($p.UsbcSt0 -ne $null -and $p.UsbcSt0 -ne 0xFFFFFFFF) { (B $p.UsbcSt0 6) -join ' '; (B $p.UsbcSt1 10) -join ' ' } else { 'USB-C block not read' }
# Decode (vendor include/linux/ccic/max77705.h BIT_* in register order): BC_STATUS 0x08: VBUSDet b7, PrChgTyp b5:3,
# ChgTyp b1:0; CC_STATUS0 0x0A: CCPinStat b7:6 (0 none, 1 CC1, 2 CC2), CCIStat b5:4, CCVcnStat b3, CCStat b2:0
# (enum max77705_cc_pin_state: 0 none, 1 SINK, 2 SOURCE, 3 audio acc, 4 debug acc, 5 error, 6 disabled);
# CC_STATUS1 0x0B: CCSBUSHORT b7:6, VCONNOCP b5, VCONNSC b4, VSAFE0V b3, AttachSrcErr b2, ConnStat b1, Altmode b0;
# PD_STATUS0 0x0C: PDMsg; PD_STATUS1 0x0D: DataRole b7 (1 DFP), PSRDY b4, FCT_ID b3:0.
if ($p.UsbcSt0 -ne $null -and $p.UsbcSt0 -ne 0xFFFFFFFF) {
  $bc = ($p.UsbcSt0 -shr 16) -band 0xFF; $c0 = $p.UsbcSt1 -band 0xFF; $c1 = ($p.UsbcSt1 -shr 8) -band 0xFF
  $pd0 = ($p.UsbcSt1 -shr 16) -band 0xFF; $pd1 = ($p.UsbcSt1 -shr 24) -band 0xFF
  $cc = @('none','SINK','SOURCE','audio-acc','debug-acc','error','disabled','rfu')[$c0 -band 7]
  "VBUS {0} ChgTyp {1} | CCStat {2} pin CC{3} CCIStat {4} VCONN {5} | ConnStat {6} Altmode {7} VSAFE0V {8} | PDMsg 0x{9:X2} DataRole {10} PSRDY {11}" -f `
    (($bc -shr 7) -band 1), ($bc -band 3), $cc, (($c0 -shr 6) -band 3), (($c0 -shr 4) -band 3), (($c0 -shr 3) -band 1), `
    (($c1 -shr 1) -band 1), ($c1 -band 1), (($c1 -shr 3) -band 1), $pd0, $(if ($pd1 -band 0x80) { 'DFP' } else { 'UFP' }), (($pd1 -shr 4) -band 1)
}
