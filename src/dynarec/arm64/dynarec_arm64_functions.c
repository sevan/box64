#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug.h"
#include "box64context.h"
#include "dynarec.h"
#include "emu/x64emu_private.h"
#include "tools/bridge_private.h"
#include "x64run.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "emu/x64run_private.h"
#include "emu/x87emu_private.h"
#include "x64trace.h"
#include "signals.h"
#include "dynarec_native.h"
#include "dynarec_arm64_private.h"
#include "dynarec_arm64_functions.h"
#include "custommem.h"
#include "bridge.h"

#define XMM0    0
#define XMM8    16
#define X870    8
#define EMM0    8
#define SCRATCH0    24

// Get a FPU scratch reg
int fpu_get_scratch(dynarec_arm_t* dyn)
{
    return SCRATCH0 + dyn->n.fpu_scratch++;  // return an Sx
}
// Reset scratch regs counter
void fpu_reset_scratch(dynarec_arm_t* dyn)
{
    dyn->n.fpu_scratch = 0;
}
// Get a x87 double reg
int fpu_get_reg_x87(dynarec_arm_t* dyn, int t, int n)
{
    int i=X870;
    while (dyn->n.fpuused[i]) ++i;
    dyn->n.fpuused[i] = 1;
    dyn->n.neoncache[i].n = n;
    dyn->n.neoncache[i].t = t;
    dyn->n.news |= (1<<i);
    return i; // return a Dx
}
// Free a FPU double reg
void fpu_free_reg(dynarec_arm_t* dyn, int reg)
{
    // TODO: check upper limit?
    dyn->n.fpuused[reg] = 0;
    if(dyn->n.neoncache[reg].t!=NEON_CACHE_ST_F && dyn->n.neoncache[reg].t!=NEON_CACHE_ST_D)
        dyn->n.neoncache[reg].v = 0;
}
// Get an MMX double reg
int fpu_get_reg_emm(dynarec_arm_t* dyn, int emm)
{
    dyn->n.fpuused[EMM0 + emm] = 1;
    dyn->n.neoncache[EMM0 + emm].t = NEON_CACHE_MM;
    dyn->n.neoncache[EMM0 + emm].n = emm;
    dyn->n.news |= (1<<(EMM0 + emm));
    return EMM0 + emm;
}
// Get an XMM quad reg
int fpu_get_reg_xmm(dynarec_arm_t* dyn, int t, int xmm)
{
    int i;
    if(xmm>7) {
        i = XMM8 + xmm - 8;
    } else {
        i = XMM0 + xmm;
    }
    dyn->n.fpuused[i] = 1;
    dyn->n.neoncache[i].t = t;
    dyn->n.neoncache[i].n = xmm;
    dyn->n.news |= (1<<i);
    return i;
}
// Reset fpu regs counter
void fpu_reset_reg(dynarec_arm_t* dyn)
{
    dyn->n.fpu_reg = 0;
    for (int i=0; i<24; ++i) {
        dyn->n.fpuused[i]=0;
        dyn->n.neoncache[i].v = 0;
    }

}

int neoncache_get_st(dynarec_arm_t* dyn, int ninst, int a)
{
    if (dyn->insts[ninst].n.swapped) {
        if(dyn->insts[ninst].n.combined1 == a)
            a = dyn->insts[ninst].n.combined2;
        else if(dyn->insts[ninst].n.combined2 == a)
            a = dyn->insts[ninst].n.combined1;
    }
    for(int i=0; i<24; ++i)
        if((dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_F
         || dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_D)
         && dyn->insts[ninst].n.neoncache[i].n==a)
            return dyn->insts[ninst].n.neoncache[i].t;
    // not in the cache yet, so will be fetched...
    return NEON_CACHE_ST_D;
}

int neoncache_get_current_st(dynarec_arm_t* dyn, int ninst, int a)
{
    (void)ninst;
    if(!dyn->insts)
        return NEON_CACHE_ST_D;
    for(int i=0; i<24; ++i)
        if((dyn->n.neoncache[i].t==NEON_CACHE_ST_F
         || dyn->n.neoncache[i].t==NEON_CACHE_ST_D)
         && dyn->n.neoncache[i].n==a)
            return dyn->n.neoncache[i].t;
    // not in the cache yet, so will be fetched...
    return NEON_CACHE_ST_D;
}

