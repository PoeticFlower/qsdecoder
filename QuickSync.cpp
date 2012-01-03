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
#define TM_PROCESS_FRAME (WM_USER + 1)

EXTERN_GUID(WMMEDIASUBTYPE_WVC1, 
0x31435657, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71); 
EXTERN_GUID(WMMEDIASUBTYPE_WMV3, 
0x33564D57, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71); 

DEFINE_GUID(WMMEDIASUBTYPE_WVC1_PDVD,
0xD979F77B, 0xDBEA, 0x4BF6, 0x9E, 0x6D, 0x1D, 0x7E, 0x57, 0xFB, 0xAD, 0x53);

////////////////////////////////////////////////////////////////////
//                      CQuickSync
////////////////////////////////////////////////////////////////////
CQuickSync::CQuickSync() :
    m_nPitch(0),
    m_pFrameConstructor(NULL),
    m_nSegmentFrameCount(0),
    m_bFlushing(false),
    m_bNeedToFlush(false),
    m_bForceOutput(false),
    m_bDvdDecoding(false),
    m_hWorkerThread(NULL),
    m_WorkerThreadId(0),
    m_WorkerThreadIsRunning(false)
{
    MSDK_TRACE("QSDcoder: Constructor\n");

    mfxStatus sts = MFX_ERR_NONE;
    m_pDecoder = new CQuickSyncDecoder(sts);

    MSDK_ZERO_VAR(m_mfxParamsVideo);
    //m_mfxParamsVideo.AsyncDepth = 1; //causes issues when 1
    m_mfxParamsVideo.mfx.ExtendedPicStruct = 1;

    // Set default configuration - override what's not zero
    m_Config.bMod16Width = false;
    m_Config.bTimeStampCorrection = true;

    m_Config.nOutputQueueLength = (m_Config.bTimeStampCorrection) ? 8 : 0;
    m_Config.bEnableH264  = true;
    m_Config.bEnableMPEG2 = true;
    m_Config.bEnableVC1   = true;
    m_Config.bEnableWMV9  = true;

    m_Config.bEnableMultithreading = true;

    // Currently not working well - menu decoding :(
    m_Config.bEnableDvdDecoding = false;

    m_OK = (sts == MFX_ERR_NONE);
}

