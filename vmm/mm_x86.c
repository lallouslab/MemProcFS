// mm_x86.c : implementation of the x86 32-bit protected mode memory model.
//
// (c) Ulf Frisk, 2018-2019
// Author: Ulf Frisk, pcileech@frizk.net
//
#include "vmm.h"
#include "vmmproc.h"

#define MMX86_MEMMAP_DISPLAYBUFFER_LINE_LENGTH      70
#define MMX86_PTE_IS_TRANSITION(pte, iPML)          ((((pte & 0x0c01) == 0x0800) && (iPML == 1) && ctxVmm && (ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X86)) ? ((pte & 0xfffff000) | 0x005) : 0)
#define MMX86_PTE_IS_VALID(pte, iPML)               (pte & 0x01)

/*
* Tries to verify that a loaded page table is correct. If just a bit strange
* bytes/ptes supplied in pb will be altered to look better.
*/
BOOL MmX86_TlbPageTableVerify(_Inout_ PBYTE pb, _In_ QWORD pa, _In_ BOOL fSelfRefReq)
{
    return TRUE;
}

/*
* Iterate over the PD to retrieve uncached PT pages and then commit them to the cache.
*/
VOID MmX86_TlbSpider(_In_ PVMM_PROCESS pProcess)
{
    PVMMOB_MEM pObPD = NULL;
    DWORD i, pte;
    POB_VSET pObPageSet = NULL;
    if(pProcess->fTlbSpiderDone) { return; }
    if(!(pObPageSet = ObVSet_New())) { return; }
    pObPD = VmmTlbGetPageTable(pProcess->paDTB & 0xfffff000, FALSE);
    if(!pObPD) { goto fail; }
    for(i = 0; i < 1024; i++) {
        pte = pObPD->pdw[i];
        if(!(pte & 0x01)) { continue; }                 // not valid
        if(pte & 0x80) { continue; }                    // not valid ptr to PT
        if(pProcess->fUserOnly && !(pte & 0x04)) { continue; }    // supervisor page when fUserOnly -> not valid
        ObVSet_Push(pObPageSet, pte & 0xfffff000);
    }
    VmmTlbPrefetch(pObPageSet);
    pProcess->fTlbSpiderDone = TRUE;
fail:
    Ob_DECREF(pObPageSet);
    Ob_DECREF(pObPD);
}

const DWORD MMX86_PAGETABLEMAP_PML_REGION_SIZE[3] = { 0, 12, 22 };

VOID MmX86_MapInitialize_Index(_In_ PVMM_PROCESS pProcess, _In_ PVMM_MEMMAP_ENTRY pMemMap, _In_ PDWORD pcMemMap, _In_ DWORD vaBase, _In_ BYTE iPML, _In_ DWORD PTEs[1024], _In_ BOOL fSupervisorPML, _In_ QWORD paMax)
{
    PVMMOB_MEM pObNextPT;
    DWORD i, va, pte;
    BOOL fUserOnly, fNextSupervisorPML, fTransition = FALSE;
    PVMM_MEMMAP_ENTRY pMemMapEntry = pMemMap + *pcMemMap - 1;
    fUserOnly = pProcess->fUserOnly;
    for(i = 0; i < 1024; i++) {
        pte = PTEs[i];
        if(!MMX86_PTE_IS_VALID(pte, iPML)) {
            if(pte && MMX86_PTE_IS_TRANSITION(pte, iPML)) {
                pte = MMX86_PTE_IS_TRANSITION(pte, iPML);   // TRANSITION PAGE
                fTransition = TRUE;
            } else {
                continue;                                   // INVALID
            }
        }
        if((pte & 0xfffff000) > paMax) { continue; }
        if(fSupervisorPML) { pte = pte & 0xfffffffb; }
        if(fUserOnly && !(pte & 0x04)) { continue; }
        va = vaBase + (i << MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
        if((iPML == 1) || (pte & 0x80) /* PS */) {
            if((*pcMemMap == 0) ||
                ((pMemMapEntry->fPage != (pte & VMM_MEMMAP_PAGE_MASK)) && !fTransition) ||
                (va != pMemMapEntry->AddrBase + (pMemMapEntry->cPages << 12))) {
                if(*pcMemMap + 1 >= VMM_MEMMAP_ENTRIES_MAX) { return; }
                pMemMapEntry = pMemMap + *pcMemMap;
                pMemMapEntry->AddrBase = va;
                pMemMapEntry->fPage = pte & VMM_MEMMAP_PAGE_MASK;
                pMemMapEntry->cPages = 1ULL << (MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML] - 12);
                *pcMemMap = *pcMemMap + 1;
                if(*pcMemMap >= VMM_MEMMAP_ENTRIES_MAX - 1) { return; }
                continue;
            }
            pMemMapEntry->cPages += 1ULL << (MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML] - 12);
            continue;
        }
        // maps page table
        fNextSupervisorPML = !(pte & 0x04);
        pObNextPT = VmmTlbGetPageTable(pte & 0xfffff000, FALSE);
        if(!pObNextPT) { continue; }
        MmX86_MapInitialize_Index(pProcess, pMemMap, pcMemMap, va, 1, pObNextPT->pdw, fNextSupervisorPML, paMax);
        Ob_DECREF(pObNextPT);
        pMemMapEntry = pMemMap + *pcMemMap - 1;
    }
}

