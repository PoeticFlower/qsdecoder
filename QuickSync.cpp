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
#include "CodecInfo.h"
#include "TimeManager.h"
#include "QuickSyncUtils.h"
#include "frame_constructors.h"
#include "QuickSyncDecoder.h"
#include "QuickSync.h"

#define PAGE_MASK 4095

EXTERN_GUID(WMMEDIASUBTYPE_WVC1, 
0x31435657, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71); 
EXTERN_GUID(WMMEDIASUBTYPE_WMV3, 
0x33564D57, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71); 

DEFINE_GUID(WMMEDIASUBTYPE_WVC1_PDVD,
0xD979F77B, 0xDBEA, 0x4BF6, 0x9E, 0x6D, 0x1D, 0x7E, 0x57, 0xFB, 0xAD, 0x53);

#define DECODE_QUEUE_LENGTH 16
#define PROCESS_QUEUE_LENGTH 2

////////////////////////////////////////////////////////////////////
//                      CQuickSync
////////////////////////////////////////////////////////////////////
CQuickSync::CQuickSync() :
    m_bInitialized(false),
    m_nPitch(0),
    m_pFrameConstructor(NULL),
    m_nSegmentFrameCount(0),
    m_bFlushing(false),
    m_bNeedToFlush(false),
    m_bDvdDecoding(false),
    m_hProcessorWorkerThread(NULL),
    m_ProcessorWorkerThreadId(0),
    m_DecodedFramesQueue(DECODE_QUEUE_LENGTH),
    m_ProcessedFramesQueue(PROCESS_QUEUE_LENGTH),
    m_FreeFramesPool(PROCESS_QUEUE_LENGTH)
{
    MSDK_TRACE("QSDcoder: Constructor\n");

    mfxStatus sts = MFX_ERR_NONE;
    m_pDecoder = new CQuickSyncDecoder(sts);
    m_pDecoder->SetOnDecodeComplete(&CQuickSync::OnDecodeComplete, this);

    MSDK_ZERO_VAR(m_mfxParamsVideo);
    //m_mfxParamsVideo.AsyncDepth = 1; //causes issues when 1
    m_mfxParamsVideo.mfx.ExtendedPicStruct = 1;

    // Set default configuration - override what's not zero/false
//    m_Config.bMod16Width = true;
    m_Config.bTimeStampCorrection = true;

    m_Config.nOutputQueueLength = 8;
    m_Config.bEnableH264  = true;
    m_Config.bEnableMPEG2 = true;
    m_Config.bEnableVC1   = true;
    m_Config.bEnableWMV9  = true;

    m_Config.bEnableMultithreading = true;
    m_Config.bEnableMtProcessing   = m_Config.bEnableMultithreading;
    m_Config.bEnableMtDecode       = m_Config.bEnableMultithreading;
    m_Config.bEnableMtCopy         = m_Config.bEnableMultithreading;

//    m_Config.bEnableSwEmulation  = true;

    // Currently not working well - menu decoding :(
    //m_Config.bEnableDvdDecoding = true;

    m_OK = (sts == MFX_ERR_NONE);
}

CQuickSync::~CQuickSync()
{
    MSDK_TRACE("QSDcoder: Destructor\n");
    
    // This will quicken the exit 
    BeginFlush();

    CQsAutoLock cObjectLock(&m_csLock);

    // Wait for the worker thread to finish
    if (m_ProcessorWorkerThreadId)
    {
        if (!m_DecodedFramesQueue.PushBack(NULL, 1000))
        {
            MSDK_TRACE("Failed to close worker thread!\n");
            TerminateThread(m_hProcessorWorkerThread, 0);
        }
        else
        {
            WaitForSingleObject(m_hProcessorWorkerThread, INFINITE);
        }

        CloseHandle(m_hProcessorWorkerThread);
    }

    // Cleanup the free frame pool
    while (!m_FreeFramesPool.Empty())
    {
        TQsQueueItem item;
        if (m_FreeFramesPool.PopFront(item, 0))
        {
            delete item.first;
            delete item.second;
        }
    }

    MSDK_SAFE_DELETE(m_pFrameConstructor);
    MSDK_SAFE_DELETE(m_pDecoder);
}

