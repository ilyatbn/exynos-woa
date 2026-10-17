# (user session) DIAGNOSIS ONLY (AGENTS.md rule 12, notes/g23-bc-emulation.md): gamedbg.ps1 with D3D10UMD_DIAG_CS4X=1 in
# the game's environment - does dgVoodoo try to create compute shaders once the UMD advertises CS 4.x?
$env:D3D10UMD_DIAG_CS4X = '1'
& C:\S10eTmp\gamedbg.ps1
