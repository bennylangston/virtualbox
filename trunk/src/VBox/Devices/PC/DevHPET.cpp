/* $Id$ */
/** @file
 * HPET virtual device - high precision event timer emulation
 */

/*
 * Copyright (C) 2009-2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_HPET
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/stam.h>
#include <VBox/log.h>
#include <iprt/assert.h>
#include <iprt/asm-math.h>
#include <iprt/string.h>

#include "VBoxDD.h"


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/*
 * Current limitations:
 *   - not entirely correct time of interrupt, i.e. never
 *     schedule interrupt earlier than in 1ms
 *   - statistics not implemented
 *   - level-triggered mode not implemented
 */
/*
 * Base address for MMIO
 */
#define HPET_BASE                   0xfed00000

/*
 * Number of available timers, cannot be changed without
 * breaking saved states.
 */
#define HPET_NUM_TIMERS             3
#define HPET_NUM_TIMERS_ICH9        4

/*
 * 10000000 femtoseconds == 10ns
 */
#define HPET_CLK_PERIOD             UINT32_C(10000000)

/*
 * 69841279 femtoseconds == 69.84 ns (1 / 14.31818MHz)
 */
#define HPET_CLK_PERIOD_ICH9        UINT32_C(69841279)

/*
 * Femptosecods in nanosecond
 */
#define FS_PER_NS                   1000000

/*
 * Interrupt type
 */
#define HPET_TIMER_TYPE_LEVEL       1
#define HPET_TIMER_TYPE_EDGE        0

/* Delivery mode */
/* Via APIC */
#define HPET_TIMER_DELIVERY_APIC    0
/* Via FSB */
#define HPET_TIMER_DELIVERY_FSB     1

#define HPET_TIMER_CAP_FSB_INT_DEL (1 << 15)
#define HPET_TIMER_CAP_PER_INT     (1 << 4)

#define HPET_CFG_ENABLE          0x001 /* ENABLE_CNF */
#define HPET_CFG_LEGACY          0x002 /* LEG_RT_CNF */

#define HPET_ID                  0x000
#define HPET_PERIOD              0x004
#define HPET_CFG                 0x010
#define HPET_STATUS              0x020
#define HPET_COUNTER             0x0f0
#define HPET_TN_CFG              0x000
#define HPET_TN_CMP              0x008
#define HPET_TN_ROUTE            0x010
#define HPET_CFG_WRITE_MASK      0x3

#define HPET_TN_INT_TYPE                      RT_BIT_64(1)
#define HPET_TN_ENABLE                        RT_BIT_64(2)
#define HPET_TN_PERIODIC                      RT_BIT_64(3)
#define HPET_TN_PERIODIC_CAP                  RT_BIT_64(4)
#define HPET_TN_SIZE_CAP                      RT_BIT_64(5)
#define HPET_TN_SETVAL                        RT_BIT_64(6)
#define HPET_TN_32BIT                         RT_BIT_64(8)
#define HPET_TN_INT_ROUTE_MASK                UINT64_C(0x3e00)
#define HPET_TN_CFG_WRITE_MASK                UINT64_C(0x3e46)
#define HPET_TN_INT_ROUTE_SHIFT               9
#define HPET_TN_INT_ROUTE_CAP_SHIFT           32
#define HPET_TN_CFG_BITS_READONLY_OR_RESERVED 0xffff80b1U

/** The version of the saved state. */
#define HPET_SAVED_STATE_VERSION       2
/** Empty saved state */
#define HPET_SAVED_STATE_VERSION_EMPTY 1


/**
 * Acquires the HPET lock or returns.
 */
#define DEVHPET_LOCK_RETURN(a_pThis, a_rcBusy)  \
    do { \
        int rcLock = PDMCritSectEnter(&(a_pThis)->csLock, (a_rcBusy)); \
        if (rcLock != VINF_SUCCESS) \
            return rcLock; \
    } while (0)

/**
 * Releases the HPET lock.
 */
#define DEVHPET_UNLOCK(a_pThis) \
    do { PDMCritSectLeave(&(a_pThis)->csLock); } while (0)


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
struct HpetState;
typedef struct HpetTimer
{
    /** The HPET timer - R3 Ptr. */
    PTMTIMERR3                       pTimerR3;
    /** Pointer to the instance data - R3 Ptr. */
    R3PTRTYPE(struct HpetState *)    pHpetR3;

    /** The HPET timer - R0 Ptr. */
    PTMTIMERR0                       pTimerR0;
    /** Pointer to the instance data - R0 Ptr. */
    R0PTRTYPE(struct HpetState *)    pHpetR0;

    /** The HPET timer - RC Ptr. */
    PTMTIMERRC                       pTimerRC;
    /** Pointer to the instance data - RC Ptr. */
    RCPTRTYPE(struct HpetState *)    pHpetRC;

    /** Timer index. */
    uint8_t                          idxTimer;
    /** Wrap. */
    uint8_t                          u8Wrap;
    /** Alignment. */
    uint32_t                         alignment0;

    /** @name Memory-mapped, software visible timer registers.
     * @{ */
    /** Configuration/capabilities. */
    uint64_t                         u64Config;
    /** Comparator. */
    uint64_t                         u64Cmp;
    /** FSB route, not supported now. */
    uint64_t                         u64Fsb;
    /** @} */

    /** @name Hidden register state.
     * @{ */
    /** Last value written to comparator. */
    uint64_t                         u64Period;
    /** @} */
} HpetTimer;
AssertCompileMemberAlignment(HpetTimer, u64Config, sizeof(uint64_t));

typedef struct HpetState
{
    /** Pointer to the device instance. - R3 ptr. */
    PPDMDEVINSR3         pDevInsR3;
    /** The HPET helpers - R3 Ptr. */
    PCPDMHPETHLPR3       pHpetHlpR3;

    /** Pointer to the device instance. - R0 ptr. */
    PPDMDEVINSR0         pDevInsR0;
    /** The HPET helpers - R0 Ptr. */
    PCPDMHPETHLPR0       pHpetHlpR0;

    /** Pointer to the device instance. - RC ptr. */
    PPDMDEVINSRC         pDevInsRC;
    /** The HPET helpers - RC Ptr. */
    PCPDMHPETHLPRC       pHpetHlpRC;

    /** Timer structures. */
    HpetTimer            aTimers[HPET_NUM_TIMERS];

    /** Offset realtive to the virtual sync clock. */
    uint64_t             u64HpetOffset;

    /** @name Memory-mapped, software visible registers
     * @{ */
    /** Capabilities. */
    uint64_t             u64Capabilities;
    /** Configuration. */
    uint64_t             u64HpetConfig;
    /** Interrupt status register. */
    uint64_t             u64Isr;
    /** Main counter. */
    uint64_t             u64HpetCounter;
    /** @}  */

    /** Global device lock. */
    PDMCRITSECT          csLock;

    /** If we emulate ICH9 HPET (different frequency).
     * @todo different number of timers */
    bool                 fIch9;
    uint8_t              padding0[7];
} HpetState;