HRESULT CQuickSync::HandleSubType(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC, mfxVideoParam& videoParams, CFrameConstructor*& pFrameConstructor)
{
    const GUID& subType = mtIn->subtype;
    const GUID& guidFormat = mtIn->formattype;

    MSDK_SAFE_DELETE(pFrameConstructor);

    // MPEG2
    if (subType == MEDIASUBTYPE_MPEG2_VIDEO)
    {
        if (!m_Config.bEnableMPEG2)
            return VFW_E_INVALIDMEDIATYPE;

        videoParams.mfx.CodecId = MFX_CODEC_MPEG2;
        pFrameConstructor = new CFrameConstructor;
    }    
    // VC1 or WMV3
    else if ((fourCC == FOURCC_VC1) || (fourCC == FOURCC_WMV3))
    {
        videoParams.mfx.CodecId = MFX_CODEC_VC1;

        if (subType == WMMEDIASUBTYPE_WMV3)
        {
            videoParams.mfx.CodecProfile = MFX_PROFILE_VC1_SIMPLE;        
            if (!m_Config.bEnableWMV9)
                return VFW_E_INVALIDMEDIATYPE;
        }   
        else if (fourCC == FOURCC_VC1)
        {
            videoParams.mfx.CodecProfile = MFX_PROFILE_VC1_ADVANCED;
            if (!m_Config.bEnableVC1)
                return VFW_E_INVALIDMEDIATYPE;
        }
        else
        {
            return VFW_E_INVALIDMEDIATYPE;
        }

        pFrameConstructor = new CVC1FrameConstructor();
    }
    // H264
    else if ((fourCC == FOURCC_H264) || (fourCC == FOURCC_X264) || (fourCC == FOURCC_h264) ||
        (fourCC == FOURCC_avc1) || (fourCC == FOURCC_VSSH) || (fourCC == FOURCC_DAVC) ||
        (fourCC == FOURCC_PAVC) || (fourCC == FOURCC_AVC1) || (fourCC == FOURCC_CCV1))
    {
        if (!m_Config.bEnableH264)
            return VFW_E_INVALIDMEDIATYPE;

        videoParams.mfx.CodecId = MFX_CODEC_AVC;
        pFrameConstructor = ((fourCC == FOURCC_avc1) || (fourCC == FOURCC_AVC1) || (fourCC == FOURCC_CCV1)) ?
            new CAVCFrameConstructor :
            new CFrameConstructor;
    }
    else
    {
        return E_NOTIMPL;
    }

    // Check if codec profile and level are supported before calling DecodeHeader
    mfxInfoMFX& mfx = videoParams.mfx;
    if (FORMAT_MPEG2_VIDEO == guidFormat)
    {
        MPEG2VIDEOINFO* mp2 = (MPEG2VIDEOINFO*)(mtIn->pbFormat);
        HRESULT hr = CheckCodecProfileSupport(mfx.CodecId, mp2->dwProfile, mp2->dwLevel);
        if (hr != S_OK)
        {
            MSDK_TRACE("QSDcoder::InitDecoder - failed due to unsupported codec (%s), profile (%s), level (%i) combination\n",
                GetCodecName(mfx.CodecId), GetProfileName(mfx.CodecId, mp2->dwProfile), mp2->dwLevel);
            return E_NOTIMPL;
        }
        else
        {
            MSDK_TRACE("QSDcoder::InitDecoder - codec (%s), profile (%s), level (%i)\n",
                GetCodecName(mfx.CodecId), GetProfileName(mfx.CodecId, mp2->dwProfile), mp2->dwLevel);
        }
    }
    else
    {
        MSDK_TRACE("QSDcoder::InitDecoder - codec (%s)\n", GetCodecName(mfx.CodecId));
    }

    return S_OK;
}

HRESULT CQuickSync::TestMediaType(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC)
{
    MSDK_TRACE("QSDcoder: TestMediaType\n");
    if (!m_OK)
    {
        MSDK_TRACE("QSDcoder: TestMediaType was called on an invalid object!\n");
        return E_FAIL;
    }
    
    // Parameter check
    MSDK_CHECK_POINTER(mtIn, E_POINTER);
    MSDK_CHECK_POINTER(mtIn->pbFormat, E_UNEXPECTED);

    // Disable DVD playback until it's OK
    if (MEDIATYPE_DVD_ENCRYPTED_PACK == mtIn->majortype)
    {
        MSDK_TRACE("QSDcoder: DVD decoding is not supported!\n");
        return E_FAIL;
    }
    else if (mtIn->majortype != MEDIATYPE_Video)
    {
        MSDK_TRACE("QSDcoder: Invalid majortype GUID!\n");
        return E_FAIL;
    }
    
    VIDEOINFOHEADER2* vih2 = NULL;
    size_t nSampleSize = 0, nVideoInfoSize = 0;
    mfxVideoParam videoParams;
    MSDK_ZERO_VAR(videoParams);
    CFrameConstructor* pFrameConstructor = NULL;
    HRESULT hr = DecodeHeader(mtIn, fourCC, pFrameConstructor, vih2, nSampleSize, nVideoInfoSize, videoParams);
    MSDK_SAFE_DELETE(pFrameConstructor);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);

    // If SW emulation is disabled - check if HW accelaration is supported
    if (!m_Config.bEnableSwEmulation)
    {
        mfxStatus sts = m_pDecoder->CheckHwAcceleration(&videoParams);
        if (sts != MFX_ERR_NONE)
        {
            MSDK_TRACE("HW accelration is not supported. Aborting");
            hr = E_FAIL;
        }
    }

    delete[] (mfxU8*)vih2;
    MSDK_TRACE("QSDcoder: TestMediaType finished: %s\n", (SUCCEEDED(hr)) ? "success" : "failure");
    return hr;
}

HRESULT CQuickSync::CopyMediaTypeToVIDEOINFOHEADER2(const AM_MEDIA_TYPE* mtIn, VIDEOINFOHEADER2*& vih2, size_t& nVideoInfoSize, size_t& nSampleSize)
{
    vih2 = NULL;
    const GUID& guidFormat = mtIn->formattype;
    nVideoInfoSize = sizeof(VIDEOINFOHEADER2);

    if (FORMAT_VideoInfo2 == guidFormat || FORMAT_MPEG2_VIDEO == guidFormat)
    {
        if (FORMAT_MPEG2_VIDEO == guidFormat)
        {
            nVideoInfoSize = sizeof(MPEG2VIDEOINFO);
        }

        nSampleSize = mtIn->cbFormat;
        vih2 = (VIDEOINFOHEADER2*) new mfxU8[nSampleSize];

        // Copy the sample as is
        memcpy(vih2, mtIn->pbFormat, nSampleSize);
    }
    // Translate VIDEOINFOHEADER to VIDEOINFOHEADER2 (easier to work with down the road)
    else if (FORMAT_VideoInfo == guidFormat)
    {
        size_t extraDataLen = mtIn->cbFormat - sizeof(VIDEOINFOHEADER);
        nSampleSize = sizeof(VIDEOINFOHEADER2) + extraDataLen;
        vih2 = (VIDEOINFOHEADER2*) new mfxU8[nSampleSize];
        MSDK_ZERO_MEMORY(vih2, sizeof(VIDEOINFOHEADER2));

        // Initialize the VIDEOINFOHEADER2 structure
        VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)(mtIn->pbFormat);
        vih2->rcSource           = pvi->rcSource;
        vih2->rcTarget           = pvi->rcTarget;
        vih2->dwBitRate          = pvi->dwBitRate;
        vih2->dwBitErrorRate     = pvi->dwBitErrorRate;
        vih2->bmiHeader          = pvi->bmiHeader;
        vih2->AvgTimePerFrame    = pvi->AvgTimePerFrame;
        vih2->dwPictAspectRatioX = pvi->bmiHeader.biWidth;
        vih2->dwPictAspectRatioY = pvi->bmiHeader.biHeight;

        // Copy the out of band data (data past the VIDEOINFOHEADER structure.
        // Note: copy past the size of vih2
        if (extraDataLen)
        {
            memcpy(((mfxU8*)vih2) + nVideoInfoSize, mtIn->pbFormat + sizeof(VIDEOINFOHEADER), extraDataLen);
        }
    }
    else
    {
        return VFW_E_INVALIDMEDIATYPE;
    }

    return S_OK;
}

