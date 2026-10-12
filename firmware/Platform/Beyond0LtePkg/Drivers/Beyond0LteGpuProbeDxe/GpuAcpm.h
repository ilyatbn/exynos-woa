/** @file
  M16 G9: ACPM DVFS FREQ_REQ (GpuAcpm.c). notes/gpu-m16-g3don.md H1-H3.
**/

#ifndef GPU_ACPM_H_
#define GPU_ACPM_H_

/**
  Send {DomainIdx, RateKhz, FREQ_REQ 0, 0} on the ACPM DVFS channel (0) and wait for the reply.

  @param[out] Reply  4 words: the APM's reply (seq in word0 bits 21:16) on success; undefined otherwise.
**/
EFI_STATUS
GpuAcpmDvfsFreqReq (
  IN  UINT32  DomainIdx,
  IN  UINT32  RateKhz,
  OUT UINT32  *Reply
  );

#endif // GPU_ACPM_H_
