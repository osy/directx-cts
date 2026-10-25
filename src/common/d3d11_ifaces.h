/*
 * The revisions of the D3D11 device and context.  Each is one object behind one
 * vtable, so a wrapper answers for every revision the real object implements, and the
 * widest of them decides how wide the wrapper's vtable is.
 */

#ifndef DXCTS_D3D11_IFACES_H
#define DXCTS_D3D11_IFACES_H

#include <d3d11_4.h>

#include "comwrap.h"

static const dxcts_iface dxcts_d3d11_devices[] = {
    { &IID_ID3D11Device,  DXCTS_WIDTH(ID3D11DeviceVtbl) },
    { &IID_ID3D11Device1, DXCTS_WIDTH(ID3D11Device1Vtbl) },
    { &IID_ID3D11Device2, DXCTS_WIDTH(ID3D11Device2Vtbl) },
    { &IID_ID3D11Device3, DXCTS_WIDTH(ID3D11Device3Vtbl) },
    { &IID_ID3D11Device4, DXCTS_WIDTH(ID3D11Device4Vtbl) },
    { &IID_ID3D11Device5, DXCTS_WIDTH(ID3D11Device5Vtbl) },
};

static const dxcts_iface dxcts_d3d11_contexts[] = {
    { &IID_ID3D11DeviceContext,  DXCTS_WIDTH(ID3D11DeviceContextVtbl) },
    { &IID_ID3D11DeviceContext1, DXCTS_WIDTH(ID3D11DeviceContext1Vtbl) },
    { &IID_ID3D11DeviceContext2, DXCTS_WIDTH(ID3D11DeviceContext2Vtbl) },
    { &IID_ID3D11DeviceContext3, DXCTS_WIDTH(ID3D11DeviceContext3Vtbl) },
    { &IID_ID3D11DeviceContext4, DXCTS_WIDTH(ID3D11DeviceContext4Vtbl) },
};

#endif