HRESULT CQuickSync::DecodeHeader(
    const AM_MEDIA_TYPE* mtIn,
    FOURCC fourCC,
    CFrameConstructor*& pFrameConstructor,
    VIDEOINFOHEADER2*& vih2,
    size_t nSampleSize,
    size_t& nVideoInfoSize,
    mfxVideoParam& videoParams)
{
    mfxInfoMFX& mfx = videoParams.mfx;
    const GUID& guidFormat = mtIn->formattype;
    HRESULT hr = S_OK;
    mfxStatus sts = MFX_ERR_NONE;
     
    hr = HandleSubType(mtIn, fourCC, videoParams, pFrameConstructor);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);

    hr = CopyMediaTypeToVIDEOINFOHEADER2(mtIn, vih2, nVideoInfoSize, nSampleSize);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);

    if (MEDIATYPE_DVD_ENCRYPTED_PACK == mtIn->majortype)
    {
        if (!m_Config.bEnableDvdDecoding)
            return VFW_E_INVALIDMEDIATYPE;

        m_pFrameConstructor->SetDvdPacketStripping(true);
        m_bDvdDecoding = true;
    }

    if (!vih2->bmiHeader.biWidth || !vih2->bmiHeader.biHeight)
    {
        return VFW_E_INVALIDMEDIATYPE;
    }

    // Construct sequence header and decode the sequence header.
    if (nVideoInfoSize < nSampleSize)
    {
        sts = pFrameConstructor->ConstructHeaders(vih2, guidFormat, nSampleSize, nVideoInfoSize);
        if (MFX_ERR_NONE == sts)
        {
            sts = m_pDecoder->DecodeHeader(&pFrameConstructor->GetHeaders(), &videoParams);
        }
    }
    // Simple info header (without extra data) or DecodeHeader failed (usually not critical)
    else
    {
        mfx.FrameInfo.CropW = (mfxU16)vih2->bmiHeader.biWidth;
        if (m_Config.bMod16Width)
        {
            mfx.FrameInfo.CropW = MSDK_ALIGN16(mfx.FrameInfo.CropW);
        }

        mfx.FrameInfo.CropH        = (mfxU16)vih2->bmiHeader.biHeight;
        mfx.FrameInfo.Width        = (mfxU16)MSDK_ALIGN16(mfx.FrameInfo.CropW);
        mfx.FrameInfo.Height       = (mfxU16)MSDK_ALIGN16(mfx.FrameInfo.CropH);
        mfx.FrameInfo.FourCC       = MFX_FOURCC_NV12;
        mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        ConvertFrameRate((vih2->AvgTimePerFrame) ? 1e7 / vih2->AvgTimePerFrame : 0, 
            mfx.FrameInfo.FrameRateExtN, 
            mfx.FrameInfo.FrameRateExtD);
    }

    hr = (sts == MFX_ERR_NONE) ? S_OK : E_FAIL;

    if (FAILED(hr))
    {
        delete[] (mfxU8*)vih2;
        vih2 = NULL;
    }
    return hr;
}

HRESULT CQuickSync::InitDecoder(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC)
{
    MSDK_TRACE("QSDcoder: InitDecoder\n");
    CQsAutoLock cObjectLock(&m_csLock);
    m_bNeedToFlush = true;
    m_bInitialized = true;

    VIDEOINFOHEADER2* vih2 = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    mfxInfoMFX& mfx = m_mfxParamsVideo.mfx;
    HRESULT hr;
    MSDK_CHECK_POINTER(mtIn, E_POINTER);
    MSDK_CHECK_POINTER(mtIn->pbFormat, E_UNEXPECTED);
    bool bIsFields = false;
    size_t nSampleSize = 0, nVideoInfoSize = 0;

    if (!(mtIn->majortype == MEDIATYPE_DVD_ENCRYPTED_PACK || mtIn->majortype == MEDIATYPE_Video))
        return VFW_E_INVALIDMEDIATYPE;


    hr = DecodeHeader(mtIn, fourCC, m_pFrameConstructor, vih2, nSampleSize, nVideoInfoSize, m_mfxParamsVideo);

    // Setup frame rate from either the media type or the decoded header
    bIsFields = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) &&
        (vih2->dwInterlaceFlags & AMINTERLACE_1FieldPerSample);

    // Try getting the frame rate from the decoded headers
    if (0 < vih2->AvgTimePerFrame)
    {
        m_TimeManager.SetFrameRate(1e7 / vih2->AvgTimePerFrame, bIsFields);
    }
    // In case we don't have a frame rate
    else if (mfx.FrameInfo.FrameRateExtN * mfx.FrameInfo.FrameRateExtD != 0)
    {
        double frameRate = (double)mfx.FrameInfo.FrameRateExtN / (double)mfx.FrameInfo.FrameRateExtD;
        m_TimeManager.SetFrameRate(frameRate, bIsFields);
    }

    // We might decode well even if DecodeHeader failed with MFX_ERR_MORE_DATA
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);

    m_nPitch = mfx.FrameInfo.Width;

    SetAspectRatio(*vih2, mfx.FrameInfo);

    // Disable MT features if main flag is off
    m_Config.bEnableMtProcessing = m_Config.bEnableMtProcessing && m_Config.bEnableMultithreading;
    m_Config.bEnableMtDecode     = m_Config.bEnableMtDecode     && m_Config.bEnableMtProcessing; // This feature relies on bEnableMtProcessing
    m_Config.bEnableMtCopy       = m_Config.bEnableMtCopy       && m_Config.bEnableMultithreading;

    // Create worker thread
    if (m_Config.bEnableMtProcessing)
    {
        m_hProcessorWorkerThread = (HANDLE)_beginthreadex(NULL, 0, &ProcessorWorkerThreadProc, this, 0, &m_ProcessorWorkerThreadId);
        //SetThreadPriority(m_hProcessorWorkerThread, THREAD_PRIORITY_HIGHEST);
    }

    // Fill free frames pool
    {
        size_t queueCapacity = m_FreeFramesPool.GetCapacity();
        for (unsigned i = 0; i < queueCapacity; ++i)
        {
            TQsQueueItem item(new QsFrameData, new CQsAlignedBuffer(0));
            m_FreeFramesPool.PushBack(item, 0); // No need to wait - we know there's room
        }
    }

    // Init Media SDK decoder is done in OnSeek to allow late initialization needed
    // by full screen exclusive mode since D3D devices can't be created. The DS filter must send
    // the D3D device manager to this decoder for surface allocation.
    if (sts == MFX_ERR_NONE)
    {
        m_pDecoder->SetConfig(m_Config);
        m_pDecoder->SetAuxFramesCount(max(8, m_Config.nOutputQueueLength) + DECODE_QUEUE_LENGTH);
    }

    delete[] (mfxU8*)vih2;
    m_OK = (sts == MFX_ERR_NONE);
    return (m_OK) ? S_OK : E_FAIL;
}