#ifndef VBOX_DEVICE_STRUCT_TESTCASE


DECLINLINE(bool) hpet32bitTimer(HpetTimer *pHpetTimer)
{
    uint64_t u64Cfg = pHpetTimer->u64Config;

    return ((u64Cfg & HPET_TN_SIZE_CAP) == 0) || ((u64Cfg & HPET_TN_32BIT) != 0);
}

DECLINLINE(uint64_t) hpetInvalidValue(HpetTimer *pHpetTimer)
{
    return hpet32bitTimer(pHpetTimer) ? UINT32_MAX : UINT64_MAX;
}

DECLINLINE(uint32_t) hpetTimeAfter32(uint64_t a, uint64_t b)
{
    return ((int32_t)(b) - (int32_t)(a) <= 0);
}

DECLINLINE(uint32_t) hpetTimeAfter64(uint64_t a, uint64_t b)
{
    return ((int64_t)(b) - (int64_t)(a) <= 0);
}

DECLINLINE(uint64_t) hpetTicksToNs(HpetState *pThis, uint64_t value)
{
    return (ASMMultU64ByU32DivByU32(value,  (uint32_t)(pThis->u64Capabilities >> 32), FS_PER_NS));
}

DECLINLINE(uint64_t) nsToHpetTicks(HpetState const *pThis, uint64_t u64Value)
{
    return (ASMMultU64ByU32DivByU32(u64Value, FS_PER_NS, (uint32_t)(pThis->u64Capabilities >> 32)));
}

DECLINLINE(uint64_t) hpetGetTicks(HpetState const *pThis)
{
    /*
     * We can use any timer to get current time, they all go
     * with the same speed.
     */
    return nsToHpetTicks(pThis,
                           TMTimerGet(pThis->aTimers[0].CTX_SUFF(pTimer))
                         + pThis->u64HpetOffset);
}

DECLINLINE(uint64_t) hpetUpdateMasked(uint64_t u64NewValue,
                                      uint64_t u64OldValue,
                                      uint64_t u64Mask)
{
    u64NewValue &= u64Mask;
    u64NewValue |= (u64OldValue & ~u64Mask);
    return u64NewValue;
}

DECLINLINE(bool) hpetBitJustSet(uint64_t u64OldValue,
                                uint64_t u64NewValue,
                                uint64_t u64Mask)
{
    return !(u64OldValue & u64Mask)
        && !!(u64NewValue & u64Mask);
}

DECLINLINE(bool) hpetBitJustCleared(uint64_t u64OldValue,
                                    uint64_t u64NewValue,
                                    uint64_t u64Mask)
{
    return !!(u64OldValue & u64Mask)
        && !(u64NewValue & u64Mask);
}

DECLINLINE(uint64_t) hpetComputeDiff(HpetTimer *pHpetTimer,
                                     uint64_t   u64Now)
{

    if (hpet32bitTimer(pHpetTimer))
    {
        uint32_t u32Diff;

        u32Diff = (uint32_t)pHpetTimer->u64Cmp - (uint32_t)u64Now;
        u32Diff = ((int32_t)u32Diff > 0) ? u32Diff : (uint32_t)0;
        return (uint64_t)u32Diff;
    }
    else
    {
        uint64_t u64Diff;

        u64Diff = pHpetTimer->u64Cmp - u64Now;
        u64Diff = ((int64_t)u64Diff > 0) ?  u64Diff : (uint64_t)0;
        return u64Diff;
    }
}


static void hpetAdjustComparator(HpetTimer *pHpetTimer, uint64_t u64Now)
{
  uint64_t u64Period = pHpetTimer->u64Period;
  if (   (pHpetTimer->u64Config & HPET_TN_PERIODIC)
      && u64Period != 0)
  {
      /* While loop is suboptimal */
      if (hpet32bitTimer(pHpetTimer))
      {
          while (hpetTimeAfter32(u64Now, pHpetTimer->u64Cmp))
              pHpetTimer->u64Cmp = (uint32_t)(pHpetTimer->u64Cmp + u64Period);
      }
      else
      {
          while (hpetTimeAfter64(u64Now, pHpetTimer->u64Cmp))
              pHpetTimer->u64Cmp += u64Period;
      }
  }
}

static void hpetProgramTimer(HpetTimer *pHpetTimer)
{
    uint64_t u64Ticks = hpetGetTicks(pHpetTimer->CTX_SUFF(pHpet));
    uint64_t u64Diff;
    uint32_t u32TillWrap;

    /* no wrapping on new timers */
    pHpetTimer->u8Wrap = 0;

    hpetAdjustComparator(pHpetTimer, u64Ticks);

    u64Diff = hpetComputeDiff(pHpetTimer, u64Ticks);

    /*
     * HPET spec says in one-shot 32-bit mode, generate an interrupt when
     * counter wraps in addition to an interrupt with comparator match.
     */
    if (    hpet32bitTimer(pHpetTimer)
        && !(pHpetTimer->u64Config & HPET_TN_PERIODIC))
    {
        u32TillWrap = 0xffffffff - (uint32_t)u64Ticks + 1;
        if (u32TillWrap < (uint32_t)u64Diff)
        {
            Log(("wrap on timer %d: till=%u ticks=%lld diff64=%lld\n",
                 pHpetTimer->idxTimer, u32TillWrap, u64Ticks, u64Diff));
            u64Diff = u32TillWrap;
            pHpetTimer->u8Wrap = 1;
        }
    }

    /* Avoid killing VM with interrupts */
#if 1
    /* @todo: HACK, rethink, may have negative impact on the guest */
    if (u64Diff == 0)
        u64Diff = 100000; /* 1 millisecond */
#endif

    Log4(("HPET: next IRQ in %lld ticks (%lld ns)\n", u64Diff, hpetTicksToNs(pHpetTimer->CTX_SUFF(pHpet), u64Diff)));

    TMTimerSetNano(pHpetTimer->CTX_SUFF(pTimer), hpetTicksToNs(pHpetTimer->CTX_SUFF(pHpet), u64Diff));
}