CQuickSync::~CQuickSync()
{
    MSDK_TRACE("QSDcoder: Destructor\n");
    
    // This will quicken the exit 
    m_bNeedToFlush = true;

    CQsAutoLock cObjectLock(&m_csLock);

    if (m_WorkerThreadId)
    {
        PostThreadMessage(m_WorkerThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(m_hWorkerThread, INFINITE);
    }

    while (!m_FreeFramesPool.Empty())
    {
        TQsQueueItem item = m_FreeFramesPool.Pop();
        delete item.second;
    }

    MSDK_SAFE_DELETE(m_pFrameConstructor);
    MSDK_SAFE_DELETE(m_pDecoder);
}

HRESULT CQuickSync::HandleSubType(const GUID& subType, FOURCC fourCC)
{
    MSDK_SAFE_DELETE(m_pFrameConstructor);

    //MPEG2
    if (subType == MEDIASUBTYPE_MPEG2_VIDEO)//(fourCC == FOURCC_mpg2) || (fourCC == FOURCC_MPG2))
    {
        if (!m_Config.bEnableMPEG2)
            return VFW_E_INVALIDMEDIATYPE;

        m_mfxParamsVideo.mfx.CodecId = MFX_CODEC_MPEG2;
        m_pFrameConstructor = new CFrameConstructor;
        return S_OK;
    }
    
    //VC1
    if ((fourCC == FOURCC_VC1) || (fourCC == FOURCC_WMV3))
    {
        m_mfxParamsVideo.mfx.CodecId = MFX_CODEC_VC1;

        if (subType == WMMEDIASUBTYPE_WMV3)
        {
            m_mfxParamsVideo.mfx.CodecProfile = MFX_PROFILE_VC1_SIMPLE;        
            if (!m_Config.bEnableWMV9)
                return VFW_E_INVALIDMEDIATYPE;
        }   
        else if (fourCC == FOURCC_VC1)
        {
            m_mfxParamsVideo.mfx.CodecProfile = MFX_PROFILE_VC1_ADVANCED;
            if (!m_Config.bEnableVC1)
                return VFW_E_INVALIDMEDIATYPE;
        }
        else
        {
            return VFW_E_INVALIDMEDIATYPE;
        }

        m_pFrameConstructor = new CVC1FrameConstructor();
        return S_OK;
    }

    // H264
    if ((fourCC == FOURCC_H264) || (fourCC == FOURCC_X264) || (fourCC == FOURCC_h264) ||
        (fourCC == FOURCC_avc1) || (fourCC == FOURCC_VSSH) || (fourCC == FOURCC_DAVC) ||
        (fourCC == FOURCC_PAVC) || (fourCC == FOURCC_AVC1))
    {
        if (!m_Config.bEnableH264)
            return VFW_E_INVALIDMEDIATYPE;

        m_mfxParamsVideo.mfx.CodecId = MFX_CODEC_AVC;

        m_pFrameConstructor = ((fourCC == FOURCC_avc1) || (fourCC == FOURCC_AVC1)) ?
            new CAVCFrameConstructor :
            new CFrameConstructor;

        return S_OK;
    }

    return E_UNEXPECTED;
}

HRESULT CQuickSync::TestMediaType(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC)
{
    MSDK_TRACE("QSDcoder: TestMediaType\n");
    if (!m_OK)
        return E_FAIL;

    // Parameter check
    MSDK_CHECK_POINTER(mtIn, E_POINTER);
    MSDK_CHECK_POINTER(mtIn->pbFormat, E_UNEXPECTED);

    // disable DVD playback until it's OK
    if (mtIn->majortype == MEDIATYPE_DVD_ENCRYPTED_PACK)
        return E_FAIL;
    else if (mtIn->majortype != MEDIATYPE_Video)
        return E_FAIL;

    HRESULT hr;
    VIDEOINFOHEADER2* vih2 = NULL;
    const GUID& subType = mtIn->subtype;
    hr = HandleSubType(subType, fourCC);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);
    const GUID& guidFormat = mtIn->formattype;

    size_t nSampleSize, nVideoInfoSize;
    hr = CopyMediaTypeToVIDEOINFOHEADER2(mtIn, vih2, nVideoInfoSize, nSampleSize);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);

    mfxVideoParam videoParams;
    MSDK_ZERO_VAR(videoParams);
    mfxInfoMFX& mfx = m_mfxParamsVideo.mfx;

    // check if codec profile and level are supported before calling DecodeHeader
    if (FORMAT_MPEG2_VIDEO == guidFormat)
    {
        MPEG2VIDEOINFO* mp2 = (MPEG2VIDEOINFO*)(mtIn->pbFormat);
        hr = CheckCodecProfileSupport(mfx.CodecId, mp2->dwProfile, mp2->dwLevel);
    }

    delete[] (mfxU8*)vih2;
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

        // copy the sample as is
        memcpy(vih2, mtIn->pbFormat, nSampleSize);
    }
    // translate VIDEOINFOHEADER to VIDEOINFOHEADER2 (easier to work with down the road)
    else if (FORMAT_VideoInfo == guidFormat)
    {
        size_t extraDataLen = mtIn->cbFormat - sizeof(VIDEOINFOHEADER);
        nSampleSize = sizeof(VIDEOINFOHEADER2) + extraDataLen;
        vih2 = (VIDEOINFOHEADER2*) new mfxU8[nSampleSize];
        MSDK_ZERO_MEMORY(vih2, sizeof(VIDEOINFOHEADER2));

        // initialize the VIDEOINFOHEADER2 structure
        VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)(mtIn->pbFormat);
        vih2->rcSource           = pvi->rcSource;
        vih2->rcTarget           = pvi->rcTarget;
        vih2->dwBitRate          = pvi->dwBitRate;
        vih2->dwBitErrorRate     = pvi->dwBitErrorRate;
        vih2->bmiHeader          = pvi->bmiHeader;
        vih2->AvgTimePerFrame    = pvi->AvgTimePerFrame;
        vih2->dwPictAspectRatioX = pvi->bmiHeader.biWidth;
        vih2->dwPictAspectRatioY = pvi->bmiHeader.biHeight;

        // copy the out of band data (data past the VIDEOINFOHEADER structure.
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

