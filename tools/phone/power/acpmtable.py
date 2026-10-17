"""acpmtable.py HEX - decode S10ePower's AcpmChannels value (ipc_channel entries, stride 0x48; offsets from
Beyond0LteGpuProbeDxe/GpuAcpm.c / kernel framework.h): id, type (1 queue / 2 buffer), q_len, elem size, ap_poll, the
AP tx/rx ring offsets."""
import struct, sys
b = bytes.fromhex(sys.argv[1])
for i in range(len(b) // 0x48):
    w = struct.unpack_from('<18I', b, i * 0x48)
    print('idx %2d id %2u field 0x%04x owner %u type %u | AP-tx rear/front/base 0x%04x/0x%04x/0x%04x '
          'AP-rx 0x%04x/0x%04x/0x%04x | q_len %u elem %u ap_poll %u' %
          (i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[10], w[11], w[12], w[13], w[14], w[17]))
