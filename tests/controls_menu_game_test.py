"""Exercise the original game's Controller route, native row visibility/arrows and row callback dispatch.

Uses the user's own Speed.exe and Unicorn; no game code or assets are redistributed.
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / '.pydeps'), str(ROOT / 'tools/recompiler'),
                str(ROOT / 'tools/recompiler/disasm')]
from disasm.image import Image  # noqa: E402
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE  # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                              UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP, UC_X86_REG_EIP)  # noqa: E402


def name_hash(name):
    value = 0xffffffff
    for c in name.upper().encode():
        value = (value * 33 + c) & 0xffffffff
    return value


def check(path):
    image = Image(str(path))
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x400000, 0x800000)
    for section in image.sections:
        uc.mem_write(section.address, section.data)
    stack, stop = 0xb00000, 0xb01000
    controller, record, package, engine, objects_array = 0xa00000, 0xa01000, 0xa02000, 0xa03000, 0xa04000
    custom_callback, third_row = 0xb02000, 0xa05000

    def put(a, v):
        uc.mem_write(a, struct.pack('<I', v))

    def get(a):
        return struct.unpack('<I', uc.mem_read(a, 4))[0]

    def string(a):
        data = bytearray()
        while (c := bytes(uc.mem_read(a + len(data), 1))) != b'\0':
            data.extend(c)
        return data.decode()

    def ret(arg_bytes=0, eax=0):
        esp = uc.reg_read(UC_X86_REG_ESP)
        uc.reg_write(UC_X86_REG_EIP, get(esp))
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + arg_bytes)
        uc.reg_write(UC_X86_REG_EAX, eax)

    calls, objects = [], {}
    names = ['OptionName_3', 'OptionNametext_3', 'OptionData_3', 'SelectButton_3',
             'option_buttonR_03', 'option_buttonL_03', 'LeftArrow_3', 'RightArrow_3', 'minus_03', 'plus_03']
    for i, name in enumerate(names):
        obj = third_row + i * 0x100
        objects[name_hash(name)] = obj
        put(obj + 0x10, name_hash(name))
        put(obj + 0x18, 2)
        put(obj + 0x1c, 0x04000001)  # hidden and disabled before the Controller builder activates this row
        put(objects_array + 4 * i, obj)
    put(controller, 0x6c3e3c)
    put(controller + 0xc, 0x6c2c10)
    put(record + 0x18, package)
    put(package + 0x84, objects_array)
    put(package + 0x88, len(names))
    put(0x73578c, engine)

    def hook(_uc, address, _size, _user):
        if address == 0x4f3f90:
            ret(4, record)
        elif address == 0x4f65d0:
            ret(eax=package)
        elif address == 0x4ffb70:
            ret(eax=objects.get(uc.reg_read(UC_X86_REG_ECX), 0))
        elif address == 0x4f68c0:  # bounded formatting; leave the original game helpers/visibility intact
            esp = uc.reg_read(UC_X86_REG_ESP)
            text = string(get(esp + 8)) % get(esp + 12)
            assert len(text) < get(esp + 4)
            uc.mem_write(uc.reg_read(UC_X86_REG_ESI), text.encode() + b'\0')
            ret(eax=len(text))
        elif address == 0x4f6360:
            calls.append(('screen', uc.reg_read(UC_X86_REG_EDI)))
            ret()
        elif address in (custom_callback, 0x4d0350, 0x4d02c0):
            calls.append(('callback', address, get(uc.reg_read(UC_X86_REG_ESP) + 4)))
            ret(4)

    uc.hook_add(UC_HOOK_CODE, hook)
    # Follow the actual Controller entry from Options: shared MU_Options_PC.fng, category 3.
    button = 0xa09000
    put(button + 0x10, name_hash('Controller'))
    uc.mem_write(stack, struct.pack('<5I', stop, 0x0c407210, button, 0, 0))
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_ECX, controller)
    uc.emu_start(0x4d4610, stop, count=10000)
    assert get(0x735e20) == 3 and calls == [('screen', 0x6c2c10)]

    # These are the exact original helpers used for Vibration, now called with row 3 by the native extension.
    uc.mem_write(stack, struct.pack('<4I', stop, 3, 1, 0))
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_EBX, controller)
    uc.emu_start(0x4d1600, stop, count=10000)
    for name in ['OptionName_3', 'OptionNametext_3', 'OptionData_3']:
        assert not get(objects[name_hash(name)] + 0x1c) & 1, name + ' must be visible'
    assert not get(third_row + 0x1c) & 0x04000000, 'third row must accept selection'
    uc.mem_write(stack, struct.pack('<3I', stop, controller, 0))
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_EBX, 3)
    uc.emu_start(0x4d16d0, stop, count=10000)
    for name in ['option_buttonR_03', 'option_buttonL_03', 'LeftArrow_3', 'RightArrow_3']:
        assert not get(objects[name_hash(name)] + 0x1c) & 1, name + ' must be visible'
    for name in ['minus_03', 'plus_03', 'SelectButton_3']:
        assert get(objects[name_hash(name)] + 0x1c) & 1, name + ' must stay hidden'

    # Actual Options notification dispatch must call the third handler, preserving Setup and Vibration routes.
    for row, handler in enumerate([0x4d0350, 0x4d02c0, custom_callback], 1):
        put(controller + 0x3c + row * 4, handler)
        put(button + 0x10, name_hash(f'OptionName_{row}'))
        for event in [0x9120409e, 0xb5971bf1]:
            calls.clear()
            uc.mem_write(stack, struct.pack('<5I', stop, event, button, 0, 0))
            uc.reg_write(UC_X86_REG_ESP, stack)
            uc.reg_write(UC_X86_REG_ECX, controller)
            uc.emu_start(0x4cf240, stop, count=10000)
            assert calls == [('callback', handler, event)]
    print('original game: Controller route, visible/enabled third row, native arrows, isolated row callbacks passed')


if __name__ == '__main__':
    check(sys.argv[1])
