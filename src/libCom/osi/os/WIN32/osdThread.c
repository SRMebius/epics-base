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
 * $Id$
 *
 * Author: Jeff Hill
 * 
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

#define VC_EXTRALEAN
#define STRICT
#if _WIN64
#   define _WIN32_WINNT 0x400 /* defining this drops support for W95 */
#endif
#include <windows.h>
#include <process.h> /* for _endthread() etc */

#define epicsExportSharedSymbols
#include "shareLib.h"
#include "epicsThread.h"
#include "cantProceed.h"
#include "errlog.h"
#include "epicsAssert.h"
#include "ellLib.h"

typedef struct win32ThreadGlobal {
    CRITICAL_SECTION mutex;
    ELLLIST threadList;
    DWORD tlsIndexThreadLibraryEPICS;
} win32ThreadGlobal;

typedef struct win32ThreadParam {
    ELLNODE node;
    HANDLE handle;
    EPICSTHREADFUNC funptr;
    void * parm;
    char * pName;
    DWORD id;
    unsigned epicsPriority;
    char isSuspended;
} win32ThreadParam;

typedef struct epicsThreadPrivateOSD {
    DWORD key;
} epicsThreadPrivateOSD;

#define osdOrdinaryPriorityStateCount 5u
static const int osdOrdinaryPriorityList [osdOrdinaryPriorityStateCount] = 
{
    THREAD_PRIORITY_LOWEST,       // -2 on >= W2K ??? on W95
    THREAD_PRIORITY_BELOW_NORMAL, // -1 on >= W2K ??? on W95
    THREAD_PRIORITY_NORMAL,       //  0 on >= W2K ??? on W95
    THREAD_PRIORITY_ABOVE_NORMAL, //  1 on >= W2K ??? on W95
    THREAD_PRIORITY_HIGHEST       //  2 on >= W2K ??? on W95
};

#   define osdRealtimePriorityStateCount 14u
static const int osdRealtimePriorityList [osdRealtimePriorityStateCount] = 
{
    -7, // allowed on >= W2k, but no #define supplied
    -6, // allowed on >= W2k, but no #define supplied
    -5, // allowed on >= W2k, but no #define supplied
    -4, // allowed on >= W2k, but no #define supplied
    -3, // allowed on >= W2k, but no #define supplied
    THREAD_PRIORITY_LOWEST,       // -2 on >= W2K ??? on W95
    THREAD_PRIORITY_BELOW_NORMAL, // -1 on >= W2K ??? on W95
    THREAD_PRIORITY_NORMAL,       //  0 on >= W2K ??? on W95
    THREAD_PRIORITY_ABOVE_NORMAL, //  1 on >= W2K ??? on W95
    THREAD_PRIORITY_HIGHEST,      //  2 on >= W2K ??? on W95
    3, // allowed on >= W2k, but no #define supplied
    4, // allowed on >= W2k, but no #define supplied
    5, // allowed on >= W2k, but no #define supplied
    6  // allowed on >= W2k, but no #define supplied
};

/*
 * fetchWin32ThreadGlobal ()
 * Search for "Synchronization and Multiprocessor Issues" in ms doc
 * to understand why this is necessary and why this works on smp systems.
 */
