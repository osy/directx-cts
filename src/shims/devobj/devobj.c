/*
 * DEVOBJ.dll - absent from Wine, imported by 46 HLK test executables.
 *
 * These six entry points are the device-enumeration half of setupapi under another
 * name, so this forwards to setupapi rather than faking devices: the device interface
 * path the tests receive is then the platform's own, which its own
 * D3DKMTOpenAdapterFromDeviceName accepts.  Synthesising a path instead forces the
 * D3DKMT shim to translate it back.
 *
 * Forwarding is permanent, since the module simply does not exist under Wine.  The
 * synthetic adapter below is the one workaround, and it has a knob.
 */

#include <windows.h>
#include <setupapi.h>

#include "../../common/knob.h"
#include "../../common/shim.h"

#define DEVOBJ_API __declspec(dllexport) WINAPI

/*
 * Wine enumerates no display adapter interface, so a synthetic one is presented when
 * the real enumeration comes up empty and the D3DKMT shim maps its path onto the
 * primary GDI display.  Off on Windows, where the real enumeration answers.
 */
DXCTS_KNOB(synthetic_adapter, "DXCTS_DEVOBJ_SYNTHETIC_ADAPTER", 1, 0)

/* GUID_DISPLAY_DEVICE_ARRIVAL (ntddvdeo.h): the interface class WDDM registers a
 * display adapter under, and the one the HLK's classification chain enumerates. */
static const GUID k_display_device_arrival =
    { 0x1ca05180, 0xa699, 0x450a, { 0x9a, 0x0c, 0xde, 0x4f, 0xbe, 0x3d, 0xdd, 0x89 } };

/* A device interface path in the form the enumerator produces, naming that class. */
static const WCHAR kSyntheticPath[] =
    L"\\\\?\\PCI#VEN_0000&DEV_0000#DXCTS#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}";

/* "dxc1": distinguishes a synthetic list from a real HDEVINFO without allocating. */
#define FAKE_HDEVINFO ((HDEVINFO)(ULONG_PTR)0x64786331)

HDEVINFO DEVOBJ_API DevObjGetClassDevs(const GUID *class_guid, PCWSTR enumerator,
                                       HWND parent, DWORD flags)
{
    HDEVINFO set = SetupDiGetClassDevsW(class_guid, enumerator, parent, flags);

    /* Only the display adapter class is synthesised, because that is the only class
     * the path below names.  Answering for any other class would hand its caller one
     * bogus display adapter in place of the devices it asked about. */
    if (!synthetic_adapter() || !class_guid
        || !IsEqualGUID(class_guid, &k_display_device_arrival))
        return set;
    if (set == INVALID_HANDLE_VALUE)
        return FAKE_HDEVINFO;
    /* An empty set classifies as "no hardware adapter" just as a failure does. */
    {
        SP_DEVICE_INTERFACE_DATA probe = { sizeof(probe) };
        if (!SetupDiEnumDeviceInterfaces(set, NULL, class_guid, 0, &probe))
        {
            SetupDiDestroyDeviceInfoList(set);
            return FAKE_HDEVINFO;
        }
    }
    return set;
}

HDEVINFO DEVOBJ_API DevObjCreateDeviceInfoList(const GUID *class_guid, HWND parent)
{
    return SetupDiCreateDeviceInfoList(class_guid, parent);
}

BOOL DEVOBJ_API DevObjDestroyDeviceInfoList(HDEVINFO set)
{
    if (set == FAKE_HDEVINFO)
        return TRUE;
    return SetupDiDestroyDeviceInfoList(set);
}

BOOL DEVOBJ_API DevObjEnumDeviceInterfaces(HDEVINFO set, PSP_DEVINFO_DATA info,
                                           const GUID *iface_class, DWORD index,
                                           PSP_DEVICE_INTERFACE_DATA iface)
{
    if (set == FAKE_HDEVINFO)
    {
        if (index > 0)
        {
            SetLastError(ERROR_NO_MORE_ITEMS);
            return FALSE;
        }
        if (iface)
        {
            iface->cbSize = sizeof(*iface);
            if (iface_class)
                iface->InterfaceClassGuid = *iface_class;
            iface->Flags = SPINT_ACTIVE;
            iface->Reserved = 0;
        }
        return TRUE;
    }
    return SetupDiEnumDeviceInterfaces(set, info, iface_class, index, iface);
}

BOOL DEVOBJ_API DevObjGetDeviceInterfaceDetail(HDEVINFO set,
                                               PSP_DEVICE_INTERFACE_DATA iface,
                                               PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail,
                                               DWORD size, PDWORD required,
                                               PSP_DEVINFO_DATA info)
{
    if (set == FAKE_HDEVINFO)
    {
        DWORD need = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)
                             + sizeof(kSyntheticPath));
        if (required)
            *required = need;
        if (!detail || size < need)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memcpy(detail->DevicePath, kSyntheticPath, sizeof(kSyntheticPath));
        return TRUE;
    }
    return SetupDiGetDeviceInterfaceDetailW(set, iface, detail, size, required, info);
}

BOOL DEVOBJ_API DevObjGetDeviceInfoListDetail(HDEVINFO set,
                                              PSP_DEVINFO_LIST_DETAIL_DATA_W detail)
{
    if (set == FAKE_HDEVINFO)
    {
        if (!detail || detail->cbSize != sizeof(*detail))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        detail->RemoteMachineHandle = NULL;
        detail->RemoteMachineName[0] = 0;
        return TRUE;
    }
    return SetupDiGetDeviceInfoListDetailW(set, detail);
}

DXCTS_SHIM_DLLMAIN()
