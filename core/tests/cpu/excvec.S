# Exception handler for the cartridge-only tests (exc.c, cop0.c). exc_install copies a stub to each vector
# (0x000 TLB refill, 0x080 XTLB refill, 0x180 general); the stub loads its vector offset into k1 and jumps here.
# The handler records Cause, EPC, BadVAddr, Status and the vector in g_exc, then returns: to g_exc.resume when set,
# past the faulting instruction otherwise, or to the same instruction for an interrupt (whose source it clears).
    .set noreorder
    .set noat
    .text
    .globl exc_stub, exc_stub_end, exc_entry
exc_stub:
    lui $k0, %hi(exc_entry)
    addiu $k0, $k0, %lo(exc_entry)
    jr $k0
    ori $k1, $zero, 0          # patched with the vector offset by exc_install
exc_stub_end:

exc_entry:
    lui $k0, %hi(g_exc)
    addiu $k0, $k0, %lo(g_exc)
    sw $k1, 28($k0)            # vector
    lw $k1, 0($k0)
    addiu $k1, $k1, 1
    sw $k1, 0($k0)             # count
    mfc0 $k1, $13
    sw $k1, 4($k0)             # cause
    dmfc0 $k1, $14
    sd $k1, 8($k0)             # epc
    dmfc0 $k1, $8
    sd $k1, 16($k0)            # badvaddr
    mfc0 $k1, $12
    sw $k1, 24($k0)            # status
    mfc0 $k1, $13
    andi $k1, $k1, 0x7c
    bnez $k1, 1f
    nop
    # interrupt: rewrite Compare (clears IP7), clear IP0 / IP1, retry the instruction
    mfc0 $k1, $11
    mtc0 $k1, $11
    mtc0 $zero, $13
    eret
1:  ld $k1, 32($k0)
    beqz $k1, 2f
    nop
    sd $zero, 32($k0)
    dmtc0 $k1, $14
    eret
2:  dmfc0 $k1, $14
    daddiu $k1, $k1, 4
    dmtc0 $k1, $14
    eret