static win32ThreadGlobal * fetchWin32ThreadGlobal ( void )
{
    void threadCleanupWIN32 ( void );
    static win32ThreadGlobal * pWin32ThreadGlobal = 0;
    static LONG initStarted = 0;
    static LONG initCompleted = 0;
    int crtlStatus;
    LONG started;
    LONG done;

    done = InterlockedCompareExchange ( & initCompleted, 0, 0 );
    if ( done ) {
        return pWin32ThreadGlobal;
    }

    started = InterlockedCompareExchange ( & initStarted, 0, 1 );
    if ( started ) {
        unsigned tries = 0u;
        while ( ! InterlockedCompareExchange ( & initCompleted, 0, 0 ) ) {
            /*
             * I am not fond of busy loops, but since this will
             * collide very infrequently and this is the lowest 
             * level init then perhaps this is ok
             */
            Sleep ( 1 );
            if ( tries++ > 1000 ) {
                return 0;
            }
        }
        return pWin32ThreadGlobal;
    }

    pWin32ThreadGlobal = ( win32ThreadGlobal * ) 
        calloc ( 1, sizeof ( * pWin32ThreadGlobal ) );
    if ( ! pWin32ThreadGlobal ) {
        InterlockedExchange ( & initStarted, 0 );
        return 0;
    }

    InitializeCriticalSection ( & pWin32ThreadGlobal->mutex );
    ellInit ( & pWin32ThreadGlobal->threadList );
    pWin32ThreadGlobal->tlsIndexThreadLibraryEPICS = TlsAlloc();
    if ( pWin32ThreadGlobal->tlsIndexThreadLibraryEPICS == 0xFFFFFFFF ) {
        DeleteCriticalSection ( & pWin32ThreadGlobal->mutex );
        free ( pWin32ThreadGlobal );
        pWin32ThreadGlobal = 0;
        return 0;
    }

    crtlStatus = atexit ( threadCleanupWIN32 );
    if ( crtlStatus ) {
        TlsFree ( pWin32ThreadGlobal->tlsIndexThreadLibraryEPICS );
        DeleteCriticalSection ( & pWin32ThreadGlobal->mutex );
        free ( pWin32ThreadGlobal );
        pWin32ThreadGlobal = 0;
        return 0;
    }

    InterlockedExchange ( & initCompleted, 1 );

    return pWin32ThreadGlobal;
}

static void epicsParmCleanupWIN32 ( win32ThreadParam * pParm )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    if ( ! pGbl )  {
        fprintf ( stderr, "epicsParmCleanupWIN32: unable to find ctx\n" );
        return;
    }

    if ( pParm ) {
        //fprintf ( stderr, "thread %s is exiting\n", pParm->pName );
        EnterCriticalSection ( & pGbl->mutex );
        ellDelete ( & pGbl->threadList, & pParm->node );
        LeaveCriticalSection ( & pGbl->mutex );

        // close the handle if its an implicit thread id
        if ( ! pParm->funptr ) {
            CloseHandle ( pParm->handle );
        }
        free ( pParm );
        TlsSetValue ( pGbl->tlsIndexThreadLibraryEPICS, 0 );
    }
}

/*
 * threadCleanupWIN32 ()
 */
static void threadCleanupWIN32 ( void )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    if ( ! pGbl )  {
        fprintf ( stderr, "threadCleanupWIN32: unable to find ctx\n" );
        return;
    }

    while ( pParm = ( win32ThreadParam * ) ellFirst ( & pGbl->threadList ) ) {
        epicsParmCleanupWIN32 ( pParm );
    }

    TlsFree ( pGbl->tlsIndexThreadLibraryEPICS );

    DeleteCriticalSection ( & pGbl->mutex );
}

/*
 * epicsThreadExitMain ()
 */
epicsShareFunc void epicsShareAPI epicsThreadExitMain ( void )
{
    _endthread ();
}

/*
 * osdPriorityMagFromPriorityOSI ()
 */
static unsigned osdPriorityMagFromPriorityOSI ( unsigned osiPriority, unsigned priorityStateCount ) 
{
    unsigned magnitude;

    // optimizer will remove this one if epicsThreadPriorityMin is zero
    // and osiPriority is unsigned
    if ( osiPriority < epicsThreadPriorityMin ) {
        osiPriority = epicsThreadPriorityMin;
    }

    if ( osiPriority > epicsThreadPriorityMax ) {
        osiPriority = epicsThreadPriorityMax;
    }

    magnitude = osiPriority * priorityStateCount;
    magnitude /= ( epicsThreadPriorityMax - epicsThreadPriorityMin ) + 1;

    return magnitude;
}

/*
 * epicsThreadGetOsdPriorityValue ()
 */
