// VR4300 address translation: segments, mode checks, the 32-entry joint TLB (VR4300 manual chapter 5).
#include "vr4300.h"

void vr_exception_vec(Vr* c, int code, u32 vecOff);

#define VPN_MASK 0xC00000FFFFFFE000ull  // R (63:62) and VPN2 (39:13)

static void tlb_fault(Vr* c, u64 va, bool write, int code, bool refill, bool addr64) {
    c->cop0[C0_BADVADDR] = va;
    // Context: BadVPN2 = va[31:13] in bits 22:4; XContext: R in 32:31, BadVPN2 = va[39:13] in 30:4.
    c->cop0[C0_CONTEXT] = (c->cop0[C0_CONTEXT] & ~0x7FFFF0ull) | (((va >> 13) & 0x7FFFF) << 4);
    c->cop0[C0_XCONTEXT] = (c->cop0[C0_XCONTEXT] & ~0x1FFFFFFF0ull) | (((va >> 62) & 3) << 31) |
                           (((va >> 13) & 0x7FFFFFF) << 4);
    c->cop0[C0_ENTRYHI] = (va & VPN_MASK) | (c->cop0[C0_ENTRYHI] & 0xFF);
    (void)write;
    vr_exception_vec(c, code, refill ? (addr64 ? 0x080 : 0x000) : 0x180);
}

static bool tlb_lookup(Vr* c, u64 va, bool write, u32* pa, bool addr64) {
    u32 asid = (u32)c->cop0[C0_ENTRYHI] & 0xFF;
    for (int i = 0; i < 32; i++) {
        const TlbEntry* e = &c->tlb[i];
        u64 mask = VPN_MASK & ~(u64)e->pageMask;
        if ((va & mask) != (e->entryHi & mask)) continue;
        if (!(e->entryHi & 0x1000) && (u32)(e->entryHi & 0xFF) != asid) continue;
        u32 size = ((e->pageMask >> 13) + 1) << 12;  // bytes per page; the entry maps an even and an odd page
        u64 lo = (va & size) ? e->entryLo1 : e->entryLo0;
        if (!(lo & 2)) {
            tlb_fault(c, va, write, write ? EXC_TLBS : EXC_TLBL, false, addr64);
            return false;
        }
        if (write && !(lo & 4)) {
            tlb_fault(c, va, write, EXC_MOD, false, addr64);
            return false;
        }
        u32 pfn = (u32)(lo >> 6) & 0xFFFFF;
        *pa = (pfn << 12) + ((u32)va & (size - 1));
        return true;
    }
    tlb_fault(c, va, write, write ? EXC_TLBS : EXC_TLBL, true, addr64);
    return false;
}

#define addr_error vr_address_error

// A TLB-mapped 32-bit address read in kernel mode, without raising anything (the JIT's dispatcher: code at a mapped
// address). False when the access would fault; the interpreter then runs the instruction and takes the exception.
bool vr_probe_fetch(const Vr* c, u64 va, u32* pa) {
    if (va != SEXT32((u32)va)) return false;
    u32 asid = (u32)c->cop0[C0_ENTRYHI] & 0xFF;
    for (int i = 0; i < 32; i++) {
        const TlbEntry* e = &c->tlb[i];
        u64 mask = VPN_MASK & ~(u64)e->pageMask;
        if ((va & mask) != (e->entryHi & mask)) continue;
        if (!(e->entryHi & 0x1000) && (u32)(e->entryHi & 0xFF) != asid) continue;
        u32 size = ((e->pageMask >> 13) + 1) << 12;
        u64 lo = (va & size) ? e->entryLo1 : e->entryLo0;
        if (!(lo & 2)) return false;
        *pa = (((u32)(lo >> 6) & 0xFFFFF) << 12) + ((u32)va & (size - 1));
        return true;
    }
    return false;
}