/* -=-=-=-=-=- Timer register accesses -=-=-=-=-=- */


/**
 * Reads a HPET timer register.
 *
 * @returns VBox strict status code.
 * @param   pThis               The HPET instance.
 * @param   iTimerNo            The timer index.
 * @param   iTimerReg           The index of the timer register to read.
 * @param   pu32Value           Where to return the register value.
 *
 * @remarks ASSUMES the caller does holds the HPET lock.
 */
static int hpetTimerRegRead32(HpetState const *pThis, uint32_t iTimerNo, uint32_t iTimerReg, uint32_t *pu32Value)
{
    if (iTimerNo >= HPET_NUM_TIMERS)
    {
        static unsigned s_cOccurences = 0;
        if (s_cOccurences++ < 10)
            LogRel(("HPET: using timer above configured range: %d\n", iTimerNo));
        *pu32Value = 0;
        return VINF_SUCCESS;
    }

    HpetTimer const *pHpetTimer = &pThis->aTimers[iTimerNo];
    uint32_t u32Value;
    switch (iTimerReg)
    {
        case HPET_TN_CFG:
            u32Value = (uint32_t)pHpetTimer->u64Config;
            Log(("read HPET_TN_CFG on %d: %#x\n", iTimerNo, u32Value));
            break;

        case HPET_TN_CFG + 4:
            u32Value = (uint32_t)(pHpetTimer->u64Config >> 32);
            Log(("read HPET_TN_CFG+4 on %d: %#x\n", iTimerNo, u32Value));
            break;

        case HPET_TN_CMP:
            u32Value = (uint32_t)pHpetTimer->u64Cmp;
            Log(("read HPET_TN_CMP on %d: %#x (%#llx)\n", pHpetTimer->idxTimer, u32Value, pHpetTimer->u64Cmp));
            break;

        case HPET_TN_CMP + 4:
            u32Value = (uint32_t)(pHpetTimer->u64Cmp >> 32);
            Log(("read HPET_TN_CMP+4 on %d: %#x (%#llx)\n", pHpetTimer->idxTimer, u32Value, pHpetTimer->u64Cmp));
            break;

        case HPET_TN_ROUTE:
            u32Value = (uint32_t)(pHpetTimer->u64Fsb >> 32); /** @todo Looks wrong, but since it's not supported, who cares. */
            Log(("read HPET_TN_ROUTE on %d: %#x\n", iTimerNo, u32Value));
            break;

        default:
        {
            static unsigned s_cOccurences = 0;
            if (s_cOccurences++ < 10)
                LogRel(("invalid HPET register read %d on %d\n", iTimerReg, pHpetTimer->idxTimer));
            u32Value = 0;
            break;
        }
    }
    *pu32Value = u32Value;
    return VINF_SUCCESS;
}


/**
 * 32-bit write to a HPET timer register.
 *
 * @returns Strict VBox status code.
 *
 * @param   pThis           The HPET state.
 * @param   idxReg          The register being written to.
 * @param   u32NewValue     The value being written.
 */
static int hpetTimerRegWrite32(HpetState *pThis, uint32_t iTimerNo, uint32_t iTimerReg, uint32_t u32NewValue)
{
    if (iTimerNo >= HPET_NUM_TIMERS)
    {
        LogRel(("HPET: using timer above configured range: %d\n", iTimerNo));
        return VINF_SUCCESS;
    }
    HpetTimer *pHpetTimer = &pThis->aTimers[iTimerNo];

    switch (iTimerReg)
    {
        case HPET_TN_CFG:
        {
            Log(("write HPET_TN_CFG: %d: %x\n", iTimerNo, u32NewValue));
            uint64_t const iOldValue = (uint32_t)pHpetTimer->u64Config;

            uint64_t u64Mask = HPET_TN_CFG_WRITE_MASK;
            if ((pHpetTimer->u64Config & HPET_TN_PERIODIC_CAP) != 0)
                u64Mask |= HPET_TN_PERIODIC;

            if ((pHpetTimer->u64Config & HPET_TN_SIZE_CAP) != 0)
                u64Mask |= HPET_TN_32BIT;
            else
                u32NewValue &= ~HPET_TN_32BIT;

            if ((u32NewValue & HPET_TN_32BIT) != 0)
            {
                Log(("setting timer %d to 32-bit mode\n", iTimerNo));
                pHpetTimer->u64Cmp    = (uint32_t)pHpetTimer->u64Cmp;
                pHpetTimer->u64Period = (uint32_t)pHpetTimer->u64Period;
            }
            if ((u32NewValue & HPET_TN_INT_TYPE) == HPET_TIMER_TYPE_LEVEL)
            {
                LogRel(("level-triggered config not yet supported\n"));
                AssertFailed();
            }

            /* We only care about lower 32-bits so far */
            pHpetTimer->u64Config = hpetUpdateMasked(u32NewValue, iOldValue, u64Mask);
            break;
        }

        case HPET_TN_CFG + 4: /* Interrupt capabilities */
        {
            Log(("write HPET_TN_CFG + 4, useless\n"));
            break;
        }

        case HPET_TN_CMP: /* lower bits of comparator register */
        {
            Log(("write HPET_TN_CMP on %d: %x\n", iTimerNo, u32NewValue));
            if (pHpetTimer->u64Config & HPET_TN_PERIODIC)
            {
                u32NewValue &= hpetInvalidValue(pHpetTimer) >> 1;
                pHpetTimer->u64Period = (pHpetTimer->u64Period & UINT64_C(0xffffffff00000000))
                                      | u32NewValue;
            }

            pHpetTimer->u64Cmp = (pHpetTimer->u64Cmp & UINT64_C(0xffffffff00000000))
                               | u32NewValue;

            pHpetTimer->u64Config &= ~HPET_TN_SETVAL;
            Log2(("after HPET_TN_CMP cmp=%llx per=%llx\n", pHpetTimer->u64Cmp, pHpetTimer->u64Period));

            if (pThis->u64HpetConfig & HPET_CFG_ENABLE)
                hpetProgramTimer(pHpetTimer);
            break;
        }

        case HPET_TN_CMP + 4: /* upper bits of comparator register */
        {
            Log(("write HPET_TN_CMP + 4 on %d: %x\n", iTimerNo, u32NewValue));
            if (hpet32bitTimer(pHpetTimer))
                break;

            if (pHpetTimer->u64Config & HPET_TN_PERIODIC)
                pHpetTimer->u64Period = RT_MAKE_U64(pHpetTimer->u64Period, u32NewValue);
            pHpetTimer->u64Cmp = RT_MAKE_U64(pHpetTimer->u64Cmp, u32NewValue);

            Log2(("after HPET_TN_CMP+4 cmp=%llx per=%llx tmr=%d\n", pHpetTimer->u64Cmp, pHpetTimer->u64Period, iTimerNo));

            pHpetTimer->u64Config &= ~HPET_TN_SETVAL;

            if (pThis->u64HpetConfig & HPET_CFG_ENABLE)
                hpetProgramTimer(pHpetTimer);
            break;
        }

        case HPET_TN_ROUTE:
        {
            Log(("write HPET_TN_ROUTE\n"));
            break;
        }

        case HPET_TN_ROUTE + 4:
        {
            Log(("write HPET_TN_ROUTE + 4\n"));
            break;
        }

        default:
        {
            LogRel(("invalid timer register write: %d\n", iTimerReg));
            AssertFailed();
            break;
        }
    }

    return VINF_SUCCESS;
}