VOID MmX86_MapCloseObCallback(_In_ PVOID pVmmOb)
{
    PVMMOB_MEMMAP pObMemMap = (PVMMOB_MEMMAP)pVmmOb;
    if(pObMemMap->pObDisplay) {
        Ob_DECREF(pObMemMap->pObDisplay);
    }
}

_Success_(return)
BOOL MmX86_MapInitialize(_In_ PVMM_PROCESS pProcess)
{
    PVMMOB_MEM pObPD;
    DWORD cMemMap = 0;
    PVMM_MEMMAP_ENTRY pMemMap = NULL;
    PVMMOB_MEMMAP pObMemMap = NULL;
    // already existing?
    if(pProcess && pProcess->pObMemMap) {
        return pProcess->pObMemMap->fValid;
    }
    EnterCriticalSection(&pProcess->LockUpdate);
    if(pProcess && pProcess->pObMemMap) {
        LeaveCriticalSection(&pProcess->LockUpdate);
        return pProcess->pObMemMap->fValid;
    }
    // allocate temporary buffer and walk page tables
    VmmTlbSpider(pProcess);
    pObPD = VmmTlbGetPageTable(pProcess->paDTB & 0xfffff000, FALSE);
    if(pObPD) {
        pMemMap = (PVMM_MEMMAP_ENTRY)LocalAlloc(LMEM_ZEROINIT, VMM_MEMMAP_ENTRIES_MAX * sizeof(VMM_MEMMAP_ENTRY));
        if(pMemMap) {
            MmX86_MapInitialize_Index(pProcess, pMemMap, &cMemMap, 0, 2, pObPD->pdw, FALSE, ctxMain->dev.paMax);
        }
        Ob_DECREF(pObPD);
    }
    // allocate VmmOb depending on result
    pObMemMap = Ob_Alloc('MM', 0, sizeof(VMMOB_MEMMAP) + cMemMap * sizeof(VMM_MEMMAP_ENTRY), MmX86_MapCloseObCallback, NULL);
    if(!pObMemMap) {
        LeaveCriticalSection(&pProcess->LockUpdate);
        LocalFree(pMemMap);
        return FALSE;
    }
    pObMemMap->fValid = cMemMap > 0;
    pObMemMap->fTagModules = FALSE;
    pObMemMap->fTagScan = FALSE;
    pObMemMap->cMap = cMemMap;
    pObMemMap->cbDisplay = cMemMap * MMX86_MEMMAP_DISPLAYBUFFER_LINE_LENGTH;
    pObMemMap->pObDisplay = NULL;
    if(cMemMap > 0) {
        memcpy(pObMemMap->pMap, pMemMap, cMemMap * sizeof(VMM_MEMMAP_ENTRY));
    }
    LocalFree(pMemMap);
    pProcess->pObMemMap = pObMemMap;
    LeaveCriticalSection(&pProcess->LockUpdate);
    return pObMemMap->fValid;
}