static int epicsThreadGetOsdPriorityValue ( unsigned osiPriority ) 
{
    const DWORD priorityClass = GetPriorityClass ( GetCurrentProcess () );
    const int * pStateList;
    unsigned stateCount;
    unsigned magnitude;

    if ( priorityClass == REALTIME_PRIORITY_CLASS ) {
        stateCount = osdRealtimePriorityStateCount;
        pStateList = osdRealtimePriorityList;
    }
    else {
        stateCount = osdOrdinaryPriorityStateCount;
        pStateList = osdOrdinaryPriorityList;
    }

    magnitude = osdPriorityMagFromPriorityOSI ( osiPriority, stateCount );
    return pStateList[magnitude];
}

/*
 * osiPriorityMagFromMagnitueOSD ()
 */
static unsigned osiPriorityMagFromMagnitueOSD ( unsigned magnitude, unsigned osdPriorityStateCount ) 
{
    unsigned osiPriority;

    osiPriority = magnitude * ( epicsThreadPriorityMax - epicsThreadPriorityMin );
    osiPriority /= osdPriorityStateCount - 1u;
    osiPriority += epicsThreadPriorityMin;

    return osiPriority;
}


/* 
 * epicsThreadGetOsiPriorityValue ()
 */
static unsigned epicsThreadGetOsiPriorityValue ( int osdPriority ) 
{
    const DWORD priorityClass = GetPriorityClass ( GetCurrentProcess () );
    const int * pStateList;
    unsigned stateCount;
    unsigned magnitude;

    if ( priorityClass == REALTIME_PRIORITY_CLASS ) {
        stateCount = osdRealtimePriorityStateCount;
        pStateList = osdRealtimePriorityList;
    }
    else {
        stateCount = osdOrdinaryPriorityStateCount;
        pStateList = osdOrdinaryPriorityList;
    }

    for ( magnitude = 0u; magnitude < stateCount; magnitude++ ) {
        if ( osdPriority == pStateList[magnitude] ) {
            break;
        }
    }

    if ( magnitude >= stateCount ) {
        errlogPrintf ( 
            "Unrecognized WIN32 thread priority level %d.\n", 
            osdPriority );
        errlogPrintf ( 
            "Mapping to EPICS thread priority level epicsThreadPriorityMin.\n" );
        return epicsThreadPriorityMin;
    }

    return osiPriorityMagFromMagnitueOSD ( magnitude, stateCount );
}

/*
 * epicsThreadLowestPriorityLevelAbove ()
 */
epicsShareFunc epicsThreadBooleanStatus epicsShareAPI epicsThreadLowestPriorityLevelAbove 
            ( unsigned int priority, unsigned * pPriorityJustAbove )
{
    const DWORD priorityClass = GetPriorityClass ( GetCurrentProcess () );
    epicsThreadBooleanStatus status;
    unsigned stateCount;
    unsigned magnitude;

    if ( priorityClass == REALTIME_PRIORITY_CLASS ) {
        stateCount = osdRealtimePriorityStateCount;
    }
    else {
        stateCount = osdOrdinaryPriorityStateCount;
    }

    magnitude = osdPriorityMagFromPriorityOSI ( priority, stateCount );

    if ( magnitude < ( stateCount - 1 ) ) {
        *pPriorityJustAbove = osiPriorityMagFromMagnitueOSD ( magnitude + 1u, stateCount );
        status = epicsThreadBooleanStatusSuccess;
    }
    else {
        status = epicsThreadBooleanStatusFail;
    }
    return status;
}

/*
 * epicsThreadHighestPriorityLevelBelow ()
 */
epicsShareFunc epicsThreadBooleanStatus epicsShareAPI epicsThreadHighestPriorityLevelBelow 
            ( unsigned int priority, unsigned * pPriorityJustBelow )
{
    const DWORD priorityClass = GetPriorityClass ( GetCurrentProcess () );
    epicsThreadBooleanStatus status;
    unsigned stateCount;
    unsigned magnitude;

    if ( priorityClass == REALTIME_PRIORITY_CLASS ) {
        stateCount = osdRealtimePriorityStateCount;
    }
    else {
        stateCount = osdOrdinaryPriorityStateCount;
    }

    magnitude = osdPriorityMagFromPriorityOSI ( priority, stateCount );

    if ( magnitude > 1u ) {
        *pPriorityJustBelow = osiPriorityMagFromMagnitueOSD ( magnitude - 1u, stateCount );
        status = epicsThreadBooleanStatusSuccess;
    }
    else {
        status = epicsThreadBooleanStatusFail;
    }
    return status;
}