int neoncache_get_st_f(dynarec_arm_t* dyn, int ninst, int a)
{
    /*if(a+dyn->insts[ninst].n.stack_next-st<0)
        // The STx has been pushed at the end of instructon, so stop going back
        return -1;*/
    for(int i=0; i<24; ++i)
        if(dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_F
         && dyn->insts[ninst].n.neoncache[i].n==a)
            return i;
    return -1;
} 
int neoncache_get_st_f_noback(dynarec_arm_t* dyn, int ninst, int a)
{
    for(int i=0; i<24; ++i)
        if(dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_F
         && dyn->insts[ninst].n.neoncache[i].n==a)
            return i;
    return -1;
} 
int neoncache_get_current_st_f(dynarec_arm_t* dyn, int a)
{
    for(int i=0; i<24; ++i)
        if(dyn->n.neoncache[i].t==NEON_CACHE_ST_F
         && dyn->n.neoncache[i].n==a)
            return i;
    return -1;
} 
static void neoncache_promote_double_forward(dynarec_arm_t* dyn, int ninst, int maxinst, int a);
static void neoncache_promote_double_internal(dynarec_arm_t* dyn, int ninst, int maxinst, int a);
static void neoncache_promote_double_combined(dynarec_arm_t* dyn, int ninst, int maxinst, int a)
{
    if(a == dyn->insts[ninst].n.combined1 || a == dyn->insts[ninst].n.combined2) {
        if(a == dyn->insts[ninst].n.combined1) {
            a = dyn->insts[ninst].n.combined2;
        } else 
            a = dyn->insts[ninst].n.combined1;
        int i = neoncache_get_st_f_noback(dyn, ninst, a);
        //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_combined, ninst=%d combined%c %d i=%d (stack:%d/%d)\n", ninst, (a == dyn->insts[ninst].n.combined2)?'2':'1', a ,i, dyn->insts[ninst].n.stack_push, -dyn->insts[ninst].n.stack_pop);
        if(i>=0) {
            dyn->insts[ninst].n.neoncache[i].t = NEON_CACHE_ST_D;
            if(!dyn->insts[ninst].n.barrier)
                neoncache_promote_double_internal(dyn, ninst-1, maxinst, a-dyn->insts[ninst].n.stack_push);
            // go forward is combined is not pop'd
            if(a-dyn->insts[ninst].n.stack_pop>=0)
                if(!dyn->insts[ninst+1].n.barrier)
                    neoncache_promote_double_forward(dyn, ninst+1, maxinst, a-dyn->insts[ninst].n.stack_pop);
        }
    }
}
static void neoncache_promote_double_internal(dynarec_arm_t* dyn, int ninst, int maxinst, int a)
{
    if(dyn->insts[ninst+1].n.barrier)
        return;
    while(ninst>=0) {
        a+=dyn->insts[ninst].n.stack_pop;    // adjust Stack depth: add pop'd ST (going backward)
        int i = neoncache_get_st_f(dyn, ninst, a);
        //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_internal, ninst=%d, a=%d st=%d:%d, i=%d\n", ninst, a, dyn->insts[ninst].n.stack, dyn->insts[ninst].n.stack_next, i);
        if(i<0) return;
        dyn->insts[ninst].n.neoncache[i].t = NEON_CACHE_ST_D;
        // check combined propagation too
        if(dyn->insts[ninst].n.combined1 || dyn->insts[ninst].n.combined2) {
            if(dyn->insts[ninst].n.swapped) {
                //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_internal, ninst=%d swapped %d/%d vs %d with st %d\n", ninst, dyn->insts[ninst].n.combined1 ,dyn->insts[ninst].n.combined2, a, dyn->insts[ninst].n.stack);
                if (a==dyn->insts[ninst].n.combined1)
                    a = dyn->insts[ninst].n.combined2;
                else if (a==dyn->insts[ninst].n.combined2)
                    a = dyn->insts[ninst].n.combined1;
            } else {
                //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_internal, ninst=%d combined %d/%d vs %d with st %d\n", ninst, dyn->insts[ninst].n.combined1 ,dyn->insts[ninst].n.combined2, a, dyn->insts[ninst].n.stack);
                neoncache_promote_double_combined(dyn, ninst, maxinst, a);
            }
        }
        a-=dyn->insts[ninst].n.stack_push;  // // adjust Stack depth: remove push'd ST (going backward)
        --ninst;
        if(ninst<0 || a<0 || dyn->insts[ninst].n.barrier)
            return;
    }
}

