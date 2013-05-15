/* ****************************************************************************** *\

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2011 - 2012 Intel Corporation. All Rights Reserved.

\* ****************************************************************************** */

#pragma once

#include "hw_device.h"

#pragma warning(disable : 4201)
#include <d3d9.h>
#include <dxva2api.h>
#include <dxva.h>
#include <windows.h>

enum {
    MFX_HANDLE_GFXS3DCONTROL = 0x100 /* A handle to the IGFXS3DControl instance */
}; //mfxHandleType

#define OVERLAY_BACKBUFFER_FORMAT D3DFMT_X8R8G8B8
#define VIDEO_MAIN_FORMAT D3DFMT_YUY2

class IGFXS3DControl;

/** Direct3D 9 device implementation.
@note Can be initilized for only 1 or two 2 views. Handle to 
MFX_HANDLE_GFXS3DCONTROL must be set prior if initializing for 2 views.

@note Device always set D3DPRESENT_PARAMETERS::Windowed to TRUE.
*/
class CD3D9Device : public CHWDevice
{
public:
    CD3D9Device(IDirect3DDeviceManager9* pDeviceManager9);
    virtual ~CD3D9Device();

    virtual mfxStatus Init(int nAdapterNum);
    virtual mfxStatus Reset();
    virtual mfxHDL    GetHandle(mfxHandleType type);
    virtual void      Close();
   
protected:
    virtual mfxStatus FillD3DPP(mfxHDL hWindow, D3DPRESENT_PARAMETERS &D3DPP);
    mfxStatus InitFromScratch(int nAdapterNum);
    mfxStatus InitFromRenderer(int nAdapterNum);

private:
    IDirect3D9Ex*               m_pD3D9;
    IDirect3DDevice9*           m_pD3DD9;
    IDirect3DDeviceManager9*    m_pDeviceManager9;
    D3DPRESENT_PARAMETERS       m_D3DPP;
    UINT                        m_ResetToken;
};

