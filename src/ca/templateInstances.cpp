/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/

/*  
 *  $Id$
 *
 *                              
 *                    L O S  A L A M O S
 *              Los Alamos National Laboratory
 *               Los Alamos, New Mexico 87545
 *                                  
 *  Copyright, 1986, The Regents of the University of California.
 *                                  
 *           
 *	Author Jeffrey O. Hill
 *	johill@lanl.gov
 *	505 665 1831
 */

#define epicsExportSharedSymbols
#include "virtualCircuit.h"
#include "bhe.h"
#include "cac.h"
#include "syncGroup.h"
#include "nciu.h"
#include "udpiiu.h"
#include "oldAccess.h"
#include "msgForMultiplyDefinedPV.h"
#include "repeaterClient.h"
#include "hostNameCache.h"

#ifdef _MSC_VER
#   pragma warning ( push )
#   pragma warning ( disable:4660 )
#endif

template class resTable < nciu, chronIntId >;
template class chronIntIdResTable < nciu >;
template class resTable < baseNMIU, chronIntId >;
template class chronIntIdResTable < baseNMIU >;
template class resTable < CASG, chronIntId >;
template class chronIntIdResTable < CASG >;
template class resTable < bhe, inetAddrID >;
template class resTable < tcpiiu, caServerID >;
template class tsFreeList < bhe, 0x100 >;
template class tsFreeList < tcpiiu, 32, epicsMutexNOOP >;
template class tsFreeList < netReadNotifyIO, 1024, epicsMutexNOOP >;
template class tsFreeList < netWriteNotifyIO, 1024, epicsMutexNOOP >;
template class tsFreeList < netSubscription, 1024, epicsMutexNOOP >;
template class tsFreeList < CASG, 128, epicsMutexNOOP  >;
template class tsFreeList < syncGroupReadNotify, 128, epicsMutexNOOP >;
template class tsFreeList < syncGroupWriteNotify, 128, epicsMutexNOOP >;
template class tsFreeList < comBuf, 0x20 >;
template class tsFreeList < getCallback, 1024, epicsMutexNOOP >;
template class tsFreeList < getCopy, 1024, epicsMutexNOOP >;
template class tsFreeList < hostNameCache, 16 >;
template class tsFreeList < msgForMultiplyDefinedPV, 16 >;
template class tsFreeList < nciu, 1024, epicsMutexNOOP>;
template class tsFreeList < oldChannelNotify, 1024, epicsMutexNOOP >;
template class tsFreeList < oldSubscription, 1024, epicsMutexNOOP >;
template class tsFreeList < putCallback, 1024, epicsMutexNOOP >;
template class tsFreeList < repeaterClient, 0x20 >;
template class epicsSingleton < localHostName >;

#ifdef _MSC_VER
#   pragma warning ( pop )
#endif