static void neoncache_promote_double_forward(dynarec_arm_t* dyn, int ninst, int maxinst, int a)
{
    while((ninst!=-1) && (ninst<maxinst) && (a>=0)) {
        a+=dyn->insts[ninst].n.stack_push;  // // adjust Stack depth: add push'd ST (going forward)
        if((dyn->insts[ninst].n.combined1 || dyn->insts[ninst].n.combined2) && dyn->insts[ninst].n.swapped) {
            //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_forward, ninst=%d swapped %d/%d vs %d with st %d\n", ninst, dyn->insts[ninst].n.combined1 ,dyn->insts[ninst].n.combined2, a, dyn->insts[ninst].n.stack);
            if (a==dyn->insts[ninst].n.combined1)
                a = dyn->insts[ninst].n.combined2;
            else if (a==dyn->insts[ninst].n.combined2)
                a = dyn->insts[ninst].n.combined1;
        }
        int i = neoncache_get_st_f_noback(dyn, ninst, a);
        //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_forward, ninst=%d, a=%d st=%d:%d(%d/%d), i=%d\n", ninst, a, dyn->insts[ninst].n.stack, dyn->insts[ninst].n.stack_next, dyn->insts[ninst].n.stack_push, -dyn->insts[ninst].n.stack_pop, i);
        if(i<0) return;
        dyn->insts[ninst].n.neoncache[i].t = NEON_CACHE_ST_D;
        // check combined propagation too
        if((dyn->insts[ninst].n.combined1 || dyn->insts[ninst].n.combined2) && !dyn->insts[ninst].n.swapped) {
            //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double_forward, ninst=%d combined %d/%d vs %d with st %d\n", ninst, dyn->insts[ninst].n.combined1 ,dyn->insts[ninst].n.combined2, a, dyn->insts[ninst].n.stack);
            neoncache_promote_double_combined(dyn, ninst, maxinst, a);
        }
        a-=dyn->insts[ninst].n.stack_pop;    // adjust Stack depth: remove pop'd ST (going forward)
        if(dyn->insts[ninst].x64.has_next && !dyn->insts[ninst].n.barrier)
            ++ninst;
        else
            ninst=-1;
    }
    if(ninst==maxinst)
        neoncache_promote_double(dyn, ninst, a);
}

void neoncache_promote_double(dynarec_arm_t* dyn, int ninst, int a)
{
    int i = neoncache_get_current_st_f(dyn, a);
    //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double, ninst=%d a=%d st=%d i=%d\n", ninst, a, dyn->n.stack, i);
    if(i<0) return;
    dyn->n.neoncache[i].t = NEON_CACHE_ST_D;
    dyn->insts[ninst].n.neoncache[i].t = NEON_CACHE_ST_D;
    // check combined propagation too
    if(dyn->n.combined1 || dyn->n.combined2) {
        if(dyn->n.swapped) {
            //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double, ninst=%d swapped! %d/%d vs %d\n", ninst, dyn->n.combined1 ,dyn->n.combined2, a);
            if(dyn->n.combined1 == a)
                a = dyn->n.combined2;
            else if(dyn->n.combined2 == a)
                a = dyn->n.combined1;
        } else {
            //if(box64_dynarec_dump) dynarec_log(LOG_NONE, "neoncache_promote_double, ninst=%d combined! %d/%d vs %d\n", ninst, dyn->n.combined1 ,dyn->n.combined2, a);
            if(dyn->n.combined1 == a)
                neoncache_promote_double(dyn, ninst, dyn->n.combined2);
            else if(dyn->n.combined2 == a)
                neoncache_promote_double(dyn, ninst, dyn->n.combined1);
        }
    }
    a-=dyn->insts[ninst].n.stack_push;  // // adjust Stack depth: remove push'd ST (going backward)
    if(!ninst || a<0) return;
    neoncache_promote_double_internal(dyn, ninst-1, ninst, a);
}

int neoncache_combine_st(dynarec_arm_t* dyn, int ninst, int a, int b)
{
    dyn->n.combined1=a;
    dyn->n.combined2=b;
    if( neoncache_get_current_st(dyn, ninst, a)==NEON_CACHE_ST_F
     && neoncache_get_current_st(dyn, ninst, b)==NEON_CACHE_ST_F )
        return NEON_CACHE_ST_F;
    return NEON_CACHE_ST_D;
}