/* -=-=-=-=-=- Non-timer register accesses -=-=-=-=-=- */


/**
 * Read a 32-bit HPET register.
 *
 * @returns Strict VBox status code.
 * @param   pThis               The HPET state.
 * @param   idxReg              The register to read.
 * @param   pu32Value           Where to return the register value.
 */
static int hpetConfigRegRead32(HpetState const *pThis, uint32_t idxReg, uint32_t *pu32Value)
{
    uint32_t u32Value;
    switch (idxReg)
    {
        case HPET_ID:
            u32Value = (uint32_t)pThis->u64Capabilities;
            Log(("read HPET_ID: %#x\n", u32Value));
            break;

        case HPET_PERIOD:
            u32Value = (uint32_t)(pThis->u64Capabilities >> 32);
            Log(("read HPET_PERIOD: %#x\n", u32Value));
            break;

        case HPET_CFG:
            u32Value = (uint32_t)pThis->u64HpetConfig;
            Log(("read HPET_CFG: %#x\n", u32Value));
            break;

        case HPET_CFG + 4:
            u32Value = (uint32_t)(pThis->u64HpetConfig >> 32);
            Log(("read of HPET_CFG + 4: %#x\n", u32Value));
            break;

        case HPET_COUNTER:
        case HPET_COUNTER + 4:
        {
            uint64_t u64Ticks;
            if (pThis->u64HpetConfig & HPET_CFG_ENABLE)
                u64Ticks = hpetGetTicks(pThis);
            else
                u64Ticks = pThis->u64HpetCounter;
            /** @todo is it correct? */
            u32Value = (idxReg == HPET_COUNTER) ? (uint32_t)u64Ticks : (uint32_t)(u64Ticks >> 32);
            Log(("read HPET_COUNTER: %s part value %x (%#llx)\n",
                 (idxReg == HPET_COUNTER) ? "low" : "high", u32Value, u64Ticks));
            break;
        }

        case HPET_STATUS:
            Log(("read HPET_STATUS\n"));
            u32Value = (uint32_t)pThis->u64Isr;
            break;

        default:
            Log(("invalid HPET register read: %x\n", idxReg));
            u32Value = 0;
            break;
    }

    *pu32Value = u32Value;
    return VINF_SUCCESS;
}


/**
 * 32-bit write to a config register.
 *
 * @returns Strict VBox status code.
 *
 * @param   pThis           The HPET state.
 * @param   idxReg          The register being written to.
 * @param   u32NewValue     The value being written.
 */
static int hpetConfigRegWrite32(HpetState *pThis, uint32_t idxReg, uint32_t u32NewValue)
{
    int rc = VINF_SUCCESS;
    switch (idxReg)
    {
        case HPET_ID:
        case HPET_ID + 4:
        {
            Log(("write HPET_ID, useless\n"));
            break;
        }

        case HPET_CFG:
        {
            uint32_t const iOldValue = (uint32_t)(pThis->u64HpetConfig);
            Log(("write HPET_CFG: %x (old %x)\n", u32NewValue, iOldValue));

            /*
             * This check must be here, before actual update, as hpetLegacyMode
             * may request retry in R3 - so we must keep state intact.
             */
            if (   ((iOldValue ^ u32NewValue) & HPET_CFG_LEGACY)
                && pThis->pHpetHlpR3 != NIL_RTR3PTR)
            {
#ifdef IN_RING3
                rc = pThis->pHpetHlpR3->pfnSetLegacyMode(pThis->pDevInsR3, RT_BOOL(u32NewValue & HPET_CFG_LEGACY));
                if (rc != VINF_SUCCESS)
                    break;
#else
                rc = VINF_IOM_HC_MMIO_WRITE;
                break;
#endif
            }

            pThis->u64HpetConfig = hpetUpdateMasked(u32NewValue, iOldValue, HPET_CFG_WRITE_MASK);

            if (hpetBitJustSet(iOldValue, u32NewValue, HPET_CFG_ENABLE))
            {
/** @todo Only get the time stamp once when reprogramming? */
                /* Enable main counter and interrupt generation. */
                pThis->u64HpetOffset = hpetTicksToNs(pThis, pThis->u64HpetCounter)
                                     - TMTimerGet(pThis->aTimers[0].CTX_SUFF(pTimer));
                for (uint32_t i = 0; i < HPET_NUM_TIMERS; i++)
                    if (pThis->aTimers[i].u64Cmp != hpetInvalidValue(&pThis->aTimers[i]))
                        hpetProgramTimer(&pThis->aTimers[i]);
            }
            else if (hpetBitJustCleared(iOldValue, u32NewValue, HPET_CFG_ENABLE))
            {
                /* Halt main counter and disable interrupt generation. */
                pThis->u64HpetCounter = hpetGetTicks(pThis);
                for (uint32_t i = 0; i < HPET_NUM_TIMERS; i++)
                    TMTimerStop(pThis->aTimers[i].CTX_SUFF(pTimer));
            }
            break;
        }

        case HPET_CFG + 4:
        {
            /** @todo why do we let the guest write to the high bits but not
             *        read them? */
            pThis->u64HpetConfig = hpetUpdateMasked((uint64_t)u32NewValue << 32,
                                                    pThis->u64HpetConfig,
                                                    UINT64_C(0xffffffff00000000));
            Log(("write HPET_CFG + 4: %x -> %#llx\n", u32NewValue, pThis->u64HpetConfig));
            break;
        }

        case HPET_STATUS:
        {
            /* Clear ISR for all set bits in u32NewValue, see p. 14 of the HPET spec. */
            pThis->u64Isr &= ~((uint64_t)u32NewValue);
            Log(("write HPET_STATUS: %x -> ISR=%#llx\n", u32NewValue, pThis->u64Isr));
            break;
        }

        case HPET_STATUS + 4:
        {
            Log(("write HPET_STATUS + 4: %x\n", u32NewValue));
            if (u32NewValue != 0)
            {
                static unsigned s_cOccurrences = 0;
                if (s_cOccurrences++ < 10)
                    LogRel(("Writing HPET_STATUS + 4 with non-zero, ignored\n"));
            }
            break;
        }

        case HPET_COUNTER:
        {
            pThis->u64HpetCounter = RT_MAKE_U64(u32NewValue, pThis->u64HpetCounter);
            Log(("write HPET_COUNTER: %#x -> %llx\n", u32NewValue, pThis->u64HpetCounter));
            break;
        }

        case HPET_COUNTER + 4:
        {
            pThis->u64HpetCounter = RT_MAKE_U64(pThis->u64HpetCounter, u32NewValue);
            Log(("write HPET_COUNTER + 4: %#x -> %llx\n", u32NewValue, pThis->u64HpetCounter));
            break;
        }

        default:
            LogRel(("invalid HPET config write: %x\n", idxReg));
            break;
    }

    return rc;
}


