#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug.h"
#include "box64stack.h"
#include "x64emu.h"
#include "x64run.h"
#include "x64emu_private.h"
#include "x64run_private.h"
#include "x64primop.h"
#include "x64trace.h"
#include "x87emu_private.h"
#include "box64context.h"
#include "bridge.h"
#ifdef DYNAREC
#include "dynarec/native_lock.h"
#endif

#include "modrm.h"

uintptr_t Run66F0(x64emu_t *emu, rex_t rex, uintptr_t addr)
{
    uint8_t opcode;
    uint8_t nextop;
    uint16_t tmp16u, tmp16u2;
    int32_t tmp32s;
    int64_t tmp64s;
    uint64_t tmp64u, tmp64u2;
    reg64_t *oped, *opgd;
    #ifdef USE_CAS
    uint64_t tmpcas;
    #endif

    opcode = F8;
    // REX prefix before the F0 are ignored
    rex.rex = 0;
    while(opcode>=0x40 && opcode<=0x4f) {
        rex.rex = opcode;
        opcode = F8;
    }

    if(rex.w) return RunF0(emu, rex, addr);

    switch(opcode) {
        
        case 0x0F:
            nextop = F8;
            switch(nextop) {

                case 0xB1:                      /* CMPXCHG Ew,Gw */
                    nextop = F8;
                    GETEW(0);
                    GETGW;
#ifdef DYNAREC
                    do {
                        tmp16u = native_lock_read_h(EW);
                        cmp16(emu, R_AX, tmp16u);
                        if(ACCESS_FLAG(F_ZF)) {
                            tmp32s = native_lock_write_h(EW, GW->word[0]);
                        } else {
                            R_AX = tmp16u;
                            tmp32s = 0;
                        }
                    } while(tmp32s);
#else
                    pthread_mutex_lock(&emu->context->mutex_lock);
                    cmp16(emu, R_AX, EW->word[0]);
                    if(ACCESS_FLAG(F_ZF)) {
                        EW->word[0] = GW->word[0];
                    } else {
                        R_AX = EW->word[0];
                    }
                    pthread_mutex_unlock(&emu->context->mutex_lock);
#endif
                    break;

                case 0xC1:                      /* XADD Gw,Ew */
                    nextop = F8;
                    GETEW(0);
                    GETGW;
#ifdef DYNAREC
                    if(rex.w) {
                        do {
                            tmp64u = native_lock_read_dd(ED);
                            tmp64u2 = add64(emu, tmp64u, GD->q[0]);
                        } while(native_lock_write_dd(ED, tmp64u2));
                        GD->q[0] = tmp64u;
                    } else {
                        if(((uintptr_t)ED)&1) {
                            do {
                                tmp16u = ED->word[0] & ~0xff;
                                tmp16u |= native_lock_read_h(ED);
                                tmp16u2 = add16(emu, tmp16u, GD->word[0]);
                            } while(native_lock_write_h(ED, tmp16u2&0xff));
                            ED->word[0] = tmp16u2;
                        } else {
                            do {
                                tmp16u = native_lock_read_h(ED);
                                tmp16u2 = add16(emu, tmp16u, GD->word[0]);
                            } while(native_lock_write_h(ED, tmp16u2));
                        }
                        GD->word[0] = tmp16u;
                    }
#else
                    pthread_mutex_lock(&emu->context->mutex_lock);
                    if(rex.w) {
                        tmp64u = add64(emu, ED->q[0], GD->q[0]);
                        GD->q[0] = ED->q[0];
                        ED->q[0] = tmp64u;
                    } else {
                        tmp16u = add16(emu, ED->word[0], GD->word[0]);
                        GD->word[0] = ED->word[0];
                        ED->word[0] = tmp16u;
                    }
                    pthread_mutex_unlock(&emu->context->mutex_lock);
#endif
                    break;

                default:
                    return 0;
            }
            break;

#ifdef DYNAREC
        #define GO(B, OP)                                           \
        case B+1:                                                   \
            nextop = F8;                                            \
            GETEW(0);                                               \
            GETGW;                                                  \
            if(rex.w) {                                             \
                do {                                                \
                    tmp64u = native_lock_read_dd(ED);               \
                    tmp64u = OP##64(emu, tmp64u, GD->q[0]);         \
                } while (native_lock_write_dd(ED, tmp64u));         \
            } else {                                                \
                do {                                                \
                    tmp16u = native_lock_read_h(ED);                \
                    tmp16u = OP##16(emu, tmp16u, GW->word[0]);      \
                } while (native_lock_write_h(ED, tmp16u));          \
            }                                                       \
            break;                                                  \
        case B+3:                                                   \
            nextop = F8;                                            \
            GETEW(0);                                               \
            GETGW;                                                  \
            if(rex.w)                                               \
                GD->q[0] = OP##64(emu, GD->q[0], ED->q[0]);         \
            else                                                    \
                GW->word[0] = OP##16(emu, GW->word[0], EW->word[0]);\
            break;                                                  \
        case B+5:                                                   \
            if(rex.w)                                               \
                R_RAX = OP##64(emu, R_RAX, F32S64);                 \
            else                                                    \
                R_AX = OP##16(emu, R_AX, F16);                      \
            break;
#else
        #define GO(B, OP)                                           \
        case B+1:                                                   \
            nextop = F8;                                            \
            GETEW(0);                                               \
            GETGW;                                                  \
            pthread_mutex_lock(&emu->context->mutex_lock);          \
            if(rex.w)                                               \
                ED->q[0] = OP##64(emu, ED->q[0], GD->q[0]);         \
            else                                                    \
                EW->word[0] = OP##16(emu, EW->word[0], GW->word[0]);\
            pthread_mutex_unlock(&emu->context->mutex_lock);        \
            break;                                                  \
        case B+3:                                                   \
            nextop = F8;                                            \
            GETEW(0);                                               \
            GETGW;                                                  \
            pthread_mutex_lock(&emu->context->mutex_lock);          \
            if(rex.w)                                               \
                GD->q[0] = OP##64(emu, GD->q[0], ED->q[0]);         \
            else                                                    \
                GW->word[0] = OP##16(emu, GW->word[0], EW->word[0]);\
            pthread_mutex_unlock(&emu->context->mutex_lock);        \
            break;                                                  \
        case B+5:                                                   \
            pthread_mutex_lock(&emu->context->mutex_lock);          \
            if(rex.w)                                               \
                R_RAX = OP##64(emu, R_RAX, F32S64);                 \
            else                                                    \
                R_AX = OP##16(emu, R_AX, F16);                      \
            pthread_mutex_unlock(&emu->context->mutex_lock);        \
            break;
#endif
        GO(0x00, add)                   /* ADD 0x00 -> 0x05 */
        GO(0x08, or)                    /*  OR 0x08 -> 0x0D */
        GO(0x10, adc)                   /* ADC 0x10 -> 0x15 */
        GO(0x18, sbb)                   /* SBB 0x18 -> 0x1D */
        GO(0x20, and)                   /* AND 0x20 -> 0x25 */
        GO(0x28, sub)                   /* SUB 0x28 -> 0x2D */
        GO(0x30, xor)                   /* XOR 0x30 -> 0x35 */
        #undef GO

        case 0x81:              /* GRP Ew,Iw */
        case 0x83:              /* GRP Ew,Ib */
            nextop = F8;
            GETED((opcode==0x83)?1:2);
            tmp64s = (opcode==0x83)?(F8S):(F16S);
            tmp64u = (uint64_t)tmp64s;
#ifdef DYNAREC
            if(MODREG)
                switch((nextop>>3)&7) {
                    case 0: ED->word[0] = add16(emu, ED->word[0], tmp64u); break;
                    case 1: ED->word[0] =  or16(emu, ED->word[0], tmp64u); break;
                    case 2: ED->word[0] = adc16(emu, ED->word[0], tmp64u); break;
                    case 3: ED->word[0] = sbb16(emu, ED->word[0], tmp64u); break;
                    case 4: ED->word[0] = and16(emu, ED->word[0], tmp64u); break;
                    case 5: ED->word[0] = sub16(emu, ED->word[0], tmp64u); break;
                    case 6: ED->word[0] = xor16(emu, ED->word[0], tmp64u); break;
                    case 7:            cmp16(emu, ED->word[0], tmp64u); break;
                }
            else
                switch((nextop>>3)&7) {
                    case 0: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = add16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 1: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 =  or16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 2: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = adc16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 3: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = sbb16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 4: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = and16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 5: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = sub16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 6: do { tmp16u2 = native_lock_read_h(ED); tmp16u2 = xor16(emu, tmp16u2, tmp64u);} while(native_lock_write_h(ED, tmp16u2)); break;
                    case 7:                                                 cmp16(emu, ED->word[0], tmp64u); break;
                }
#else
            pthread_mutex_lock(&emu->context->mutex_lock);
            switch((nextop>>3)&7) {
                case 0: ED->word[0] = add16(emu, ED->word[0], tmp64u); break;
                case 1: ED->word[0] =  or16(emu, ED->word[0], tmp64u); break;
                case 2: ED->word[0] = adc16(emu, ED->word[0], tmp64u); break;
                case 3: ED->word[0] = sbb16(emu, ED->word[0], tmp64u); break;
                case 4: ED->word[0] = and16(emu, ED->word[0], tmp64u); break;
                case 5: ED->word[0] = sub16(emu, ED->word[0], tmp64u); break;
                case 6: ED->word[0] = xor16(emu, ED->word[0], tmp64u); break;
                case 7:               cmp16(emu, ED->word[0], tmp64u); break;
            }
            pthread_mutex_unlock(&emu->context->mutex_lock);
#endif
            break;

        case 0xFF:              /* GRP 5 Ed */
            nextop = F8;
            GETED(0);
            switch((nextop>>3)&7) {
                case 0:                 /* INC Ed */
#ifdef DYNAREC
                    if(rex.w)
                        if(((uintptr_t)ED)&7) {
                            // unaligned
                            do {
                                tmp64u = ED->q[0] & 0xffffffffffffff00LL;
                                tmp64u |= native_lock_read_b(ED);
                                tmp64u = inc64(emu, tmp64u);
                            } while(native_lock_write_b(ED, tmp64u&0xff));
                            ED->q[0] = tmp64u;
                        }
                        else
                            do {
                                tmp64u = native_lock_read_dd(ED);
                            } while(native_lock_write_dd(ED, inc64(emu, tmp64u)));
                    else {
                        if((uintptr_t)ED&1) { 
                            //meh.
                            do {
                                tmp16u = ED->word[0];
                                tmp16u &=~0xff;
                                tmp16u |= native_lock_read_b(ED);
                                tmp16u = inc16(emu, tmp16u);
                            } while(native_lock_write_b(ED, tmp16u&0xff));
                            ED->word[0] = tmp16u;
                        } else {
                            do {
                                tmp16u = native_lock_read_h(ED);
                            } while(native_lock_write_h(ED, inc16(emu, tmp16u)));
                        }
                    }
#else
                    pthread_mutex_lock(&emu->context->mutex_lock);
                    if(rex.w) {
                        ED->q[0] = inc64(emu, ED->q[0]);
                    } else {
                        ED->word[0] = inc16(emu, ED->word[0]);
                    }
                    pthread_mutex_unlock(&emu->context->mutex_lock);
#endif
                    break;
                case 1:                 /* DEC Ed */
#ifdef DYNAREC
                    if(rex.w)
                        if(((uintptr_t)ED)&7) {
                            // unaligned
                            do {
                                tmp64u = ED->q[0] & 0xffffffffffffff00LL;
                                tmp64u |= native_lock_read_b(ED);
                                tmp64u = dec64(emu, tmp64u);
                            } while(native_lock_write_b(ED, tmp64u&0xff));
                            ED->q[0] = tmp64u;
                        }
                        else
                            do {
                                tmp64u = native_lock_read_dd(ED);
                            } while(native_lock_write_dd(ED, dec64(emu, tmp64u)));
                    else {
                        do {
                            tmp16u = native_lock_read_h(ED);
                        } while(native_lock_write_h(ED, dec16(emu, tmp16u)));
                    }
#else
                    pthread_mutex_lock(&emu->context->mutex_lock);
                    if(rex.w) {
                        ED->q[0] = dec64(emu, ED->q[0]);
                    } else {
                        ED->word[0] = dec16(emu, ED->word[0]);
                    }
                    pthread_mutex_unlock(&emu->context->mutex_lock);
#endif
                    break;
                default:
                    printf_log(LOG_NONE, "Illegal Opcode 0xF0 0xFF 0x%02X 0x%02X\n", nextop, PK(0));
                    emu->quit=1;
                    emu->error |= ERR_ILLEGAL;
                    break;
            }
            break;
       default:
            return 0;
    }
    return addr;
}
