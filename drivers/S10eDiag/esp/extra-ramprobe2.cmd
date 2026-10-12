@echo off
rem S10eAgent extra step (m13 R1, notes/cpu-ram-plan-20261012.md plan #2): can Windows (EL1 under Samsung EL2) use
rem the regions our memory map still carves out? Per region: mark, read, write 0x5A5AA5A5, read back (no restore:
rem the regions are unused while the modem is never booted). A mark precedes every access, so an abort (bugcheck /
rem reset) names the address in the PRAM ring. debug_snapshot (0xFD900000..4 GiB) and the uH regions are not probed.
set R=%OUT%\last\ramprobe2.txt
echo ramprobe2 > "%R%"
s10ediag mark r2: rdx start 0x880000000
echo --- rdx start >> "%R%"
s10ediag peek 0x880000000 >> "%R%" 2>&1
s10ediag poke 0x880000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0x880000000 >> "%R%" 2>&1
s10ediag mark r2: rdx mid 0x885000000
echo --- rdx mid >> "%R%"
s10ediag peek 0x885000000 >> "%R%" 2>&1
s10ediag poke 0x885000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0x885000000 >> "%R%" 2>&1
s10ediag mark r2: rdx end page 0x88ACFF000
echo --- rdx end page >> "%R%"
s10ediag peek 0x88ACFF000 >> "%R%" 2>&1
s10ediag poke 0x88ACFF000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0x88ACFF000 >> "%R%" 2>&1
s10ediag mark r2: modem_spare 0xD0000000
echo --- modem_spare >> "%R%"
s10ediag peek 0xD0000000 >> "%R%" 2>&1
s10ediag poke 0xD0000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xD0000000 >> "%R%" 2>&1
s10ediag mark r2: modem_if2 0xD1600000
echo --- modem_if2 >> "%R%"
s10ediag peek 0xD1600000 >> "%R%" 2>&1
s10ediag poke 0xD1600000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xD1600000 >> "%R%" 2>&1
s10ediag mark r2: modem_if2 mid 0xD3000000
echo --- modem_if2 mid >> "%R%"
s10ediag peek 0xD3000000 >> "%R%" 2>&1
s10ediag poke 0xD3000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xD3000000 >> "%R%" 2>&1
s10ediag mark r2: cp2cp 0xD5800000
echo --- cp2cp >> "%R%"
s10ediag peek 0xD5800000 >> "%R%" 2>&1
s10ediag poke 0xD5800000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xD5800000 >> "%R%" 2>&1
s10ediag mark r2: cp2cp end 0xD97FF000
echo --- cp2cp end >> "%R%"
s10ediag peek 0xD97FF000 >> "%R%" 2>&1
s10ediag poke 0xD97FF000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xD97FF000 >> "%R%" 2>&1
s10ediag mark r2: modem_if 0xF0000000
echo --- modem_if >> "%R%"
s10ediag peek 0xF0000000 >> "%R%" 2>&1
s10ediag poke 0xF0000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xF0000000 >> "%R%" 2>&1
s10ediag mark r2: modem_if mid 0xF6000000
echo --- modem_if mid >> "%R%"
s10ediag peek 0xF6000000 >> "%R%" 2>&1
s10ediag poke 0xF6000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xF6000000 >> "%R%" 2>&1
s10ediag mark r2: modem_if end 0xFB8FF000
echo --- modem_if end >> "%R%"
s10ediag peek 0xFB8FF000 >> "%R%" 2>&1
s10ediag poke 0xFB8FF000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xFB8FF000 >> "%R%" 2>&1
s10ediag mark r2: cp_ram_logging 0xFB900000
echo --- cp_ram_logging >> "%R%"
s10ediag peek 0xFB900000 >> "%R%" 2>&1
s10ediag poke 0xFB900000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xFB900000 >> "%R%" 2>&1
s10ediag mark r2: cp_ram_logging end 0xFD8FF000
echo --- cp_ram_logging end >> "%R%"
s10ediag peek 0xFD8FF000 >> "%R%" 2>&1
s10ediag poke 0xFD8FF000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0xFD8FF000 >> "%R%" 2>&1
s10ediag mark r2: secure_camera (riskiest, last) 0x96000000
echo --- secure_camera (riskiest, last) >> "%R%"
s10ediag peek 0x96000000 >> "%R%" 2>&1
s10ediag poke 0x96000000 0x5A5AA5A5 >> "%R%" 2>&1
s10ediag peek 0x96000000 >> "%R%" 2>&1
s10ediag mark r2: all probed regions survived
echo RAMPROBE2 DONE >> "%R%"