/* -=-=-=-=-=- MMIO callbacks -=-=-=-=-=- */


/**
 * @callback_method_impl{FNIOMMMIOREAD}
 */
PDMBOTHCBDECL(int)  hpetMMIORead(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    HpetState      *pThis  = PDMINS_2_DATA(pDevIns, HpetState*);
    uint32_t const  idxReg = (uint32_t)(GCPhysAddr - HPET_BASE);

    LogFlow(("hpetMMIORead (%d): %llx (%x)\n", cb, (uint64_t)GCPhysAddr, idxReg));
    DEVHPET_LOCK_RETURN(pThis, VINF_IOM_HC_MMIO_READ);

    int rc = VINF_SUCCESS;
    switch (cb)
    {
        case 4:
            if (idxReg >= 0x100 && idxReg < 0x400)
                rc = hpetTimerRegRead32(pThis,
                                        (idxReg - 0x100) / 0x20,
                                        (idxReg - 0x100) % 0x20,
                                        (uint32_t *)pv);
            else
                rc = hpetConfigRegRead32(pThis, idxReg, (uint32_t *)pv);
            break;

        case 8:
        {
            /* Unaligned accesses not allowed */
            if (RT_UNLIKELY(idxReg % 8 != 0))
            {
                rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "idxReg=%#x cb=8\n", idxReg);
                break;
            }

            /* Split the access except for timing sensitive registers.  The
               others assume the protection of the lock. */
            PRTUINT64U pValue = (PRTUINT64U)pv;
            if (idxReg == HPET_COUNTER)
            {
                /* When reading HPET counter we must read it in a single read,
                   to avoid unexpected time jumps on 32-bit overflow. */
                pValue->u = (pThis->u64HpetConfig & HPET_CFG_ENABLE) != 0
                          ? hpetGetTicks(pThis)
                          : pThis->u64HpetCounter;
                rc = VINF_SUCCESS;
            }
            else if (idxReg >= 0x100 && idxReg < 0x400)
            {
                uint32_t iTimer    = (idxReg - 0x100) / 0x20;
                uint32_t iTimerReg = (idxReg - 0x100) % 0x20;
                rc = hpetTimerRegRead32(pThis, iTimer, iTimerReg, &pValue->s.Lo);
                if (rc == VINF_SUCCESS)
                    rc = hpetTimerRegRead32(pThis, iTimer, iTimerReg + 4, &pValue->s.Hi);
            }
            else
            {
                /* for most 8-byte accesses we just split them, happens under lock anyway. */
                rc = hpetConfigRegRead32(pThis, idxReg, &pValue->s.Lo);
                if (rc == VINF_SUCCESS)
                    rc = hpetConfigRegRead32(pThis, idxReg + 4, &pValue->s.Hi);
            }
            break;
        }

        case 1:
        case 2:
            Log(("Narrow read: %d\n", cb));
            rc = VINF_SUCCESS;
            break;

        default:
            AssertReleaseMsgFailed(("cb=%d\n", cb)); /* for now we assume simple accesses. */
            rc = VINF_SUCCESS;
    }

    DEVHPET_UNLOCK(pThis);
    return rc;
}


/**
 * @callback_method_impl{FNIOMMMIOWRITE}
 */
PDMBOTHCBDECL(int) hpetMMIOWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    HpetState  *pThis  = PDMINS_2_DATA(pDevIns, HpetState*);
    uint32_t    idxReg = (uint32_t)(GCPhysAddr - HPET_BASE);
    LogFlow(("hpetMMIOWrite: cb=%u reg=%03x (%RGp) val=%llx\n",
             cb, idxReg, GCPhysAddr, cb == 4 ? *(uint32_t *)pv : cb == 8 ? *(uint64_t *)pv : 0xdeadbeef));

    DEVHPET_LOCK_RETURN(pThis, VINF_IOM_HC_MMIO_WRITE);

    int rc;
    switch (cb)
    {
        case 4:
            if (idxReg >= 0x100 && idxReg < 0x400)
                rc = hpetTimerRegWrite32(pThis,
                                         (idxReg - 0x100) / 0x20,
                                         (idxReg - 0x100) % 0x20,
                                         *(uint32_t const *)pv);
            else
                rc = hpetConfigRegWrite32(pThis, idxReg, *(uint32_t const *)pv);
            break;

        case 8:
        {
            /* Unaligned accesses are not allowed. */
            if (RT_UNLIKELY(idxReg % 8 != 0))
            {
                rc = PDMDevHlpDBGFStop(pDevIns, RT_SRC_POS, "idxReg=%#x cb=8\n", idxReg);
                break;
            }

            /* Split the access and rely on the locking to prevent trouble. */
            RTUINT64U uValue;
            uValue.u = *(uint64_t const *)pv;
            if (idxReg >= 0x100 && idxReg < 0x400)
            {
                uint32_t iTimer    = (idxReg - 0x100) / 0x20;
                uint32_t iTimerReg = (idxReg - 0x100) % 0x20;
/** @todo Consider handling iTimerReg == HPET_TN_CMP specially here */
                rc = hpetTimerRegWrite32(pThis, iTimer, iTimerReg, uValue.s.Lo);
                if (RT_LIKELY(rc == VINF_SUCCESS))
                    rc = hpetTimerRegWrite32(pThis, iTimer, iTimerReg + 4, uValue.s.Hi);
            }
            else
            {
                rc = hpetConfigRegWrite32(pThis, idxReg, uValue.s.Lo);
                if (RT_LIKELY(rc == VINF_SUCCESS))
                    rc = hpetConfigRegWrite32(pThis, idxReg + 4, uValue.s.Hi);
            }
            break;
        }

        case 1:
        case 2:
            Log(("Narrow write: %d\n", cb));
            rc = VINF_SUCCESS;
            break;

        default:
            AssertReleaseMsgFailed(("cb=%d\n", cb)); /* for now we assume simple accesses. */
            rc = VERR_INTERNAL_ERROR;
            break;
    }

    DEVHPET_UNLOCK(pThis);
    return rc;
}

