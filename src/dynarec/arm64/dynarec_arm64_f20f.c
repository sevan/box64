#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>

#include "debug.h"
#include "box64context.h"
#include "dynarec.h"
#include "emu/x64emu_private.h"
#include "emu/x64run_private.h"
#include "x64run.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "emu/x64run_private.h"
#include "x64trace.h"
#include "dynarec_native.h"

#include "arm64_printer.h"
#include "dynarec_arm64_private.h"
#include "dynarec_arm64_functions.h"
#include "dynarec_arm64_helper.h"

uintptr_t dynarec64_F20F(dynarec_arm_t* dyn, uintptr_t addr, uintptr_t ip, int ninst, rex_t rex, int* ok, int* need_epilog)
{
    (void)ip; (void)need_epilog;

    uint8_t opcode = F8;
    uint8_t nextop;
    uint8_t gd, ed;
    uint8_t wback;
    uint8_t u8;
    uint64_t u64, j64;
    int v0, v1;
    int q0;
    int d0, d1;
    int64_t fixedaddress;
    int unscaled;

    MAYUSE(d0);
    MAYUSE(d1);
    MAYUSE(q0);
    MAYUSE(v0);
    MAYUSE(v1);

    switch(opcode) {

        case 0x10:
            INST_NAME("MOVSD Gx, Ex");
            nextop = F8;
            GETG;
            if(MODREG) {
                ed = (nextop&7)+ (rex.b<<3);
                v0 = sse_get_reg(dyn, ninst, x1, gd, 1);
                d0 = sse_get_reg(dyn, ninst, x1, ed, 0);
                VMOVeD(v0, 0, d0, 0);
            } else {
                SMREAD();
                v0 = sse_get_reg_empty(dyn, ninst, x1, gd);
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, &unscaled, 0xfff<<3, 7, rex, NULL, 0, 0);
                VLD64(v0, ed, fixedaddress); // upper part reseted
            }
            break;
        case 0x11:
            INST_NAME("MOVSD Ex, Gx");
            nextop = F8;
            GETG;
            v0 = sse_get_reg(dyn, ninst, x1, gd, 0);
            if(MODREG) {
                ed = (nextop&7)+ (rex.b<<3);
                d0 = sse_get_reg(dyn, ninst, x1, ed, 1);
                VMOVeD(d0, 0, v0, 0);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, &unscaled, 0xfff<<3, 7, rex, NULL, 0, 0);
                VST64(v0, ed, fixedaddress);
                SMWRITE2();
            }
            break;
        case 0x12:
            INST_NAME("MOVDDUP Gx, Ex");
            nextop = F8;
            GETG;
            if(MODREG) {
                d0 = sse_get_reg(dyn, ninst, x1, (nextop&7)+(rex.b<<3), 0);
                v0 = sse_get_reg_empty(dyn, ninst, x1, gd);
                VMOVeD(v0, 0, d0, 0);
            } else {
                SMREAD();
                v0 = sse_get_reg_empty(dyn, ninst, x1, gd);
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, &unscaled, 0xfff<<3, 7, rex, NULL, 0, 0);
                VLD64(v0, ed, fixedaddress);
            }
            VMOVeD(v0, 1, v0, 0);
            break;

        case 0x2A:
            INST_NAME("CVTSI2SD Gx, Ed");
            nextop = F8;
            GETGX(v0, 1);
            GETED(0);
            d1 = fpu_get_scratch(dyn);
            if(rex.w) {
                SCVTFDx(d1, ed);
            } else {
                SCVTFDw(d1, ed);
            }
            VMOVeD(v0, 0, d1, 0);
            break;

        case 0x2C:
            INST_NAME("CVTTSD2SI Gd, Ex");
            nextop = F8;
            GETGD;
            GETEXSD(q0, 0, 0);
            if(!box64_dynarec_fastround) {
                MRS_fpsr(x5);
                BFCw(x5, FPSR_IOC, 1);   // reset IOC bit
                MSR_fpsr(x5);
            }
            FCVTZSxwD(gd, q0);
            if(!box64_dynarec_fastround) {
                MRS_fpsr(x5);   // get back FPSR to check the IOC bit
                TBZ_NEXT(x5, FPSR_IOC);
                if(rex.w) {
                    ORRx_mask(gd, xZR, 1, 1, 0);    //0x8000000000000000
                } else {
                    MOV32w(gd, 0x80000000);
                }
            }
            break;
        case 0x2D:
            INST_NAME("CVTSD2SI Gd, Ex");
            nextop = F8;
            GETGD;
            GETEXSD(q0, 0, 0);
            if(!box64_dynarec_fastround) {
                MRS_fpsr(x5);
                BFCw(x5, FPSR_IOC, 1);   // reset IOC bit
                MSR_fpsr(x5);
            }
            u8 = sse_setround(dyn, ninst, x1, x2, x3);
            d1 = fpu_get_scratch(dyn);
            FRINTID(d1, q0);
            x87_restoreround(dyn, ninst, u8);
            FCVTZSxwD(gd, d1);
            if(!box64_dynarec_fastround) {
                MRS_fpsr(x5);   // get back FPSR to check the IOC bit
                TBZ_NEXT(x5, FPSR_IOC);
                if(rex.w) {
                    ORRx_mask(gd, xZR, 1, 1, 0);    //0x8000000000000000
                } else {
                    MOV32w(gd, 0x80000000);
                }
            }
            break;

        case 0x38:  // these are some more SSSE4.2+ opcodes
            opcode = F8;
            switch(opcode) {

                case 0xF0:
                    INST_NAME("(unsupported) CRC32 Gd, Eb)");
                    nextop = F8;
                    addr = fakeed(dyn, addr, ninst, nextop);
                    SETFLAGS(X_ALL, SF_SET);    // Hack to set flags in "don't care" state
                    GETIP(ip);
                    STORE_XEMU_CALL(xRIP);
                    CALL(native_ud, -1);
                    LOAD_XEMU_CALL(xRIP);
                    jump_to_epilog(dyn, 0, xRIP, ninst);
                    *need_epilog = 0;
                    *ok = 0;
                    break;
                case 0xF1:
                    INST_NAME("(unsupported) CRC32 Gd, Ed)");
                    nextop = F8;
                    addr = fakeed(dyn, addr, ninst, nextop);
                    SETFLAGS(X_ALL, SF_SET);    // Hack to set flags in "don't care" state
                    GETIP(ip);
                    STORE_XEMU_CALL(xRIP);
                    CALL(native_ud, -1);
                    LOAD_XEMU_CALL(xRIP);
                    jump_to_epilog(dyn, 0, xRIP, ninst);
                    *need_epilog = 0;
                    *ok = 0;
                    break;

                default:
                    DEFAULT;
            }
            break;


        case 0x51:
            INST_NAME("SQRTSD Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            d1 = fpu_get_scratch(dyn);
            GETEXSD(d0, 0, 0);
            if(!box64_dynarec_fastnan) {
                v1 = fpu_get_scratch(dyn);
                FCMLTD_0(v1, d0);
                SHL_64(v1, v1, 63);
                FSQRTD(d1, d0);
                VORR(d1, d1, v1);
            } else {
                FSQRTD(d1, d0);
            }
            VMOVeD(v0, 0, d1, 0);
            break;

        case 0x58:
            INST_NAME("ADDSD Gx, Ex");
            nextop = F8;
            GETGX(d1, 1);
            v1 = fpu_get_scratch(dyn);
            GETEXSD(d0, 0, 0);
            if(!box64_dynarec_fastnan) {
                v0 = fpu_get_scratch(dyn);
                q0 = fpu_get_scratch(dyn);
                // check if any input value was NAN
                FMAXD(v0, d0, d1);    // propagate NAN
                FCMEQD(v0, v0, v0);    // 0 if NAN, 1 if not NAN
                FADDD(v1, d1, d0);  // the high part of the vector is erased...
                FCMEQD(q0, v1, v1);    // 0 => out is NAN
                VBIC(q0, v0, q0);      // forget it in any input was a NAN already
                SHL_64(q0, q0, 63);     // only keep the sign bit
                VORR(v1, v1, q0);      // NAN -> -NAN
            } else {
                FADDD(v1, d1, d0);  // the high part of the vector is erased...
            }
            VMOVeD(d1, 0, v1, 0);
            break;
        case 0x59:
            INST_NAME("MULSD Gx, Ex");
            nextop = F8;
            GETGX(d1, 1);
            v1 = fpu_get_scratch(dyn);
            GETEXSD(d0, 0, 0);
            if(!box64_dynarec_fastnan) {
                v0 = fpu_get_scratch(dyn);
                q0 = fpu_get_scratch(dyn);
                // check if any input value was NAN
                FMAXD(v0, d0, d1);    // propagate NAN
                FCMEQD(v0, v0, v0);    // 0 if NAN, 1 if not NAN
                FMULD(v1, d1, d0);
                FCMEQD(q0, v1, v1);    // 0 => out is NAN
                VBIC(q0, v0, q0);      // forget it in any input was a NAN already
                SHL_64(q0, q0, 63);     // only keep the sign bit
                VORR(v1, v1, q0);      // NAN -> -NAN
            } else {
                FMULD(v1, d1, d0);
            }
            VMOVeD(d1, 0, v1, 0);
            break;
        case 0x5A:
            INST_NAME("CVTSD2SS Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            GETEXSD(d0, 0, 0);
            d1 = fpu_get_scratch(dyn);
            FCVT_S_D(d1, d0);
            VMOVeS(v0, 0, d1, 0);
            break;

        case 0x5C:
            INST_NAME("SUBSD Gx, Ex");
            nextop = F8;
            GETGX(d1, 1);
            v1 = fpu_get_scratch(dyn);
            GETEXSD(d0, 0, 0);
            if(!box64_dynarec_fastnan) {
                v0 = fpu_get_scratch(dyn);
                q0 = fpu_get_scratch(dyn);
                // check if any input value was NAN
                FMAXD(v0, d0, d1);    // propagate NAN
                FCMEQD(v0, v0, v0);    // 0 if NAN, 1 if not NAN
                FSUBD(v1, d1, d0);
                FCMEQD(q0, v1, v1);    // 0 => out is NAN
                VBIC(q0, v0, q0);      // forget it in any input was a NAN already
                SHL_64(q0, q0, 63);     // only keep the sign bit
                VORR(v1, v1, q0);      // NAN -> -NAN
            } else {
                FSUBD(v1, d1, d0);
            }
            VMOVeD(d1, 0, v1, 0);
            break;
        case 0x5D:
            INST_NAME("MINSD Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            GETEXSD(v1, 0, 0);
            // MINSD: if any input is NaN, or Ex[0]<Gx[0], copy Ex[0] -> Gx[0]
            #if 0
            d0 = fpu_get_scratch(dyn);
            FMINNMD(d0, v0, v1);    // NaN handling may be slightly different, is that a problem?
            VMOVeD(v0, 0, d0, 0);   // to not erase uper part
            #else
            FCMPD(v0, v1);
            B_NEXT(cLS);    //Less than or equal
            VMOVeD(v0, 0, v1, 0);   // to not erase uper part
            #endif
            break;
        case 0x5E:
            INST_NAME("DIVSD Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            d1 = fpu_get_scratch(dyn);
            GETEXSD(v1, 0, 0);
            if(!box64_dynarec_fastnan) {
                d0 = fpu_get_scratch(dyn);
                q0 = fpu_get_scratch(dyn);
                // check if any input value was NAN
                FMAXD(d0, v0, v1);      // propagate NAN
                FCMEQD(d0, d0, d0);     // 0 if NAN, 1 if not NAN
                FDIVD(d1, v0, v1);
                FCMEQD(q0, d1, d1);     // 0 => out is NAN
                VBIC(q0, d0, q0);       // forget it in any input was a NAN already
                SHL_64(q0, q0, 63);     // only keep the sign bit
                VORR(d1, d1, q0);       // NAN -> -NAN
            } else {
                FDIVD(d1, v0, v1);
            }
            VMOVeD(v0, 0, d1, 0);
            break;
        case 0x5F:
            INST_NAME("MAXSD Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            GETEXSD(v1, 0, 0);
            // MAXSD: if any input is NaN, or Ex[0]>Gx[0], copy Ex[0] -> Gx[0]
            #if 0
            d0 = fpu_get_scratch(dyn);
            FMAXNMD(d0, v0, v1);    // NaN handling may be slightly different, is that a problem?
            VMOVeD(v0, 0, d0, 0);   // to not erase uper part
            #else
            FCMPD(v0, v1);
            B_NEXT(cGE);    //Greater than or equal
            VMOVeD(v0, 0, v1, 0);   // to not erase uper part
            #endif
            break;

        case 0x70:
            INST_NAME("PSHUFLW Gx, Ex, Ib");
            nextop = F8;
            GETEX(v1, 0, 1);
            GETGX(v0, 1);

            u8 = F8;
            // only low part need to be suffled. VTBL only handle 8bits value, so the 16bits suffles need to be changed in 8bits
            u64 = 0;
            for (int i=0; i<4; ++i) {
                u64 |= ((uint64_t)((u8>>(i*2))&3)*2+0)<<(i*16+0);
                u64 |= ((uint64_t)((u8>>(i*2))&3)*2+1)<<(i*16+8);
            }
            MOV64x(x2, u64);
            d0 = fpu_get_scratch(dyn);
            VMOVQDfrom(d0, 0, x2);
            VTBL1_8(d0, v1, d0);
            VMOVeD(v0, 0, d0, 0);
            if(v0!=v1) {
                VMOVeD(v0, 1, v1, 1);
            }
            break;

        case 0x7C:
            INST_NAME("HADDPS Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            if(MODREG) {
                v1 = sse_get_reg(dyn, ninst, x1, (nextop&7)+(rex.b<<3), 0);
            } else {
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, &unscaled, 0xfff<<4, 15, rex, NULL, 0, 0);
                v1 = fpu_get_scratch(dyn);
                VLD128(v1, ed, fixedaddress);
            }
            VFADDPQS(v0, v0, v1);
            break;
            
        case 0xC2:
            INST_NAME("CMPSD Gx, Ex, Ib");
            nextop = F8;
            GETGX(v0, 1);
            GETEXSD(v1, 0, 1);
            u8 = F8;
            FCMPD(v0, v1);
            switch(u8&7) {
                case 0: CSETMx(x2, cEQ); break;   // Equal
                case 1: CSETMx(x2, cCC); break;   // Less than
                case 2: CSETMx(x2, cLS); break;   // Less or equal
                case 3: CSETMx(x2, cVS); break;   // NaN
                case 4: CSETMx(x2, cNE); break;   // Not Equal or unordered
                case 5: CSETMx(x2, cCS); break;   // Greater or equal or unordered
                case 6: CSETMx(x2, cHI); break;   // Greater or unordered, test inverted, N!=V so unordered or less than (inverted)
                case 7: CSETMx(x2, cVC); break;   // not NaN
            }
            VMOVQDfrom(v0, 0, x2);
            break;

        case 0xD0:
            INST_NAME("ADDSUBPS Gx, Ex");
            nextop = F8;
            GETGX(v0, 1);
            GETEXSD(v1, 0, 0);
            q0 = fpu_get_scratch(dyn);
            static float addsubps[4] = {-1.f, 1.f, -1.f, 1.f};
            MAYUSE(addsubps);
            TABLE64(x2, (uintptr_t)&addsubps);
            VLDR128_U12(q0, x2, 0);
            VFMLAQS(v0, v1, q0);
            break;

        case 0xD6:
            INST_NAME("MOVDQ2Q Gm, Ex");
            nextop = F8;
            GETGM(v0);
            GETEXSD(v1, 0, 0);
            VMOV(v0, v1);
            break;

        case 0xE6:
            INST_NAME("CVTPD2DQ Gx, Ex");
            nextop = F8;
            GETEXSD(v1, 0, 0);
            GETGX_empty(v0);
            u8 = sse_setround(dyn, ninst, x1, x2, x3);
            VFRINTIDQ(v0, v1);
            x87_restoreround(dyn, ninst, u8);
            VFCVTNSQD(v0, v0);  // convert double -> int64
            SQXTN_32(v0, v0);   // convert int64 -> int32 with saturation in lower part, RaZ high part
            break;

        case 0xF0:
            INST_NAME("LDDQU Gx,Ex");
            nextop = F8;
            GETG;
            if(MODREG) {
                v1 = sse_get_reg(dyn, ninst, x1, (nextop&7)+(rex.b<<3), 0);
                v0 = sse_get_reg_empty(dyn, ninst, x1, gd);
                VMOVQ(v0, v1);
            } else {
                v0 = sse_get_reg_empty(dyn, ninst, x1, gd);
                SMREAD();
                addr = geted(dyn, addr, ninst, nextop, &ed, x1, &fixedaddress, &unscaled, 0xfff<<4, 7, rex, NULL, 0, 0);
                VLD128(v0, ed, fixedaddress);
            }
            break;

        default:
            DEFAULT;
    }
    return addr;
}
