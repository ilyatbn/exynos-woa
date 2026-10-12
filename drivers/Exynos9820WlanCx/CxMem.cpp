/*  CxMem.cpp - Exynos9820WlanCx: allocator for the WDI TLV generator/parser (WificxTLVGenParse.lib).

    The library allocates with operator new(size_t, ULONG_PTR AllocationContext) and frees with the matching
    deletes; the client supplies them (WiFiCx sample memorymanagement.cpp:117-148). TLV_CONTEXT.AllocationContext is
    always 0 in this driver, so every allocation comes from non-paged pool (the generators run on the worker and the
    parsers on the worker, both PASSIVE, but non-paged keeps them usable at DISPATCH too).
*/

#include "Cx.h"

#define CX_TLV_TAG  'tClW'

void * __cdecl
operator new(size_t Size) noexcept
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, (Size != 0) ? Size : 1, CX_TLV_TAG);
}

void * __cdecl
operator new[](size_t Size) noexcept
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, (Size != 0) ? Size : 1, CX_TLV_TAG);
}

void * __cdecl
operator new(size_t Size, ULONG_PTR AllocationContext) noexcept
{
    UNREFERENCED_PARAMETER(AllocationContext);   /* always 0 here (see file comment) */
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, (Size != 0) ? Size : 1, CX_TLV_TAG);
}

void * __cdecl
operator new[](size_t Size, ULONG_PTR AllocationContext) noexcept
{
    UNREFERENCED_PARAMETER(AllocationContext);
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, (Size != 0) ? Size : 1, CX_TLV_TAG);
}

void __cdecl
operator delete(void *P) noexcept
{
    if (P != nullptr) {
        ExFreePoolWithTag(P, CX_TLV_TAG);
    }
}

void __cdecl
operator delete[](void *P) noexcept
{
    if (P != nullptr) {
        ExFreePoolWithTag(P, CX_TLV_TAG);
    }
}

void __cdecl
operator delete(void *P, size_t) noexcept
{
    if (P != nullptr) {
        ExFreePoolWithTag(P, CX_TLV_TAG);
    }
}

void __cdecl
operator delete[](void *P, size_t) noexcept
{
    if (P != nullptr) {
        ExFreePoolWithTag(P, CX_TLV_TAG);
    }
}

/* operator delete(void *, ULONG_PTR) - the TLV library's placement delete - is the sized delete above: ULONG_PTR and
   size_t are the same 64-bit type on ARM64. */
