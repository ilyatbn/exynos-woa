# Parked: a power button Windows recognises (2026-10-14)

Today the power key is polled by S10eAgent (GPIO) and handled by s10ekeys; Windows itself never sees a power button.
Consequence (notes/brightness-research.md "Power-button screen off"): once Windows turns the display off
(SC_MONITORPOWER 2, display-off timeout, idle dim chain) it never turns it on again - touch is ignored while the display
is off and there is no wake source - so the display-off timeout must stay "never" and screen off is done by s10ekeys +
the KMD's MW_ESCAPE_PANEL_POWER (DCS 0x28/0x29) behind a touch-blocking window.

Proper fix: expose the key the way Windows tablets do - a Windows Button Array (ACPI PNP0C40, GPIO interrupt resources
for power / volume) or a HID button collection (System Control: power) - so the power manager handles press -> display
off / on, and DxgkDdiSetPowerState D0 arrives (KMD 1.0.46.1 already lights the panel on D0; re-enable D3 -> 0x28 then).
Also brings back idle dim / display-off timeouts (notes/brightness-research.md idle dim).
