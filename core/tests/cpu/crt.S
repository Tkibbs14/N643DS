# Entry for the CPU test programs. Cartridge build: a stack below 0x80380000, .bss cleared, main, then "PASS" /
# "FAIL" for run_tests.sh. Linux build: main, then exit.
    .set noreorder
    .text
    .globl _start
_start:
#ifdef PLAT_ROM
    lui $sp, 0x8038
    la $t0, __bss_start
    la $t1, _end
1:  beq $t0, $t1, 2f
    nop
    sw $zero, 0($t0)
    b 1b
    addiu $t0, $t0, 4
2:
#endif
    jal crt_main
    nop
3:  b 3b
    nop