/*
 * epicsThreadGetStackSize ()
 */
epicsShareFunc unsigned int epicsShareAPI epicsThreadGetStackSize ( epicsThreadStackSizeClass stackSizeClass ) 
{
    static const unsigned stackSizeTable[epicsThreadStackBig+1] = {4000, 6000, 11000};

    if (stackSizeClass<epicsThreadStackSmall) {
        errlogPrintf("epicsThreadGetStackSize illegal argument (too small)");
        return stackSizeTable[epicsThreadStackBig];
    }

    if (stackSizeClass>epicsThreadStackBig) {
        errlogPrintf("epicsThreadGetStackSize illegal argument (too large)");
        return stackSizeTable[epicsThreadStackBig];
    }

    return stackSizeTable[stackSizeClass];
}

void epicsThreadCleanupWIN32 ()
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    if ( ! pGbl )  {
        fprintf ( stderr, "epicsThreadCleanupWIN32: unable to find ctx\n" );
        return;
    }

    pParm = ( win32ThreadParam * ) 
        TlsGetValue ( pGbl->tlsIndexThreadLibraryEPICS );
    epicsParmCleanupWIN32 ( pParm );
}

/*
 * this was copied directly from example in visual c++ 7 documentation
 *
 * Usage: SetThreadName (-1, "MainThread");
 */
void SetThreadName( DWORD dwThreadID, LPCSTR szThreadName )
{
    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // must be 0x1000
        LPCSTR szName; // pointer to name (in user addr space)
        DWORD dwThreadID; // thread ID (-1=caller thread)
        DWORD dwFlags; // reserved for future use, must be zero
    } THREADNAME_INFO;
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = szThreadName;
    info.dwThreadID = dwThreadID;
    info.dwFlags = 0;

    __try
    {
        RaiseException ( 0x406D1388, 0, 
            sizeof(info)/sizeof(DWORD), (DWORD*)&info );
    }
    __except ( EXCEPTION_CONTINUE_EXECUTION )
    {
    }
}

/*
 * epicsWin32ThreadEntry()
 */
static unsigned WINAPI epicsWin32ThreadEntry ( LPVOID lpParameter )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm = ( win32ThreadParam * ) lpParameter;
    unsigned retStat = 0u;
    BOOL success;

    if ( pGbl )  {
        SetThreadName ( pParm->id, pParm->pName );

        success = TlsSetValue ( pGbl->tlsIndexThreadLibraryEPICS, pParm );
        if ( success ) {
            /* printf ( "starting thread %d\n", pParm->id ); */
            ( *pParm->funptr ) ( pParm->parm );
            /* printf ( "terminating thread %d\n", pParm->id ); */
            retStat = 1;
        }
        else {
            fprintf ( stderr, "epicsWin32ThreadEntry: unable to set private\n" );
        }
    }
    else {
        fprintf ( stderr, "epicsWin32ThreadEntry: unable to find ctx\n" );
    }

    /*
     * CAUTION: !!!! the thread id might continue to be used after this thread exits !!!!
     */
    epicsParmCleanupWIN32 ( pParm );

    return retStat; /* this indirectly closes the thread handle */
}

static win32ThreadParam * epicsThreadParmCreate ( const char *pName )
{
    win32ThreadParam *pParmWIN32;

    pParmWIN32 = calloc ( 1, sizeof ( *pParmWIN32 ) + strlen ( pName ) + 1 );
    if ( pParmWIN32  ) {
        if ( pName ) {
            pParmWIN32->pName = (char *) ( pParmWIN32 + 1 );
            strcpy ( pParmWIN32->pName, pName );
        }
        else {
            pParmWIN32->pName = 0;
        }
        pParmWIN32->isSuspended = 0;
    }
    return pParmWIN32;
}