bool vr_translate(Vr* c, u64 va, bool write, u32* pa) {
    u32 s = (u32)c->cop0[C0_STATUS];
    int ksu = (s & (ST_EXL | ST_ERL)) ? 0 : ((s >> 3) & 3);
    if (va == SEXT32((u32)va)) {
        u32 a = (u32)va;
        if (ksu == 2 && a >= 0x80000000u) { addr_error(c, va, write); return false; }
        if (ksu == 1 && a >= 0x80000000u && (a < 0xC0000000u || a >= 0xE0000000u)) {
            addr_error(c, va, write);
            return false;
        }
        if (a >= 0x80000000u && a < 0xC0000000u) {  // kseg0 (cached), kseg1 (uncached)
            *pa = a & 0x1FFFFFFF;
            return true;
        }
        // The refill vector follows the mode's addressing (KX / SX / UX), not the address: with KX set, a miss on a
        // 32-bit address takes the XTLB vector 0x080 (PeterLemon/N64 Exceptions/TLB, hardware captures).
        bool mode64 = ksu == 0 ? (s & ST_KX) : ksu == 1 ? (s & ST_SX) : (s & ST_UX);
        return tlb_lookup(c, va, write, pa, mode64);
    }
    // 64-bit addresses need the mode's 64-bit addressing bit (KX / SX / UX).
    bool ok64 = ksu == 0 ? (s & ST_KX) : ksu == 1 ? (s & ST_SX) : (s & ST_UX);
    if (!ok64) { addr_error(c, va, write); return false; }
    u32 region = (u32)(va >> 62);
    if (region == 2 && ksu == 0) {  // xkphys: bits 31:0 are the physical address (bits 61:59 the cache mode)
        if (va & 0x07FFFFFF00000000ull) { addr_error(c, va, write); return false; }
        *pa = (u32)va;
        return true;
    }
    if ((va & 0x3FFFFF0000000000ull) != 0 && !(region == 3 && ksu == 0)) {  // outside the 40-bit segments
        addr_error(c, va, write);
        return false;
    }
    if ((region == 1 && ksu == 2) || (region == 3 && ksu != 0)) { addr_error(c, va, write); return false; }
    return tlb_lookup(c, va, write, pa, true);
}

void vr_tlbr(Vr* c) {
    const TlbEntry* e = &c->tlb[c->cop0[C0_INDEX] & 31];
    u64 g = (e->entryHi >> 12) & 1;
    c->cop0[C0_PAGEMASK] = e->pageMask;
    c->cop0[C0_ENTRYHI] = e->entryHi & ~(u64)e->pageMask & ~0x1F00ull;
    c->cop0[C0_ENTRYLO0] = (e->entryLo0 & ~1ull) | g;
    c->cop0[C0_ENTRYLO1] = (e->entryLo1 & ~1ull) | g;
}

static void tlb_write(Vr* c, int i) {
    TlbEntry* e = &c->tlb[i & 31];
    u32 pm = (u32)c->cop0[C0_PAGEMASK];
    u64 g = c->cop0[C0_ENTRYLO0] & c->cop0[C0_ENTRYLO1] & 1;
    e->pageMask = pm;
    e->entryHi = (c->cop0[C0_ENTRYHI] & ~(u64)pm & ~0x1F00ull) | (g << 12);
    e->entryLo0 = c->cop0[C0_ENTRYLO0] & ~1ull;
    e->entryLo1 = c->cop0[C0_ENTRYLO1] & ~1ull;
}

void vr_tlbwi(Vr* c) { tlb_write(c, (int)(c->cop0[C0_INDEX] & 31)); }

void vr_tlbwr(Vr* c) {
    u32 wired = (u32)c->cop0[C0_WIRED] & 63;
    u32 r = 31;
    if (wired <= 31) r = 31 - (u32)((c->cycles - c->cop0[C0_RANDOM]) % (32 - wired));
    tlb_write(c, (int)r);
}

void vr_tlbp(Vr* c) {
    u64 hi = c->cop0[C0_ENTRYHI];
    u32 asid = (u32)hi & 0xFF;
    for (int i = 0; i < 32; i++) {
        const TlbEntry* e = &c->tlb[i];
        u64 mask = VPN_MASK & ~(u64)e->pageMask;
        if ((hi & mask) != (e->entryHi & mask)) continue;
        if (!(e->entryHi & 0x1000) && (u32)(e->entryHi & 0xFF) != asid) continue;
        c->cop0[C0_INDEX] = (u64)i;
        return;
    }
    c->cop0[C0_INDEX] = 0x80000000u | (c->cop0[C0_INDEX] & 0x3F);
}
