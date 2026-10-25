/*
 * Wrapping a COM object whose vtable cannot be written.
 *
 * The host D3D implementation's objects and their vtables live in the graphics
 * backend's own address space, which Wine reports as unmapped (VirtualQuery:
 * MEM_FREE) and VirtualProtect refuses with ERROR_INVALID_PARAMETER.  Reads work, so
 * a shim that has to intervene in one method builds a whole new vtable instead:
 * overridden slots are ordinary C functions, and every other slot is a thunk that
 * swaps `this` for the real object and tail-jumps to that slot of the real object's
 * vtable.  That keeps signatures and calling conventions exact without naming a
 * hundred method prototypes.  The thunks are compiled in, one per slot index, because
 * a test process may prohibit dynamic code (see comwrap.c).
 *
 * The wrapper holds exactly one reference on the real object for its whole life, so
 * AddRef and Release count only the wrapper and the real object is released once.
 */

#ifndef DXCTS_COMWRAP_H
#define DXCTS_COMWRAP_H

#include <windows.h>
#include <stddef.h>

/* Vtable geometry taken from the headers rather than counted by hand. */
#define DXCTS_SLOT(vtbl, method) ((int)(offsetof(vtbl, method) / sizeof(void *)))
#define DXCTS_WIDTH(vtbl) ((int)(sizeof(vtbl) / sizeof(void *)))
#define DXCTS_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/*
 * Override one slot, but only where the object is wide enough to have it: the later
 * interface revisions' methods do not exist on an object the implementation never
 * extended, and writing past the wrapper's vtable would corrupt the allocation.
 */
#define DXCTS_INSTALL(w, vtbl, method, fn)                                    \
    do {                                                                      \
        int slot_ = DXCTS_SLOT(vtbl, method);                                 \
                                                                              \
        if (slot_ < (w)->slots)                                               \
            (w)->vtable[slot_] = (void *)(fn);                                \
    } while (0)

/* Must stay the first member of any wrapper: the thunks reach `real` at a fixed
 * offset, and a COM object is a pointer to its vtable. */
typedef struct
{
    void **vtable;
    void *real;
    LONG refs;
    int slots;              /* how wide the vtable is, so QueryInterface can decline
                             * an interface the real object does not implement */
} dxcts_wrapper;

typedef HRESULT (WINAPI *dxcts_qi_fn)(void *self, const GUID *iid, void **out);
typedef ULONG (WINAPI *dxcts_ref_fn)(void *self);

/* Vtable width of one interface, for the tables below. */
typedef struct
{
    const GUID *iid;
    int slots;
} dxcts_iface;

/* Release and AddRef of any COM object, through slots 2 and 1; NULL allowed. */
static inline void dxcts_release(void *obj)
{
    if (obj)
        ((dxcts_ref_fn)(*(void ***)obj)[2])(obj);
}

static inline void dxcts_addref(void *obj)
{
    if (obj)
        ((dxcts_ref_fn)(*(void ***)obj)[1])(obj);
}

/* 0 on an architecture with no thunk, where a shim must pass the object through. */
int dxcts_wrap_supported(void);

/*
 * A wrapper of `size` bytes (>= sizeof(dxcts_wrapper)) over `object`, with a vtable
 * `slots` wide.  Slots 0-2 are left as thunks; the caller installs its own IUnknown
 * and whichever methods it overrides.  Takes over the caller's reference on `object`.
 * NULL if the wrapper cannot be built, in which case the object is untouched.
 *
 * `slots` must not exceed the real object's vtable: filling slots the object does not
 * have reads whatever follows it in memory, and calling one of those jumps to a
 * garbage address.
 */
dxcts_wrapper *dxcts_wrap(void *object, int slots, size_t size);

ULONG WINAPI dxcts_wrapper_addref(void *self);
ULONG WINAPI dxcts_wrapper_release(void *self);

/* Frees a wrapper without releasing the real object, for a Release override that
 * keeps the object alive (a parked device). */
void dxcts_wrapper_free(dxcts_wrapper *w);

static inline void *dxcts_real(void *self)
{
    return ((dxcts_wrapper *)self)->real;
}

static inline void **dxcts_real_vtable(void *self)
{
    return *(void ***)((dxcts_wrapper *)self)->real;
}

/* The width recorded for `iid`, or 0 when it is not one of these interfaces. */
int dxcts_iface_slots(const GUID *iid, const dxcts_iface *set, size_t count);

/*
 * Whether the wrapper should answer `iid` itself.  It answers the revisions the real
 * object implements and no others: a wrapper that claimed the newest revision over an
 * object that stops at the base would hand out a vtable whose later slots do not
 * exist.  The wrapper's own width already records what the object implements, so no
 * probing is needed.
 */
int dxcts_wrapper_answers(const dxcts_wrapper *w, const GUID *iid,
                          const dxcts_iface *set, size_t count);

/*
 * The width to build a wrapper to.
 *
 * Sizing it to the interface the caller asked for is not enough: these interfaces form
 * inheritance chains, so an object handed out as IDXGIAdapter1 still carries the full
 * IDXGIAdapter4 vtable, and whoever receives it may call a method past IDXGIAdapter1's
 * end without querying for anything first.  Asking the real object what it implements
 * gives the true length.
 *
 * The answer is a property of the implementation rather than of the object, so it is
 * asked once through *cached.  Probing per call is also not free: a QueryInterface
 * storm against an adapter while a device is live wedges this host.
 */
int dxcts_widest_slots(void *object, const dxcts_iface *set, size_t count, int *cached);

/*
 * A standalone object that answers `iids` and returns `result` (S_OK or a failure)
 * from every method past IUnknown, writing no output parameter.  Used where a test
 * queries an undocumented interface and does not check the result.  The vtable is
 * deliberately wider than any interface revision adds, since over-allocating our own
 * memory is harmless where over-reading a real vtable is not.
 *
 * NULL on x86, where it cannot be built: COM there is __stdcall, the callee pops the
 * caller's arguments, and a generic slot cannot know how many an undocumented method
 * takes.  A caller that would have received a stub gets E_NOINTERFACE and its own
 * unsupported path instead, which is what it would see with no shim at all - where
 * popping the wrong count would corrupt its stack.
 */
void *dxcts_stub_object(const GUID *const *iids, size_t iid_count, HRESULT result);

#endif