/*
* Map a tag into the sorted memory map in O(log2) operations. Supply only one of szTag or wszTag.
* -- pProcess
* -- vaBase
* -- vaLimit = limit == vaBase + size (== top address in range +1)
* -- szTag
* -- wszTag
* -- fWoW64
* -- fOverwrite
*/
VOID MmX86_MapTag(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaBase, _In_ QWORD vaLimit, _In_opt_ LPSTR szTag, _In_opt_ LPWSTR wszTag, _In_ BOOL fWoW64, _In_ BOOL fOverwrite)
{
    // NB! update here may take placey without acquiring the process 'LockUpdate'
    // Data is not super important so it should be ok. Also, in many cases the
    // lock will already be acquired by MapGetEntries function.
    PVMM_MEMMAP_ENTRY pMap;
    QWORD i, lvl, cMap;
    if(!MmX86_MapInitialize(pProcess)) { return; }
    if((vaBase > 0xffffffff) || (vaLimit > 0xffffffff)) { return; }
    pMap = pProcess->pObMemMap->pMap;
    cMap = pProcess->pObMemMap->cMap;
    if(!pMap || !cMap) { return; }
    // 1: locate base
    lvl = 1;
    i = cMap >> lvl;
    while(TRUE) {
        lvl++;
        if((cMap >> lvl) == 0) {
            break;
        }
        if(pMap[i].AddrBase > vaBase) {
            i -= (cMap >> lvl);
        } else {
            i += (cMap >> lvl);
        }
    }
    // 2: scan back if needed
    while(i && (pMap[i].AddrBase > vaBase)) {
        i--;
    }
    // 3: fill in tag
    while((i < cMap) && (pMap[i].AddrBase + (pMap[i].cPages << 12) <= vaLimit)) {
        if((pMap[i].AddrBase >= vaBase) && (fOverwrite || !pMap[i].szTag[0])) {
            if(wszTag) {
                snprintf(pMap[i].szTag, 31, "%S", wszTag);
            }
            if(szTag) {
                snprintf(pMap[i].szTag, 31, "%s", szTag);
            }
        }
        i++;
    }
}

_Success_(return)
BOOL MmX86_MapGetEntries(_In_ PVMM_PROCESS pProcess, _In_ DWORD flags, _Out_ PVMMOB_MEMMAP *ppObMemMap)
{
    DWORD i;
    PVMM_MODULEMAP_ENTRY pModule;
    PVMMOB_MODULEMAP pObModuleMap;
    if(!MmX86_MapInitialize(pProcess)) { return FALSE; }
    if((!pProcess->pObMemMap->fTagModules && (flags & VMM_MEMMAP_FLAG_MODULES)) || (!pProcess->pObMemMap->fTagScan && (flags & VMM_MEMMAP_FLAG_SCAN))) {
        EnterCriticalSection(&pProcess->LockUpdate);
        if(!pProcess->pObMemMap->fTagModules && (flags & VMM_MEMMAP_FLAG_MODULES)) {
            pProcess->pObMemMap->fTagModules = TRUE;
            if(VmmProc_ModuleMapGet(pProcess, &pObModuleMap)) {
                // update memory map with names
                for(i = 0; i < pObModuleMap->cMap; i++) {
                    pModule = pObModuleMap->pMap + i;
                    MmX86_MapTag(pProcess, pModule->BaseAddress, pModule->BaseAddress + pModule->SizeOfImage, pModule->szName, NULL, FALSE, FALSE);
                }
                Ob_DECREF(pObModuleMap);
            }
        }
        if(!pProcess->pObMemMap->fTagScan && (flags & VMM_MEMMAP_FLAG_SCAN)) {
            pProcess->pObMemMap->fTagScan = TRUE;
            VmmProc_ScanTagsMemMap(pProcess);
        }
        LeaveCriticalSection(&pProcess->LockUpdate);
    }
    *ppObMemMap = Ob_INCREF(pProcess->pObMemMap);
    return TRUE;
}