#ifdef IN_RING3

/* -=-=-=-=-=- Timer Callback Processing -=-=-=-=-=- */

/**
 * Gets the IRQ of an HPET timer.
 *
 * @returns IRQ number.
 * @param   pHpetTimer          The HPET timer.
 */
static uint32_t hpetTimerCbGetIrq(struct HpetTimer const *pHpetTimer)
{
    /*
     * Per spec, in legacy mode HPET timers wired as:
     *   timer 0: IRQ0 for PIC and IRQ2 for APIC
     *   timer 1: IRQ8 for both PIC and APIC
     *
     * ISA IRQ delivery logic will take care of correct delivery
     * to the different ICs.
     */
    if (   (pHpetTimer->idxTimer <= 1)
        && (pHpetTimer->CTX_SUFF(pHpet)->u64HpetConfig & HPET_CFG_LEGACY))
        return (pHpetTimer->idxTimer == 0) ? 0 : 8;

    return (pHpetTimer->u64Config & HPET_TN_INT_ROUTE_MASK) >> HPET_TN_INT_ROUTE_SHIFT;
}


/**
 * Used by hpetTimerCb to update the IRQ status.
 *
 * @param   pThis               The HPET device state.
 * @param   pHpetTimer          The HPET timer.
 */
static void hpetTimerCbUpdateIrq(HpetState *pThis, struct HpetTimer *pHpetTimer)
{
    /** @todo: is it correct? */
    if (   !!(pHpetTimer->u64Config & HPET_TN_ENABLE)
        && !!(pThis->u64HpetConfig & HPET_CFG_ENABLE))
    {
        uint32_t irq = hpetTimerCbGetIrq(pHpetTimer);
        Log4(("HPET: raising IRQ %d\n", irq));

        /* ISR bits are only set in level-triggered mode. */
        if ((pHpetTimer->u64Config & HPET_TN_INT_TYPE) == HPET_TIMER_TYPE_LEVEL)
            pThis->u64Isr |= (uint64_t)(1 << pHpetTimer->idxTimer);

        /* We trigger flip/flop in edge-triggered mode and do nothing in
           level-triggered mode yet. */
        if ((pHpetTimer->u64Config & HPET_TN_INT_TYPE) == HPET_TIMER_TYPE_EDGE)
            pThis->pHpetHlpR3->pfnSetIrq(pThis->CTX_SUFF(pDevIns), irq, PDM_IRQ_LEVEL_FLIP_FLOP);
        else
            AssertFailed();
        /** @todo: implement IRQs in level-triggered mode */
    }
}

/**
 * Device timer callback function.
 *
 * @param   pDevIns         Device instance of the device which registered the timer.
 * @param   pTimer          The timer handle.
 * @param   pvUser          Pointer to the HPET timer state.
 */
static DECLCALLBACK(void) hpetTimerCb(PPDMDEVINS pDevIns, PTMTIMER pTimer, void *pvUser)
{
    HpetState *pThis      = PDMINS_2_DATA(pDevIns, HpetState *);
    HpetTimer *pHpetTimer = (HpetTimer *)pvUser;
    uint64_t   u64Period  = pHpetTimer->u64Period;
    uint64_t   u64CurTick = hpetGetTicks(pThis);
    uint64_t   u64Diff;

    /* Lock in R3 must either block or succeed */
    int rc = PDMCritSectEnter(&pThis->csLock, VERR_IGNORED);
    AssertRC(rc);

    if ((pHpetTimer->u64Config & HPET_TN_PERIODIC) && (u64Period != 0))
    {
        hpetAdjustComparator(pHpetTimer, u64CurTick);

        u64Diff = hpetComputeDiff(pHpetTimer, u64CurTick);

        Log4(("HPET: periodical: next in %llu\n", hpetTicksToNs(pThis, u64Diff)));
        TMTimerSetNano(pTimer, hpetTicksToNs(pThis, u64Diff));
    }
    else if (    hpet32bitTimer(pHpetTimer)
             && !(pHpetTimer->u64Config & HPET_TN_PERIODIC))
    {
        if (pHpetTimer->u8Wrap)
        {
            u64Diff = hpetComputeDiff(pHpetTimer, u64CurTick);
            TMTimerSetNano(pTimer, hpetTicksToNs(pThis, u64Diff));
            pHpetTimer->u8Wrap = 0;
        }
    }

    /* Should it really be under lock, does it really matter? */
    hpetTimerCbUpdateIrq(pThis, pHpetTimer);

    PDMCritSectLeave(&pThis->csLock);
}


/* -=-=-=-=-=- DBGF Info Handlers -=-=-=-=-=- */


/**
 * @callback_method_impl{FNDBGFHANDLERDEV}
 */
static DECLCALLBACK(void) hpetInfo(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);

    pHlp->pfnPrintf(pHlp,
                    "HPET status:\n"
                    " config = %016RX64\n"
                    " offset = %016RX64 counter = %016RX64 isr = %016RX64\n"
                    " legacy mode is %s\n",
                    pThis->u64HpetConfig,
                    pThis->u64HpetOffset, pThis->u64HpetCounter, pThis->u64Isr,
                    !!(pThis->u64HpetConfig & HPET_CFG_LEGACY) ? "on" : "off");
    pHlp->pfnPrintf(pHlp,
                    "Timers:\n");
    for (unsigned i = 0; i < HPET_NUM_TIMERS; i++)
    {
        pHlp->pfnPrintf(pHlp, " %d: comparator=%016RX64 period(hidden)=%016RX64 cfg=%016RX64\n",
                        pThis->aTimers[i].idxTimer,
                        pThis->aTimers[i].u64Cmp,
                        pThis->aTimers[i].u64Period,
                        pThis->aTimers[i].u64Config);
    }
}


