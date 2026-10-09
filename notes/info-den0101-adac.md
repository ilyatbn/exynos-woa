# The PDF in `info/` — what it actually is, and what it does NOT close

File: `info/DEN0101_00bet1_ADAC.pdf` (478 KB, 85 pages). Extracted to text
outside the repo (`%TEMP%\opencode\den0101.txt`, 117 KB) with `pypdf` 6.19.0,
then searched with `rg` — the whole document was never loaded into context.

## What it is

**DEN0101 = "Authenticated Debug Access Control" (ADAC)**, 00bet1, dated
2021-06-24, BETA/engineering draft. Chapters: A1 About the Architecture, A2
System Architecture, A3 Security Model, plus a command protocol, a
**certificate** format and a **token** format.

Its subject is a **physical debug link**: the "Debug Port (DP)" is a
SWJ-DP/JTAG interface (`:2367` "tools use a JTAG interface port to
communicate with processors"), with certificate-based authentication
(`adac_certificate_v1_0`, EC P-256/P-521, RSA-3072/4096, Ed255/Ed448, SM2/SM3,
CMAC/HMAC variants) and access-control domains that gate debug while the
system boots (`:460` Immutable Root of Trust / boot ROM, `:510` "a target in
the Partially or Fully Unlocked state can temporarily disable debug access
during boot following a Warm reset").

## What it does NOT close

It does **not** contain the ACPI Processor Sub-Device spec. `rg -i
"_MAT|membership|topology|sub-device|ACPI0007"` over the extracted text returns
**nothing**; `Processor` appears only in the Cortex-M/JTAG sense. So the
`_MAT` = "Processor Group Membership And Topology" field layout that
`notes/m8-dsdt-mat.md` §4 says is needed to re-derive the eight DSDT CPU
sub-device buffers is **still missing**. The document number I had assumed
(DEN0101) is wrong — DEN0101 is ADAC.

## What it is plausibly for, later

The DSC carries two ACPI TODOs that were deferred on the same reasoning:
`Dbg2.aslc` ("debug port — only once a Windows-recognized transport exists")
and `Spcr.aslc` (serial console redirect). ADAC is about a JTAG/SWJ debug
link with an authenticated protocol, which is **not** what ACPI `Dbg2`
describes (a link the OS can use for remote debug), so it does not unblock
those either. Recorded so the next session does not re-open the PDF expecting
it to answer the `_MAT` question.

## Sources

* `info/DEN0101_00bet1_ADAC.pdf` — the document itself (title page, release
  information, chapter list, and the greps above).
* `notes/m8-dsdt-mat.md` §4 — what is actually needed to close the `_MAT` gap.
