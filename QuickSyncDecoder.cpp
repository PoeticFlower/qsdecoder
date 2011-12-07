/*
 * Copyright (c) 2011, INTEL CORPORATION
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation and/or
 * other materials provided with the distribution.
 * Neither the name of INTEL CORPORATION nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "stdafx.h"
#include "IQuickSyncDecoder.h"
#include "QuickSync_defs.h"
#include "QuickSyncUtils.h"
#include "QuickSyncDecoder.h"

#define MIN_REQUIRED_API_VER_MINOR 1
#define MIN_REQUIRED_API_VER_MAJOR 1

static int GetIntelAdapterId(IDirect3D9* pd3d)
{
    MSDK_CHECK_POINTER(pd3d, 0);

    unsigned adapterCount = (int)pd3d->GetAdapterCount();
    D3DADAPTER_IDENTIFIER9 adIdentifier;
    for (int i = adapterCount-1; i >= 0; --i)
    {
        HRESULT hr = pd3d->GetAdapterIdentifier(i, 0, &adIdentifier);
        if (SUCCEEDED(hr))
        {
            // Look for Intel's vendor ID (8086h)
            if (adIdentifier.VendorId == 0x8086)
                return i;
        }
    }

    return -1;
}

CQuickSyncDecoder::CQuickSyncDecoder(mfxStatus& sts) :
    m_pFrameSurfaces(NULL),
    m_pmfxDEC(0),
    m_nRequiredFramesNum(0),
    m_pFrameAllocator(NULL),
    m_mfxImpl(MFX_IMPL_UNSUPPORTED),
    m_pVideoParams(0),
    m_pRendererD2dDeviceManager(NULL),
    m_pD3dDeviceManager(NULL),
    m_pD3dDevice(NULL)
{
    MSDK_ZERO_VAR(m_AllocResponse);

    m_ApiVersion.Major = MIN_REQUIRED_API_VER_MAJOR;
    m_ApiVersion.Minor = MIN_REQUIRED_API_VER_MINOR;
    mfxIMPL impl = MFX_IMPL_AUTO_ANY;

    // Uncomment for SW emulation
    //impl = MFX_IMPL_SOFTWARE;
   
    sts = m_mfxVideoSession.Init(impl, &m_ApiVersion);
    if (MFX_ERR_NONE != sts)
        return;

    m_mfxVideoSession.QueryIMPL(&m_mfxImpl);
    m_mfxVideoSession.QueryVersion(&m_ApiVersion);

    m_bHwAcceleration = m_mfxImpl != MFX_IMPL_SOFTWARE;
    m_bUseD3DAlloc = m_bHwAcceleration;
    m_pmfxDEC = new MFXVideoDECODE(m_mfxVideoSession);
}

CQuickSyncDecoder::~CQuickSyncDecoder()
{
    if (m_pmfxDEC)
    {
        m_pmfxDEC->Close();
        delete m_pmfxDEC;
    }

    FreeFrameAllocator();
    delete m_pFrameAllocator;
    CloseD3D();
}

mfxFrameSurface1* CQuickSyncDecoder::FindFreeSurface()
{
    MSDK_CHECK_POINTER(m_pFrameSurfaces, NULL);

    //0.1 seconds cycle
    for (int tries = 0; tries < 100; ++tries)
    {
        for (mfxU8 i = 0; i < m_nRequiredFramesNum; ++i)
        {
            if (0 == m_pFrameSurfaces[i].Data.Locked)
            {
                // find if surface is in output queue
                bool bInQueue = false;
                for (size_t j = 0; j <  m_OutputSurfaceQueue.size(); ++j)
                {
                    if (m_OutputSurfaceQueue[j] == &m_pFrameSurfaces[i])
                    {
                        bInQueue = true;
                        break;
                    }
                }

                // found free surface :)
                if (!bInQueue)
                    return &m_pFrameSurfaces[i];
            }
        }

        MSDK_TRACE("QSDcoder: FindFreeSurface - all surfaces are in use, retrying in 1ms\n");
        Sleep(1);
    }

    return NULL;
}

mfxStatus CQuickSyncDecoder::InitFrameAllocator(mfxVideoParam* pVideoParams, mfxU32 nPitch)
{
    // Already initialized
    if (m_pFrameSurfaces)
    {
        return MFX_ERR_NONE;
    }

    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);
    mfxStatus sts = MFX_ERR_NONE;

    // Initialize frame allocator (if needed)
    sts = CreateAllocator();
    MSDK_CHECK_NOT_EQUAL(sts, MFX_ERR_NONE, sts);
    MSDK_CHECK_NOT_EQUAL(sts, MFX_ERR_NONE, MFX_ERR_MEMORY_ALLOC);
   
    // Find how many surfaces are needed
    mfxFrameAllocRequest allocRequest;
    MSDK_ZERO_VAR(allocRequest);
    sts = m_pmfxDEC->QueryIOSurf(pVideoParams, &allocRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    allocRequest.NumFrameSuggested = (mfxU16)m_Config.nOutputQueueLength + allocRequest.NumFrameSuggested + 1;
    allocRequest.NumFrameMin = allocRequest.NumFrameSuggested;
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

    // decide memory type
    allocRequest.Type = MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE;
    allocRequest.Type |= (m_bUseD3DAlloc) ?
       MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET : MFX_MEMTYPE_SYSTEM_MEMORY;

    memcpy(&allocRequest.Info, &pVideoParams->mfx.FrameInfo, sizeof(mfxFrameInfo));

    // allocate frames with H aligned at 32 for both progressive and interlaced content
    allocRequest.Info.Height = MSDK_ALIGN32(allocRequest.Info.Height); 
    allocRequest.Info.Width = (mfxU16)nPitch;

    // perform allocation call. result is saved in m_AllocResponse
    sts = m_pFrameAllocator->Alloc(m_pFrameAllocator->pthis, &allocRequest, &m_AllocResponse);
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

    m_nRequiredFramesNum = m_AllocResponse.NumFrameActual;
    ASSERT(m_nRequiredFramesNum == allocRequest.NumFrameSuggested);
    m_pFrameSurfaces = new mfxFrameSurface1[m_nRequiredFramesNum];
    MSDK_CHECK_POINTER(m_pFrameSurfaces, MFX_ERR_MEMORY_ALLOC);
    MSDK_ZERO_MEMORY(m_pFrameSurfaces, sizeof(mfxFrameSurface1) * m_nRequiredFramesNum);

    // alloc decoder work & output surfaces
    for (mfxU32 i = 0; i < m_nRequiredFramesNum; ++i)
    {
        // copy frame info
        memcpy(&(m_pFrameSurfaces[i].Info), &pVideoParams->mfx.FrameInfo, sizeof(mfxFrameInfo));

        // save pointer to allocator specific surface object (mid)
        m_pFrameSurfaces[i].Data.MemId  = m_AllocResponse.mids[i];
        m_pFrameSurfaces[i].Data.Pitch  = (mfxU16)nPitch;
    }

    return sts;
}

mfxStatus CQuickSyncDecoder::FreeFrameAllocator()
{
    mfxStatus sts = MFX_ERR_NONE;
    if (m_pFrameAllocator)
    {
        sts = m_pFrameAllocator->Free(m_pFrameAllocator->pthis, &m_AllocResponse);
        MSDK_ZERO_VAR(m_AllocResponse);
    }

    m_nRequiredFramesNum = 0;
    MSDK_SAFE_DELETE_ARRAY(m_pFrameSurfaces);
    return MFX_ERR_NONE;
}

mfxStatus CQuickSyncDecoder::InternalReset(mfxVideoParam* pVideoParams, mfxU32 nPitch, bool bInited)
{
    MSDK_CHECK_POINTER(pVideoParams, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);
    
    mfxStatus sts = MFX_ERR_NONE;
    m_pVideoParams = pVideoParams;
    if (NULL == m_pFrameAllocator)
    {
        bInited = false;
    }

    // reset decoder
    if (bInited)
    {
        // flush - VC1 decoder needs this or surfaces will remain in use (bug workaround)
        // SW decoder doesn't like this :(
        if (MFX_IMPL_SOFTWARE != QueryIMPL())
        {
            do
            {
                mfxFrameSurface1* pSurf = NULL;
                sts = Decode(NULL, pSurf);
            } while (sts == MFX_ERR_NONE);
        }

        sts = m_pmfxDEC->Reset(pVideoParams);
        // need to reset the frame allocator
        if (MFX_ERR_NONE != sts)
        {
            m_pmfxDEC->Close();
            FreeFrameAllocator();
            bInited = false;
        }
    }

    // Full init
    if (!bInited)
    {
        // Check if video format is supported by HW acceleration
        if (MFX_WRN_PARTIAL_ACCELERATION == CheckHwAcceleration(pVideoParams))
        {
            // Change allocator to system memory
            //m_bUseD3DAlloc = false; // causes crashes!
            m_bHwAcceleration = false; // this is just for knowledge
        }

        // Setup allocator - will initialize D3D if needed
        sts = InitFrameAllocator(pVideoParams, nPitch);
        MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

        // Init MSDK decoder
        sts = m_pmfxDEC->Init(pVideoParams);
    }

    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);

    return sts;
}

mfxStatus CQuickSyncDecoder::CheckHwAcceleration(mfxVideoParam* pVideoParams)
{
    MSDK_CHECK_POINTER(pVideoParams, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);

    mfxFrameAllocRequest allocRequest;
    MSDK_ZERO_VAR(allocRequest);

    // If we are already using SW decoder then no need for further checks...
    mfxIMPL impl = QueryIMPL();
    if (MFX_IMPL_SOFTWARE == impl)
        return MFX_ERR_NONE;

    mfxU16 ioPaternSave = pVideoParams->IOPattern;
    pVideoParams->IOPattern = (mfxU16)MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    mfxStatus sts = m_pmfxDEC->QueryIOSurf(pVideoParams, &allocRequest);
    pVideoParams->IOPattern = ioPaternSave;
    return sts;
}

mfxStatus CQuickSyncDecoder::GetVideoParams(mfxVideoParam* pVideoParams)
{
    MSDK_CHECK_POINTER(pVideoParams, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);

    return m_pmfxDEC->GetVideoParam(pVideoParams);
}

mfxStatus CQuickSyncDecoder::Decode(mfxBitstream* pBS, mfxFrameSurface1*& pFrameSurface)
{
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);    

    mfxStatus sts = MFX_ERR_NONE;
    mfxSyncPoint syncp;
    mfxFrameSurface1* pWorkSurface = FindFreeSurface();
    MSDK_CHECK_POINTER(pWorkSurface, MFX_ERR_NOT_ENOUGH_BUFFER);

    do
    {
        sts = m_pmfxDEC->DecodeFrameAsync(pBS, pWorkSurface, &pFrameSurface, &syncp);
        // need 1 more work surface
        if (MFX_ERR_MORE_SURFACE == sts)
        {
            pWorkSurface = FindFreeSurface();
            MSDK_CHECK_POINTER(pWorkSurface, MFX_ERR_NOT_ENOUGH_BUFFER);
        }
        else if (MFX_WRN_DEVICE_BUSY == sts)
        {
            Sleep(1);
        }
    } while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_MORE_SURFACE == sts);

    // Wait for the asynch decoding to finish
    if (MFX_ERR_NONE == sts) 
    {
        sts = m_mfxVideoSession.SyncOperation(syncp, 0xFFFF);
    }

    return sts;
}

mfxStatus CQuickSyncDecoder::InitD3D()
{
    if (!m_bUseD3DAlloc)
    {
        return MFX_ERR_NONE;
    }

    // check if the d3d device is functional:
    if (m_pD3dDeviceManager != NULL)
    {
        HRESULT hr = m_pD3dDevice->TestCooperativeLevel();
        if (FAILED(hr))
        {
            CloseD3D();
        }
        else
        {
            return MFX_ERR_NONE;
        }
    }

    // Create Direct3D
    CComPtr<IDirect3D9> pd3d = Direct3DCreate9(D3D_SDK_VERSION);
    UINT resetToken;

    if (!pd3d)
    {
        return MFX_ERR_NULL_PTR;
    }

    static const POINT point = {0, 0};
    const HWND hWnd = GetForegroundWindow();
    if (hWnd == NULL)
    {
        MSDK_TRACE("QSDcoder: failed to create HWND.\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    D3DPRESENT_PARAMETERS d3dParams = {1, 1, D3DFMT_X8R8G8B8, 1, D3DMULTISAMPLE_NONE, 0, D3DSWAPEFFECT_DISCARD, hWnd, TRUE, FALSE, D3DFMT_UNKNOWN, 0, 0, D3DPRESENT_INTERVAL_IMMEDIATE};

    // find Intel adapter number - not always the default adapter
    int adapterId = GetIntelAdapterId(pd3d);
    if (adapterId < 0)
    {
        MSDK_TRACE("QSDcoder: didn't find an Intel GPU.\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    // create d3d device
    m_pD3dDevice = 0;
    HRESULT hr = pd3d->CreateDevice(
        adapterId,
        D3DDEVTYPE_HAL,
        hWnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &d3dParams,
        &m_pD3dDevice);

    if (FAILED(hr) || !m_pD3dDevice)
    {
        MSDK_TRACE("QSDcoder: InitD3d CreateDevice failed!\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    // create device manager
    m_pD3dDeviceManager = NULL;
    hr = DXVA2CreateDirect3DDeviceManager9(&resetToken, &m_pD3dDeviceManager);
    if (FAILED(hr) || !m_pD3dDeviceManager)
    {
        MSDK_TRACE("QSDcoder: InitD3d DXVA2CreateDirect3DDeviceManager9 failed!\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    // reset the d3d device
    hr = m_pD3dDeviceManager->ResetDevice(m_pD3dDevice, resetToken);
    if (FAILED(hr))
    {
        MSDK_TRACE("QSDcoder: InitD3d ResetDevice failed!\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    return MFX_ERR_NONE;
}

void CQuickSyncDecoder::CloseD3D()
{
    if (m_bUseD3DAlloc)
    {
        m_mfxVideoSession.SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER, NULL);

        if (m_pD3dDevice)
        {
            MSDK_SAFE_RELEASE(m_pD3dDeviceManager);
            MSDK_SAFE_RELEASE(m_pD3dDevice);
        }
    }
}

mfxStatus CQuickSyncDecoder::DecodeHeader(mfxBitstream* bs, mfxVideoParam* par)
{
    MSDK_CHECK_POINTER(m_pmfxDEC && bs && par, MFX_ERR_NULL_PTR);
    mfxStatus sts = m_pmfxDEC->DecodeHeader(bs, par);

    // Try again, marking the bitstream as complete
    // This workaround should work on all driver versions.
    if (MFX_ERR_MORE_DATA == sts)
    {
        mfxU16 oldFlag = bs->DataFlag;
        bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
        sts = m_pmfxDEC->DecodeHeader(bs, par);
        bs->DataFlag = oldFlag;
    }

    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    return sts; 
}

mfxStatus CQuickSyncDecoder::CreateAllocator()
{
    if (m_pFrameAllocator != NULL)
        return MFX_ERR_NONE;

    MSDK_TRACE("QSDcoder: CreateAllocator\n");

    ASSERT(m_pVideoParams != NULL);
    std::auto_ptr<D3DAllocatorParams> pParam(NULL);
    mfxStatus sts = MFX_ERR_NONE;

    // Setup allocator - HW acceleration
    if (m_bUseD3DAlloc)
    {
        m_pVideoParams->IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        if (!m_pD3dDeviceManager)
        {
            sts = InitD3D();
            if (sts != MFX_ERR_NONE)
            {
                // Couldn't create our own device - probably a full screen exclusive player.
                // We'll use the supplied renderer's device manager.
                if (m_pRendererD2dDeviceManager != NULL)
                {
                    CloseD3D();

                    m_pD3dDeviceManager = m_pRendererD2dDeviceManager;
                    FreeFrameAllocator();
                }
                else
                {
                    MSDK_TRACE("QSDcoder: InitD3D failed!\n");
                    return sts;
                }
            }
        }

        m_mfxVideoSession.SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER, m_pD3dDeviceManager);

        pParam.reset(new D3DAllocatorParams);
        pParam->pManager = m_pD3dDeviceManager;
        m_pFrameAllocator = new D3DFrameAllocator();
    }
    // Setup allocator - No HW acceleration
    else
    {
        m_bUseD3DAlloc = false;
        m_pVideoParams->IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        m_pFrameAllocator = new SysMemFrameAllocator();
    }

    sts = m_pFrameAllocator->Init(pParam.get());
    if (sts == MFX_ERR_NONE)
    {
        // Note - setting the seesion allocator can be done only once!
        sts = m_mfxVideoSession.SetFrameAllocator(m_pFrameAllocator);
        if (sts != MFX_ERR_NONE)
        {
            MSDK_TRACE("QSDcoder: Session SetFrameAllocator failed!\n");    
        }
    }
    else
    // Allocator failed to initialize
    {
        MSDK_TRACE("QSDcoder: Allocator Init failed!\n");

        MSDK_SAFE_DELETE(m_pFrameAllocator);
        ASSERT(false);
    }

    return sts;
}

mfxStatus CQuickSyncDecoder::LockFrame(mfxFrameSurface1* pSurface, mfxFrameData* pFrameData)
{
    MSDK_CHECK_POINTER(pSurface, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(pFrameData, MFX_ERR_NULL_PTR);
    return m_pFrameAllocator->Lock(m_pFrameAllocator, pSurface->Data.MemId, pFrameData);
}

mfxStatus CQuickSyncDecoder::UnlockFrame(mfxFrameSurface1* pSurface, mfxFrameData* pFrameData)
{
    MSDK_CHECK_POINTER(pSurface, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(pFrameData, MFX_ERR_NULL_PTR);
    return m_pFrameAllocator->Unlock(m_pFrameAllocator, pSurface->Data.MemId, pFrameData);
}

void CQuickSyncDecoder::SetD3DDeviceManager(IDirect3DDeviceManager9* pDeviceManager)
{
    if (m_pRendererD2dDeviceManager == pDeviceManager)
        return;

    m_pRendererD2dDeviceManager = pDeviceManager;
}