static win32ThreadParam * epicsThreadImplicitCreate ( void )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    DWORD id = GetCurrentThreadId ();
    win32ThreadParam * pParm;
    char name[64];
    HANDLE handle;
    BOOL success;

    if ( ! pGbl )  {
        fprintf ( stderr, "epicsThreadImplicitCreate: unable to find ctx\n" );
        return 0;
    }

    success = DuplicateHandle ( GetCurrentProcess (), GetCurrentThread (),
            GetCurrentProcess (), & handle, 0, FALSE, DUPLICATE_SAME_ACCESS );
    if ( ! success ) {
        return 0;
    }
    sprintf ( name, "win%x", id );
    pParm = epicsThreadParmCreate ( name );
    if ( pParm ) {
        int win32ThreadPriority;

        pParm->handle = handle;
        pParm->id = id;
        win32ThreadPriority = GetThreadPriority ( pParm->handle );
        assert ( win32ThreadPriority != THREAD_PRIORITY_ERROR_RETURN );
        pParm->epicsPriority = epicsThreadGetOsiPriorityValue ( win32ThreadPriority );
        success = TlsSetValue ( pGbl->tlsIndexThreadLibraryEPICS, pParm );
        if ( ! success ) {
            epicsParmCleanupWIN32 ( pParm );
            pParm = 0;
        }
        else {
            EnterCriticalSection ( & pGbl->mutex );
            ellAdd ( & pGbl->threadList, & pParm->node );
            LeaveCriticalSection ( & pGbl->mutex );
        }
    }
    return pParm;
}

/*
 * epicsThreadCreate ()
 */
epicsShareFunc epicsThreadId epicsShareAPI epicsThreadCreate (const char *pName,
    unsigned int priority, unsigned int stackSize, EPICSTHREADFUNC pFunc,void *pParm)
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParmWIN32;
    int osdPriority;
    DWORD wstat;
    BOOL bstat;

    if ( ! pGbl )  {
        return NULL;
    }
    pParmWIN32 = epicsThreadParmCreate ( pName );
    if ( pParmWIN32 == 0 ) {
        return ( epicsThreadId ) pParmWIN32;
    }
    pParmWIN32->funptr = pFunc;
    pParmWIN32->parm = pParm;
    pParmWIN32->epicsPriority = priority;

    pParmWIN32->handle = (HANDLE) _beginthreadex ( 0, 
        stackSize, epicsWin32ThreadEntry, 
        pParmWIN32, CREATE_SUSPENDED, &pParmWIN32->id );
    if ( pParmWIN32->handle == 0 ) {
        free ( pParmWIN32 );
        return NULL;
    }

    osdPriority = epicsThreadGetOsdPriorityValue (priority);
    bstat = SetThreadPriority ( pParmWIN32->handle, osdPriority );
    if (!bstat) {
        CloseHandle ( pParmWIN32->handle ); 
        free ( pParmWIN32 );
        return NULL;
    }

    wstat =  ResumeThread ( pParmWIN32->handle );
    if (wstat==0xFFFFFFFF) {
        CloseHandle ( pParmWIN32->handle ); 
        free ( pParmWIN32 );
        return NULL;
    }

    EnterCriticalSection ( & pGbl->mutex );
    ellAdd ( & pGbl->threadList, & pParmWIN32->node );
    LeaveCriticalSection ( & pGbl->mutex );

    return ( epicsThreadId ) pParmWIN32;
}

/*
 * epicsThreadSuspendSelf ()
 */
epicsShareFunc void epicsShareAPI epicsThreadSuspendSelf ()
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;
    DWORD stat;

    assert ( pGbl );

    pParm = ( win32ThreadParam * ) 
        TlsGetValue ( pGbl->tlsIndexThreadLibraryEPICS );
    if ( ! pParm ) {
        pParm = epicsThreadImplicitCreate ();
        if ( ! pParm ) {
            stat =  SuspendThread ( GetCurrentThread () );
            assert ( stat != 0xFFFFFFFF );
            return;
        }
    }

    EnterCriticalSection ( & pGbl->mutex );

    stat =  SuspendThread ( pParm->handle );
    pParm->isSuspended = 1;

    LeaveCriticalSection ( & pGbl->mutex );

    assert ( stat != 0xFFFFFFFF );
}

