#include <stddef.h>
#include <string.h>

#include "comwrap.h"

/*
 * The thunks are compiled into this module rather than generated at run time: the
 * HLK binaries opt into the dynamic-code prohibition (SetProcessMitigationPolicy), so
 * on Windows VirtualAlloc(PAGE_EXECUTE_READWRITE) fails in a test process and a
 * generated thunk could never have been built there.  One thunk per slot index, each
 * a naked function: `this` becomes the real object, and control jumps to that slot of
 * the real object's own vtable.  `this` is the first argument even for a method
 * returning a large struct: MSVC passes that hidden pointer after `this` on x64 and
 * x86, and in x8 on arm64.
 */
#include "comwrap_thunks.h"

#define STR_(x) #x
#define STR(x) STR_(x)

#if defined(__x86_64__) || defined(_M_X64)
/* mov rcx,[rcx+8] ; mov rax,[rcx] ; jmp [rax+8*i] */
#define THUNK(i)                                                              \
    __attribute__((naked)) static void thunk_##i(void)                        \
    {                                                                         \
        __asm__("movq 8(%rcx), %rcx\n\tmovq (%rcx), %rax\n\t"                 \
                "jmpq *(" STR(i) "*8)(%rax)");                                \
    }
#define THUNK_SUPPORTED 1
#elif defined(__i386__) || defined(_M_IX86)
/* COM on x86 is __stdcall with `this` as the first stack argument. */
#define THUNK(i)                                                              \
    __attribute__((naked)) static void thunk_##i(void)                        \
    {                                                                         \
        __asm__("movl 4(%esp), %eax\n\tmovl 4(%eax), %eax\n\t"                \
                "movl %eax, 4(%esp)\n\tmovl (%eax), %eax\n\t"                 \
                "jmpl *(" STR(i) "*4)(%eax)");                                \
    }
#define THUNK_SUPPORTED 1
#elif defined(__aarch64__) || defined(_M_ARM64)
/* ldr x0,[x0,#8] ; ldr x16,[x0] ; ldr x16,[x16,#8*i] ; br x16 */
#define THUNK(i)                                                              \
    __attribute__((naked)) static void thunk_##i(void)                        \
    {                                                                         \
        __asm__("ldr x0, [x0, #8]\n\tldr x16, [x0]\n\t"                       \
                "ldr x16, [x16, #(" STR(i) "*8)]\n\tbr x16");                 \
    }
#define THUNK_SUPPORTED 1
#else
#define THUNK_SUPPORTED 0
#endif

#if THUNK_SUPPORTED
DXCTS_THUNK_LIST(THUNK)
#define THUNK_ENTRY(i) (void *)thunk_##i,
static void *const k_thunks[DXCTS_THUNK_COUNT] = { DXCTS_THUNK_LIST(THUNK_ENTRY) };
#endif

/*
 * A stub slot is one C function standing in for a method whose arity is unknown.  That
 * is safe wherever the caller owns the argument area, and unsafe on x86, where
 * __stdcall makes the callee pop it: a one-parameter stub emits `ret 4` however many
 * bytes the caller pushed.
 */
#if defined(__i386__) || defined(_M_IX86)
#define STUB_SUPPORTED 0
#else
#define STUB_SUPPORTED 1
#endif

#if THUNK_SUPPORTED
/* The thunk reads `real` at this offset off `this`. */
_Static_assert(offsetof(dxcts_wrapper, real) == sizeof(void *),
               "the thunk's `this` fixup depends on this layout");

int dxcts_wrap_supported(void)
{
    return 1;
}

dxcts_wrapper *dxcts_wrap(void *object, int slots, size_t size)
{
    dxcts_wrapper *w;
    void **vtable;
    int i;

    if (slots <= 3 || slots > DXCTS_THUNK_COUNT || size < sizeof(*w))
        return NULL;
    vtable = HeapAlloc(GetProcessHeap(), 0, (size_t)slots * sizeof(void *));
    if (!vtable)
        return NULL;
    for (i = 0; i < slots; i++)
        vtable[i] = k_thunks[i];
    w = HeapAlloc(GetProcessHeap(), 0, size);
    if (!w)
    {
        HeapFree(GetProcessHeap(), 0, vtable);
        return NULL;
    }
    memset(w, 0, size);
    w->vtable = vtable;
    w->real = object;
    w->refs = 1;
    w->slots = slots;
    return w;
}
#else
int dxcts_wrap_supported(void)
{
    return 0;
}

dxcts_wrapper *dxcts_wrap(void *object, int slots, size_t size)
{
    (void)object; (void)slots; (void)size;
    return NULL;
}
#endif

