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
    m_mfxVideoSession(NULL),
    m_mfxImpl(MFX_IMPL_UNSUPPORTED),
    m_bHwAccelInit(false),
    m_pmfxDEC(0),
    m_pVideoParams(0),
    m_pFrameAllocator(NULL),
    m_pFrameSurfaces(NULL),
    m_nRequiredFramesNum(0),
    m_nLastSurfaceId(0),
    m_nAuxFrameCount(0),
    m_pRendererD3dDeviceManager(NULL),
    m_pD3dDeviceManager(NULL),
    m_pD3dDevice(NULL),
    m_hDecoderWorkerThread(NULL),
    m_DecoderWorkerThreadId(0),
    m_OnDecodeComplete(NULL)
{
    MSDK_ZERO_VAR(m_AllocResponse);

    m_ApiVersion.Major = MIN_REQUIRED_API_VER_MAJOR;
    m_ApiVersion.Minor = MIN_REQUIRED_API_VER_MINOR;
    mfxIMPL impl = MFX_IMPL_AUTO_ANY;

    // Uncomment for SW emulation
    //impl = MFX_IMPL_SOFTWARE;

    sts = InitSession(impl);
}


CQuickSyncDecoder::~CQuickSyncDecoder()
{
    // Wait for the worker thread to finish
    if (m_DecoderWorkerThreadId)
    {
        PostThreadMessage(m_DecoderWorkerThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(m_hDecoderWorkerThread, INFINITE);
        CloseHandle(m_hDecoderWorkerThread);
    }

    CloseSession();

    FreeFrameAllocator();
    delete m_pFrameAllocator;
    CloseD3D();
}

mfxStatus CQuickSyncDecoder::InitSession(mfxIMPL impl)
{
    if (m_mfxVideoSession != NULL)
        return MFX_ERR_NONE;

    m_mfxVideoSession = new MFXVideoSession;
    mfxStatus sts = m_mfxVideoSession->Init(impl, &m_ApiVersion);
    if (MFX_ERR_NONE != sts)
    {
        MSDK_TRACE("QsDecoder: failed to initialize MSDK session!\n");
        return sts;
    }

    m_mfxVideoSession->QueryIMPL(&m_mfxImpl);
    m_mfxVideoSession->QueryVersion(&m_ApiVersion);

    m_bHwAcceleration = m_mfxImpl != MFX_IMPL_SOFTWARE;
    m_bUseD3DAlloc = m_bHwAcceleration;
    m_pmfxDEC = new MFXVideoDECODE((mfxSession)*m_mfxVideoSession);

    MSDK_ZERO_MEMORY((void*)&m_LockedSurfaces, sizeof(m_LockedSurfaces));
    return MFX_ERR_NONE;
}

void CQuickSyncDecoder::CloseSession()
{
    MSDK_SAFE_DELETE(m_pmfxDEC);
    MSDK_SAFE_DELETE(m_mfxVideoSession);
}

mfxFrameSurface1* CQuickSyncDecoder::FindFreeSurface()
{
    MSDK_CHECK_POINTER(m_pFrameSurfaces, NULL);
#ifdef _DEBUG
    static int s_SleepCount = 0;
#endif

    // 1 second cycle
    for (int tries = 0; tries < 1000; ++tries)
    {
        for (int i = 0; i < m_nRequiredFramesNum; ++i)
        {
            if (!IsSurfaceLocked(&m_pFrameSurfaces[i]))
            {
                // Found free surface :)
                m_nLastSurfaceId = (++m_nLastSurfaceId) % m_nRequiredFramesNum;
                return m_pFrameSurfaces + i;
            }
        }

        // Note: code should not reach here!
        MSDK_TRACE("QSDcoder: FindFreeSurface - all surfaces are in use, retrying in 1ms (%d)\n", ++s_SleepCount);
        Sleep(1);
    }

    return NULL;
}

mfxStatus CQuickSyncDecoder::InitFrameAllocator(mfxVideoParam* pVideoParams, mfxU32 nPitch)
{
    MSDK_TRACE("QSDcoder: InitFrameAllocator\n");
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

    // Find how many surfaces are needed
    mfxFrameAllocRequest allocRequest;
    MSDK_ZERO_VAR(allocRequest);
    sts = m_pmfxDEC->QueryIOSurf(pVideoParams, &allocRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);
    allocRequest.NumFrameSuggested = (mfxU16)m_nAuxFrameCount + allocRequest.NumFrameSuggested;
    allocRequest.NumFrameMin = allocRequest.NumFrameSuggested;

    // Decide memory type
    allocRequest.Type = MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE;
    allocRequest.Type |= (m_bUseD3DAlloc) ?
       MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET : MFX_MEMTYPE_SYSTEM_MEMORY;

    memcpy(&allocRequest.Info, &pVideoParams->mfx.FrameInfo, sizeof(mfxFrameInfo));

    // Allocate frames with H aligned at 32 for both progressive and interlaced content
    allocRequest.Info.Height = MSDK_ALIGN32(allocRequest.Info.Height); 
    allocRequest.Info.Width = (mfxU16)nPitch;

    // Perform allocation call. result is saved in m_AllocResponse
    sts = m_pFrameAllocator->Alloc(m_pFrameAllocator->pthis, &allocRequest, &m_AllocResponse);
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

    m_nRequiredFramesNum = m_AllocResponse.NumFrameActual;
    ASSERT(m_nRequiredFramesNum == allocRequest.NumFrameSuggested);
    m_pFrameSurfaces = new mfxFrameSurface1[m_nRequiredFramesNum];
    MSDK_CHECK_POINTER(m_pFrameSurfaces, MFX_ERR_MEMORY_ALLOC);
    MSDK_ZERO_MEMORY(m_pFrameSurfaces, sizeof(mfxFrameSurface1) * m_nRequiredFramesNum);

    // Allocate decoder work & output surfaces
    for (mfxU32 i = 0; i < m_nRequiredFramesNum; ++i)
    {
        // Copy frame info
        memcpy(&(m_pFrameSurfaces[i].Info), &pVideoParams->mfx.FrameInfo, sizeof(mfxFrameInfo));

        // Save pointer to allocator specific surface object (mid)
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

    // Create worker thread
    if (m_Config.bEnableMtDecode && m_hDecoderWorkerThread == NULL)
    {
        m_hDecoderWorkerThread = (HANDLE)_beginthreadex(NULL, 0, &DecoderWorkerThreadProc, this, 0, &m_DecoderWorkerThreadId);
//        SetThreadPriority(m_hDecoderWorkerThread, THREAD_PRIORITY_HIGHEST);
    }

    if (NULL == m_pFrameAllocator)
    {
        bInited = false;
    }

    // Reset decoder
    if (bInited)
    {
        // Kill the session for VC1 only (workaround)
        // Doing this for MPEG2 will cause issues like aspect ratio change will not be detected.
        if (pVideoParams->mfx.CodecId == MFX_CODEC_VC1 && pVideoParams->mfx.CodecProfile == MFX_PROFILE_VC1_ADVANCED)
        {
            MFXVideoSession* saveSession = m_mfxVideoSession; // Kill the old session later so MSDK DLL will not unload
            m_mfxVideoSession = NULL;
            CloseSession();
            sts = InitSession(m_mfxImpl);
            delete saveSession;
            MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);
            if (m_bUseD3DAlloc)
            {
                m_mfxVideoSession->SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER, m_pD3dDeviceManager);
                // Note - setting the session allocator can be done only once (per session)!
                sts = m_mfxVideoSession->SetFrameAllocator(m_pFrameAllocator);
                if (sts != MFX_ERR_NONE)
                {
                    MSDK_TRACE("QSDcoder: Session SetFrameAllocator failed!\n");    
                }
            }

            // Another VC1 decoder workaround
            for (int i = 0; i < m_nRequiredFramesNum; ++i)
            {
                m_pFrameSurfaces[i].Data.Locked = 0;
                m_LockedSurfaces[i] = 0;
            }

            bInited = false;
        }
        else
        {
            sts = m_pmfxDEC->Reset(pVideoParams);
            // Need to reset the frame allocator
            if (MFX_ERR_NONE != sts)
            {
                m_pmfxDEC->Close();
                FreeFrameAllocator();
                bInited = false;
            }
        }
    }

    // Full init
    if (!bInited)
    {
        // Check if video format is supported by HW acceleration
        if (!m_bHwAccelInit)
        {
            if (m_bHwAcceleration && MFX_WRN_PARTIAL_ACCELERATION == CheckHwAcceleration(pVideoParams))
            {
                // Do not change allocator to system memory - will cause a crash!
                m_bHwAcceleration = false; // This is just for knowledge
            }

            m_bHwAccelInit = true;
        }

        // Setup allocator - will initialize D3D if needed
        sts = InitFrameAllocator(pVideoParams, nPitch);
        MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

        // Init MSDK decoder
        sts = m_pmfxDEC->Init(pVideoParams);
        switch (sts)
        {
        case MFX_ERR_NONE:
            MSDK_TRACE("QsDecoder: decoder Init is successful\n");
            break;
        case MFX_WRN_PARTIAL_ACCELERATION:
            MSDK_TRACE("QsDecoder: decoder Init is successful w/o HW acceleration\n");
            break;
        case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:
            MSDK_TRACE("QsDecoder: decoder Init is successful - wrong video parameters\n");
            break;
        default:
            MSDK_TRACE("QsDecoder: decoder Init has failed!\n");
            break;
        }
    }

    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    return sts;
}