/*
 * epicsThreadResume ()
 */
epicsShareFunc void epicsShareAPI epicsThreadResume ( epicsThreadId id )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm = ( win32ThreadParam * ) id;
    DWORD stat;

    assert ( pGbl );

    EnterCriticalSection ( & pGbl->mutex );

    stat =  ResumeThread ( pParm->handle );
    pParm->isSuspended = 0;

    LeaveCriticalSection ( & pGbl->mutex );

    assert ( stat != 0xFFFFFFFF );
}

/*
 * epicsThreadGetPriority ()
 */
epicsShareFunc unsigned epicsShareAPI epicsThreadGetPriority (epicsThreadId id) 
{ 
    win32ThreadParam * pParm = ( win32ThreadParam * ) id;
    return pParm->epicsPriority;
}

/*
 * epicsThreadGetPriority ()
 */
epicsShareFunc unsigned epicsShareAPI epicsThreadGetPrioritySelf () 
{ 
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    assert ( pGbl );

    pParm = ( win32ThreadParam * ) 
        TlsGetValue ( pGbl->tlsIndexThreadLibraryEPICS );
    if ( ! pParm ) {
        pParm = epicsThreadImplicitCreate ();
    }
    if ( pParm ) {
        return pParm->epicsPriority;
    }
    else {
        int win32ThreadPriority = 
            GetThreadPriority ( GetCurrentThread () );
        assert ( win32ThreadPriority != THREAD_PRIORITY_ERROR_RETURN );
        return epicsThreadGetOsiPriorityValue ( win32ThreadPriority );
    }
}

/*
 * epicsThreadSetPriority ()
 */
epicsShareFunc void epicsShareAPI epicsThreadSetPriority ( epicsThreadId id, unsigned priority ) 
{
    win32ThreadParam * pParm = ( win32ThreadParam * ) id;
    BOOL stat = SetThreadPriority ( pParm->handle, epicsThreadGetOsdPriorityValue (priority) );
    assert (stat);
}

/*
 * epicsThreadIsEqual ()
 */
epicsShareFunc int epicsShareAPI epicsThreadIsEqual ( epicsThreadId id1, epicsThreadId id2 ) 
{
    win32ThreadParam * pParm1 = ( win32ThreadParam * ) id1;
    win32ThreadParam * pParm2 = ( win32ThreadParam * ) id2;
    return ( id1 == id2 && pParm1->id == pParm2->id );
}

/*
 * epicsThreadIsSuspended () 
 */
epicsShareFunc int epicsShareAPI epicsThreadIsSuspended ( epicsThreadId id ) 
{
    win32ThreadParam *pParm = ( win32ThreadParam * ) id;
    DWORD exitCode;
    BOOL stat;
    
    stat = GetExitCodeThread ( pParm->handle, & exitCode );
    if ( stat ) {
        if ( exitCode != STILL_ACTIVE ) {
            return 1;
        }
        else {
            return pParm->isSuspended;
        }
    }
    else {
        return 1;
    }
}

/*
 * epicsThreadSleep ()
 */
epicsShareFunc void epicsShareAPI epicsThreadSleep ( double seconds )
{
    static const unsigned mSecPerSec = 1000;
    DWORD milliSecDelay;

    if ( seconds <= 0.0 ) {
        milliSecDelay = 0u;
    }
    else if ( seconds >= INFINITE / mSecPerSec ) {
        milliSecDelay = INFINITE - 1;
    }
    else {
        milliSecDelay = ( DWORD ) ( ( seconds * mSecPerSec ) + 0.5 );
        if ( milliSecDelay == 0 ) {
            milliSecDelay = 1;
        }
    }
    Sleep ( milliSecDelay );
}

/*
 * epicsThreadSleepQuantum ()
 */
