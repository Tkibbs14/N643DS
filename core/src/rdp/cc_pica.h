#pragma once
// The RDP's colour combiner as PICA200 texture-combiner (TEV) stages: cc_pica_compile turns the combine word and the
// cycle type into at most six stages, cc_pica_eval runs them in C with the PICA's per-stage clamping (the PC checks
// them against the reference combiner, core/tests/cc), and core/host3ds/rdp_pica.c hands them to citro3d.
// Plain C with its own copies of the PICA's source / operand / function numbers (citro3d's GPU_* values), so it builds
// and is checked on the PC.
#include "../common.h"

enum {  // sources (GPU_TEVSRC)
    PS_PRIMARY = 0x00, PS_TEX0 = 0x03, PS_TEX1 = 0x04, PS_BUFFER = 0x0D, PS_CONSTANT = 0x0E, PS_PREVIOUS = 0x0F
};
enum {  // operands (GPU_TEVOP_RGB / GPU_TEVOP_A)
    PO_COLOR = 0, PO_ONE_MINUS_COLOR = 1, PO_ALPHA = 2, PO_ONE_MINUS_ALPHA = 3,  // RGB operands
    PO_A_ALPHA = 0, PO_A_ONE_MINUS_ALPHA = 1                                      // alpha operands
};
enum {  // functions (GPU_COMBINEFUNC)
    PF_REPLACE = 0, PF_MODULATE = 1, PF_ADD = 2, PF_ADD_SIGNED = 3, PF_INTERPOLATE = 4, PF_SUBTRACT = 5,
    PF_MULTIPLY_ADD = 8, PF_ADD_MULTIPLY = 9
};
enum { PK_ZERO, PK_ONE, PK_PRIM, PK_ENV, PK_PRIM_ALPHA, PK_ENV_ALPHA, PK_PRIM_LOD, PK_PRIM_MINUS_ENV };  // constants

typedef struct {
    u8 srcRgb[3], opRgb[3], funcRgb;
    u8 srcA[3], opA[3], funcA;
    u8 constRgb, constA;  // what the stage's constant colour holds (PK_*), for its RGB and its alpha
    bool bufRgb, bufA;    // the stage's output also goes to the buffer the next stages read
} TevStage;

typedef struct {
    int n;               // stages used (1..6)
    TevStage st[6];
    bool usesTex0, usesTex1, usesShade;
    bool approx;         // something the PICA cannot do was approximated (noise, keying, LOD fraction, negatives)
    u8 globalRgb, globalA;  // the buffer's initial colour (PK_*): a second constant the stages read
} TevProgram;

// Compiles combine (the SET_COMBINE word) for cycle type cyc (RDP_CYC1 / RDP_CYC2; copy and fill need no combiner).
void cc_pica_compile(u64 combine, u32 cyc, TevProgram* p);

typedef struct { int r, g, b, a; } CcCol;  // 0..255
// Runs the program on one pixel's inputs (the PICA's arithmetic: 0..255, each stage clamped).
CcCol cc_pica_eval(const TevProgram* p, CcCol tex0, CcCol tex1, CcCol shade, CcCol prim, CcCol env, int primLod);
// The constant colour a stage holds for these state colours (RGBA, 0..255).
CcCol cc_pica_const(const TevStage* s, CcCol prim, CcCol env, int primLod);
CcCol cc_pica_buffer(const TevProgram* p, CcCol prim, CcCol env, int primLod);  // the buffer's initial colour
extern int g_ccPlant;  // checker self-test: 1 drops the second operand of every A * C stage (cc_check.c must report it)