static int isCacheEmpty(dynarec_native_t* dyn, int ninst) {
    if(dyn->insts[ninst].n.stack_next) {
        return 0;
    }
    for(int i=0; i<24; ++i)
        if(dyn->insts[ninst].n.neoncache[i].v) {       // there is something at ninst for i
            if(!(
            (dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_F || dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_D)
            && dyn->insts[ninst].n.neoncache[i].n<dyn->insts[ninst].n.stack_pop))
                return 0;
        }
    return 1;

}

int fpuCacheNeedsTransform(dynarec_arm_t* dyn, int ninst) {
    int i2 = dyn->insts[ninst].x64.jmp_insts;
    if(i2<0)
        return 1;
    if((dyn->insts[i2].x64.barrier&BARRIER_FLOAT))
        // if the barrier as already been apply, no transform needed
        return ((dyn->insts[ninst].x64.barrier&BARRIER_FLOAT))?0:(isCacheEmpty(dyn, ninst)?0:1);
    int ret = 0;
    if(!i2) { // just purge
        if(dyn->insts[ninst].n.stack_next) {
            return 1;
        }
        for(int i=0; i<24 && !ret; ++i)
            if(dyn->insts[ninst].n.neoncache[i].v) {       // there is something at ninst for i
                if(!(
                (dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_F || dyn->insts[ninst].n.neoncache[i].t==NEON_CACHE_ST_D)
                && dyn->insts[ninst].n.neoncache[i].n<dyn->insts[ninst].n.stack_pop))
                    ret = 1;
            }
        return ret;
    }
    // Check if ninst can be compatible to i2
    if(dyn->insts[ninst].n.stack_next != dyn->insts[i2].n.stack-dyn->insts[i2].n.stack_push) {
        return 1;
    }
    neoncache_t cache_i2 = dyn->insts[i2].n;
    neoncacheUnwind(&cache_i2);

    for(int i=0; i<24; ++i) {
        if(dyn->insts[ninst].n.neoncache[i].v) {       // there is something at ninst for i
            if(!cache_i2.neoncache[i].v) {    // but there is nothing at i2 for i
                ret = 1;
            } else if(dyn->insts[ninst].n.neoncache[i].v!=cache_i2.neoncache[i].v) {  // there is something different
                if(dyn->insts[ninst].n.neoncache[i].n!=cache_i2.neoncache[i].n) {   // not the same x64 reg
                    ret = 1;
                }
                else if(dyn->insts[ninst].n.neoncache[i].t == NEON_CACHE_XMMR && cache_i2.neoncache[i].t == NEON_CACHE_XMMW)
                    {/* nothing */ }
                else
                    ret = 1;
            }
        } else if(cache_i2.neoncache[i].v)
            ret = 1;
    }
    return ret;
}