void CQuickSync::SetAspectRatio(VIDEOINFOHEADER2& vih2, mfxFrameInfo& frameInfo)
{
    DWORD alignedWidth = (m_Config.bMod16Width) ? MSDK_ALIGN16(frameInfo.CropW) : frameInfo.CropW;
    
    // Fix small aspect ratio errors
    if (MSDK_ALIGN16(vih2.dwPictAspectRatioX) == alignedWidth)
    {
        vih2.dwPictAspectRatioX = alignedWidth;
    }

    // AR is not always contained in the header (it's optional for MFX decoder initialization though)
    if (frameInfo.AspectRatioW * frameInfo.AspectRatioH == 0)
    {
        // Convert display aspect ratio (is in VIDEOINFOHEADER2) to pixel aspect ratio (is accepted by MediaSDK components)
        mfxStatus sts = DARtoPAR(vih2.dwPictAspectRatioX, vih2.dwPictAspectRatioY, 
            frameInfo.CropW, frameInfo.CropH,
            frameInfo.AspectRatioW, frameInfo.AspectRatioH);

        if (sts != MFX_ERR_NONE)
        {
            frameInfo.AspectRatioW = frameInfo.AspectRatioH = 1;
        }
    }
}

HRESULT CQuickSync::Decode(IMediaSample* pSample)
{
    // DEBUG
#ifdef _DEBUG
    static bool isSetThreadName = false;
    if (!isSetThreadName) 
    {
        isSetThreadName = true;
        SetThreadName("*** Decoder ***");
    }
#endif
    
    // Input samples should be discarded - we are in the middle of a flush.
    if (m_bFlushing)
        return S_FALSE;
    
    CQsAutoLock cObjectLock(&m_csLock);

    MSDK_VTRACE("QSDcoder: Decode\n");

    // We haven't flushed since the last BeginFlush call - probably a DVD playback scenario
    // where NewSegment/OnSeek isn't issued.
    if (m_bNeedToFlush)
    {
        OnSeek(0);
    }

    // Deliver ready surfaces
    int sentFrames = (m_ProcessedFramesQueue.Full()) ? DeliverSurface(false) : 0;

    MSDK_CHECK_NOT_EQUAL(m_OK, true, E_UNEXPECTED);
    HRESULT hr = S_OK;
    mfxStatus sts = MFX_ERR_NONE;
    mfxFrameSurface1* pSurfaceOut = NULL;
    mfxBitstream mfxBS; 
    MSDK_ZERO_VAR(mfxBS);

    // Check error(s)
    if (NULL == pSample)
    {
        return S_OK;
    }

    if (0 == pSample->GetActualDataLength())
    {
        return S_FALSE;
    }

    // Manipulate bitstream for decoder
    sts = m_pFrameConstructor->ConstructFrame(pSample, &mfxBS);
    ASSERT(mfxBS.DataLength <= mfxBS.MaxLength);
    MSDK_CHECK_ERROR(sts, MFX_ERR_MORE_DATA, S_OK); // not an error
    MSDK_CHECK_NOT_EQUAL(sts, MFX_ERR_NONE, E_FAIL);
    bool flushed = false;

    // Decode mfxBitstream until all data is taken by decoder
    while (mfxBS.DataLength > 0 && !m_bNeedToFlush)
    {
        // Decode the bitstream
        sts = m_pDecoder->Decode(&mfxBS, m_Config.bEnableMtDecode, pSurfaceOut);                

        if (MFX_ERR_NONE == sts)
        {
            // Sync decode - pSurfaceOut holds decoded surface
            if (NULL != pSurfaceOut)
            {
                // Queue the frame for processing
                QueueSurface(pSurfaceOut, true);
            }
            // Async decode - flow continues on another thread
            else
            {
                // Make sure various queues are flushed (avoid deadlock)
                if (m_ProcessedFramesQueue.Full())
                {
                    sentFrames += DeliverSurface(false);
                }
            }

            continue;
        }
        else if (MFX_ERR_MORE_DATA == sts)
        {
            break; // Need to receive more data from upstream filter
        }
        else if (MFX_WRN_VIDEO_PARAM_CHANGED == sts)
        {
            MSDK_TRACE("QSDcoder: Decode MFX_WRN_VIDEO_PARAM_CHANGED\n");

            // Need to handle a possible change in the stream parameters
            if (!flushed)
            {
                sts = OnVideoParamsChanged();
                flushed = true;
            }

            continue; // Just continue processing
        }
        // Need another work surface
        else if (MFX_ERR_MORE_SURFACE == sts)
        {
            // Just continue, new work surface will be found in m_pDecoder->Decode
            continue;
        }
        else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts)
        {
            MSDK_TRACE("QSDcoder: Error - ran out of work buffers!\n");
            // This is a system malfunction!
            break;
        }
        else if (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM == sts)
        {
            MSDK_TRACE("QSDcoder: Decode MFX_ERR_INCOMPATIBLE_VIDEO_PARAM\n");

            // Flush existing frames
            Flush(true);

            // Retrieve new parameters
            mfxVideoParam VideoParams;
            MSDK_ZERO_VAR(VideoParams);
            VideoParams.mfx.CodecId = m_mfxParamsVideo.mfx.CodecId;
            sts = m_pDecoder->DecodeHeader(&mfxBS, &VideoParams); 
            if (MFX_ERR_MORE_DATA == sts)
            {
                break;
            }

            // Save IOPattern and update parameters
            VideoParams.IOPattern = m_mfxParamsVideo.IOPattern; 
            memcpy(&m_mfxParamsVideo, &VideoParams, sizeof(mfxVideoParam));
            m_nPitch = MSDK_ALIGN16(m_mfxParamsVideo.mfx.FrameInfo.Width);
            sts = m_pDecoder->Reset(&m_mfxParamsVideo, m_nPitch);
            if (MFX_ERR_NONE == sts)
            {
                continue;
            }

            MSDK_TRACE("QSDcoder: Decode didn't recover from MFX_ERR_INCOMPATIBLE_VIDEO_PARAM\n");
        }

        // The decoder has returned with an error
        hr = E_FAIL;
        break;
    }

    if (SUCCEEDED(hr) && !m_bNeedToFlush)
        m_pFrameConstructor->SaveResidialData(&mfxBS);
    
    MSDK_SAFE_DELETE_ARRAY(mfxBS.Data);

    // Deliver what's available
    if (sentFrames == 0 || m_ProcessedFramesQueue.Full())
    {
        DeliverSurface(false);
    }

    return hr;
}