void dxcts_wrapper_free(dxcts_wrapper *w)
{
    HeapFree(GetProcessHeap(), 0, w->vtable);
    HeapFree(GetProcessHeap(), 0, w);
}

ULONG WINAPI dxcts_wrapper_addref(void *self)
{
    return (ULONG)InterlockedIncrement(&((dxcts_wrapper *)self)->refs);
}

ULONG WINAPI dxcts_wrapper_release(void *self)
{
    dxcts_wrapper *w = self;
    LONG refs = InterlockedDecrement(&w->refs);

    if (refs == 0)
    {
        ((dxcts_ref_fn)dxcts_real_vtable(self)[2])(w->real);   /* slot 2 is Release */
        dxcts_wrapper_free(w);
    }
    return (ULONG)refs;
}

int dxcts_iface_slots(const GUID *iid, const dxcts_iface *set, size_t count)
{
    size_t i;

    for (i = 0; iid && i < count; i++)
        if (IsEqualGUID(iid, set[i].iid))
            return set[i].slots;
    return 0;
}

int dxcts_wrapper_answers(const dxcts_wrapper *w, const GUID *iid,
                          const dxcts_iface *set, size_t count)
{
    int width;

    if (IsEqualGUID(iid, &IID_IUnknown))
        return 1;
    width = dxcts_iface_slots(iid, set, count);
    return width && width <= w->slots;
}

int dxcts_widest_slots(void *object, const dxcts_iface *set, size_t count, int *cached)
{
    dxcts_qi_fn qi;
    int best = 0;
    size_t i;

    if (*cached)
        return *cached;
    qi = (dxcts_qi_fn)(*(void ***)object)[0];
    for (i = 0; i < count; i++)
    {
        void *queried = NULL;

        if (SUCCEEDED(qi(object, set[i].iid, &queried)) && queried)
        {
            dxcts_release(queried);
            if (set[i].slots > best)
                best = set[i].slots;
        }
    }
    *cached = best;
    return best;
}

/* --- stub object --- */

#if STUB_SUPPORTED

typedef struct
{
    void **vtable;
    LONG refs;
    const GUID *const *iids;
    size_t iid_count;
} stub_object;

/* Wider than any interface revision here adds past its base. */
#define STUB_SLOTS 32

static HRESULT WINAPI stub_qi(void *self, const GUID *iid, void **out)
{
    stub_object *o = self;
    size_t i;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!iid)
        return E_POINTER;
    for (i = 0; i < o->iid_count; i++)
        if (IsEqualGUID(iid, o->iids[i]))
            break;
    if (i == o->iid_count && !IsEqualGUID(iid, &IID_IUnknown))
        return E_NOINTERFACE;
    InterlockedIncrement(&o->refs);
    *out = o;
    return S_OK;
}

static ULONG WINAPI stub_addref(void *self)
{
    return (ULONG)InterlockedIncrement(&((stub_object *)self)->refs);
}

static ULONG WINAPI stub_release(void *self)
{
    stub_object *o = self;
    LONG refs = InterlockedDecrement(&o->refs);

    if (refs == 0)
    {
        void **vtable = o->vtable;

        HeapFree(GetProcessHeap(), 0, o);
        HeapFree(GetProcessHeap(), 0, vtable);
    }
    return (ULONG)refs;
}

/* One function per HRESULT the callers want, since a slot carries no argument
 * information to pass a value through. */
static HRESULT WINAPI stub_ok(void *self)
{
    (void)self;
    return S_OK;
}

static HRESULT WINAPI stub_fail(void *self)
{
    (void)self;
    return E_FAIL;
}

void *dxcts_stub_object(const GUID *const *iids, size_t iid_count, HRESULT result)
{
    stub_object *o = HeapAlloc(GetProcessHeap(), 0, sizeof(*o));
    void **vtable = HeapAlloc(GetProcessHeap(), 0, STUB_SLOTS * sizeof(void *));
    int i;

    if (!o || !vtable)
    {
        if (o) HeapFree(GetProcessHeap(), 0, o);
        if (vtable) HeapFree(GetProcessHeap(), 0, vtable);
        return NULL;
    }
    for (i = 0; i < STUB_SLOTS; i++)
        vtable[i] = result == S_OK ? (void *)stub_ok : (void *)stub_fail;
    vtable[0] = (void *)stub_qi;
    vtable[1] = (void *)stub_addref;
    vtable[2] = (void *)stub_release;
    o->vtable = vtable;
    o->refs = 1;
    o->iids = iids;
    o->iid_count = iid_count;
    return o;
}

#else

void *dxcts_stub_object(const GUID *const *iids, size_t iid_count, HRESULT result)
{
    (void)iids; (void)iid_count; (void)result;
    return NULL;
}

#endif