double epicsShareAPI epicsThreadSleepQuantum ()
{
    static const double secPerTick = 100e-9;
    DWORD adjustment;
    DWORD delay;
    BOOL disabled;
    BOOL success;

    success = GetSystemTimeAdjustment (
        & adjustment, & delay, & disabled );
    if ( success ) {
        return delay * secPerTick;
    }
    else {
        return 0.0;
    }
}

/*
 * epicsThreadGetIdSelf ()
 */
epicsShareFunc epicsThreadId epicsShareAPI epicsThreadGetIdSelf (void) 
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    assert ( pGbl );

    pParm = ( win32ThreadParam * ) TlsGetValue ( 
        pGbl->tlsIndexThreadLibraryEPICS );
    if ( ! pParm ) {
        pParm = epicsThreadImplicitCreate ();
        assert ( pParm ); /* very dangerous to allow non-unique thread id into use */
    }
    return ( epicsThreadId ) pParm;
}

epicsShareFunc epicsThreadId epicsShareAPI epicsThreadGetId ( const char * pName )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    if ( ! pGbl ) {
        return 0;
    }

    EnterCriticalSection ( & pGbl->mutex );

    for ( pParm = ( win32ThreadParam * ) ellFirst ( & pGbl->threadList ); 
            pParm; pParm = ( win32ThreadParam * ) ellNext ( & pParm->node ) ) {
        if ( pParm->pName ) {
            if ( strcmp ( pParm->pName, pName ) == 0 ) {
                break;
            }
        }
    }

    LeaveCriticalSection ( & pGbl->mutex );

    // !!!! warning - the thread parm could vanish at any time !!!!

    return ( epicsThreadId ) pParm;
}


/*
 * epicsThreadGetNameSelf ()
 */
epicsShareFunc const char * epicsShareAPI epicsThreadGetNameSelf (void)
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    if ( ! pGbl ) {
        return "thread library not initialized";
    }

    pParm = ( win32ThreadParam * ) 
        TlsGetValue ( pGbl->tlsIndexThreadLibraryEPICS );
    if ( ! pParm ) {
        pParm = epicsThreadImplicitCreate ();
    }

    if ( pParm ) {
        if ( pParm->pName ) {
            return pParm->pName;
        }
    }
    return "anonymous";
}

/*
 * epicsThreadGetName ()
 */
epicsShareFunc void epicsShareAPI epicsThreadGetName ( 
    epicsThreadId id, char * pName, size_t size )
{
    win32ThreadParam * pParm = ( win32ThreadParam * ) id;

    if ( size ) {
        size_t sizeMinusOne = size-1;
        strncpy ( pName, pParm->pName, sizeMinusOne );
        pName [sizeMinusOne] = '\0';
    }
}

/*
 * epics_GetThreadPriorityAsString ()
 */
static const char * epics_GetThreadPriorityAsString ( HANDLE thr )
{
    const char * pPriName = "?????";
    switch ( GetThreadPriority ( thr ) ) {
    case THREAD_PRIORITY_TIME_CRITICAL:
        pPriName = "tm-crit";
        break;
    case THREAD_PRIORITY_HIGHEST:
        pPriName = "high";
        break;
    case THREAD_PRIORITY_ABOVE_NORMAL:
        pPriName = "normal+";
        break;
    case THREAD_PRIORITY_NORMAL:
        pPriName = "normal";
        break;
    case THREAD_PRIORITY_BELOW_NORMAL:
        pPriName = "normal-";
        break;
    case THREAD_PRIORITY_LOWEST:
        pPriName = "low";
        break;
    case THREAD_PRIORITY_IDLE:
        pPriName = "idle";
        break;
    }
    return pPriName;
}

/*
 * epicsThreadShowPrivate ()
 */
