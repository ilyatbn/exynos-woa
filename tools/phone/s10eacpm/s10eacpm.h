/* s10eacpm.h - ACPM IPC client over S10eDiag (s10eacpm.c); shared with the governor (tools/phone/s10egov). */
#pragma once

typedef struct
{
    unsigned           Id, Len, Size;
    unsigned long long TxRear, TxFront, TxBase, RxRear, RxFront, RxBase;
} ACPM_CHANNEL;

int AcpmOpen(void);
int AcpmFindChannel(unsigned id, ACPM_CHANNEL *ch, int verbose);
int AcpmSend(ACPM_CHANNEL *ch, unsigned long cmd[4]);
int AcpmFreqReq(unsigned domain, unsigned khz, unsigned long reply[4]);
