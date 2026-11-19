#include "vm_track.h"

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

extern uint8_t* vm_base;

enum { PROTECTED = 1, DIRTY = 2 };
#define PAGE_SHIFT 12
#define CHUNK_PAGES 16u
#define PAGES ((1u << (32 - PAGE_SHIFT)) + CHUNK_PAGES)

static uint8_t s_tracked[PAGES];
static uint8_t s_page[PAGES];
static uint16_t s_pin[PAGES];
static uint32_t s_dirty[PAGES];
static uint32_t s_dirty_n;
static SRWLOCK s_lock = SRWLOCK_INIT;

static uint8_t* page_ptr(uint32_t page) { return vm_base + ((size_t)page << PAGE_SHIFT); }

static uint32_t region_end(const MEMORY_BASIC_INFORMATION* mi, uint32_t limit)
{
    const uint64_t end = (uint64_t)((uint8_t*)mi->BaseAddress + mi->RegionSize - vm_base) >> PAGE_SHIFT;
    return end < limit ? (uint32_t)end : limit;
}

static void mark_dirty(uint32_t page)
{
    if (s_page[page] & DIRTY) return;
    s_page[page] |= DIRTY;
    s_dirty[s_dirty_n++] = page;
}

static int protectable(uint32_t page)
{
    return s_tracked[page] && !(s_page[page] & PROTECTED) && !s_pin[page];
}

static void protect(uint32_t first, uint32_t end)
{
    for (uint32_t p = first; p < end;) {
        if (!protectable(p)) {
            p++;
            continue;
        }
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(page_ptr(p), &mi, sizeof mi)) return;
        const uint32_t stop = region_end(&mi, end);
        uint32_t q = p + 1u;
        while (q < stop && protectable(q)) q++;
        DWORD old;
        if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && VirtualProtect(page_ptr(p), (size_t)(q - p) << PAGE_SHIFT, PAGE_READONLY, &old))
            for (uint32_t k = p; k < q; k++) s_page[k] |= PROTECTED;
        p = q;
    }
}

static void unprotect(uint32_t page)
{
    DWORD old;
    if (VirtualProtect(page_ptr(page), (size_t)1 << PAGE_SHIFT, PAGE_READWRITE, &old)) s_page[page] &= (uint8_t)~PROTECTED;
}

static int commit(uint32_t first, uint32_t end)
{
    for (uint32_t p = first; p < end;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(page_ptr(p), &mi, sizeof mi)) return 0;
        const uint32_t stop = region_end(&mi, end);
        if (mi.State == MEM_RESERVE) {
            if (!VirtualAlloc(page_ptr(p), (size_t)(stop - p) << PAGE_SHIFT, MEM_COMMIT, PAGE_READWRITE)) return 0;
            for (uint32_t k = p; k < stop; k++)
                if (s_tracked[k]) mark_dirty(k);
        } else if (mi.State != MEM_COMMIT) {
            return 0;
        }
        p = stop;
    }
    return 1;
}

static int host_pages(const void* host, size_t size, uint32_t* first, uint32_t* end)
{
    const uintptr_t a = (uintptr_t)host, base = (uintptr_t)vm_base;
    if (!size || !vm_base || a < base || a - base >= ((uint64_t)PAGES << PAGE_SHIFT)) return 0;
    const uint64_t last = ((uint64_t)(a - base) + size - 1u) >> PAGE_SHIFT;
    *first = (uint32_t)((a - base) >> PAGE_SHIFT);
    *end = last >= PAGES ? PAGES : (uint32_t)last + 1u;
    return 1;
}