int CQuickSync::DeliverSurface(bool bWaitForCompletion)
{
    MSDK_VTRACE("QSDcoder: DeliverSurface\n");

    TQsQueueItem item;
    int sentFrames = 0;

    // Wait for worker thread to complete a frame (should be used only when flushing)
    if (bWaitForCompletion)
    {
        while (!m_FreeFramesPool.Full() || !m_DecodedFramesQueue.Empty())
        {
            if (m_ProcessedFramesQueue.PopFront(item, 10))
            {
                if (!m_bNeedToFlush)
                {
                    QsFrameData* pFrameData = item.first;
                    // Send the surface out - return code from dshow filter is ignored.
                    MSDK_VTRACE("QSDcoder: DeliverSurfaceCallback (%I64d, %I64d)\n", pFrameData->rtStart, pFrameData->rtStop);
                    m_DeliverSurfaceCallback(m_ObjParent, pFrameData);
                    ++sentFrames;
                }

                m_FreeFramesPool.PushBack(item, 0); // No need to wait - we know there's room
            }
        }
    }
    else
    {
        if (m_ProcessedFramesQueue.PopFront(item, 0 /* don't wait, use what's ready*/))
        {
            if (!m_bNeedToFlush)
            {
                QsFrameData* pFrameData = item.first;
                // Send the surface out - return code from dshow filter is ignored.
                MSDK_VTRACE("QSDcoder: DeliverSurfaceCallback (%I64d, %I64d)\n", pFrameData->rtStart, pFrameData->rtStop);
                m_DeliverSurfaceCallback(m_ObjParent, pFrameData);
                ++sentFrames;
            }

            m_FreeFramesPool.PushBack(item, 0); // No need to wait - we know there's room
        }
    }

    return sentFrames;
}

void CQuickSync::PicStructToDsFlags(mfxU32 picStruct, DWORD& flags, QsFrameData::QsFrameStructure& frameStructure)
{
    // Note MSDK will never output fields
    frameStructure = (picStruct & MFX_PICSTRUCT_PROGRESSIVE) ?
        QsFrameData::fsProgressiveFrame :
        QsFrameData::fsInterlacedFrame;

    if (m_Config.bTimeStampCorrection && m_TimeManager.GetInverseTelecine())
    {
        flags = AM_VIDEO_FLAG_WEAVE;
        return;
    }

    // Progressive frame - note that sometimes interlaced content has the MFX_PICSTRUCT_PROGRESSIVE in combination with other flags
    if (picStruct == MFX_PICSTRUCT_PROGRESSIVE)
    {
        flags = AM_VIDEO_FLAG_WEAVE;
        return;
    }

    // Interlaced
    flags = 0;

    // Top field first
    if (picStruct & MFX_PICSTRUCT_FIELD_TFF)
    {
        flags |= AM_VIDEO_FLAG_FIELD1FIRST;
    }

    // Telecine flag
    if (picStruct & MFX_PICSTRUCT_FIELD_REPEATED)
    {
        flags |= AM_VIDEO_FLAG_REPEAT_FIELD;
    }
}

mfxStatus CQuickSync::ConvertFrameRate(mfxF64 dFrameRate, mfxU32& nFrameRateExtN, mfxU32& nFrameRateExtD)
{
    mfxU32 fr = (mfxU32)(dFrameRate + .5);
    if (fabs(fr - dFrameRate) < 0.0001) 
    {
        nFrameRateExtN = fr;
        nFrameRateExtD = 1;
        return MFX_ERR_NONE;
    }

    fr = (mfxU32)(dFrameRate * 1.001 + .5);

    if (fabs(fr * 1000 - dFrameRate * 1001) < 10) 
    {
        nFrameRateExtN = fr * 1000;
        nFrameRateExtD = 1001;
        return MFX_ERR_NONE;
    }

    nFrameRateExtN = (mfxU32)(dFrameRate * 10000 + .5);
    nFrameRateExtD = 10000;
    return MFX_ERR_NONE;    
}