/* -=-=-=-=-=- Saved State -=-=-=-=-=- */


/**
 * @callback_method_impl{FNSSMDEVLIVEEXEC}
 */
static DECLCALLBACK(int) hpetLiveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uPass)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);

    SSMR3PutU8(pSSM, HPET_NUM_TIMERS);

    return VINF_SSM_DONT_CALL_AGAIN;
}


/**
 * @callback_method_impl{FNSSMDEVSAVEEXEC}
 */
static DECLCALLBACK(int) hpetSaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);

    /*
     * The config.
     */
    hpetLiveExec(pDevIns, pSSM, SSM_PASS_FINAL);

    /*
     * The state.
     */
    for (uint32_t iTimer = 0; iTimer < HPET_NUM_TIMERS; iTimer++)
    {
        HpetTimer *pHpetTimer = &pThis->aTimers[iTimer];
        TMR3TimerSave(pHpetTimer->pTimerR3, pSSM);
        SSMR3PutU8(pSSM,  pHpetTimer->u8Wrap);
        SSMR3PutU64(pSSM, pHpetTimer->u64Config);
        SSMR3PutU64(pSSM, pHpetTimer->u64Cmp);
        SSMR3PutU64(pSSM, pHpetTimer->u64Fsb);
        SSMR3PutU64(pSSM, pHpetTimer->u64Period);
    }

    SSMR3PutU64(pSSM, pThis->u64HpetOffset);
    SSMR3PutU64(pSSM, pThis->u64Capabilities);
    SSMR3PutU64(pSSM, pThis->u64HpetConfig);
    SSMR3PutU64(pSSM, pThis->u64Isr);
    return SSMR3PutU64(pSSM, pThis->u64HpetCounter);
}


/**
 * @callback_method_impl{FNSSMDEVLOADEXEC}
 */
static DECLCALLBACK(int) hpetLoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);

    /*
     * Version checks.
     */
    if (uVersion == HPET_SAVED_STATE_VERSION_EMPTY)
        return VINF_SUCCESS;
    if (uVersion != HPET_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    /*
     * The config.
     */
    uint8_t cTimers;
    int rc = SSMR3GetU8(pSSM, &cTimers);
    AssertRCReturn(rc, rc);
    if (cTimers != HPET_NUM_TIMERS)
        return SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Config mismatch - wrong number of timers: saved=%#x config=%#x"),
                                cTimers, HPET_NUM_TIMERS);

    if (uPass != SSM_PASS_FINAL)
        return VINF_SUCCESS;

    /*
     * The state.
     */
    for (uint32_t iTimer = 0; iTimer < HPET_NUM_TIMERS; iTimer++)
    {
        HpetTimer *pHpetTimer = &pThis->aTimers[iTimer];
        TMR3TimerLoad(pHpetTimer->pTimerR3, pSSM);
        SSMR3GetU8(pSSM,  &pHpetTimer->u8Wrap);
        SSMR3GetU64(pSSM, &pHpetTimer->u64Config);
        SSMR3GetU64(pSSM, &pHpetTimer->u64Cmp);
        SSMR3GetU64(pSSM, &pHpetTimer->u64Fsb);
        SSMR3GetU64(pSSM, &pHpetTimer->u64Period);
    }

    SSMR3GetU64(pSSM, &pThis->u64HpetOffset);
    SSMR3GetU64(pSSM, &pThis->u64Capabilities);
    SSMR3GetU64(pSSM, &pThis->u64HpetConfig);
    SSMR3GetU64(pSSM, &pThis->u64Isr);
    return SSMR3GetU64(pSSM, &pThis->u64HpetCounter);
}


/* -=-=-=-=-=- PDMDEVREG -=-=-=-=-=- */


/**
 * @interface_method_impl{PDMDEVREG,pfnRelocate}
 */
static DECLCALLBACK(void) hpetRelocate(PPDMDEVINS pDevIns, RTGCINTPTR offDelta)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);
    LogFlow(("hpetRelocate:\n"));

    pThis->pDevInsRC    = PDMDEVINS_2_RCPTR(pDevIns);
    pThis->pHpetHlpRC   = pThis->pHpetHlpR3->pfnGetRCHelpers(pDevIns);

    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aTimers); i++)
    {
        HpetTimer *pTm = &pThis->aTimers[i];
        if (pTm->pTimerR3)
            pTm->pTimerRC = TMTimerRCPtr(pTm->pTimerR3);
        pTm->pHpetRC = PDMINS_2_DATA_RCPTR(pDevIns);
    }
}


/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 */
static DECLCALLBACK(void) hpetReset(PPDMDEVINS pDevIns)
{
    HpetState *pThis = PDMINS_2_DATA(pDevIns, HpetState *);
    LogFlow(("hpetReset:\n"));

    pThis->u64HpetConfig = 0;
    for (unsigned i = 0; i < HPET_NUM_TIMERS; i++)
    {
        HpetTimer *pHpetTimer = &pThis->aTimers[i];
        Assert(pHpetTimer->idxTimer == i);

        /* capable of periodic operations and 64-bits */
        if (pThis->fIch9)
            pHpetTimer->u64Config = (i == 0)
                                  ? (HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP)
                                  : 0;
        else
            pHpetTimer->u64Config = HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;

        /* We can do all IRQs */
        uint32_t u32RoutingCap = 0xffffffff;
        pHpetTimer->u64Config |= ((uint64_t)u32RoutingCap) << 32;
        pHpetTimer->u64Period  = 0;
        pHpetTimer->u8Wrap     = 0;
        pHpetTimer->u64Cmp     = hpetInvalidValue(pHpetTimer);
        /** @todo shouldn't we stop any active timers at this point? */
    }
    pThis->u64HpetCounter = 0;
    pThis->u64HpetOffset  = 0;

    uint32_t u32Vendor = 0x8086;
    /* 64-bit main counter; 3 timers supported; LegacyReplacementRoute. */
    uint32_t u32Caps = (1 << 15)              /* LEG_RT_CAP       - LegacyReplacementRoute capable. */
                     | (1 << 13)              /* COUNTER_SIZE_CAP - Main counter is 64-bit capable. */
                     | (HPET_NUM_TIMERS << 8) /* NUM_TIM_CAP      - Number of timers -1.
                                                 Actually ICH9 has 4 timers, but to avoid breaking
                                                 saved state we'll stick with 3 so far. */ /** @todo fix this ICH9 timer count bug. */
                     | 1                      /* REV_ID           - Revision, must not be 0 */
                     ;
    pThis->u64Capabilities = (u32Vendor << 16) | u32Caps;
    pThis->u64Capabilities |= ((uint64_t)(pThis->fIch9 ? HPET_CLK_PERIOD_ICH9 : HPET_CLK_PERIOD) << 32);

    /* Notify PIT/RTC devices */
    if (pThis->pHpetHlpR3)
        pThis->pHpetHlpR3->pfnSetLegacyMode(pDevIns, false /*fActive*/);
}