void neoncacheUnwind(neoncache_t* cache)
{
    if(cache->swapped) {
        // unswap
        int a = -1; 
        int b = -1;
        for(int j=0; j<24 && ((a==-1) || (b==-1)); ++j)
            if((cache->neoncache[j].t == NEON_CACHE_ST_D || cache->neoncache[j].t == NEON_CACHE_ST_F)) {
                if(cache->neoncache[j].n == cache->combined1)
                    a = j;
                else if(cache->neoncache[j].n == cache->combined2)
                    b = j;
            }
        if(a!=-1 && b!=-1) {
            int tmp = cache->neoncache[a].n;
            cache->neoncache[a].n = cache->neoncache[b].n;
            cache->neoncache[b].n = tmp;
        }
        cache->swapped = 0;
        cache->combined1 = cache->combined2 = 0;
    }
    if(cache->news) {
        // reove the newly created neoncache
        for(int i=0; i<24; ++i)
            if(cache->news&(1<<i))
                cache->neoncache[i].v = 0;
        cache->news = 0;
    }
    if(cache->stack_push) {
        // unpush
        for(int j=0; j<24; ++j) {
            if((cache->neoncache[j].t == NEON_CACHE_ST_D || cache->neoncache[j].t == NEON_CACHE_ST_F)) {
                if(cache->neoncache[j].n<cache->stack_push)
                    cache->neoncache[j].v = 0;
                else
                    cache->neoncache[j].n-=cache->stack_push;
            }
        }
        cache->x87stack-=cache->stack_push;
        cache->stack-=cache->stack_push;
        cache->stack_push = 0;
    }
    cache->x87stack+=cache->stack_pop;
    cache->stack_next = cache->stack;
    cache->stack_pop = 0;
    cache->barrier = 0;
    // And now, rebuild the x87cache info with neoncache
    cache->mmxcount = 0;
    cache->fpu_scratch = 0;
    cache->fpu_extra_qscratch = 0;
    cache->fpu_reg = 0;
    for(int i=0; i<8; ++i) {
        cache->x87cache[i] = -1;
        cache->mmxcache[i] = -1;
        cache->x87reg[i] = 0;
        cache->ssecache[i*2].v = -1;
        cache->ssecache[i*2+1].v = -1;
    }
    int x87reg = 0;
    for(int i=0; i<24; ++i) {
        if(cache->neoncache[i].v) {
            cache->fpuused[i] = 1;
            switch (cache->neoncache[i].t) {
                case NEON_CACHE_MM:
                    cache->mmxcache[cache->neoncache[i].n] = i;
                    ++cache->mmxcount;
                    ++cache->fpu_reg;
                    break;
                case NEON_CACHE_XMMR:
                case NEON_CACHE_XMMW:
                    cache->ssecache[cache->neoncache[i].n].reg = i;
                    cache->ssecache[cache->neoncache[i].n].write = (cache->neoncache[i].t==NEON_CACHE_XMMW)?1:0;
                    ++cache->fpu_reg;
                    break;
                case NEON_CACHE_ST_F:
                case NEON_CACHE_ST_D:
                    cache->x87cache[x87reg] = cache->neoncache[i].n;
                    cache->x87reg[x87reg] = i;
                    ++x87reg;
                    ++cache->fpu_reg;
                    break;
                case NEON_CACHE_SCR:
                    cache->fpuused[i] = 0;
                    cache->neoncache[i].v = 0;
                    break;
            }
        } else {
            cache->fpuused[i] = 0;
        }
    }
}

#define F8      *(uint8_t*)(addr++)
#define F32S64  (uint64_t)(int64_t)*(int32_t*)(addr+=4, addr-4)
// Get if ED will have the correct parity. Not emiting anything. Parity is 2 for DWORD or 3 for QWORD
int getedparity(dynarec_arm_t* dyn, int ninst, uintptr_t addr, uint8_t nextop, int parity, int delta)
{
    (void)dyn; (void)ninst;

    uint32_t tested = (1<<parity)-1;
    if((nextop&0xC0)==0xC0)
        return 0;   // direct register, no parity...
    if(!(nextop&0xC0)) {
        if((nextop&7)==4) {
            uint8_t sib = F8;
            int sib_reg = (sib>>3)&7;
            if((sib&0x7)==5) {
                uint64_t tmp = F32S64;
                if (sib_reg!=4) {
                    // if XXXXXX+reg<<N then check parity of XXXXX and N should be enough
                    return ((tmp&tested)==0 && (sib>>6)>=parity)?1:0;
                } else {
                    // just a constant...
                    return (tmp&tested)?0:1;
                }
            } else {
                if(sib_reg==4 && parity<3)
                    return 0;   // simple [reg]
                // don't try [reg1 + reg2<<N], unless reg1 is ESP
                return ((sib&0x7)==4 && (sib>>6)>=parity)?1:0;
            }
        } else if((nextop&7)==5) {
            uint64_t tmp = F32S64;
            tmp+=addr+delta;
            return (tmp&tested)?0:1;
        } else {
            return 0;
        }
    } else {
        return 0; //Form [reg1 + reg2<<N + XXXXXX]
    }
}
#undef F8
#undef F32S64

const char* getCacheName(int t, int n)
{
    static char buff[20];
    switch(t) {
        case NEON_CACHE_ST_D: sprintf(buff, "ST%d", n); break;
        case NEON_CACHE_ST_F: sprintf(buff, "st%d", n); break;
        case NEON_CACHE_MM: sprintf(buff, "MM%d", n); break;
        case NEON_CACHE_XMMW: sprintf(buff, "XMM%d", n); break;
        case NEON_CACHE_XMMR: sprintf(buff, "xmm%d", n); break;
        case NEON_CACHE_SCR: sprintf(buff, "Scratch"); break;
        case NEON_CACHE_NONE: buff[0]='\0'; break;
    }
    return buff;
}
