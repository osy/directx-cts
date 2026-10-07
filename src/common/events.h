/*
 * The event stream the runner reads back: one JSON object per line in the file named
 * by DXCTS_EVENTS.  With the variable unset there is no stream and every writer is
 * inert.
 *
 * Shared because two shims write it - the WTTLog proxy per case, and the d3d11 shim
 * once at process detach - and both must agree on the sharing mode, or one of them
 * fails to open the file the other holds.  Opened for atomic append so records from a
 * test's helper processes interleave whole lines rather than fragments; each record
 * carries its own pid so the runner can tell them apart.
 *
 * Kernel32 only, no CRT: one caller runs inside DllMain, where the CRT of another
 * module may already have been torn down.
 */

#ifndef DXCTS_EVENTS_H
#define DXCTS_EVENTS_H

#include <windows.h>

static inline HANDLE dxcts_events_open(void)
{
    WCHAR path[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"DXCTS_EVENTS", path, MAX_PATH);

    if (!len || len >= MAX_PATH)
        return INVALID_HANDLE_VALUE;
    return CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static inline void dxcts_events_write(HANDLE fh, const char *bytes, DWORD len)
{
    DWORD written;

    if (fh != INVALID_HANDLE_VALUE && len)
        WriteFile(fh, bytes, len, &written, NULL);
}

/* Decimal, appended at *pos.  The CRT's formatters are not reachable from DllMain and
 * wsprintf lives in user32, which is no better. */
static inline void dxcts_events_append_ulong(char *out, size_t cap, size_t *pos,
                                             unsigned long value)
{
    char digits[24];
    int n = 0;

    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n > 0 && *pos + 1 < cap)
        out[(*pos)++] = digits[--n];
    out[*pos] = 0;
}

static inline void dxcts_events_append_str(char *out, size_t cap, size_t *pos,
                                           const char *text)
{
    while (*text && *pos + 1 < cap)
        out[(*pos)++] = *text++;
    out[*pos] = 0;
}

#endif