mfxStatus CQuickSyncDecoder::CheckHwAcceleration(mfxVideoParam* pVideoParams)
{
    MSDK_CHECK_POINTER(pVideoParams, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);

    // If we are already using SW decoder then no need for further checks...
    mfxIMPL impl = QueryIMPL();
    if (MFX_IMPL_SOFTWARE == impl)
        return MFX_ERR_NONE;

    mfxU16 ioPaternSave = pVideoParams->IOPattern;
    pVideoParams->IOPattern = (mfxU16)MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    mfxVideoParam tmp = *pVideoParams;
    mfxStatus sts = m_pmfxDEC->Query(pVideoParams, &tmp);
    pVideoParams->IOPattern = ioPaternSave;
    return sts;
}

mfxStatus CQuickSyncDecoder::GetVideoParams(mfxVideoParam* pVideoParams)
{
    MSDK_CHECK_POINTER(pVideoParams, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);

    return m_pmfxDEC->GetVideoParam(pVideoParams);
}

mfxStatus CQuickSyncDecoder::Decode(mfxBitstream* pBS, bool bAsync, mfxFrameSurface1*& pFrameSurface)
{
    MSDK_CHECK_POINTER(m_pmfxDEC, MFX_ERR_NOT_INITIALIZED);    

    WaitForDecoder();

    mfxStatus sts = MFX_ERR_NONE;
    mfxSyncPoint syncp;
    mfxFrameSurface1* pWorkSurface = FindFreeSurface();
    MSDK_CHECK_POINTER(pWorkSurface, MFX_ERR_NOT_ENOUGH_BUFFER);

    do
    {
        sts = m_pmfxDEC->DecodeFrameAsync(pBS, pWorkSurface, &pFrameSurface, &syncp);
        // Need 1 more work surface
        if (MFX_ERR_MORE_SURFACE == sts)
        {
            pWorkSurface = FindFreeSurface();
            MSDK_CHECK_POINTER(pWorkSurface, MFX_ERR_NOT_ENOUGH_BUFFER);
        }
        else if (MFX_WRN_DEVICE_BUSY == sts)
        {
            SwitchToThread();
        }
    } while (MFX_WRN_DEVICE_BUSY == sts || MFX_ERR_MORE_SURFACE == sts);

    // Output will be shortly available
    if (MFX_ERR_NONE == sts) 
    {
        if (m_Config.bEnableMtDecode && bAsync)
        {
            // Turn event to non signalled (locked)
            // The state will change to signalled after the worker thread has finished decoding
            m_AsyncDecodeInfo.lock.Lock();
            m_AsyncDecodeInfo.pSurface = pFrameSurface;
            m_AsyncDecodeInfo.syncPoint = syncp;

            PostThreadMessage(m_DecoderWorkerThreadId, TM_DECODE_FRAME, 0, 0);
            pFrameSurface = NULL; // Return pFrameSurface only in sync mode
        }
        else
        {
            // Wait for the asynch decoding to finish
            sts = m_mfxVideoSession->SyncOperation(syncp, 0xFFFF);
        }
    }

    return sts;
}