HRESULT CQuickSync::Flush(bool deliverFrames)
{
    MSDK_TRACE("QSDcoder: Flush\n");

    CQsAutoLock cObjectLock(&m_csLock);

    HRESULT hr = S_OK;
    mfxStatus sts = MFX_ERR_NONE;

    // Recieved a BeginFlush that wasn't handled for some reason.
    // This overrides the request to output the remaining frames
    m_bNeedToFlush = m_bNeedToFlush || !deliverFrames;

    // Flush internal queue
    FlushOutputQueue();

    // Flush HW decoder by sending NULL bitstreams.
    while (MFX_ERR_NONE == sts || MFX_ERR_MORE_SURFACE == sts)
    {
        mfxFrameSurface1* pSurf = NULL;
        sts = m_pDecoder->Decode(NULL, false /* sync mode */, pSurf);

        if (MFX_ERR_NONE == sts && !m_bNeedToFlush)
        {
            QueueSurface(pSurf, false /* syncronous*/);
            if (FAILED(hr))
                break;
        }
    }

    // Flush internal queue again
    FlushOutputQueue();
    
    m_TimeManager.Reset();

    // All data has been flushed
    m_bNeedToFlush = false;

    MSDK_TRACE("QSDcoder: Flush ended\n");
    return hr;
}

void CQuickSync::FlushOutputQueue()
{
    // Wait for async decoder to become available
    m_pDecoder->WaitForDecoder();

    // Note that m_bNeedToFlush can be changed by another thread at any time
    // Make sure worker thread has completed all it's tasks
    if (m_bNeedToFlush)
    {
        ClearQueue();
    }

    DeliverSurface(true);

    MSDK_TRACE("QSDcoder: FlushOutputQueue (deliverFrames=%s)\n", (!m_bNeedToFlush) ? "TRUE" : "FALSE");
    for (size_t i = m_pDecoder->OutputQueueSize(); i > 0; --i)
    {
        QueueSurface(NULL, false);
    }

    // Clear the internal frame queue - either failure in flushing or no need to deliver the frames.
    while (!m_pDecoder->OutputQueueEmpty())
    {
        // Surface is not needed anymore
        m_pDecoder->UnlockSurface(PopSurface());
    }

    ASSERT(m_pDecoder->OutputQueueEmpty());
    ASSERT(m_ProcessedFramesQueue.Empty());
    ASSERT(m_FreeFramesPool.Full());
}

void CQuickSync::ClearQueue()
{
    TQsQueueItem item;
    m_bNeedToFlush = true;

    // Loop until free frames pool is full
    while (!m_FreeFramesPool.Full())
    {
        if (m_ProcessedFramesQueue.PopFront(item, 10))
        {
            m_FreeFramesPool.PushBack(item, 0);  // No need to wait - we know there's room
        }
    }
}

HRESULT CQuickSync::OnSeek(REFERENCE_TIME segmentStart)
{
    MSDK_TRACE("QSDcoder: OnSeek\n");

    CQsAutoLock cObjectLock(&m_csLock);

    m_bNeedToFlush = true;
    mfxStatus sts = MFX_ERR_NONE;
    m_nSegmentFrameCount = 0;

    // Make sure the worker thread is idle and released all released all resources
    FlushOutputQueue();

    if (m_pDecoder)
    {
        m_TimeManager.Reset();
        sts = m_pDecoder->Reset(&m_mfxParamsVideo, m_nPitch);
        if (sts != MFX_ERR_NONE)
        {
            MSDK_TRACE("QSDcoder: reset failed!\n");
            return E_FAIL;
        }
        
        m_pFrameConstructor->Reset();
        FlushOutputQueue();
    }

    m_bNeedToFlush = false;
    MSDK_TRACE("QSDcoder: OnSeek complete\n");
    return (sts == MFX_ERR_NONE) ? S_OK : E_FAIL;
}

bool CQuickSync::SetTimeStamp(mfxFrameSurface1* pSurface, QsFrameData& frameData)
{
    // Find corrected time stamp
    if (m_Config.bTimeStampCorrection)
    {
        TFrameVector frames;
        frames.push_back(pSurface);
        auto queue = m_pDecoder->GetOutputQueue();
        frames.insert(frames.end(), queue.begin(), queue.end());
        if (!m_TimeManager.GetSampleTimeStamp(frames, frameData.rtStart, frameData.rtStop))
            return false;
    
        return frameData.rtStart >= 0;
    }

    // Just convert the time stamp from the HW decoder
    frameData.rtStart = m_TimeManager.ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
    frameData.rtStop = (INVALID_REFTIME == frameData.rtStart) ? INVALID_REFTIME : frameData.rtStart + 1;
    return true; // Return all frames DS filter will handle this
}

mfxStatus CQuickSync::OnVideoParamsChanged()
{    
    mfxVideoParam params;
    MSDK_ZERO_VAR(params);
    mfxStatus sts = m_pDecoder->GetVideoParams(&params);
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

    mfxFrameInfo& curInfo = m_mfxParamsVideo.mfx.FrameInfo;
    mfxFrameInfo& newInfo = params.mfx.FrameInfo;

    bool bFrameRateChange = (curInfo.FrameRateExtN != newInfo.FrameRateExtN) || (curInfo.FrameRateExtD != newInfo.FrameRateExtD);

    if (!bFrameRateChange)
        return MFX_ERR_NONE;

    // Copy video params
    params.AsyncDepth = m_mfxParamsVideo.AsyncDepth;
    params.IOPattern = m_mfxParamsVideo.IOPattern;
    memcpy(&m_mfxParamsVideo, &params, sizeof(params));

    // Flush images with old parameters
    FlushOutputQueue();

    double frameRate = (double)curInfo.FrameRateExtN / (double)curInfo.FrameRateExtD;
    m_TimeManager.OnVideoParamsChanged(frameRate);
    return MFX_ERR_NONE;
}

