/*  WlanSup.c - milestone 7b: the crypto of a host WPA2-PSK supplicant (the BCM4375 firmware has none: run
    m6-3, sup_wpa -> BCME_UNSUPPORTED). Kernel CNG only (ksecdd.lib). The state machine is in WlanMsg.c.
    References (notes/m12-wifi-m7.md "Protocol facts - M7b"): hostap wpa_common.c wpa_pmk_to_ptk / wpa_eapol_key_mic,
    IEEE 802.11i-2004 8.5.1.1 (PRF) and H.3 (PRF test vectors), RFC 3394 2.2.2 (unwrap) and 4.1 (test vector). */

#include <ntddk.h>
#include <bcrypt.h>
#include "WlanSup.h"

static NTSTATUS
HmacSha1(_In_reads_bytes_(KLen) const UCHAR *K, _In_ ULONG KLen, _In_reads_bytes_(L1) const UCHAR *P1, _In_ ULONG L1,
         _In_reads_bytes_opt_(L2) const UCHAR *P2, _In_ ULONG L2, _Out_writes_(20) PUCHAR Out)
{
    BCRYPT_ALG_HANDLE  Alg = NULL;
    BCRYPT_HASH_HANDLE H = NULL;
    NTSTATUS           Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = BCryptCreateHash(Alg, &H, NULL, 0, (PUCHAR)K, KLen, 0);
    if (NT_SUCCESS(Status)) {
        Status = BCryptHashData(H, (PUCHAR)P1, L1, 0);
    }
    if (NT_SUCCESS(Status) && P2 != NULL && L2 != 0) {
        Status = BCryptHashData(H, (PUCHAR)P2, L2, 0);
    }
    if (NT_SUCCESS(Status)) {
        Status = BCryptFinishHash(H, Out, 20, 0);
    }
    if (H != NULL) {
        BCryptDestroyHash(H);
    }
    BCryptCloseAlgorithmProvider(Alg, 0);
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SupPrf(const UCHAR *K, ULONG KLen, PCSTR Label, const UCHAR *B, ULONG BLen, PUCHAR Out, ULONG OutLen)
{
    UCHAR    Msg[128];   /* Label + 0 + B + counter: "Pairwise key expansion" (22) + 1 + 76 + 1 = 100 */
    UCHAR    H[20];
    ULONG    LabelLen = (ULONG)strlen(Label), Done = 0, N;
    UCHAR    I;
    NTSTATUS Status = STATUS_SUCCESS;

    if (LabelLen + 1 + BLen + 1 > sizeof(Msg) || OutLen > 255 * 20) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Msg, Label, LabelLen);
    Msg[LabelLen] = 0;
    RtlCopyMemory(Msg + LabelLen + 1, B, BLen);
    for (I = 0; Done < OutLen; I++) {
        Msg[LabelLen + 1 + BLen] = I;
        Status = HmacSha1(K, KLen, Msg, LabelLen + 1 + BLen + 1, NULL, 0, H);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        N = (OutLen - Done < 20) ? OutLen - Done : 20;
        RtlCopyMemory(Out + Done, H, N);
        Done += N;
    }
    RtlSecureZeroMemory(Msg, sizeof(Msg));
    RtlSecureZeroMemory(H, sizeof(H));
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SupPtk(const UCHAR *Pmk, const UCHAR *Aa, const UCHAR *Spa, const UCHAR *ANonce, const UCHAR *SNonce, PUCHAR Ptk)
{
    UCHAR    B[6 + 6 + SUP_NONCE_LEN + SUP_NONCE_LEN];
    BOOLEAN  AaFirst = memcmp(Aa, Spa, 6) < 0, AnFirst = memcmp(ANonce, SNonce, SUP_NONCE_LEN) < 0;
    NTSTATUS Status;

    RtlCopyMemory(B, AaFirst ? Aa : Spa, 6);
    RtlCopyMemory(B + 6, AaFirst ? Spa : Aa, 6);
    RtlCopyMemory(B + 12, AnFirst ? ANonce : SNonce, SUP_NONCE_LEN);
    RtlCopyMemory(B + 12 + SUP_NONCE_LEN, AnFirst ? SNonce : ANonce, SUP_NONCE_LEN);
    Status = SupPrf(Pmk, 32, "Pairwise key expansion", B, sizeof(B), Ptk, SUP_PTK_LEN);
    RtlSecureZeroMemory(B, sizeof(B));
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SupMic(const UCHAR *Kck, const UCHAR *Frame, ULONG Len, PUCHAR Mic)
{
    UCHAR    H[20];
    NTSTATUS Status = HmacSha1(Kck, SUP_KEY_LEN, Frame, Len, NULL, 0, H);

    RtlCopyMemory(Mic, H, SUP_MIC_LEN);
    RtlSecureZeroMemory(H, sizeof(H));
    return Status;
}

/* RFC 3394 2.2.2 (index based): A = C0, R[i] = Ci; for j = 5..0, i = n..1: B = AES-1(K, (A ^ t) | R[i]),
   t = n*j + i (64-bit big endian); A = MSB64(B), R[i] = LSB64(B). Check A == A6A6A6A6A6A6A6A6. */
_Use_decl_annotations_
NTSTATUS
SupAesUnwrap(const UCHAR *Kek, const UCHAR *In, ULONG Len, PUCHAR Out)
{
    BCRYPT_ALG_HANDLE Alg = NULL;
    BCRYPT_KEY_HANDLE Key = NULL;
    UCHAR             A[8], Blk[16];
    ULONG             N, I, Cb, K;
    LONG              J;
    ULONGLONG         T;
    NTSTATUS          Status;

    if (Len < 24 || (Len % 8) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    N = Len / 8 - 1;
    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = BCryptSetProperty(Alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    if (NT_SUCCESS(Status)) {
        Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, (PUCHAR)Kek, SUP_KEY_LEN, 0);
    }
    if (!NT_SUCCESS(Status)) {
        goto Done;
    }
    RtlCopyMemory(A, In, 8);
    RtlCopyMemory(Out, In + 8, Len - 8);   /* R[1..n] */
    for (J = 5; J >= 0; J--) {
        for (I = N; I >= 1; I--) {
            T = (ULONGLONG)N * (ULONG)J + I;
            for (K = 0; K < 8; K++) {
                Blk[K] = A[K] ^ (UCHAR)(T >> (56 - 8 * K));
            }
            RtlCopyMemory(Blk + 8, Out + (I - 1) * 8, 8);
            Status = BCryptDecrypt(Key, Blk, 16, NULL, NULL, 0, Blk, 16, &Cb, 0);
            if (!NT_SUCCESS(Status) || Cb != 16) {
                Status = NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
                goto Done;
            }
            RtlCopyMemory(A, Blk, 8);
            RtlCopyMemory(Out + (I - 1) * 8, Blk + 8, 8);
        }
    }
    for (K = 0; K < 8; K++) {
        if (A[K] != 0xA6) {
            Status = STATUS_DATA_ERROR;
            break;
        }
    }
Done:
    RtlSecureZeroMemory(Blk, sizeof(Blk));
    if (Key != NULL) {
        BCryptDestroyKey(Key);
    }
    BCryptCloseAlgorithmProvider(Alg, 0);
    if (!NT_SUCCESS(Status)) {
        RtlSecureZeroMemory(Out, Len - 8);
    }
    return Status;
}

_Use_decl_annotations_
NTSTATUS
SupRandom(PUCHAR Out, ULONG Len)
{
    return BCryptGenRandom(NULL, Out, Len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

/* Vectors recomputed on the PC with Python hmac/hashlib + cryptography (notes/m12-wifi-m7.md). */
_Use_decl_annotations_
BOOLEAN
SupSelfTest(PWLAN_REPORT R)
{
    static const UCHAR Prf1[64] = {   /* K 0x0b x 20, "prefix", "Hi There" (802.11i H.3 case 1) */
        0xbc, 0xd4, 0xc6, 0x50, 0xb3, 0x0b, 0x96, 0x84, 0x95, 0x18, 0x29, 0xe0, 0xd7, 0x5f, 0x9d, 0x54,
        0xb8, 0x62, 0x17, 0x5e, 0xd9, 0xf0, 0x06, 0x06, 0xe1, 0x7d, 0x8d, 0xa3, 0x54, 0x02, 0xff, 0xee,
        0x75, 0xdf, 0x78, 0xc3, 0xd3, 0x1e, 0x0f, 0x88, 0x9f, 0x01, 0x21, 0x20, 0xc0, 0x86, 0x2b, 0xeb,
        0x67, 0x75, 0x3e, 0x74, 0x39, 0xae, 0x24, 0x2e, 0xdb, 0x83, 0x73, 0x69, 0x83, 0x56, 0xcf, 0x5a };
    static const UCHAR Prf2[64] = {   /* K "Jefe", "prefix", "what do ya want for nothing?" (case 2) */
        0x51, 0xf4, 0xde, 0x5b, 0x33, 0xf2, 0x49, 0xad, 0xf8, 0x1a, 0xeb, 0x71, 0x3a, 0x3c, 0x20, 0xf4,
        0xfe, 0x63, 0x14, 0x46, 0xfa, 0xbd, 0xfa, 0x58, 0x24, 0x47, 0x59, 0xae, 0x58, 0xef, 0x90, 0x09,
        0xa9, 0x9a, 0xbf, 0x4e, 0xac, 0x2c, 0xa5, 0xfa, 0x87, 0xe6, 0x92, 0xc4, 0x40, 0xeb, 0x40, 0x02,
        0x3e, 0x7b, 0xab, 0xb2, 0x06, 0xd6, 0x1d, 0xe7, 0xb9, 0x2f, 0x41, 0x52, 0x90, 0x92, 0xb8, 0xfc };
    static const UCHAR Wrapped[24] = {   /* RFC 3394 4.1 */
        0x1f, 0xa6, 0x8b, 0x0a, 0x81, 0x12, 0xb4, 0x47, 0xae, 0xf3, 0x4b, 0xd8, 0xfb, 0x5a, 0x7b, 0x82,
        0x9d, 0x3e, 0x86, 0x23, 0x71, 0xd2, 0xcf, 0xe5 };
    static const UCHAR Plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const UCHAR Kek[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const CHAR  Data2[] = "what do ya want for nothing?";
    UCHAR   K1[20], Out[64], Un[16];
    BOOLEAN P1, P2, W;

    RtlFillMemory(K1, sizeof(K1), 0x0b);
    P1 = NT_SUCCESS(SupPrf(K1, sizeof(K1), "prefix", (const UCHAR *)"Hi There", 8, Out, 64)) &&
         RtlCompareMemory(Out, Prf1, 64) == 64;
    P2 = NT_SUCCESS(SupPrf((const UCHAR *)"Jefe", 4, "prefix", (const UCHAR *)Data2, sizeof(Data2) - 1, Out, 64)) &&
         RtlCompareMemory(Out, Prf2, 64) == 64;
    W  = NT_SUCCESS(SupAesUnwrap(Kek, Wrapped, sizeof(Wrapped), Un)) && RtlCompareMemory(Un, Plain, 16) == 16;
    ReportLine(R, "[sup] self-test: PRF 802.11i H.3 case 1 %s, case 2 %s; AES key unwrap RFC 3394 4.1 %s -> %s\r\n",
               P1 ? "ok" : "BAD", P2 ? "ok" : "BAD", W ? "ok" : "BAD", (P1 && P2 && W) ? "PASS" : "FAIL");
    return P1 && P2 && W;
}