static void epicsThreadShowPrivate ( epicsThreadId id, unsigned level )
{
    win32ThreadParam * pParm = ( win32ThreadParam * ) id;

    if ( pParm ) {
        printf ( "%-15s %-8p %-8x %-9u %-9s %-7s", pParm->pName, 
            (void *) pParm, pParm->id, pParm->epicsPriority,
            epics_GetThreadPriorityAsString ( pParm->handle ),
            epicsThreadIsSuspended ( id ) ? "suspend" : "ok" );
        if ( level ) {
            printf ( " %-8p %-8p %-8p ",
                (void *) pParm->handle, (void *) pParm->funptr, 
                (void *) pParm->parm );
        }
    }
    else {
        printf ( 
            "NAME            EPICS-ID WIN32-ID EPICS-PRI WIN32-PRI STATE  " );
        if ( level ) {
            printf ( " HANDLE   FUNCTION PARAMETER" );
        }
    }
    printf ("\n" );
}

/*
 * epicsThreadShowAll ()
 */
epicsShareFunc void epicsShareAPI epicsThreadShowAll ( unsigned level )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();
    win32ThreadParam * pParm;

    if ( ! pGbl ) {
        return;
    }

    EnterCriticalSection ( & pGbl->mutex );
    
    epicsThreadShowPrivate ( 0, level );
    for ( pParm = ( win32ThreadParam * ) ellFirst ( & pGbl->threadList ); 
            pParm; pParm = ( win32ThreadParam * ) ellNext ( & pParm->node ) ) {
        epicsThreadShowPrivate ( ( epicsThreadId ) pParm, level );
    }

    LeaveCriticalSection ( & pGbl->mutex );
}

/*
 * epicsThreadShow ()
 */
epicsShareFunc void epicsShareAPI epicsThreadShow ( epicsThreadId id, unsigned level )
{
    epicsThreadShowPrivate ( 0, level );
    epicsThreadShowPrivate ( id, level );
}

/*
 * epicsThreadOnce ()
 */
epicsShareFunc void epicsShareAPI epicsThreadOnceOsd (
    epicsThreadOnceId *id, void (*func)(void *), void *arg )
{
    win32ThreadGlobal * pGbl = fetchWin32ThreadGlobal ();

    assert ( pGbl );
    
    EnterCriticalSection ( & pGbl->mutex );

    if ( ! *id ) {
        ( *func ) ( arg );
        *id = 1;
    }

    LeaveCriticalSection ( & pGbl->mutex );
}

/*
 * epicsThreadPrivateCreate ()
 */
epicsShareFunc epicsThreadPrivateId epicsShareAPI epicsThreadPrivateCreate ()
{
    epicsThreadPrivateOSD *p = ( epicsThreadPrivateOSD * ) malloc ( sizeof ( *p ) );
    if ( p ) {
        p->key = TlsAlloc ();
        if ( p->key == 0xFFFFFFFF ) {
            free ( p );
            p = 0;
        }
    }
    return p;
}

/*
 * epicsThreadPrivateDelete ()
 */
epicsShareFunc void epicsShareAPI epicsThreadPrivateDelete ( epicsThreadPrivateId p )
{
    BOOL stat = TlsFree ( p->key );
    assert ( stat );
    free ( p );
}

/*
 * epicsThreadPrivateSet ()
 */
epicsShareFunc void epicsShareAPI epicsThreadPrivateSet ( epicsThreadPrivateId pPvt, void *pVal )
{
    BOOL stat = TlsSetValue ( pPvt->key, (void *) pVal );
    assert (stat);
}

/*
 * epicsThreadPrivateGet ()
 */
epicsShareFunc void * epicsShareAPI epicsThreadPrivateGet ( epicsThreadPrivateId pPvt )
{
    return ( void * ) TlsGetValue ( pPvt->key );
}

#ifdef TEST_CODES
void testPriorityMapping ()
{
    unsigned i;

    for (i=epicsThreadPriorityMin; i<epicsThreadPriorityMax; i++) {
        printf ("%u %d\n", i, epicsThreadGetOsdPriorityValue (i) );
    }

    for (i=0; i<osdPriorityStateCount; i++) {
        printf ("%d %u\n", osdPriorityList[i], epicsThreadGetOsiPriorityValue(osdPriorityList[i]));
    }
    return 0;
}
#endif