#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-${SCRIPT_DIR}/out}"
CLANG="${CLANG:-clang}"
LLD="${LLD:-ld.lld}"
OBJCOPY="${OBJCOPY:-llvm-objcopy}"

: "${UEFI_BASE:?Set UEFI_BASE to the FD base address from the matching FDF}"
: "${UEFI_SIZE:?Set UEFI_SIZE to the FD size from the matching FDF}"

mkdir -p "${OUT_DIR}"
"${CLANG}" -target aarch64-none-elf -ffreestanding -nostdlib -mgeneral-regs-only \
  -DUEFI_BASE="${UEFI_BASE}" -DUEFI_SIZE="${UEFI_SIZE}" \
  -c "${SCRIPT_DIR}/bootshim.S" -o "${OUT_DIR}/bootshim.o"
"${LLD}" -o "${OUT_DIR}/bootshim.elf" "${OUT_DIR}/bootshim.o" \
  -Ttext="${UEFI_BASE}" -e _head --no-relax
"${OBJCOPY}" -O binary "${OUT_DIR}/bootshim.elf" "${OUT_DIR}/bootshim.bin"
python3 "${SCRIPT_DIR}/verify.py" "${OUT_DIR}/bootshim.bin" "${UEFI_BASE}" "${UEFI_SIZE}"