void CQuickSync::PushSurface(mfxFrameSurface1* pSurface)
{
    m_pDecoder->PushSurface(pSurface);

    // Note - no need to lock as all API functions are already exclusive.
    if (m_Config.bTimeStampCorrection)
    {
        m_TimeManager.AddOutputTimeStamp(pSurface);
    }
}

mfxFrameSurface1* CQuickSync::PopSurface()
{
    return m_pDecoder->PopSurface();
}

HRESULT CQuickSync::CheckCodecProfileSupport(DWORD codec, DWORD profile, DWORD level)
{
    // H264
    if (codec == MFX_CODEC_AVC)
    {
        switch (profile)
        {
        case QS_PROFILE_H264_BASELINE:
        case QS_PROFILE_H264_CONSTRAINED_BASELINE:
        case QS_PROFILE_H264_MAIN:
        case QS_PROFILE_H264_HIGH:
            return S_OK;
        default:
            return E_NOTIMPL;
        }
    }

    // MPEG2
    if (codec == MFX_CODEC_MPEG2)
    {
        switch (profile)
        {
        case QS_PROFILE_MPEG2_SIMPLE:
        case QS_PROFILE_MPEG2_MAIN:
        case QS_PROFILE_MPEG2_HIGH:
        case QS_PROFILE_MPEG2_SPATIALLY_SCALABLE:
            return S_OK;
        default:
            return E_NOTIMPL;
        }
    }

    // VC1/WMV
    if (codec == MFX_CODEC_VC1)
    {
        switch (profile)
        {
        case QS_PROFILE_VC1_SIMPLE:
        case QS_PROFILE_VC1_MAIN:
            return S_FALSE;
        case QS_PROFILE_VC1_ADVANCED:
            return S_OK;
        default:
            return E_NOTIMPL;
        }
    }

    return E_NOTIMPL;
}

void CQuickSync::SetD3DDeviceManager(IDirect3DDeviceManager9* pDeviceManager)
{
    m_pDecoder->SetD3DDeviceManager(pDeviceManager);
}

void CQuickSync::GetConfig(CQsConfig* pConfig)
{
    if (NULL == pConfig)
        return;

    *pConfig = m_Config;
}

void CQuickSync::SetConfig(CQsConfig* pConfig)
{
    if (NULL == pConfig)
        return;

    if (m_bInitialized)
        return;

    m_Config = *pConfig;
}

unsigned CQuickSync::ProcessorWorkerThreadProc(void* pThis)
{
    ASSERT(pThis != NULL);
    return static_cast<CQuickSync*>(pThis)->ProcessorWorkerThreadMsgLoop();
}


unsigned CQuickSync::ProcessorWorkerThreadMsgLoop()
{
#ifdef _DEBUG
    SetThreadName("*** QS processor WT ***");
#endif

    mfxFrameSurface1* pSurface = NULL;
    while (m_DecodedFramesQueue.PopFront(pSurface, INFINITE))
    {
        if (NULL == pSurface)
        {
            break;
        }
        
        ProcessDecodedFrame(pSurface);
    }

    // All's well
    return 0;
}

// This function queues decoded frame for processing.
// Only called when decoding is not async.
HRESULT CQuickSync::QueueSurface(mfxFrameSurface1* pSurface, bool async)
{
    // The surface is locked from being reused in another Decode call
    if (NULL != pSurface)
    {
        m_pDecoder->LockSurface(pSurface);
    }

    if (async && m_Config.bEnableMtProcessing)
    {
        // Post message to worker thread
        m_DecodedFramesQueue.PushBack(pSurface, INFINITE);
        DeliverSurface(false /* don't wait deliver what's ready */);
    }
    // MT frame processing is disabled
    else
    {
        ProcessDecodedFrame(pSurface);
        DeliverSurface(true);
    }

    return S_OK;
}

