"""Exercise the original game's joystick-mask builder and camera event scanners.

Uses the user's own Speed.exe; no game bytes are stored in this test. Run:
  python3 -m pip install --target .pydeps unicorn==2.1.4
  python3 tests/camera_game_test.py binaries/Speed.exe
"""
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
    image = Image(str(path))
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x400000, 0x800000)
    uc.mem_map(0x1000000, 0x10000)
    for section in image.sections:
        uc.mem_write(section.address, section.data)

    def put(address, value):
        uc.mem_write(address, struct.pack('<I', value))

    def get(address):
        return struct.unpack('<I', uc.mem_read(address, 4))[0]

    slot, raw, old, node, stack, stop = 0x78DAA8, 0x1000000, 0x1002000, 0x1003000, 0x100F000, 0x100F800
    put(0x736500, raw)
    put(0x71D89C, 1)  # joystick 0 exists
    put(slot + 0x130, 0)
    for i in range(4):
        put(raw + 0x20 + 4 * i, 0xFFFFFFFF)  # neutral POVs
    for i in range(16):
        put(slot + 0x18 + 4 * i, 0)
        put(slot + 0x98 + 4 * i, 0)

    # Up/down/left/right: previously unassigned menu buttons, existing action bits and scanner event IDs.
    directions = [(4, 29, 0x3C), (5, 30, 0x3D), (10, 31, 0x3A), (11, 32, 0x3B)]
    for button, action, _ in directions:
        put(slot + 0x98 + 4 * button, action)

    def joystick_mask(buttons):
        uc.mem_write(0x71D8D8, bytes(0x80 if i in buttons else 0 for i in range(16)))
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_ESI, slot)
        # Start after device polling and stop before analog pedals: execute the game's button/hat mapping.
        uc.emu_start(0x4071B8, 0x4074D0, count=10000)
        return bytes(uc.mem_read(slot + 8, 8))

    def scan(config, mask, strength):
        uc.mem_write(raw, mask)
        uc.mem_write(old, b'\xff' * 8)
        put(config + 0x14, strength)
        uc.mem_write(stack, struct.pack('<5I', stop, node, raw, old, config))
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.emu_start(0x5744B0, stop, count=10000)
        return uc.reg_read(UC_X86_REG_EAX)

    for direction, (button, action, event) in enumerate(directions):
        config = 0x703B60 + direction * 0x20
        assert get(config + 8) == event, 'unexpected camera scanner event'
        assert get(config + 0x10) == ((action << 16) | 1), 'unexpected camera action bit'
        uc.mem_write(node, bytes(32))
        put(node + 8, event)
        mask = joystick_mask({button})
        assert not (int.from_bytes(mask, 'little') & (1 << action)), 'pad did not activate camera action'
        assert scan(config, mask, 128) == (128 << 8) | event, 'full camera movement event missing'
        assert scan(config, mask, 208) == (208 << 8) | event, 'faster camera movement event missing'
        assert scan(config, mask, 64) == (64 << 8) | event, 'changed stick speed did not reach game'
        assert scan(config, mask, 64) == 0, 'unchanged held input should not duplicate events'
        assert scan(config, joystick_mask(set()), 0) == event, 'release event did not stop camera'

    assert int.from_bytes(joystick_mask({10, 4}), 'little') & ((1 << 31) | (1 << 29)) == 0
    print('game camera: all four menu actions, proportional speed, diagonal and release events passed')


if __name__ == '__main__':
    check(pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'binaries/Speed.exe')
