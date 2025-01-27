// ob_container.c : implementation of object manager container functionality.
//
// A container provides atomic access to a single Ob object. This is useful
// if a Ob object is to frequently be replaced by a new object in an atomic
// way. An example of this is the process list object containing the process
// information. The container holds a reference count to the object that is
// contained. The object container itself is an object manager object and
// must be DECREF'ed when required.
//
// (c) Ulf Frisk, 2018-2019
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "ob.h"

#define OB_CONTAINER_IS_VALID(p)        (p && (p->ObHdr.Reserved.magic == OB_HEADER_MAGIC) && (p->ObHdr.Reserved.tag == 'CO'))

/*
* Object Container object manager cleanup function to be called when reference
* count reaches zero.
* -- pObContainer
*/
VOID ObContainer_ObCloseCallback(_In_ POB_CONTAINER pObContainer)
{
    if(!OB_CONTAINER_IS_VALID(pObContainer)) { return; }
    DeleteCriticalSection(&pObContainer->Lock);
    Ob_DECREF(pObContainer->pOb);
}

/*
* Create a new object container object with an optional contained object.
* An object container provides atomic access to its contained object in a
* multithreaded environment. The object container is in itself an object
* manager object and must be DECREF'ed by the caller when use is complete.
* CALLER DECREF: return
* -- pOb = optional contained object.
* -- return
*/
POB_CONTAINER ObContainer_New(_In_opt_ PVOID pOb)
{
    POB_CONTAINER pObContainer = Ob_Alloc('CO', 0, sizeof(OB_CONTAINER) - sizeof(OB), ObContainer_ObCloseCallback, NULL);
    if(!pObContainer) { return NULL; }
    if(!InitializeCriticalSectionAndSpinCount(&pObContainer->Lock, 4096)) {
        LocalFree(pObContainer);
        return NULL;
    }
    pObContainer->pOb = Ob_INCREF(pOb);
    return pObContainer;
}

/*
* Retrieve an enclosed object from the given pObContainer.
* CALLER DECREF: return
* -- pObContainer
* -- return
*/
PVOID ObContainer_GetOb(_In_ POB_CONTAINER pObContainer)
{
    POB pOb;
    if(!OB_CONTAINER_IS_VALID(pObContainer)) { return NULL; }
    EnterCriticalSection(&pObContainer->Lock);
    pOb = Ob_INCREF(pObContainer->pOb);
    LeaveCriticalSection(&pObContainer->Lock);
    return pOb;
}

/*
* Set or Replace an object in the object container.
* -- pObContainer
* -- pOb
*/
VOID ObContainer_SetOb(_In_ POB_CONTAINER pObContainer, _In_opt_ PVOID pOb)
{
    if(!OB_CONTAINER_IS_VALID(pObContainer)) { return; }
    EnterCriticalSection(&pObContainer->Lock);
    Ob_DECREF(pObContainer->pOb);
    pObContainer->pOb = Ob_INCREF(pOb);
    LeaveCriticalSection(&pObContainer->Lock);
}
