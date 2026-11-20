#include "vm_track.h"
#include "vm_page_map.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "../platform/win32_compat.h"
#endif

extern uint8_t* vm_base;

#define PAGE_SHIFT 12
#define PAGES (1u << (32 - PAGE_SHIFT))

static uint8_t s_tracked[PAGES];
static uint16_t s_pin[PAGES];
static uint32_t s_dirty[PAGES];
static SRWLOCK s_lock = SRWLOCK_INIT;

static int arm(uint32_t page)
{
    if (!s_tracked[page] || s_pin[page]) return 0;
    __atomic_fetch_or(&ppu_slow_page[page], VM_PAGE_TRACKED, __ATOMIC_SEQ_CST);
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

#ifdef _WIN32
#define CHUNK_PAGES 16u

static uint8_t* page_ptr(uint32_t page) { return vm_base + ((size_t)page << PAGE_SHIFT); }

static uint32_t region_end(const MEMORY_BASIC_INFORMATION* mi, uint32_t limit)
{
    const uint64_t end = (uint64_t)((uint8_t*)mi->BaseAddress + mi->RegionSize - vm_base) >> PAGE_SHIFT;
    return end < limit ? (uint32_t)end : limit;
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
                if (s_tracked[k]) vm_page_mark_dirty(k);
        } else if (mi.State != MEM_COMMIT) {
            return 0;
        }
        p = stop;
    }
    return 1;
}

static LONG CALLBACK commit_fault(EXCEPTION_POINTERS* e)
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
        } else if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE) {
            out = EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return out;
}

void vm_track_install(void)
{
    AddVectoredExceptionHandler(1, commit_fault);
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
#else
static int commit(uint32_t first, uint32_t end) { (void)first; (void)end; return 1; }
void vm_track_install(void) {}
int vm_commit_host(const void* host, size_t size) { (void)host; (void)size; return 1; }
#endif

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
    int armed = 0;
    for (p = first; p < end; p++) armed |= arm(p);
    if (armed) FlushProcessWriteBuffers();
    ReleaseSRWLockExclusive(&s_lock);
}

uint32_t vm_track_drain(vm_page_fn changed, void* user)
{
    AcquireSRWLockExclusive(&s_lock);
    uint32_t n = 0;
    int armed = 0;
    for (uint32_t w = 0; w < sizeof vm_dirty_group / sizeof vm_dirty_group[0]; w++) {
        if (!__atomic_load_n(&vm_dirty_group[w], __ATOMIC_RELAXED)) continue;
        for (uint64_t bits = __atomic_exchange_n(&vm_dirty_group[w], 0, __ATOMIC_ACQ_REL); bits; bits &= bits - 1u) {
            const uint32_t first = (w << 12) | ((uint32_t)__builtin_ctzll(bits) << 6);
            for (uint32_t p = first; p < first + 64u; p++) {
                if (!__atomic_load_n(&vm_page_dirty[p], __ATOMIC_RELAXED) || !__atomic_exchange_n(&vm_page_dirty[p], 0, __ATOMIC_ACQ_REL)) continue;
                s_dirty[n++] = p;
                armed |= arm(p);
            }
        }
    }
    if (armed) FlushProcessWriteBuffers();
    for (uint32_t i = 0; i < n; i++) {
        changed(user, s_dirty[i]);
        if (s_pin[s_dirty[i]]) vm_page_mark_dirty(s_dirty[i]);
    }
    ReleaseSRWLockExclusive(&s_lock);
    return n;
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
        __atomic_fetch_and(&ppu_slow_page[p], (unsigned char)~VM_PAGE_TRACKED, __ATOMIC_ACQ_REL);
        vm_page_mark_dirty(p);
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
        if (s_tracked[p]) vm_page_mark_dirty(p);
    }
    ReleaseSRWLockExclusive(&s_lock);
}