mfxStatus CQuickSyncDecoder::InitD3D()
{
    if (!m_bUseD3DAlloc)
    {
        return MFX_ERR_NONE;
    }

    // Check if the d3d device is functional:
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

    // Find Intel adapter number - not always the default adapter
    int adapterId = GetIntelAdapterId(pd3d);
    if (adapterId < 0)
    {
        MSDK_TRACE("QSDcoder: didn't find an Intel GPU.\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    // Create d3d device
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

    // Create device manager
    m_pD3dDeviceManager = NULL;
    hr = DXVA2CreateDirect3DDeviceManager9(&resetToken, &m_pD3dDeviceManager);
    if (FAILED(hr) || !m_pD3dDeviceManager)
    {
        MSDK_TRACE("QSDcoder: InitD3d DXVA2CreateDirect3DDeviceManager9 failed!\n");
        return MFX_ERR_DEVICE_FAILED;
    }

    // Reset the d3d device
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
        if (m_mfxVideoSession)
        {
            m_mfxVideoSession->SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER, NULL);
        }

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
    int adapterId = -1;

    // Setup allocator - HW acceleration
    if (m_bUseD3DAlloc)
    {
        m_pVideoParams->IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        ASSERT (m_pD3dDeviceManager == NULL);

        // Couldn't create our own device - probably a full screen exclusive player.
        // We'll use the supplied renderer's device manager.
        if (m_pRendererD3dDeviceManager)
        {
            // Get DirectX Object
            HANDLE hDevice;
            IDirect3DDevice9* pDevice = NULL;
            CComPtr<IDirect3D9> pD3D;
            D3DDEVICE_CREATION_PARAMETERS devParames;
            D3DADAPTER_IDENTIFIER9 adIdentifier;
            D3DPRESENT_PARAMETERS d3dParams = {1, 1, D3DFMT_X8R8G8B8, 1, D3DMULTISAMPLE_NONE, 0, D3DSWAPEFFECT_DISCARD, NULL, TRUE, FALSE, D3DFMT_UNKNOWN, 0, 0, D3DPRESENT_INTERVAL_IMMEDIATE};

            HRESULT hr = m_pRendererD3dDeviceManager->OpenDeviceHandle(&hDevice);
            if (FAILED(hr))
            {
                MSDK_TRACE("QsDecoder: failed to open device handle!\n");
                goto done;
            }
            hr = m_pRendererD3dDeviceManager->LockDevice(hDevice, &pDevice, TRUE);
            if (FAILED(hr) && NULL == pDevice)
            {
                MSDK_TRACE("QsDecoder: failed to lock device!\n");
                goto done;
            }

            hr = pDevice->GetDirect3D(&pD3D);
            if (FAILED(hr))
            {
                MSDK_TRACE("QsDecoder: failed to get D3D9 object!\n");
                goto done;
            }

            hr = pDevice->GetCreationParameters(&devParames);
            if (FAILED(hr))
            {
                MSDK_TRACE("QsDecoder: failed to get device creation params!\n");
                goto done;
            }

            hr = pD3D->GetAdapterIdentifier(devParames.AdapterOrdinal, 0, &adIdentifier);
            if (FAILED(hr))
            {
                MSDK_TRACE("QsDecoder: failed to get adapter identifier!\n");
                goto done;
            }

            // If renderer is already on Intel's GPU than we can reuse the device.
            if (adIdentifier.VendorId == 0x8086) // Intel's vendor ID  is 8086h
            {
                m_pD3dDeviceManager = m_pRendererD3dDeviceManager;
                goto done;
            }

            // Find Intel adapter number - not always the default adapter
            adapterId = GetIntelAdapterId(pD3D);
            if (adapterId < 0)
            {
                hr = E_FAIL;
                MSDK_TRACE("QSDcoder: didn't find an Intel GPU.\n");
                goto done;
            }

            // Create d3d device
            m_pD3dDevice = 0;
            d3dParams.hDeviceWindow = devParames.hFocusWindow;
            hr = pD3D->CreateDevice(
                adapterId,
                D3DDEVTYPE_HAL,
                devParames.hFocusWindow,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
                &d3dParams,
                &m_pD3dDevice);

            if (FAILED(hr) || !m_pD3dDevice)
            {
                hr = E_FAIL;
                MSDK_TRACE("QSDcoder: InitD3d CreateDevice failed!\n");
                goto done;
            }

            // Create device manager
            UINT resetToken;
            hr = DXVA2CreateDirect3DDeviceManager9(&resetToken, &m_pD3dDeviceManager);
            if (FAILED(hr) || !m_pD3dDeviceManager)
            {
                hr = E_FAIL;
                MSDK_TRACE("QSDcoder: InitD3d DXVA2CreateDirect3DDeviceManager9 failed!\n");
                goto done;
            }

            // Reset the d3d device
            hr = m_pD3dDeviceManager->ResetDevice(m_pD3dDevice, resetToken);
            if (FAILED(hr))
            {
                hr = E_FAIL;
                MSDK_TRACE("QSDcoder: InitD3d ResetDevice failed!\n");
                goto done;
            }

            // Cleanup
done:
            if (FAILED(hr))
            {
                sts = MFX_ERR_DEVICE_FAILED;
            }

            MSDK_SAFE_RELEASE(pDevice);
            if (hDevice != NULL)
            {
                m_pRendererD3dDeviceManager->UnlockDevice(hDevice, FALSE);
                m_pRendererD3dDeviceManager->CloseDeviceHandle(&hDevice);
            }

            sts = MFX_ERR_NONE;
        }
        else
        {
            sts = InitD3D();
        }

        if (sts != MFX_ERR_NONE)
        {
            MSDK_TRACE("QSDcoder: InitD3D failed!\n");
            return sts;
        }

        m_mfxVideoSession->SetHandle(MFX_HANDLE_D3D9_DEVICE_MANAGER, m_pD3dDeviceManager);

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
        // Note - setting the session allocator can be done only once per session!
        sts = m_mfxVideoSession->SetFrameAllocator(m_pFrameAllocator);
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

bool CQuickSyncDecoder::SetD3DDeviceManager(IDirect3DDeviceManager9* pDeviceManager)
{
    if (m_pRendererD3dDeviceManager == pDeviceManager)
        return false;

    MSDK_TRACE("QsDecoder: SetD3DDeviceManager called\n");
    m_pRendererD3dDeviceManager = pDeviceManager;
    return true;
}

unsigned CQuickSyncDecoder::DecoderWorkerThreadProc(void* pThis)
{
    ASSERT(pThis != NULL);
    return static_cast<CQuickSyncDecoder*>(pThis)->DecoderWorkerThreadMsgLoop();
}

unsigned CQuickSyncDecoder::DecoderWorkerThreadMsgLoop()
{
#ifdef _DEBUG
    SetThreadName("*** QS decoder WT ***");
#endif

    BOOL bRet;

    // Note: GetMessage returns zero on WM_QUIT
    MSG msg;
    while ((bRet = GetMessage(&msg, (HWND)-1, 0, 0 )) != 0)
    {
        // Error
        if (bRet == -1)
        {
            return (unsigned) bRet;
        }

        // Ignore WM_NULL messages
        if (WM_NULL == msg.message)
            continue;

        if (TM_DECODE_FRAME == msg.message)
        {
            ASSERT(NULL != m_AsyncDecodeInfo.pSurface);
            ASSERT(NULL != m_AsyncDecodeInfo.syncPoint);

            // Wait for the asynch decoding to finish
            mfxStatus sts = m_mfxVideoSession->SyncOperation(m_AsyncDecodeInfo.syncPoint, 0xFFFF);
            if (sts == MFX_ERR_NONE)
            {
                ASSERT(m_OnDecodeComplete != NULL && m_Parent != NULL);
                LockSurface(m_AsyncDecodeInfo.pSurface);
                m_OnDecodeComplete(m_AsyncDecodeInfo.pSurface, m_Parent);
            }

            m_AsyncDecodeInfo.lock.Unlock();
        }
    }

    // All's well
    return 0;
}
