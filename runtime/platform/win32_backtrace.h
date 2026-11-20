#ifndef PS3RECOMP_WIN32_BACKTRACE_H
#define PS3RECOMP_WIN32_BACKTRACE_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#else

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GLIBC__) || defined(__APPLE__)
#  include <execinfo.h>
#  define PS3_HAVE_BACKTRACE 1
#else
#  define PS3_HAVE_BACKTRACE 0
#endif
#include <dlfcn.h>
#if defined(__GLIBC__) && !defined(__USE_GNU)
#  error "win32_backtrace.h needs _GNU_SOURCE defined before the first #include on glibc (dladdr's Dl_info)"
#endif

#ifndef PS3_WIN32_TYPES_USHORT
#define PS3_WIN32_TYPES_USHORT
typedef unsigned short USHORT;
#endif
typedef void*       HMODULE;
typedef const char* LPCSTR;
typedef const char* PCSTR;
typedef char*       LPSTR;
typedef char        CHAR;
typedef wchar_t*       LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef int         BOOL;
typedef uint32_t    DWORD;
typedef uint32_t    ULONG;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef void*       HANDLE;

#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS       0x00000004

static inline void* ps3__image_base_of(const void* addr)
{
    Dl_info info;
    if (addr && dladdr(addr, &info) && info.dli_fbase)
        return info.dli_fbase;
    return NULL;
}

static inline HMODULE GetModuleHandleA(LPCSTR name)
{
    (void)name;
    return (HMODULE)ps3__image_base_of((const void*)(uintptr_t)&ps3__image_base_of);
}

static inline int GetModuleHandleExA(unsigned long flags, LPCSTR addr,
                                     HMODULE* out)
{
    (void)flags;
    if (!out) return 0;
    *out = (HMODULE)ps3__image_base_of((const void*)addr);
    return *out != NULL;
}

static inline HMODULE GetModuleHandleW(LPCWSTR name)
{
    (void)name;
    return (HMODULE)ps3__image_base_of((const void*)(uintptr_t)&ps3__image_base_of);
}

static inline int GetModuleHandleExW(unsigned long flags, LPCWSTR addr,
                                     HMODULE* out)
{
    (void)flags;
    if (!out) return 0;
    *out = (HMODULE)ps3__image_base_of((const void*)addr);
    return *out != NULL;
}

static inline USHORT RtlCaptureStackBackTrace(unsigned long skip,
                                             unsigned long count,
                                             void** frames,
                                             unsigned long* hash)
{
    if (hash) *hash = 0;
    if (!frames || count == 0) return 0;
#if PS3_HAVE_BACKTRACE
    enum { PS3_BT_CAP = 160 };
    void* raw[PS3_BT_CAP];
    unsigned long want = skip + count + 1;
    if (want > PS3_BT_CAP) want = PS3_BT_CAP;

    int got = backtrace(raw, (int)want);
    if (got <= 0) return 0;

    unsigned long first = skip + 1;
    if (first >= (unsigned long)got) return 0;

    unsigned long n = (unsigned long)got - first;
    if (n > count) n = count;
    for (unsigned long i = 0; i < n; i++) frames[i] = raw[first + i];
    return (USHORT)n;
#else
    (void)skip; (void)count;
    return 0;
#endif
}

typedef struct {
    ULONG   SizeOfStruct;
    ULONG   TypeIndex;
    ULONG64 Reserved[2];
    ULONG   Index;
    ULONG   Size;
    ULONG64 ModBase;
    ULONG   Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG   Register;
    ULONG   Scope;
    ULONG   Tag;
    ULONG   NameLen;
    ULONG   MaxNameLen;
    CHAR    Name[1];
} SYMBOL_INFO;
typedef SYMBOL_INFO* PSYMBOL_INFO;

static inline BOOL SymInitialize(HANDLE process, PCSTR search_path, BOOL invade)
{ (void)process; (void)search_path; (void)invade; return 1; }
static inline BOOL SymCleanup(HANDLE process) { (void)process; return 1; }

static inline BOOL SymFromAddr(HANDLE process, DWORD64 address,
                               DWORD64* displacement, PSYMBOL_INFO symbol)
{
    (void)process;
    if (displacement) *displacement = 0;
    if (!symbol) return 0;

    Dl_info info;
    if (!dladdr((const void*)(uintptr_t)address, &info) || !info.dli_sname || !info.dli_saddr)
        return 0;

    if (displacement)
        *displacement = (DWORD64)((uintptr_t)address - (uintptr_t)info.dli_saddr);
    symbol->Address = (ULONG64)(uintptr_t)info.dli_saddr;
    symbol->Value   = symbol->Address;
    symbol->ModBase = (ULONG64)(uintptr_t)info.dli_fbase;
    symbol->Size    = 0;
    symbol->Flags   = 0;

    ULONG cap = symbol->MaxNameLen ? symbol->MaxNameLen : 1u;
    size_t n = strlen(info.dli_sname);
    if (n > (size_t)(cap - 1)) n = (size_t)(cap - 1);
    memcpy(symbol->Name, info.dli_sname, n);
    symbol->Name[n] = '\0';
    symbol->NameLen = (ULONG)n;
    return 1;
}

static inline DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size)
{
    if (!buffer || size == 0) return 0;
    buffer[0] = '\0';
    const void* probe = module ? (const void*)module
                              : (const void*)(uintptr_t)&ps3__image_base_of;
    Dl_info info;
    if (!dladdr(probe, &info) || !info.dli_fname) return 0;
    size_t n = strlen(info.dli_fname);
    if (n > (size_t)(size - 1)) n = (size_t)(size - 1);
    memcpy(buffer, info.dli_fname, n);
    buffer[n] = '\0';
    return (DWORD)n;
}

static inline DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size)
{
    if (!buffer || size == 0) return 0;
    buffer[0] = L'\0';
    const void* probe = module ? (const void*)module
                              : (const void*)(uintptr_t)&ps3__image_base_of;
    Dl_info info;
    if (!dladdr(probe, &info) || !info.dli_fname) return 0;
    size_t n = strlen(info.dli_fname);
    if (n > (size_t)(size - 1)) n = (size_t)(size - 1);
    for (size_t i = 0; i < n; i++)
        buffer[i] = (wchar_t)(unsigned char)info.dli_fname[i];
    buffer[n] = L'\0';
    return (DWORD)n;
}

#endif
#endif
