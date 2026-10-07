/*
 * The RPC entry points Wine's rpcrt4 lacks.
 *
 * TE.Common.dll and TE.Model.dll import Conduit.Broker.dll statically, so TAEF does
 * not start unless Conduit loads.  Conduit then creates a binding to its own broker
 * during startup, so RpcBindingCreateW has to work rather than refuse: it is
 * rebuilt here on the string-binding API Wine does implement.  The server-side
 * notification calls remain refusals - they belong to the remote-endpoint transport,
 * which local execution never drives.
 */

#include <windows.h>

#include "../../common/shim.h"

/* Not exported by name here: the .def maps the real names onto these. */
#define RPC_API WINAPI

#ifndef RPC_S_CANNOT_SUPPORT
#define RPC_S_CANNOT_SUPPORT 1764L
#endif

/* Wine binds lazily on first call, so there is nothing to do here. */
long RPC_API dxcts_RpcBindingBind(void *async, void *binding, void *iface)
{
    (void)async; (void)binding; (void)iface;
    return RPC_S_OK;
}

/* RPC_BINDING_HANDLE_TEMPLATE_V1_W.  Only version 1 is defined, and a later one would
 * lay its fields out differently, so an unrecognised version is refused rather than
 * read as if it were this one. */
#define BINDING_TEMPLATE_VERSION_V1 1

typedef struct
{
    unsigned long Version;
    unsigned long Flags;
    unsigned long ProtocolSequence;
    unsigned short *NetworkAddress;
    unsigned short *StringEndpoint;
    unsigned short *Reserved;
    UUID ObjectUuid;
} binding_template_v1;

/* rpcdce.h's RPC_PROTSEQ_* constants, which the template gives numerically. */
#ifndef RPC_PROTSEQ_TCP
#define RPC_PROTSEQ_TCP  0x1
#define RPC_PROTSEQ_NMP  0x2
#define RPC_PROTSEQ_LRPC 0x3
#define RPC_PROTSEQ_HTTP 0x4
#endif

static const WCHAR *protseq_name(unsigned long protseq)
{
    switch (protseq)
    {
    case RPC_PROTSEQ_TCP:  return L"ncacn_ip_tcp";
    case RPC_PROTSEQ_NMP:  return L"ncacn_np";
    case RPC_PROTSEQ_LRPC: return L"ncalrpc";
    case RPC_PROTSEQ_HTTP: return L"ncacn_http";
    default: return NULL;
    }
}

long RPC_API dxcts_RpcBindingCreateW(void *template_, void *security, void *options,
                                     void **binding)
{
    binding_template_v1 *tmpl = template_;
    const WCHAR *protseq;
    WCHAR *string_binding = NULL;
    RPC_STATUS status;

    (void)security; (void)options;
    if (binding)
        *binding = NULL;
    if (!tmpl || !binding)
        return RPC_S_INVALID_ARG;
    if (tmpl->Version != BINDING_TEMPLATE_VERSION_V1)
        return RPC_S_INVALID_ARG;

    protseq = protseq_name(tmpl->ProtocolSequence);
    if (!protseq)
        return RPC_S_PROTSEQ_NOT_SUPPORTED;

    status = RpcStringBindingComposeW(NULL, (RPC_WSTR)protseq,
                                      (RPC_WSTR)tmpl->NetworkAddress,
                                      (RPC_WSTR)tmpl->StringEndpoint, NULL,
                                      &string_binding);
    if (status != RPC_S_OK)
        return status;

    status = RpcBindingFromStringBindingW(string_binding,
                                          (RPC_BINDING_HANDLE *)binding);
    RpcStringFreeW(&string_binding);
    if (status != RPC_S_OK)
        return status;

    /* The template carries the object UUID separately from the string binding. */
    RpcBindingSetObject(*(RPC_BINDING_HANDLE *)binding, &tmpl->ObjectUuid);
    return RPC_S_OK;
}

long RPC_API dxcts_RpcServerInqBindingHandle(void **binding)
{
    if (binding)
        *binding = NULL;
    return RPC_S_CANNOT_SUPPORT;
}

long RPC_API dxcts_RpcServerSubscribeForNotification(void *iface, unsigned int notification,
                                               unsigned int type, void *handle)
{
    (void)iface; (void)notification; (void)type; (void)handle;
    return RPC_S_CANNOT_SUPPORT;
}

long RPC_API dxcts_RpcServerUnsubscribeForNotification(void *iface, unsigned int notification,
                                                 unsigned int *count)
{
    (void)iface; (void)notification;
    if (count)
        *count = 0;
    return RPC_S_CANNOT_SUPPORT;
}

void RPC_API dxcts_Ndr64AsyncServerCallAll(void *request)
{
    (void)request;
}

DXCTS_SHIM_DLLMAIN()