_Success_(return)
BOOL MmX86_MapGetDisplay(_In_ PVMM_PROCESS pProcess, _In_ DWORD flags, _Out_ PVMMOB_PDATA *ppObDisplay)
{
    DWORD i, o = 0;
    PVMMOB_MEMMAP pObMemMap = NULL;
    PVMMOB_PDATA pObDisplay = NULL;
    // memory map display data already exists
    if(!MmX86_MapInitialize(pProcess)) { return FALSE; }
    if(pProcess->pObMemMap->pObDisplay) {
        *ppObDisplay = Ob_INCREF(pProcess->pObMemMap->pObDisplay);
        return TRUE;
    }
    // create new memory map display data
    EnterCriticalSection(&pProcess->LockUpdate);
    if(!pProcess->pObMemMap->pObDisplay) {
        if(MmX86_MapGetEntries(pProcess, flags, &pObMemMap)) {
            pObDisplay = Ob_Alloc('MD', LMEM_ZEROINIT, pObMemMap->cbDisplay, NULL, NULL);
            if(pObDisplay) {
                for(i = 0; i < pObMemMap->cMap; i++) {
                    if(o + MMX86_MEMMAP_DISPLAYBUFFER_LINE_LENGTH > pObMemMap->cbDisplay) {
                        vmmprintf_fn("ERROR: SHOULD NOT HAPPEN! LENGTH DIFFERS #1: %i %i\n", o + MMX86_MEMMAP_DISPLAYBUFFER_LINE_LENGTH, pObMemMap->cbDisplay);
                        Ob_DECREF(pObDisplay);
                        pObDisplay = NULL;
                        goto fail;
                    }
                    o += snprintf(
                        pObDisplay->pbData + o,
                        pObMemMap->cbDisplay - o,
                        "%04x %8x %08x-%08x %sr%sx %-32s\n",
                        i,
                        (DWORD)pObMemMap->pMap[i].cPages,
                        (DWORD)pObMemMap->pMap[i].AddrBase,
                        (DWORD)(pObMemMap->pMap[i].AddrBase + (pObMemMap->pMap[i].cPages << 12) - 1),
                        pObMemMap->pMap[i].fPage & VMM_MEMMAP_PAGE_NS ? "-" : "s",
                        pObMemMap->pMap[i].fPage & VMM_MEMMAP_PAGE_W ? "w" : "-",
                        pObMemMap->pMap[i].szTag
                    );
                }
                if(o != pObMemMap->cbDisplay) {
                    vmmprintf_fn("ERROR: SHOULD NOT HAPPEN! LENGTH DIFFERS #2: %i %i\n", o, pObMemMap->cbDisplay);
                    Ob_DECREF(pObDisplay);
                    pObDisplay = NULL;
                    goto fail;
                }
                pObDisplay->pbData[o - 1] = '\n';
            }
        }
        pProcess->pObMemMap->pObDisplay = pObDisplay;
    }
fail:
    Ob_DECREF(pObMemMap);
    LeaveCriticalSection(&pProcess->LockUpdate);
    if(pProcess->pObMemMap->pObDisplay) {
        *ppObDisplay = Ob_INCREF(pProcess->pObMemMap->pObDisplay);
        return TRUE;
    }
    return FALSE;
}

