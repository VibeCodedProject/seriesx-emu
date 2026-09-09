#!/usr/bin/env python3.13
# Smoke test: feasibility gates for wiring Unicorn x86-32 under seriesx-emu.
# Gates:
#   G1: x86-32 guest executes real instructions
#   G2: FS-relative TEB access via a REAL GDT (FS_BASE reg is a no-op in
#       32-bit mode on unicorn 2.1.4 -- verified earlier, deprecation warning)
#   G3: kernel-thunk transition, Cxbx-style: the HLE page holds REAL
#       `ret imm16` stubs; guest `call [slot]` lands on the stub; a code hook
#       performs HLE side effects + EAX (regs stick; EIP writes in hooks DO
#       NOT -- verified: fell through and faulted); the native `ret imm`
#       returns to the guest and pops stdcall args. No EIP manipulation.
#   G4: per-fiber context save/restore carries regs + FS base
import struct, sys
import unicorn as U
from unicorn import x86_const as X

OK = []
def won(g, msg): OK.append(g); print(f"[PASS] {g}: {msg}")
def fail(g, msg): print(f"[FAIL] {g}: {msg}"); sys.exit(1)

def desc(base=0, limit=0xFFFFF, access=0x9A, flags=0xC):
    """8-byte x86 GDT entry. flags nibble: G(gran) D(32bit) L AVL."""
    return struct.pack("<HHBBB",
        limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF,
        access, ((flags & 0xF) << 4) | ((limit >> 16) & 0x0F)) + \
        bytes([(base >> 24) & 0xFF])

# ---- guest layout (mirrors Xbox-ish addressing: all below 4GiB) -------------
CODE   = 0x00400000      # code page
GDT    = 0x00300000      # our GDT
STACK  = 0x01000000      # stack (1 MiB, grows down)
TEB_A  = 0x7E000000      # fiber A TEB
TEB_B  = 0x7E001000      # fiber B TEB
THUNK  = 0x00500000      # thunk slot page (XBE kernel_thunk_table analogue)
DATA   = 0x00600000      # data page (counter + result slot)
MARKER = 0xF00F0000      # HLE page base == kHleMarker (include/xbox_krnl.h)
ORD_INTERLOCKED_DECREMENT = 52   # LithiumX's first real import; stdcall, 4 arg bytes

SEL_CS, SEL_DS, SEL_FS = 0x08, 0x10, 0x18
gdt_bytes  = b"\x00" * 8                                             # null
gdt_bytes += desc(0, 0xFFFFF, 0x9A, 0xC)                             # CS: flat 4G RX 32-bit
gdt_bytes += desc(0, 0xFFFFF, 0x92, 0xC)                             # DS: flat 4G RW 32-bit
gdt_bytes += desc(TEB_A, 0xFFF, 0x92, 0x4)                           # FS_A: base=TEB_A, 4KiB, byte-gran
gdt_bytes += desc(TEB_B, 0xFFF, 0x92, 0x4)                           # FS_B: base=TEB_B (added later in C++)
SEL_FS_B = 0x20

# guest code (32-bit):
#   mov eax, fs:[0x18]        ; self-pointer field, real TEB semantics
#   mov [eax+0x04], 0xBEEF0001; write into OUR TEB
#   mov esp, STACK_TOP
#   push 0x00600000           ; arg: guest pointer to counter
#   call [THUNK+0]            ; slot holds MARKER|52 -> HLE stub `ret 4`
#   mov [0x00600004], eax     ; store result where host can check
#   hlt
code = bytes([
    0x64, 0xA1, 0x18, 0x00, 0x00, 0x00,             # mov eax, fs:[0x18]
    0xC7, 0x40, 0x04, 0x01, 0x00, 0xEF, 0xBE,       # mov dword [eax+4], 0xBEEF0001
    0xBC, 0x00, 0x00, 0x10, 0x01,                   # mov esp, 0x01100000 (stack top)
    0x68, 0x00, 0x00, 0x60, 0x00,                   # push 0x00600000
    0xFF, 0x15, 0x00, 0x00, 0x50, 0x00,             # call dword ptr [0x00500000]
    0xA3, 0x04, 0x00, 0x60, 0x00,                   # mov [0x00600004], eax
    0xF4,                                           # hlt
])

uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
for addr, size in ((GDT, 0x1000), (CODE, 0x1000), (STACK, 0x100000), (THUNK, 0x1000),
                   (DATA, 0x1000), (TEB_A, 0x1000), (TEB_B, 0x1000), (MARKER, 0x1000)):
    uc.mem_map(addr, size)
uc.mem_write(GDT, gdt_bytes)
uc.mem_write(CODE, code)
uc.mem_write(DATA, struct.pack("<II", 7, 0))            # counter=7, result=0
uc.mem_write(THUNK, struct.pack("<I", MARKER | ORD_INTERLOCKED_DECREMENT))
uc.mem_write(TEB_A + 0x18, struct.pack("<I", TEB_A))    # real TEB: Self field AT +0x18
uc.mem_write(TEB_B + 0x18, struct.pack("<I", TEB_B))

