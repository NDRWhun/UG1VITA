"""Check the original game's steering handler and held-throttle event after a menu transition."""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / '.pydeps'), str(ROOT / 'tools/recompiler'),
                str(ROOT / 'tools/recompiler/disasm')]
from disasm.image import Image  # noqa: E402
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32  # noqa: E402
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_ESI, UC_X86_REG_EAX  # noqa: E402


def check(path):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x400000, 0x800000)
    for section in Image(str(path)).sections:
        uc.mem_write(section.address, section.data)
    stack, stop, driver, raw, node, old = 0xb00000, 0xb01000, 0xa00000, 0xa01000, 0xa02000, 0xa03000

    def put(a, v):
        uc.mem_write(a, struct.pack('<I', v))

    def steer(event, value):
        uc.mem_write(stack, struct.pack('<5I', stop, 0, event, value, driver))
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.emu_start(0x461840, stop, count=1000)
        return struct.unpack('<f', uc.mem_read(driver + 0x28c, 4))[0]

    uc.mem_write(driver + 0x288, b'\x80\x80')
    samples = [steer(0x88, byte) for byte in range(256)]
    assert samples[0] == 1 and samples[128] == 0 and samples[255] == -1
    assert samples[32] == 0.75 and samples[64] == 0.5 and samples[96] == 0.25
    assert samples[160] == -0.25 and samples[192] == -0.5 and samples[224] == -0.75
    assert all(a > b for a, b in zip(samples, samples[1:])), 'all 256 steering positions must be distinct'
    assert steer(0x87, 0) == 1  # d-pad keeps its digital steering priority
    assert steer(0x88, 192) == 1
    assert steer(0x87, 128) == -0.5  # releasing d-pad returns to the held stick

    slot, throttle = 0x78daa8, 0x703c00
    put(0x736500, raw)
    put(0x71d89c, 1)
    put(slot + 0x130, 0)
    for i in range(4): put(raw + 0x20 + i * 4, 0xffffffff)
    for i in range(16):
        put(slot + 0x18 + i * 4, 0)
        put(slot + 0x98 + i * 4, 0)
    uc.mem_write(0x71d8d8, bytes(0x80 if i == 7 else 0 for i in range(16)))  # R held throughout
    put(node + 8, 0x89)

    def throttle_event(action):
        put(slot + 0x18 + 7 * 4, action)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_ESI, slot)
        uc.emu_start(0x4071b8, 0x4074d0, count=10000)
        uc.mem_write(raw, bytes(uc.mem_read(slot + 8, 8)))
        uc.mem_write(stack, struct.pack('<5I', stop, node, raw, old, throttle))
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.emu_start(0x5744b0, stop, count=1000)
        return uc.reg_read(UC_X86_REG_EAX)

    put(node + 0x1c, 1)
    assert throttle_event(1) == 0xff89
    assert throttle_event(0) == 0x89  # pause disables the race action
    assert throttle_event(1) == 0xff89  # held R accelerates on Resume without a new physical press
    print('original game: 256 analog steering levels, fractional angles, d-pad priority and held-R Resume passed')


if __name__ == '__main__':
    check(sys.argv[1])