/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int) hpetConstruct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);
    HpetState   *pThis = PDMINS_2_DATA(pDevIns, HpetState *);

    /* Only one HPET device now, as we use fixed MMIO region. */
    Assert(iInstance == 0);

    /*
     * Validate and read the configuration.
     */
    PDMDEV_VALIDATE_CONFIG_RETURN(pDevIns, "GCEnabled|R0Enabled|ICH9", "");

    bool fRCEnabled;
    int rc = CFGMR3QueryBoolDef(pCfg, "GCEnabled", &fRCEnabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Querying \"GCEnabled\" as a bool failed"));

    bool fR0Enabled;
    rc = CFGMR3QueryBoolDef(pCfg, "R0Enabled", &fR0Enabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: failed to read R0Enabled as boolean"));

    rc = CFGMR3QueryBoolDef(pCfg, "ICH9", &pThis->fIch9, false);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: failed to read ICH9 as boolean"));

    /*
     * Initialize the device state
     */
    pThis->pDevInsR3 = pDevIns;
    pThis->pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    pThis->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);

    rc = PDMDevHlpCritSectInit(pDevIns, &pThis->csLock, RT_SRC_POS, "HPET#%u", pDevIns->iInstance);
    AssertRCReturn(rc, rc);

    /* Init the HPET timers. */
    for (unsigned i = 0; i < HPET_NUM_TIMERS; i++)
    {
        HpetTimer *pHpetTimer = &pThis->aTimers[i];

        pHpetTimer->idxTimer = i;
        pHpetTimer->pHpetR3  = pThis;
        pHpetTimer->pHpetR0  = PDMINS_2_DATA_R0PTR(pDevIns);
        pHpetTimer->pHpetRC  = PDMINS_2_DATA_RCPTR(pDevIns);

        rc = PDMDevHlpTMTimerCreate(pDevIns, TMCLOCK_VIRTUAL_SYNC, hpetTimerCb, pHpetTimer,
                                    TMTIMER_FLAGS_NO_CRIT_SECT, "HPET Timer",
                                    &pThis->aTimers[i].pTimerR3);
        AssertRCReturn(rc, rc);
        pThis->aTimers[i].pTimerRC = TMTimerRCPtr(pThis->aTimers[i].pTimerR3);
        pThis->aTimers[i].pTimerR0 = TMTimerR0Ptr(pThis->aTimers[i].pTimerR3);
        /// @todo TMR3TimerSetCritSect(pThis->aTimers[i].pTimerR3, &pThis->csLock);
    }

    /* This must be done prior to registering the HPET, right? */
    hpetReset(pDevIns);

    /*
     * Register the HPET and get helpers.
     */
    PDMHPETREG HpetReg;
    HpetReg.u32Version = PDM_HPETREG_VERSION;
    rc = PDMDevHlpHPETRegister(pDevIns, &HpetReg, &pThis->pHpetHlpR3);
    AssertRCReturn(rc, rc);

    /*
     * Register the MMIO range, PDM API requests page aligned
     * addresses and sizes.
     */
    rc = PDMDevHlpMMIORegister(pDevIns, HPET_BASE, 0x1000, pThis,
                               hpetMMIOWrite, hpetMMIORead, NULL, "HPET Memory");
    AssertRCReturn(rc, rc);

    if (fRCEnabled)
    {
        rc = PDMDevHlpMMIORegisterRC(pDevIns, HPET_BASE, 0x1000, 0,
                                     "hpetMMIOWrite", "hpetMMIORead", NULL);
        AssertRCReturn(rc, rc);

        pThis->pHpetHlpRC = pThis->pHpetHlpR3->pfnGetRCHelpers(pDevIns);
        AssertReturn(pThis->pHpetHlpRC != NIL_RTRCPTR, VERR_INTERNAL_ERROR);
    }

    if (fR0Enabled)
    {
        rc = PDMDevHlpMMIORegisterR0(pDevIns, HPET_BASE, 0x1000, 0,
                                     "hpetMMIOWrite", "hpetMMIORead", NULL);
        AssertRCReturn(rc, rc);

        pThis->pHpetHlpR0 = pThis->pHpetHlpR3->pfnGetR0Helpers(pDevIns);
        AssertReturn(pThis->pHpetHlpR0 != NIL_RTR0PTR, VERR_INTERNAL_ERROR);
    }

    /* Register SSM callbacks */
    rc = PDMDevHlpSSMRegister3(pDevIns, HPET_SAVED_STATE_VERSION, sizeof(*pThis), hpetLiveExec, hpetSaveExec, hpetLoadExec);
    AssertRCReturn(rc, rc);

    /* Register an info callback. */
    PDMDevHlpDBGFInfoRegister(pDevIns, "hpet", "Display HPET status. (no arguments)", hpetInfo);

    return VINF_SUCCESS;
}


/**
 * The device registration structure.
 */
const PDMDEVREG g_DeviceHPET =
{
    /* u32Version */
    PDM_DEVREG_VERSION,
    /* szName */
    "hpet",
    /* szRCMod */
    "VBoxDDGC.gc",
    /* szR0Mod */
    "VBoxDDR0.r0",
    /* pszDescription */
    " High Precision Event Timer (HPET) Device",
    /* fFlags */
    PDM_DEVREG_FLAGS_HOST_BITS_DEFAULT | PDM_DEVREG_FLAGS_GUEST_BITS_32_64 | PDM_DEVREG_FLAGS_PAE36 | PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0,
    /* fClass */
    PDM_DEVREG_CLASS_PIT,
    /* cMaxInstances */
    1,
    /* cbInstance */
    sizeof(HpetState),
    /* pfnConstruct */
    hpetConstruct,
    /* pfnDestruct */
    NULL,
    /* pfnRelocate */
    hpetRelocate,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    hpetReset,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnQueryInterface. */
    NULL,
    /* pfnInitComplete */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DEVREG_VERSION
};

#endif /* IN_RING3 */
#endif /* !VBOX_DEVICE_STRUCT_TESTCASE */