static LONG CALLBACK track_fault(EXCEPTION_POINTERS* e)
{
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    uint32_t page, end;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !host_pages((const void*)r->ExceptionInformation[1], 1, &page, &end))
        return EXCEPTION_CONTINUE_SEARCH;
    LONG out = EXCEPTION_CONTINUE_SEARCH;
    AcquireSRWLockExclusive(&s_lock);
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(page_ptr(page), &mi, sizeof mi)) {
        if (mi.State == MEM_RESERVE) {
            const uint32_t chunk = page & ~(CHUNK_PAGES - 1u);
            if (commit(chunk, chunk + CHUNK_PAGES)) out = EXCEPTION_CONTINUE_EXECUTION;
        } else if (s_page[page] & PROTECTED) {
            unprotect(page);
            mark_dirty(page);
            out = EXCEPTION_CONTINUE_EXECUTION;
        } else if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE) {
            out = EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return out;
}

void vm_track_install(void)
{
    AddVectoredExceptionHandler(1, track_fault);
}

void vm_track(uint32_t ea, uint32_t size, vm_page_fn changed, void* user)
{
    const uint32_t first = ea >> PAGE_SHIFT, end = (uint32_t)((((uint64_t)ea + size - 1u) >> PAGE_SHIFT) + 1u);
    uint32_t p = first;
    while (p < end && __atomic_load_n(&s_tracked[p], __ATOMIC_ACQUIRE)) p++;
    if (p == end) return;
    AcquireSRWLockExclusive(&s_lock);
    for (; p < end; p++) {
        if (s_tracked[p]) continue;
        __atomic_store_n(&s_tracked[p], 1, __ATOMIC_RELEASE);
        changed(user, p);
    }
    protect(first, end);
    ReleaseSRWLockExclusive(&s_lock);
}


static int by_page(const void* a, const void* b)
{
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

uint32_t vm_track_drain(vm_page_fn changed, void* user)
{
    AcquireSRWLockExclusive(&s_lock);
    const uint32_t n = s_dirty_n;
    s_dirty_n = 0;
    qsort(s_dirty, n, sizeof s_dirty[0], by_page);
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1u;
        while (j < n && s_dirty[j] == s_dirty[j - 1u] + 1u) j++;
        const uint32_t lo = s_dirty[i], hi = s_dirty[j - 1u] + 1u;
        protect(lo, hi);
        for (uint32_t k = i; k < j; k++) {
            const uint32_t pg = s_dirty[k];
            s_page[pg] &= (uint8_t)~DIRTY;
            changed(user, pg);
            if (s_pin[pg]) mark_dirty(pg);
        }
        i = j;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return n;
}

int vm_commit_host(const void* host, size_t size)
{
    uint32_t first, end;
    if (!host_pages(host, size, &first, &end)) return 1;
    AcquireSRWLockExclusive(&s_lock);
    const int ok = commit(first, end);
    ReleaseSRWLockExclusive(&s_lock);
    return ok;
}

void vm_io_begin(const void* host, size_t size)
{
    uint32_t first, end;
    if (!host_pages(host, size, &first, &end)) return;
    AcquireSRWLockExclusive(&s_lock);
    commit(first, end);
    for (uint32_t p = first; p < end; p++) {
        s_pin[p]++;
        if (!s_tracked[p]) continue;
        if (s_page[p] & PROTECTED) unprotect(p);
        mark_dirty(p);
    }
    ReleaseSRWLockExclusive(&s_lock);
}

void vm_io_end(const void* host, size_t size)
{
    uint32_t first, end;
    if (!host_pages(host, size, &first, &end)) return;
    AcquireSRWLockExclusive(&s_lock);
    for (uint32_t p = first; p < end; p++) {
        s_pin[p]--;
        if (s_tracked[p]) mark_dirty(p);
    }
    ReleaseSRWLockExclusive(&s_lock);
}
#else
void vm_track_install(void) {}
void vm_track(uint32_t ea, uint32_t size, vm_page_fn changed, void* user) { (void)ea; (void)size; (void)changed; (void)user; }
uint32_t vm_track_drain(vm_page_fn changed, void* user) { (void)changed; (void)user; return 0; }
int vm_commit_host(const void* host, size_t size) { (void)host; (void)size; return 1; }
void vm_io_begin(const void* host, size_t size) { (void)host; (void)size; }
void vm_io_end(const void* host, size_t size) { (void)host; (void)size; }
#endif
