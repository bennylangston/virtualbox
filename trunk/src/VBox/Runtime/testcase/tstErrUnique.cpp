/* $Id$ */
/** @file
 * InnoTek Portable Runtime Testcase - Error Messages.
 */

/*
 * Copyright (C) 2006 InnoTek Systemberatung GmbH
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * in version 2 as it comes in the "COPYING" file of the VirtualBox OSE
 * distribution. VirtualBox OSE is distributed in the hope that it will
 * be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * If you received this file as part of a commercial VirtualBox
 * distribution, then only the terms of your commercial VirtualBox
 * license agreement apply instead of the previous paragraph.
 */


/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <iprt/err.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/runtime.h>
#include <VBox/err.h>


/********************************************************************************   Global Variables                                                           ********************************************************************************//** Array of messages.
 * The data is generated by a sed script.
 */
static const RTSTATUSMSG  g_aErrorMessages[] =
{
#include "errmsgdata.h"
};

static bool strIsPermissibleDuplicate(const RTSTATUSMSG *msg)
{
    const char *pszMsgShort = msg->pszMsgShort;
    const char *pszDefine = msg->pszDefine;
    size_t cbDefine = strlen(pszDefine);

    return    (strstr(pszMsgShort, "(mapped to") != 0)
           || (strstr(pszDefine, "FIRST") == pszDefine + (cbDefine - 5))
           || (strstr(pszDefine, "LAST") == pszDefine + (cbDefine - 4));
}


int main()
{
    int         cErrors = 0;
    RTPrintf("tstErrUnique: TESTING\n");
    RTR3Init();

    for (uint32_t i = 0; i < ELEMENTS(g_aErrorMessages) - 1; i++)
    {
        if (strIsPermissibleDuplicate(&g_aErrorMessages[i]))
            continue;

        for (uint32_t j = i + 1; j < ELEMENTS(g_aErrorMessages); j++)
        {
            if (strIsPermissibleDuplicate(&g_aErrorMessages[j]))
                continue;

            if (g_aErrorMessages[i].iCode == g_aErrorMessages[j].iCode)
            {
                RTPrintf("tstErrUnique: status code %d can mean '%s' or '%s'\n", g_aErrorMessages[i].iCode, g_aErrorMessages[i].pszMsgShort, g_aErrorMessages[j]);
                cErrors++;
            }
        }
    }

    /*
     * Summary
     */
    if (cErrors == 0)
        RTPrintf("tstErrUnique: SUCCESS\n");
    else
        RTPrintf("tstErrUnique: FAILURE - %d errors\n", cErrors);
    return !!cErrors;
}

