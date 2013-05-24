/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineWindowSeamless class declaration
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef __UIMachineWindowSeamless_h__
#define __UIMachineWindowSeamless_h__

/* Local includes: */
#include "UIMachineWindow.h"

/* Forward declarations: */
class UIRuntimeMiniToolBar;

/* Seamless machine-window implementation: */
class UIMachineWindowSeamless : public UIMachineWindow
{
    Q_OBJECT;

protected:

    /* Constructor: */
    UIMachineWindowSeamless(UIMachineLogic *pMachineLogic, ulong uScreenId);

private slots:

#ifndef Q_WS_MAC
    /* Session event-handlers: */
    void sltMachineStateChanged();
#endif /* !Q_WS_MAC */

    /* Show in necessary mode: */
    void sltShowInNecessaryMode() { showInNecessaryMode(); }

    /* Popup main menu: */
    void sltPopupMainMenu();

private:

    /* Prepare helpers: */
    void prepareMenu();
    void prepareVisualState();
#ifndef Q_WS_MAC
    void prepareMiniToolbar();
#endif /* !Q_WS_MAC */

    /* Cleanup helpers: */
#ifndef Q_WS_MAC
    void cleanupMiniToolbar();
#endif /* !Q_WS_MAC */
    void cleanupVisualState();
    void cleanupMenu();

    /* Show stuff: */
    void placeOnScreen();
    void showInNecessaryMode();

#ifndef Q_WS_MAC
    /* Update routines: */
    void updateAppearanceOf(int iElement);
#endif /* !Q_WS_MAC */

#ifdef Q_WS_WIN
    /* Handler: Translucency stuff: */
    void showEvent(QShowEvent *pEvent);
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
    /* Helper: Masking stuff: */
    void setMask(const QRegion &region);
#endif /* Q_WS_X11 */

    /* Widgets: */
    QMenu *m_pMainMenu;
#ifndef Q_WS_MAC
    UIRuntimeMiniToolBar *m_pMiniToolBar;
#endif /* !Q_WS_MAC */

#ifdef Q_WS_X11
    /* Variable: Masking stuff: */
    QRegion m_maskRegion;
#endif /* Q_WS_X11 */

    /* Factory support: */
    friend class UIMachineWindow;
};

#endif // __UIMachineWindowSeamless_h__

