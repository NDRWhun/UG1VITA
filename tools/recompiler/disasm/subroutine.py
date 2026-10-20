"""A decoded function: its instructions in address order and its control-flow references."""

from capstone import x86


class FakeJmpInstruction:
    """A jump the generator adds where control falls through to code that is not the next instruction in address
    order (another function's entry, or code decoded out of order). It takes the address of the instruction it
    follows and has no size."""
    class Operand:
        def __init__(self, address):
            self.type = x86.X86_OP_IMM
            self.imm = address
            self.size = 4

    def __init__(self, instruction_address, jump_address):
        self.prefix = [0, 0, 0, 0]
        self.bytes = b'\xca\xca'
        self.opcode = b'\xca\xca'
        self.mnemonic = 'jmp'
        self.op_str = '0x%x' % jump_address
        self.operands = [self.Operand(jump_address)]
        self.address = instruction_address
        self.size = 0
        self.eflags = 0

    def regs_access(self):
        return ([], [])


class FakeTrapInstruction:
    """A place the generated code traps: bytes the decoder could not decode on a reachable path, or the return of a
    call that never returns. It has no size."""
    def __init__(self, address, reason='undecodable'):
        self.prefix = [0, 0, 0, 0]
        self.bytes = b'\xcc'
        self.opcode = b'\xcc'
        self.mnemonic = 'int3'
        self.op_str = '(%s)' % reason
        self.reason = reason
        self.operands = []
        self.address = address
        self.size = 0
        self.eflags = 0

    def regs_access(self):
        return ([], [])


class Subroutine:
    def __init__(self, entry):
        self.entry = entry
        self.instructions = []
        self.calls = []             # (instruction address, target)
        self.static_labels = []     # (instruction address, target inside the function)
        self.dynamic_labels = []    # (switch jump address, case target)

    def get_entry_point(self):
        return self.entry