# HLE page: ordinal 52 slot = `ret 4` (0xC2,04,00); rest ret (0xC3) padding
stub = bytearray(b"\xC3" * 0x1000)
stub[ORD_INTERLOCKED_DECREMENT:ORD_INTERLOCKED_DECREMENT+3] = b"\xC2\x04\x00"  # ret 4
uc.mem_write(MARKER, bytes(stub))

# segment regs (GDTR FIRST -- verified: selector loads cache the descriptor,
# loading before GDTR exists raises UC_ERR_EXCEPTION; RPL3 selectors also #GP
# because UC CPL is 0 -- ring0-only caveat documented)
uc.reg_write(X.UC_X86_REG_GDTR, (0, GDT, len(gdt_bytes) - 1, 0))
uc.reg_write(X.UC_X86_REG_CS, SEL_CS)
uc.reg_write(X.UC_X86_REG_DS, SEL_DS)
uc.reg_write(X.UC_X86_REG_ES, SEL_DS)
uc.reg_write(X.UC_X86_REG_SS, SEL_DS)
uc.reg_write(X.UC_X86_REG_FS, SEL_FS)

# ---- G3: HLE hook: side effects + EAX only ----------------------------------
hle_calls = []
def on_code(uc_, address, size, user):
    if (address & ~0xFFFF) == MARKER:
        ordinal = address & 0xFFFF
        esp = uc_.reg_read(X.UC_X86_REG_ESP)
        arg = struct.unpack("<I", uc_.mem_read(esp + 4, 4))[0]
        if ordinal == ORD_INTERLOCKED_DECREMENT:
            val = struct.unpack("<i", uc_.mem_read(arg, 4))[0]
            val -= 1
            uc_.mem_write(arg, struct.pack("<i", val))
            hle_calls.append((ordinal, val))
            uc_.reg_write(X.UC_X86_REG_EAX, val & 0xFFFFFFFF)
        # NO EIP write, NO emu_stop: the real `ret 4` at this address does the
        # stdcall return natively. Verified: EIP-in-hook is ignored by UC2.
uc.hook_add(U.UC_HOOK_CODE, on_code, begin=MARKER, end=MARKER + 0xFFFF)

uc.reg_write(X.UC_X86_REG_ESP, STACK + 0x100000)
uc.reg_write(X.UC_X86_REG_ECX, 0x11111111)
uc.reg_write(X.UC_X86_REG_EDX, 0x22222222)
uc.emu_start(CODE, CODE + len(code) - 1, count=100000)

eax = uc.reg_read(X.UC_X86_REG_EAX)
esp = uc.reg_read(X.UC_X86_REG_ESP)
res = struct.unpack("<i", uc.mem_read(DATA + 4, 4))[0]
teb_self = struct.unpack("<I", uc.mem_read(TEB_A + 0x18, 4))[0]
teb4 = struct.unpack("<I", uc.mem_read(TEB_A + 4, 4))[0]

if eax != 6 or res != 6:
    fail("G1/G3", f"expected EAX=6, got EAX={eax} res={res}  hle={hle_calls}")
if esp != STACK + 0x100000:      # push+call+ret4 must net back to start
    fail("G3", f"stdcall stack cleanup wrong: ESP={esp:#x}")
if teb_self != TEB_A or teb4 != 0xBEEF0001:
    fail("G2", f"FS-relative access wrong: teb_self={teb_self:#x} teb4={teb4:#x}")
if hle_calls != [(ORD_INTERLOCKED_DECREMENT, 6)]:
    fail("G3", f"unexpected HLE trace {hle_calls}")
won("G1", "real 32-bit instructions executed (mov/push/call/ret imm/mov)")
won("G2", "fs:[0x18] read + fs-relative write via real GDT FS descriptor")
won("G3", "call [slot]->`ret 4` stub->hook HLE->EAX=6->native stdcall return")

# ---- G4: per-fiber context save/restore -------------------------------------
ctx = uc.context_save()
uc.reg_write(X.UC_X86_REG_FS, SEL_FS_B)   # fiber B
# read fs:[0x18] into ecx: mov ecx, fs:[0x18]
uc.mem_write(CODE + 0x100, bytes([0x64, 0x8B, 0x0D, 0x18, 0x00, 0x00, 0x00]))
uc.emu_start(CODE + 0x100, CODE + 0x100 + 7, count=10)
ecx_b = uc.reg_read(X.UC_X86_REG_ECX)
uc.context_restore(ctx)                   # fiber A restored
ecx_a = uc.reg_read(X.UC_X86_REG_ECX)
fs_sel = uc.reg_read(X.UC_X86_REG_FS)
uc.emu_start(CODE + 0x100, CODE + 0x100 + 7, count=10)
ecx_restored = uc.reg_read(X.UC_X86_REG_ECX)
if not (ecx_b == TEB_B and ecx_a == 0x11111111 and fs_sel == SEL_FS and ecx_restored == TEB_A):
    fail("G4", f"ctx: B.ecx={ecx_b:#x} restored.ecx={ecx_a:#x} sel={fs_sel:#x} teb={ecx_restored:#x}")
won("G4", "context_save/restore carries regs + FS segment base (per-fiber swap viable)")

print("\nALL GATES PASSED:", ", ".join(OK))
