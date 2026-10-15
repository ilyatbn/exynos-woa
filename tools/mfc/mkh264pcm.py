#!/usr/bin/env python3
"""mkh264pcm.py OUT.h264 [W H] - writes a tiny Baseline H.264 Annex-B stream whose decoded output is known exactly.

D10 V-W2 test vector (notes/d10-mfc-decode-flow.md). No encoder needed:
  frame 0: IDR, every macroblock I_PCM (raw samples) - pattern A
  frame 1: P, every macroblock P_Skip              - must equal frame 0
  frame 2: IDR (idr_pic_id 1), I_PCM               - pattern B
Deblocking is disabled in every slice, so the decoder must reproduce the PCM samples bit-exactly.
Prints, per displayed frame, FNV-1a 32 of the luma plane (W x H, row order) and of the interleaved CbCr plane
(W x H/2, NV12 order) - the same hashes the Exynos9820Mfc self-test logs.
"""
import sys

W, H = 128, 96


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, n, v):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def ue(self, v):
        v += 1
        n = v.bit_length()
        self.u(n - 1, 0)
        self.u(n, v)

    def se(self, v):
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def align_zero(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def trailing(self):
        self.bits.append(1)
        self.align_zero()

    def data(self):
        assert len(self.bits) % 8 == 0
        return bytes(int("".join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def nal(ref_idc, nal_type, rbsp):
    out = bytearray(b"\x00\x00\x00\x01")
    out.append((ref_idc << 5) | nal_type)
    zeros = 0
    for b in rbsp:   # emulation prevention
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def sps(mbw, mbh):
    b = Bits()
    b.u(8, 66)        # profile_idc Baseline
    b.u(8, 0xC0)      # constraint_set0/1
    b.u(8, 30)        # level 3.0
    b.ue(0)           # sps id
    b.ue(0)           # log2_max_frame_num_minus4 -> frame_num u(4)
    b.ue(2)           # pic_order_cnt_type 2 (output order = decode order)
    b.ue(1)           # max_num_ref_frames
    b.u(1, 0)         # gaps_in_frame_num_value_allowed_flag
    b.ue(mbw - 1)
    b.ue(mbh - 1)
    b.u(1, 1)         # frame_mbs_only_flag
    b.u(1, 1)         # direct_8x8_inference_flag
    b.u(1, 0)         # frame_cropping_flag
    b.u(1, 0)         # vui_parameters_present_flag
    b.trailing()
    return nal(3, 7, b.data())


def pps():
    b = Bits()
    b.ue(0); b.ue(0)  # pps id, sps id
    b.u(1, 0)         # entropy_coding_mode_flag (CAVLC)
    b.u(1, 0)         # bottom_field_pic_order_in_frame_present_flag
    b.ue(0)           # num_slice_groups_minus1
    b.ue(0); b.ue(0)  # num_ref_idx_l0/l1_default_active_minus1
    b.u(1, 0)         # weighted_pred_flag
    b.u(2, 0)         # weighted_bipred_idc
    b.se(0); b.se(0); b.se(0)   # pic_init_qp/qs_minus26, chroma_qp_index_offset
    b.u(1, 1)         # deblocking_filter_control_present_flag
    b.u(1, 0)         # constrained_intra_pred_flag
    b.u(1, 0)         # redundant_pic_cnt_present_flag
    b.trailing()
    return nal(3, 8, b.data())


def pattern(which, w, h):
    """Planes Y (h rows x w), Cb, Cr (h/2 x w/2). Samples kept in 16..235 / 16..240 (no zero bytes)."""
    y = [[16 + ((x * 3 + r * 2 + which * 97) % 220) for x in range(w)] for r in range(h)]
    cb = [[16 + ((x * 5 + r * 7 + which * 31) % 225) for x in range(w // 2)] for r in range(h // 2)]
    cr = [[16 + ((x * 11 + r * 3 + which * 59) % 225) for x in range(w // 2)] for r in range(h // 2)]
    return y, cb, cr


def idr_slice(planes, mbw, mbh, idr_id):
    y, cb, cr = planes
    b = Bits()
    b.ue(0)           # first_mb_in_slice
    b.ue(7)           # slice_type I (all slices)
    b.ue(0)           # pps id
    b.u(4, 0)         # frame_num
    b.ue(idr_id)      # idr_pic_id
    b.u(1, 0); b.u(1, 0)   # no_output_of_prior_pics_flag, long_term_reference_flag
    b.se(0)           # slice_qp_delta
    b.ue(1)           # disable_deblocking_filter_idc = 1 (off)
    for my in range(mbh):
        for mx in range(mbw):
            b.ue(25)  # mb_type I_PCM
            b.align_zero()
            for r in range(16):
                for c in range(16):
                    b.u(8, y[my * 16 + r][mx * 16 + c])
            for plane in (cb, cr):
                for r in range(8):
                    for c in range(8):
                        b.u(8, plane[my * 8 + r][mx * 8 + c])
    b.trailing()
    return nal(3, 5, b.data())


def p_skip_slice(mbw, mbh, frame_num):
    b = Bits()
    b.ue(0)           # first_mb_in_slice
    b.ue(5)           # slice_type P (all slices)
    b.ue(0)           # pps id
    b.u(4, frame_num)
    b.u(1, 0)         # num_ref_idx_active_override_flag
    b.u(1, 0)         # ref_pic_list_modification_flag_l0
    b.u(1, 0)         # adaptive_ref_pic_marking_mode_flag
    b.se(0)           # slice_qp_delta
    b.ue(1)           # disable_deblocking_filter_idc
    b.ue(mbw * mbh)   # mb_skip_run: the whole picture
    b.trailing()
    return nal(2, 1, b.data())


def fnv(data):
    h = 0x811C9DC5
    for x in data:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def nv12_hashes(planes):
    y, cb, cr = planes
    luma = bytes(v for row in y for v in row)
    chroma = bytes(v for r in range(len(cb)) for c in range(len(cb[0])) for v in (cb[r][c], cr[r][c]))
    return fnv(luma), fnv(chroma)


def main():
    global W, H
    if len(sys.argv) >= 4:
        W, H = int(sys.argv[2]), int(sys.argv[3])
    assert W % 16 == 0 and H % 16 == 0
    mbw, mbh = W // 16, H // 16
    a, bpat = pattern(0, W, H), pattern(1, W, H)
    stream = sps(mbw, mbh) + pps() + idr_slice(a, mbw, mbh, 0) + p_skip_slice(mbw, mbh, 1) + idr_slice(bpat, mbw, mbh, 1)
    with open(sys.argv[1], "wb") as f:
        f.write(stream)
    print(f"{sys.argv[1]}: {len(stream)} bytes, {W}x{H}, 3 frames")
    for i, p in enumerate((a, a, bpat)):
        ly, lc = nv12_hashes(p)
        print(f"frame {i}: luma fnv 0x{ly:08x} chroma fnv 0x{lc:08x}")


if __name__ == "__main__":
    main()