HRESULT CQuickSync::InitDecoder(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC)
{
    MSDK_TRACE("QSDcoder: InitDecoder\n");

    CQsAutoLock cObjectLock(&m_csLock);

    VIDEOINFOHEADER2* vih2 = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    mfxInfoMFX& mfx = m_mfxParamsVideo.mfx;
    HRESULT hr;
    MSDK_CHECK_POINTER(mtIn, E_POINTER);
    MSDK_CHECK_POINTER(mtIn->pbFormat, E_UNEXPECTED);
    bool bIsFields = false;

    if (!(mtIn->majortype == MEDIATYPE_DVD_ENCRYPTED_PACK || mtIn->majortype == MEDIATYPE_Video))
        return VFW_E_INVALIDMEDIATYPE;

    const GUID& guidFormat = mtIn->formattype;
    const GUID& subType = mtIn->subtype;
    hr = HandleSubType(subType, fourCC);
    MSDK_CHECK_RESULT_P_RET(hr, S_OK);

    size_t nSampleSize, nVideoInfoSize;
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

    // check if codec profile and level are supported before calling DecodeHeader
    if (FORMAT_MPEG2_VIDEO == guidFormat)
    {
        MPEG2VIDEOINFO* mp2 = (MPEG2VIDEOINFO*)(mtIn->pbFormat);
        hr = CheckCodecProfileSupport(mfx.CodecId, mp2->dwProfile, mp2->dwLevel);
        if (hr != S_OK)
        {
            MSDK_TRACE("QSDcoder::InitDecoder - failed due to unsupported codec (%s), profile (%s), level (%i) combination\n",
                GetCodecName(mfx.CodecId), GetProfileName(mfx.CodecId, mp2->dwProfile), mp2->dwLevel);
            sts = MFX_ERR_UNSUPPORTED;
            goto done;
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

    // construct sequence header and decode the sequence header.
    if (nVideoInfoSize < nSampleSize)
    {
        sts = m_pFrameConstructor->ConstructHeaders(vih2, guidFormat, nSampleSize, nVideoInfoSize);
        if (MFX_ERR_NONE == sts)
        {
            sts = m_pDecoder->DecodeHeader(&m_pFrameConstructor->GetHeaders(), &m_mfxParamsVideo);
        }
    }
    // Simple info header (without extra data) or DecodeHeader failed (usually not critical)
    else
    {
        mfx.FrameInfo.CropW        = (mfxU16)vih2->bmiHeader.biWidth;
        if (m_Config.bMod16Width)
        {
            mfx.FrameInfo.CropW = MSDK_ALIGN16(mfx.FrameInfo.CropW);
        }

        mfx.FrameInfo.CropH        = (mfxU16)MSDK_ALIGN16(vih2->bmiHeader.biHeight);
        mfx.FrameInfo.Width        = mfx.FrameInfo.CropW;
        mfx.FrameInfo.Height       = mfx.FrameInfo.CropH;
        mfx.FrameInfo.FourCC       = MFX_FOURCC_NV12;
        mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        ConvertFrameRate(m_TimeManager.GetFrameRate(), 
            mfx.FrameInfo.FrameRateExtN, 
            mfx.FrameInfo.FrameRateExtD);
    }

    ASSERT(sts == MFX_ERR_NONE);

    // setup frame rate from either the media type or the decoded header
    bIsFields = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) &&
        (vih2->dwInterlaceFlags & AMINTERLACE_1FieldPerSample);
    // Try getting the frame rate from the decoded headers
    if (0 < vih2->AvgTimePerFrame)
    {
        m_TimeManager.SetFrameRate(1e7 / vih2->AvgTimePerFrame, bIsFields);
    }
    else if (mfx.FrameInfo.FrameRateExtN * mfx.FrameInfo.FrameRateExtD != 0)
    {
        double frameRate = (double)mfx.FrameInfo.FrameRateExtN / (double)mfx.FrameInfo.FrameRateExtD;
        m_TimeManager.SetFrameRate(frameRate, bIsFields);
    }

    // we might decode well even if DecodeHeader failed with MFX_ERR_MORE_DATA
    MSDK_IGNORE_MFX_STS(sts, MFX_ERR_MORE_DATA);

    m_nPitch = mfx.FrameInfo.Width;

    SetAspectRatio(*vih2, mfx.FrameInfo);

    // Init Media SDK decoder is done in OnSeek to allow late initialization needed
    // by full screen exclusive mode since D3D devices can't be created. The DS filter must send
    // the D3D device manager to this decoder for surface allocation.
    if (sts == MFX_ERR_NONE)
    {
        m_pDecoder->SetConfig(m_Config);
    }

    // This is constant
    m_FrameDataTemplate.fourCC = FOURCC_NV12;

    // Create worker thread
    if (m_Config.bEnableMultithreading)
    {
        m_hWorkerThread = (HANDLE)_beginthreadex(NULL, 0, &WorkerThreadProc, this, 0, &m_WorkerThreadId);
    }

    // Fill free frames pool
    {
        size_t queueCapacity = 4;    
        m_FreeFramesPool.SetCapacity(queueCapacity);
        m_ProcessedFramesQueue.SetCapacity(queueCapacity);
        for (unsigned i = 0; i < queueCapacity; ++i)
        {
            TQsQueueItem item(m_FrameDataTemplate, new CQsAlignedBuffer(0));
            m_FreeFramesPool.Push(item);
        }
    }

done:
    delete[] (mfxU8*)vih2;
    m_OK = (sts == MFX_ERR_NONE);
    return (m_OK) ? S_OK : E_FAIL;
}

void CQuickSync::SetAspectRatio(VIDEOINFOHEADER2& vih2, mfxFrameInfo& frameInfo)
{
    DWORD alignedWidth = (m_Config.bMod16Width) ? MSDK_ALIGN16(frameInfo.CropW) : frameInfo.CropW;
    
    // fix small aspect ratio errors
    if (MSDK_ALIGN16(vih2.dwPictAspectRatioX) == alignedWidth)
    {
        vih2.dwPictAspectRatioX = alignedWidth;
    }

    // AR is not always contained in the header (it's optional for MFX decoder initialization though)
    if (frameInfo.AspectRatioW * frameInfo.AspectRatioH == 0)
    {
        //convert display aspect ratio (is in VIDEOINFOHEADER2) to pixel aspect ratio (is accepted by MediaSDK components)
        mfxStatus sts = DARtoPAR(vih2.dwPictAspectRatioX, vih2.dwPictAspectRatioY, 
            frameInfo.CropW, frameInfo.CropH,
            frameInfo.AspectRatioW, frameInfo.AspectRatioH);

        if (sts != MFX_ERR_NONE)
        {
            frameInfo.AspectRatioW = frameInfo.AspectRatioH = 1;
        }
    }

    // use image size as aspect ratio when PAR is 1x1
    if (frameInfo.AspectRatioW == frameInfo.AspectRatioH)
    {
        m_FrameDataTemplate.dwPictAspectRatioX = alignedWidth;
        m_FrameDataTemplate.dwPictAspectRatioY = frameInfo.CropH;
    }
    else
    {
        m_FrameDataTemplate.dwPictAspectRatioX = vih2.dwPictAspectRatioX;
        m_FrameDataTemplate.dwPictAspectRatioY = vih2.dwPictAspectRatioY;
    }
}

HRESULT CQuickSync::Decode(IMediaSample* pSample)
{
    // DEBUG
#if 0 //def _DEBUG
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

    // Wait for worker thread to become less busy (free pool not empty)
    while (m_FreeFramesPool.Empty())
    {
        if (m_bNeedToFlush)
        {
            return S_OK;
        }

        Sleep(1);
    }

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
        sts = m_pDecoder->Decode(&mfxBS, pSurfaceOut);                

        if (MFX_ERR_NONE == sts)
        {
            // Lock the surface so it will not be used by decoder->FindFreeSurface
            m_pDecoder->LockSurface(pSurfaceOut);
            QueueSurface(pSurfaceOut);
            continue;
        }
        else if (MFX_ERR_MORE_DATA == sts)
        {
            break; // Need to receive more data from upstream filter
        }
        else if (MFX_WRN_VIDEO_PARAM_CHANGED == sts)
        {
            MSDK_TRACE("QSDcoder: Decode MFX_WRN_VIDEO_PARAM_CHANGED\n");

            // Need to reset the analysis done so far...

            if (!flushed)
            {
                sts = OnVideoParamsChanged();
//                Flush(true);
                flushed = true;
            }

            continue; // just continue processing
        }
        // Need another work surface
        else if (MFX_ERR_MORE_SURFACE == sts)
        {
            // Just continue, new work surface will be found in m_pDecoder->Decode
            continue;
        }
        else if (MFX_ERR_NOT_ENOUGH_BUFFER == sts)
        {
            MSDK_TRACE("QSDcoder: Decode MFX_ERR_NOT_ENOUGH_BUFFER\n");
            if (!m_pDecoder->OutputQueueEmpty())
            {
                FlushOutputQueue(true);
                continue;
            }

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

    // Deliver the samres we already processed
    DeliverSurface(false);
    return hr;
}

HRESULT CQuickSync::DeliverSurface(bool bWaitForCompletion)
{
    MSDK_VTRACE("QSDcoder: DeliverSurface\n");

    // Output a free if any are available or wait wait for one to become available if reuqested (bWaitForCompletion)
    while (!m_ProcessedFramesQueue.Empty() ||
        (bWaitForCompletion && m_FreeFramesPool.HasCapacity()))
    {
        if (!m_ProcessedFramesQueue.Empty())
        {
            TQsQueueItem item = m_ProcessedFramesQueue.Pop();
            ASSERT(item.second);
            QsFrameData& pFrameData = item.first;

            if (!m_bNeedToFlush)
            {
                // Send the surface out - return code from dshow filter is ignored.
                MSDK_VTRACE("QSDcoder: DeliverSurfaceCallback (%I64d, %I64d)\n", pFrameData.rtStart, pFrameData.rtStop);
                m_DeliverSurfaceCallback(m_ObjParent, &pFrameData);
            }

            m_FreeFramesPool.Push(item);
        }
        else
        {
            Sleep(1);
        }

        if (m_bNeedToFlush)
        {
            return S_OK;
        }
    }

    return S_OK;
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
    deliverFrames = deliverFrames && !m_bNeedToFlush;
    m_bForceOutput = deliverFrames;

    // Flush internal queue
    FlushOutputQueue(deliverFrames);

    // flush HW decoder by sending NULL bitstreams.
    while (MFX_ERR_NONE == sts || MFX_ERR_MORE_SURFACE == sts)
    {
        mfxFrameSurface1* pSurf = NULL;
        sts = m_pDecoder->Decode(NULL, pSurf);

        if (MFX_ERR_NONE == sts && deliverFrames && !m_bNeedToFlush)
        {
            QueueSurface(pSurf);
            DeliverSurface(false);
            if (FAILED(hr))
                break;
        }
    }

    // Flush internal queue again
    FlushOutputQueue(deliverFrames);
    
    m_TimeManager.Reset();
    m_bForceOutput = false;

    // all data has been flushed
    m_bNeedToFlush = false;

    MSDK_TRACE("QSDcoder: Flush ended\n");
    return hr;
}

void CQuickSync::FlushOutputQueue(bool deliverFrames)
{
    bool bForceOutputSave = m_bForceOutput;
    m_bForceOutput = deliverFrames && !m_bNeedToFlush;

    // Note that m_bNeedToFlush can be changed by  another thread at any time
    if (!deliverFrames || m_bNeedToFlush)
    {
        ClearQueue();
    }

    MSDK_TRACE("QSDcoder: FlushOutputQueue (deliverFrames=%s)\n", (m_bForceOutput) ? "TRUE" : "FALSE");

    while (!m_pDecoder->OutputQueueEmpty() && deliverFrames && !m_bNeedToFlush)
    {
        QueueSurface(NULL);
        DeliverSurface(deliverFrames && !m_bNeedToFlush);
    }

    // Clear the internal frame queue - either failure in flushing or no need to deliver the frames.
    while (!m_pDecoder->OutputQueueEmpty())
    {
        mfxFrameSurface1* pSurface = PopSurface();

        // Surface is not needed anymore
        m_pDecoder->UnlockSurface(pSurface);
    }

    if (deliverFrames && !m_bNeedToFlush)
    {
        DeliverSurface(true);
    }
    else
    {
        ClearQueue();
    }

    ASSERT(m_pDecoder->OutputQueueEmpty());
    ASSERT(m_ProcessedFramesQueue.Empty());
    ASSERT(!m_FreeFramesPool.HasCapacity());

    m_bForceOutput = bForceOutputSave;
}

void CQuickSync::ClearQueue()
{
    // Loop until free frames pool is full
    while (m_FreeFramesPool.HasCapacity())
    {
        if (m_ProcessedFramesQueue.Empty())
        {
            Sleep(1);
        }
        else
        {
            m_FreeFramesPool.Push(m_ProcessedFramesQueue.Pop());
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
    FlushOutputQueue(false);

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
        FlushOutputQueue(false);
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
        if (!m_TimeManager.GetSampleTimeStamp(pSurface, m_pDecoder->GetOutputQueue(), frameData.rtStart, frameData.rtStop))
            return false;
    
        return frameData.rtStart >= 0;
    }

    // Just convert the time stamp from the HW decoder
    frameData.rtStart = m_TimeManager.ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
    frameData.rtStop = (INVALID_REFTIME == frameData.rtStart) ? INVALID_REFTIME : frameData.rtStart + 1;
    return true; // return all frames DS filter will handle this
}

mfxStatus CQuickSync::OnVideoParamsChanged()
{    
    mfxVideoParam params;
    MSDK_ZERO_VAR(params);
    mfxStatus sts = m_pDecoder->GetVideoParams(&params);
    MSDK_CHECK_RESULT_P_RET(sts, MFX_ERR_NONE);

    mfxFrameInfo& curInfo = m_mfxParamsVideo.mfx.FrameInfo;
    mfxFrameInfo& newInfo = params.mfx.FrameInfo;

    bool bResChange = (curInfo.CropH != newInfo.CropH) || (curInfo.CropW != newInfo.CropW);
    bool bAspectRatioChange = (curInfo.AspectRatioH != newInfo.AspectRatioH) || (curInfo.AspectRatioW != newInfo.AspectRatioW);
    bool bFrameRateChange = (curInfo.FrameRateExtN != newInfo.FrameRateExtN) ||(curInfo.FrameRateExtD != newInfo.FrameRateExtD);

    if (!(bResChange || bAspectRatioChange || bFrameRateChange))
        return MFX_ERR_NONE;

    // Copy video params
    params.AsyncDepth = m_mfxParamsVideo.AsyncDepth;
    params.IOPattern = m_mfxParamsVideo.IOPattern;
    memcpy(&m_mfxParamsVideo, &params, sizeof(params));

    // Flush images with old parameters
    FlushOutputQueue(true);

    // Calculate new size and aspect ratio
    if (bAspectRatioChange || bResChange)
    {
        DWORD alignedWidth = (m_Config.bMod16Width) ? MSDK_ALIGN16(newInfo.CropW) : newInfo.CropW;
        PARtoDAR(newInfo.AspectRatioW, newInfo.AspectRatioH, alignedWidth, newInfo.CropH,
            m_FrameDataTemplate.dwPictAspectRatioX, m_FrameDataTemplate.dwPictAspectRatioY);
    }

    if (bFrameRateChange)
    {
        double frameRate = (double)curInfo.FrameRateExtN / (double)curInfo.FrameRateExtD;
        m_TimeManager.OnVideoParamsChanged(frameRate);
    }

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
    if (m_pDecoder->SetD3DDeviceManager(pDeviceManager) && NULL != pDeviceManager)
    {
        // Reset/init in this stage will cause faster initialization in benchmarks
        m_pDecoder->Reset(&m_mfxParamsVideo, m_nPitch);
    }
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

    m_Config = *pConfig;
}

unsigned CQuickSync::WorkerThreadProc(void* pThis)
{
    ASSERT(pThis != NULL);
    return static_cast<CQuickSync*>(pThis)->WorkerThreadMsgLoop();
}

unsigned CQuickSync::WorkerThreadMsgLoop()
{
#ifdef _DEBUG
    SetThreadName("*** QS worker ***");
#endif

    BOOL bRet;

    // Note: GetMessage returns zero on WM_QUIT
    MSG msg;
    while ((bRet = GetMessage(&msg, (HWND)-1, 0, 0 )) != 0)
    {
        m_WorkerThreadIsRunning = true;
        // Error
        if (bRet == -1)
        {
            return (unsigned) bRet;
        }

        if (TM_PROCESS_FRAME == msg.message)
        {
            mfxFrameSurface1* pSurface = (mfxFrameSurface1*)msg.wParam;
            HRESULT hr = ProcessDecodedFrame(pSurface);
            if (FAILED(hr))
            {
                // Error
                bRet = -1;
                break;
            }
        }
        else
        {
            TranslateMessage(&msg); 
            DispatchMessage(&msg);
        }

        m_WorkerThreadIsRunning = false;
    }

    m_WorkerThreadIsRunning = false;
    // All's well
    return 0;
}

HRESULT CQuickSync::QueueSurface(mfxFrameSurface1* pSurface)
{
    if (m_Config.bEnableMultithreading)
    {
        // Post message to worker thread
        PostThreadMessage(m_WorkerThreadId, TM_PROCESS_FRAME, (WPARAM)pSurface, 0);
    }
    else
    {
        ProcessDecodedFrame(pSurface);
    }

    return S_OK;
}

// This function works on the worker thread
HRESULT CQuickSync::ProcessDecodedFrame(mfxFrameSurface1* pSurface)
{
    MSDK_VTRACE("QSDcoder: ProcessDecodedFrame\n");

    // Got a new surface. A NULL surface means to get a surface from the output queue
    if (pSurface != NULL)
    {
        // Initial frames with invalid times are discarded
        REFERENCE_TIME rtStart = m_TimeManager.ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
        if (m_pDecoder->OutputQueueEmpty() &&  rtStart == INVALID_REFTIME && m_Config.bTimeStampCorrection)
        {
            m_pDecoder->UnlockSurface(pSurface);
            return S_OK;
        }

        PushSurface(pSurface);

        // Not enough surfaces for proper time stamp correction
        size_t queueSize = (m_bDvdDecoding || !m_Config.bTimeStampCorrection) ? 0 : m_Config.nOutputQueueLength;
        if (!m_bForceOutput && m_pDecoder->OutputQueueSize() < queueSize)
        {
            return S_OK;
        }
    }

    // Get oldest surface from queue
    pSurface = PopSurface();
    MSDK_CHECK_POINTER(pSurface, S_OK); // decoder queue is empty - return without error

    while (m_FreeFramesPool.Empty())
    {
        if (m_bNeedToFlush)
        {
            m_pDecoder->UnlockSurface(pSurface);
            return S_OK;
        }

        Sleep(1);
    }
    
    TQsQueueItem item = m_FreeFramesPool.Pop();
    QsFrameData& outFrameData     = item.first;
    CQsAlignedBuffer*& pOutBuffer = item.second;

    m_FrameDataTemplate = m_FrameDataTemplate;

    if (pSurface->Data.Corrupted)
    {
        // Do something with a corrupted surface?
        pSurface->Data.Corrupted = pSurface->Data.Corrupted;
    }

    ++m_nSegmentFrameCount;

    // Result is in m_FrameData
    // False return value means that the frame has a negative time stamp and should not be displayed.
    if (!SetTimeStamp(pSurface, outFrameData))
    {
        // Return frame to free pool
        m_FreeFramesPool.Push(item);
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
    outFrameData.rcClip.bottom = (LONG)height - 1;

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

    // Offset output buffer's address for fastest SSE4 copy.
    // Page offset (12 lsb of addresses) sould be 2K apart from source buffer
    size_t offset = ((size_t)frameData.Y & PAGE_MASK) ^ (1 << 11);
    
    // Mark Y, U & V pointers on output buffer
    outFrameData.y = pOutBuffer->GetBuffer() + offset;
    outFrameData.u = outFrameData.y + (pitch * height);
    outFrameData.v = outFrameData.u + 1;

    // App can modify this buffer
    outFrameData.bReadOnly = false;

        // Source is D3D9 surface
    if (m_pDecoder->IsD3DAlloc())
    {
        // Copy Y
        gpu_memcpy(outFrameData.y, frameData.Y + (pSurface->Info.CropY * pitch), height * pitch);

        // Copy UV
        gpu_memcpy(outFrameData.u, frameData.CbCr + (pSurface->Info.CropY * pitch), pitch * height / 2);
    }
    // Source is system memory
    else
    {
        // Copy Y
        memcpy(outFrameData.y, frameData.Y + (pSurface->Info.CropY * pitch), height * pitch);

        // Copy UV
        memcpy(outFrameData.u, frameData.CbCr + (pSurface->Info.CropY * pitch), pitch * height / 2);
    }

    // Unlock the frame
    m_pDecoder->UnlockFrame(pSurface, &frameData);

    // We don't need the surface anymore
    m_pDecoder->UnlockSurface(pSurface);

#ifdef _DEBUG
    // Debug only - mark top left corner when working with D3D
    *((REFERENCE_TIME*)outFrameData.u) = (REFERENCE_TIME)(-1); // 4 pink pixels
#endif

    // Keep or drop the processed frame
    if (!m_bNeedToFlush)
    {
        // Enter result in the processed queue
        m_ProcessedFramesQueue.Push(item);
    }
    else
    {
        // Return to free pool
        m_FreeFramesPool.Push(item);
    }

    return S_OK;
}