// This function works on a worker thread
HRESULT CQuickSync::ProcessDecodedFrame(mfxFrameSurface1* pSurface)
{
    MSDK_VTRACE("QSDcoder: ProcessDecodedFrame\n");

    // Got a new surface. A NULL surface means to get a surface from the output queue
    if (pSurface != NULL)
    {
        // Initial frames with invalid times are discarded
        REFERENCE_TIME rtStart = m_TimeManager.ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
        if (m_pDecoder->OutputQueueEmpty() && rtStart == INVALID_REFTIME && m_Config.bTimeStampCorrection)
        {
            m_pDecoder->UnlockSurface(pSurface);
            return S_OK;
        }

        PushSurface(pSurface);

        // Not enough surfaces for proper time stamp correction
        size_t queueSize = (m_bDvdDecoding) ? 0 : m_Config.nOutputQueueLength;
        if (m_pDecoder->OutputQueueSize() <= queueSize)
        {
            return S_OK;
        }
    }

    // Get oldest surface from queue
    pSurface = PopSurface();
    MSDK_CHECK_POINTER(pSurface, S_OK); // Decoder queue is empty - return without error
    
    TQsQueueItem item;
    while (!m_FreeFramesPool.PopFront(item, 10))
    {
//        static int count = 0;
//        MSDK_TRACE("** waiting for m_FreeFramesPool %d\n", ++count);

        // A flush event has just occurred - abort frame processing
        if (m_bNeedToFlush)
        {
            m_pDecoder->UnlockSurface(pSurface);
            return S_OK;
        }
    }

    QsFrameData* pOutFrameData = item.first;
    QsFrameData& outFrameData = *pOutFrameData;
    CQsAlignedBuffer*& pOutBuffer = item.second;

    // Clear the outFrameData
    MSDK_ZERO_VAR(outFrameData);

    UpdateAspectRatio(pSurface, outFrameData);
    outFrameData.fourCC = pSurface->Info.FourCC;

    if (pSurface->Data.Corrupted)
    {
        // Do something with a corrupted surface?
        outFrameData.bCorrupted = true;
        MSDK_TRACE("QsDecoder: warning received a corrupted frame\n");
    }

    ++m_nSegmentFrameCount;
     
    // Result is in m_FrameData
    // False return value means that the frame has a negative time stamp and should not be displayed.
    if (!SetTimeStamp(pSurface, outFrameData))
    {
        // Return frame to free pool
        m_FreeFramesPool.PushBack(item, 0);
        m_pDecoder->UnlockSurface(pSurface);
        return S_OK;
    }

    // Setup interlacing info
    PicStructToDsFlags(pSurface->Info.PicStruct, outFrameData.dwInterlaceFlags, outFrameData.frameStructure);
    outFrameData.bFilm = 0 != (outFrameData.dwInterlaceFlags & AM_VIDEO_FLAG_REPEAT_FIELD);
    
    // TODO: find actual frame type I/P/B
    // Media sample isn't reliable as it referes to an older frame!
    outFrameData.frameType = QsFrameData::I;

    // Obtain surface data and copy it to temp buffer.
    mfxFrameData frameData;
    m_pDecoder->LockFrame(pSurface, &frameData);
    
    // Setup output buffer
    outFrameData.dwStride = frameData.Pitch;
    size_t outSize = 4096 + // Adding 4K for page alignment optimizations
        outFrameData.dwStride * pSurface->Info.CropH * 3 / 2;

    // Make ure we have a buffer with the right size
    if (pOutBuffer->GetBufferSize() < outSize)
    {
        delete pOutBuffer;
        pOutBuffer = new CQsAlignedBuffer(outSize);
    }

    size_t height = pSurface->Info.CropH; // Cropped image height
    size_t pitch  = frameData.Pitch;      // Image line + padding in bytes --> set by the driver

    // Fill image size
    outFrameData.rcFull.top    = outFrameData.rcFull.left = 0;
    outFrameData.rcFull.bottom = (LONG)height - 1;
    outFrameData.rcFull.right  = MSDK_ALIGN16(pSurface->Info.CropW + pSurface->Info.CropX) - 1;
    outFrameData.rcClip.top    = 0;
    outFrameData.rcClip.bottom = (LONG)height - 1; // Height is not padded in output buffer

    if (m_Config.bMod16Width)
    {
        outFrameData.rcClip = outFrameData.rcFull;
    }
    else
    {
        // Note that we always crop the height
        outFrameData.rcClip.left  = pSurface->Info.CropX;
        outFrameData.rcClip.right = pSurface->Info.CropW + pSurface->Info.CropX - 1;
    }

    // Offset output buffer's address for fastest SSE4.1 copy.
    // Page offset (12 lsb of addresses) sould be 2K apart from source buffer
    size_t offset = ((size_t)frameData.Y & PAGE_MASK) ^ (1 << 11);
    
    // Mark Y, U & V pointers on output buffer
    outFrameData.y = pOutBuffer->GetBuffer() + offset;
    outFrameData.u = outFrameData.y + (pitch * height);
    outFrameData.v = 0;
    outFrameData.a = 0;

    // App can modify this buffer
    outFrameData.bReadOnly = false;
#if 1 // Use this to disable actual copying for benchmarking
    Tmemcpy memcpyFunc = (m_pDecoder->IsD3DAlloc()) ?
        ( (m_Config.bEnableMtCopy) ? mt_gpu_memcpy : gpu_memcpy ) :
        ( (m_Config.bEnableMtCopy) ? mt_memcpy     : memcpy );

    // Copy Y
    !m_bNeedToFlush && memcpyFunc(outFrameData.y, frameData.Y + (pSurface->Info.CropY * pitch), height * pitch);

    // Copy UV
    !m_bNeedToFlush && memcpyFunc(outFrameData.u, frameData.CbCr + (pSurface->Info.CropY * pitch), pitch * height / 2);
#endif

    // Unlock the frame
    m_pDecoder->UnlockFrame(pSurface, &frameData);

    // We don't need the surface anymore
    m_pDecoder->UnlockSurface(pSurface);

#ifdef _DEBUG
    // Debug only - mark top left corner when working with D3D
    ULONGLONG markY = 0;
    ULONGLONG markUV = (m_pDecoder->IsHwAccelerated()) ? 0x80FF80FF80FF80FF : 0xFF80FF80FF80FF80;
    *((ULONGLONG*)outFrameData.y) = markY;
    *((ULONGLONG*)(outFrameData.y + outFrameData.dwStride)) = markY;
    *((ULONGLONG*)(outFrameData.y + 2 * outFrameData.dwStride)) = markY;
    *((ULONGLONG*)(outFrameData.y + 3 * outFrameData.dwStride)) = markY;
    *((ULONGLONG*)outFrameData.u) = markUV; // 4 blue (hw) or red (sw)
    *((ULONGLONG*)(outFrameData.u + outFrameData.dwStride)) = markUV;
#endif

    // Keep or drop the processed frame
    if (!m_bNeedToFlush)
    {
        // Enter result in the processed queue
        m_ProcessedFramesQueue.PushBack(item, 0);
    }
    else
    {
        // Return to free pool
        m_FreeFramesPool.PushBack(item, 0);
    }

    MSDK_VTRACE("QSDcoder: ProcessDecodedFrame completed\n");
    return S_OK;
}

void CQuickSync::UpdateAspectRatio(mfxFrameSurface1* pSurface, QsFrameData& frameData)
{
    mfxFrameInfo& info = pSurface->Info;
    DWORD alignedWidth = (m_Config.bMod16Width) ? MSDK_ALIGN16(info.CropW) : info.CropW;
    PARtoDAR(info.AspectRatioW, info.AspectRatioH, alignedWidth, info.CropH,
        frameData.dwPictAspectRatioX, frameData.dwPictAspectRatioY);
}

void CQuickSync::OnDecodeComplete(mfxFrameSurface1* pSurface, void* obj)
{
    // Note - this function is called from the decode thread.
    CQuickSync* pThis = (CQuickSync*)obj;
    ASSERT(pThis != NULL);

    // Post message to worker thread
    pThis->m_DecodedFramesQueue.PushBack(pSurface, INFINITE);
}