_Success_(return)
BOOL MmX86_Virt2Phys(_In_ QWORD paPT, _In_ BOOL fUserOnly, _In_ BYTE iPML, _In_ QWORD va, _Out_ PQWORD ppa)
{
    DWORD pte, i;
    PVMMOB_MEM pObPTEs;
    //PBYTE pbPTEs;
    if(va > 0xffffffff) { return FALSE; }
    if(paPT > 0xffffffff) { return FALSE; }
    if(iPML == (BYTE)-1) { iPML = 2; }
    pObPTEs = VmmTlbGetPageTable(paPT & 0xfffff000, FALSE);
    if(!pObPTEs) { return FALSE; }
    i = 0x3ff & (va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
    pte = pObPTEs->pdw[i];
    Ob_DECREF(pObPTEs);
    if(!MMX86_PTE_IS_VALID(pte, iPML)) {
        if(pte && MMX86_PTE_IS_TRANSITION(pte, iPML)) {
            pte = MMX86_PTE_IS_TRANSITION(pte, iPML);       // TRANSITION
        } else {
            if(iPML == 1) { *ppa = pte; }                   // NOT VALID
            return FALSE;
        }
    }
    if(fUserOnly && !(pte & 0x04)) { return FALSE; }        // SUPERVISOR PAGE & USER MODE REQ
    if((iPML == 2) && !(pte & 0x80) /* PS */) {
        return MmX86_Virt2Phys(pte, fUserOnly, 1, va, ppa);
    }
    if(iPML == 1) { // 4kB PAGE
        *ppa = pte & 0xfffff000;
        return TRUE;
    }
    // 4MB PAGE
    if(pte & 0x003e0000) { return FALSE; }                  // RESERVED
    *ppa = (((QWORD)(pte & 0x0001e000)) << (32 - 13)) + (pte & 0xffc00000) + (va & 0x003ff000);
    return TRUE;
}

VOID MmX86_Virt2PhysGetInformation_DoWork(_Inout_ PVMM_PROCESS pProcess, _Inout_ PVMM_VIRT2PHYS_INFORMATION pVirt2PhysInfo, _In_ BYTE iPML, _In_ QWORD paPT)
{
    PVMMOB_MEM pObPTEs;
    DWORD pte, i;
    pObPTEs = VmmTlbGetPageTable(paPT, FALSE);
    if(!pObPTEs) { return; }
    i = 0x3ff & (pVirt2PhysInfo->va >> MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
    pte = pObPTEs->pdw[i];
    Ob_DECREF(pObPTEs);
    pVirt2PhysInfo->pas[iPML] = paPT;
    pVirt2PhysInfo->iPTEs[iPML] = (WORD)i;
    pVirt2PhysInfo->PTEs[iPML] = pte;
    if(!MMX86_PTE_IS_VALID(pte, iPML)) { return; }          // NOT VALID
    if(pProcess->fUserOnly && !(pte & 0x04)) { return; }    // SUPERVISOR PAGE & USER MODE REQ
    if(iPML == 1) {     // 4kB page
        pVirt2PhysInfo->pas[0] = pte & 0xfffff000;
        return;
    }
    if(pte & 0x80) {    // 4MB page
        if(pte & 0x003e0000) { return; }                    // RESERVED
        pVirt2PhysInfo->pas[0] = (pte & 0xffc00000) + (((QWORD)(pte & 0x0001e000)) << (32 - 13));
        return;
    }
    MmX86_Virt2PhysGetInformation_DoWork(pProcess, pVirt2PhysInfo, 1, pte & 0xfffff000); // PDE
}

VOID MmX86_Virt2PhysGetInformation(_Inout_ PVMM_PROCESS pProcess, _Inout_ PVMM_VIRT2PHYS_INFORMATION pVirt2PhysInfo)
{
    QWORD va;
    if(pVirt2PhysInfo->va > 0xffffffff) { return; }
    va = pVirt2PhysInfo->va;
    ZeroMemory(pVirt2PhysInfo, sizeof(VMM_VIRT2PHYS_INFORMATION));
    pVirt2PhysInfo->tpMemoryModel = VMM_MEMORYMODEL_X86;
    pVirt2PhysInfo->va = va;
    MmX86_Virt2PhysGetInformation_DoWork(pProcess, pVirt2PhysInfo, 2, pProcess->paDTB & 0xfffff000);
}

VOID MmX86_Phys2VirtGetInformation_Index(_In_ PVMM_PROCESS pProcess, _In_ DWORD vaBase, _In_ BYTE iPML, _In_ DWORD PTEs[1024], _In_ QWORD paMax, _Inout_ PVMMOB_PHYS2VIRT_INFORMATION pP2V)
{
    BOOL fUserOnly;
    QWORD pa;
    DWORD i, va, pte;
    PVMMOB_MEM pObNextPT;
    if(!pProcess->fTlbSpiderDone) {
        VmmTlbSpider(pProcess);
    }
    fUserOnly = pProcess->fUserOnly;
    for(i = 0; i < 1024; i++) {
        pte = PTEs[i];
        if(!MMX86_PTE_IS_VALID(pte, iPML)) { continue; }
        if((pte & 0xfffff000) > paMax) { continue; }
        if(fUserOnly && !(pte & 0x04)) { continue; }
        va = vaBase + (i << MMX86_PAGETABLEMAP_PML_REGION_SIZE[iPML]);
        if(iPML == 1) {
            if((pte & 0xfffff000) == (pP2V->paTarget & 0xfffff000)) {
                pP2V->pvaList[pP2V->cvaList] = va | (pP2V->paTarget & 0xfff);
                pP2V->cvaList++;
                if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
            }
            continue;
        }
        if(pte & 0x80 /* PS */) {
            pa = (pte & 0xffc00000 | ((QWORD)(pte & 0x001fe000) << 19));
            if(pa == (pP2V->paTarget & 0xffc00000)) {
                pP2V->pvaList[pP2V->cvaList] = va | (pP2V->paTarget & 0x3fffff);
                pP2V->cvaList++;
                if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
            }
            continue;
        }
        // maps page table
        if(fUserOnly && !(pte & 0x04)) { continue; }    // do not go into supervisor pages if user-only adderss space
        pObNextPT = VmmTlbGetPageTable(pte & 0xfffff000, FALSE);
        if(!pObNextPT) { continue; }
        MmX86_Phys2VirtGetInformation_Index(pProcess, va, 1, pObNextPT->pdw, paMax, pP2V);
        Ob_DECREF(pObNextPT);
        if(pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) { return; }
    }
}

VOID MmX86_Phys2VirtGetInformation(_In_ PVMM_PROCESS pProcess, _Inout_ PVMMOB_PHYS2VIRT_INFORMATION pP2V)
{
    PVMMOB_MEM pObPD;
    if((pP2V->cvaList == VMM_PHYS2VIRT_INFORMATION_MAX_PROCESS_RESULT) || (pP2V->paTarget > ctxMain->dev.paMax)) { return; }
    pObPD = VmmTlbGetPageTable(pProcess->paDTB & 0xfffff000, FALSE);
    if(!pObPD) { return; }
    MmX86_Phys2VirtGetInformation_Index(pProcess, 0, 2, pObPD->pdw, ctxMain->dev.paMax, pP2V);
    Ob_DECREF(pObPD);
}

VOID MmX86_Close()
{
    ctxVmm->f32 = FALSE;
    ctxVmm->tpMemoryModel = VMM_MEMORYMODEL_NA;
    ZeroMemory(&ctxVmm->fnMemoryModel, sizeof(VMM_MEMORYMODEL_FUNCTIONS));
}

VOID MmX86_Initialize()
{
    if(ctxVmm->fnMemoryModel.pfnClose) {
        ctxVmm->fnMemoryModel.pfnClose();
    }
    ctxVmm->fnMemoryModel.pfnClose = MmX86_Close;
    ctxVmm->fnMemoryModel.pfnVirt2Phys = MmX86_Virt2Phys;
    ctxVmm->fnMemoryModel.pfnVirt2PhysGetInformation = MmX86_Virt2PhysGetInformation;
    ctxVmm->fnMemoryModel.pfnPhys2VirtGetInformation = MmX86_Phys2VirtGetInformation;
    ctxVmm->fnMemoryModel.pfnMapTag = MmX86_MapTag;
    ctxVmm->fnMemoryModel.pfnMapGetEntries = MmX86_MapGetEntries;
    ctxVmm->fnMemoryModel.pfnMapGetDisplay = MmX86_MapGetDisplay;
    ctxVmm->fnMemoryModel.pfnTlbSpider = MmX86_TlbSpider;
    ctxVmm->fnMemoryModel.pfnTlbPageTableVerify = MmX86_TlbPageTableVerify;
    ctxVmm->tpMemoryModel = VMM_MEMORYMODEL_X86;
    ctxVmm->f32 = TRUE;
}
