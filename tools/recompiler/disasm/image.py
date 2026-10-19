"""The game executable as the generator sees it: sections at their preferred addresses, base relocations, imports.

The image is never rebased; the runtime loads the same file at the same addresses. Every import gets a fake address
in the header page below the code; the runtime writes these into the import address table and registers its shims
there, so an indirect call through the table reaches the shim.
"""

import hashlib

from . import pefile

# Fake import addresses: one per import, in the PE header page (the code starts at FAKE_IMPORT_END).
FAKE_IMPORT_BASE = 0x400a00
FAKE_IMPORT_END = 0x401000

# Names of imports by ordinal (pefile's table does not know these DLLs).
ORDINALS = {
    ('dsound', 1): 'DirectSoundCreate',
    ('dsound', 2): 'DirectSoundEnumerateA',
}

IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_WRITE = 0x80000000


class Section:
    def __init__(self, name, address, virtual_size, file_offset, raw_size, data, code, writable, discardable):
        self.name = name                    # e.g. '.text'
        self.address = address              # guest address
        self.virtual_size = virtual_size
        self.file_offset = file_offset      # raw data position in the file (0: none)
        self.raw_size = raw_size            # bytes of raw data (the rest of the virtual size is zero)
        self.data = data                    # raw data (bytes)
        self.code = code
        self.writable = writable
        self.discardable = discardable

    @property
    def end(self):
        return self.address + max(self.virtual_size, self.raw_size)

    def contains(self, address):
        return self.address <= address < self.end


class Import:
    def __init__(self, dll, name, iat_address, fake_address):
        self.dll = dll                      # lower case without .dll, e.g. 'kernel32'
        self.name = name
        self.iat_address = iat_address      # guest address of the import address table slot
        self.fake_address = fake_address


class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.file_bytes = f.read()
        self.sha256 = hashlib.sha256(self.file_bytes).hexdigest()
        pe = pefile.PE(data=self.file_bytes)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry_point = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size_of_image = pe.OPTIONAL_HEADER.SizeOfImage
        self.header_size = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.sections = []
        for s in pe.sections:
            flags = s.Characteristics
            self.sections.append(Section(
                s.Name.rstrip(b'\0').decode(), self.base + s.VirtualAddress, s.Misc_VirtualSize,
                s.PointerToRawData, s.SizeOfRawData, s.get_data()[:s.SizeOfRawData],
                bool(flags & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)), bool(flags & IMAGE_SCN_MEM_WRITE),
                bool(flags & IMAGE_SCN_MEM_DISCARDABLE)))
        self.relocations = set()
        for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for entry in block.entries:
                if entry.type == IMAGE_REL_BASED_HIGHLOW:
                    self.relocations.add(self.base + entry.rva)
                elif entry.type != 0:
                    raise AssertionError('relocation type %d at 0x%x' % (entry.type, self.base + entry.rva))
        self.imports = []
        fake = FAKE_IMPORT_BASE
        for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
            dll = descriptor.dll.decode().lower()
            if dll.endswith('.dll'):
                dll = dll[:-4]
            for imp in descriptor.imports:
                if imp.name:
                    name = imp.name.decode()
                else:
                    name = ORDINALS.get((dll, imp.ordinal))
                    if not name:
                        raise AssertionError('unknown import %s ordinal %d' % (dll, imp.ordinal))
                if fake >= FAKE_IMPORT_END:
                    raise AssertionError('fake import addresses reach 0x%x' % FAKE_IMPORT_END)
                self.imports.append(Import(dll, name, imp.address, fake))
                fake += 1
        self.code_sections = [s for s in self.sections if s.code]

    def section_of(self, address):
        for s in self.sections:
            if s.contains(address):
                return s
        return None

    def is_code(self, address):
        return any(s.contains(address) for s in self.code_sections)

    def read(self, address, size):
        """Bytes at address; bytes past the raw data read as zero, outside every section as nothing."""
        s = self.section_of(address)
        if s is None:
            return b''
        offset = address - s.address
        end = min(offset + size, s.end - s.address)
        return s.data[offset:end].ljust(end - offset, b'\0')

    def dword(self, address):
        data = self.read(address, 4)
        return int.from_bytes(data, 'little') if len(data) == 4 else None

    def end_address(self):
        return max(s.end for s in self.sections)
