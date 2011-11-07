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
#include "QuickSyncDecoder.h"
#include "frame_constructors.h"
#include "QuickSyncUtils.h"
#include "QuickSync.h"

#define MEMCPY gpu_memcpy
//#define MEMCPY memcpy
#define PAGE_MASK 4095

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
    m_OutputBuffer(NULL),
    m_OutputBufferSize(0),
    m_nSegmentFrameCount(0),
    m_bForceOutput(false),
    m_bDvdDecoding(false),
    m_bFlushing(false),
    m_bNeedToFlush(false)
{
    MSDK_TRACE("QSDcoder: Constructor\n");


    mfxStatus sts = MFX_ERR_NONE;
    m_pDecoder = new CQuickSyncDecoder(sts);

    MSDK_ZERO_VAR(m_mfxParamsVideo);
    //m_mfxParamsVideo.AsyncDepth = 1; //causes issues when 1
    m_mfxParamsVideo.mfx.ExtendedPicStruct = 1;
//    m_mfxParamsVideo.mfx.TimeStampCalc = MFX_TIMESTAMPCALC_TELECINE;
    m_Config.dwOutputQueueLength = 8;

    m_OK = (sts == MFX_ERR_NONE);
}

CQuickSync::~CQuickSync()
{
    MSDK_TRACE("QSDcoder: Destructor\n");

    CQsAutoLock cObjectLock(&m_csLock);

    MSDK_SAFE_DELETE(m_pFrameConstructor);
    MSDK_SAFE_DELETE(m_pDecoder);
    _aligned_free(m_OutputBuffer);
}

HRESULT CQuickSync::HandleSubType(const GUID& subType, FOURCC fourCC)
{
    MSDK_SAFE_DELETE(m_pFrameConstructor);

    //MPEG2
    if (subType == MEDIASUBTYPE_MPEG2_VIDEO)//(fourCC == FOURCC_mpg2) || (fourCC == FOURCC_MPG2))
    {
        m_mfxParamsVideo.mfx.CodecId = MFX_CODEC_MPEG2;
        m_pFrameConstructor = new CFrameConstructor;
        return S_OK;
    }
    
    //VC1
    if ((fourCC == FOURCC_VC1) || (fourCC == FOURCC_WMV3))
    {
        m_mfxParamsVideo.mfx.CodecId = MFX_CODEC_VC1;

        if (subType ==  WMMEDIASUBTYPE_WMV3)
        {
            m_mfxParamsVideo.mfx.CodecProfile = MFX_PROFILE_VC1_SIMPLE;        
        }   
        else if (fourCC == FOURCC_VC1)
        {
            m_mfxParamsVideo.mfx.CodecProfile = MFX_PROFILE_VC1_ADVANCED;
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


    return S_OK;





    CQsAutoLock cObjectLock(&m_csLock);

    // Parameter check
    MSDK_CHECK_POINTER(mtIn, E_POINTER);
    MSDK_CHECK_POINTER(mtIn->pbFormat, E_UNEXPECTED);

    if (!(mtIn->majortype == MEDIATYPE_DVD_ENCRYPTED_PACK || mtIn->majortype == MEDIATYPE_Video))
        return VFW_E_INVALIDMEDIATYPE;

    HRESULT hr;
    VIDEOINFOHEADER2* vih2 = NULL;
    mfxStatus sts = MFX_ERR_NONE;
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



done:
    delete[] (mfxU8*)vih2;

    return S_OK;
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
        mfx.FrameInfo.CropH        = (mfxU16)MSDK_ALIGN16(vih2->bmiHeader.biHeight);
        mfx.FrameInfo.CropW        = (mfxU16)MSDK_ALIGN16(vih2->bmiHeader.biWidth);
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
    bool bIsFields = (vih2->dwInterlaceFlags & AMINTERLACE_IsInterlaced) &&
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

    // Init Media SDK decoder
    if (sts == MFX_ERR_NONE)
    {
        m_pDecoder->SetConfig(m_Config);
        sts = m_pDecoder->Init(&m_mfxParamsVideo, m_nPitch);
        
        if (sts == MFX_WRN_PARTIAL_ACCELERATION)
        {
            MSDK_TRACE("\n\n\nQSDcoder::InitDecoder - stream isn't supported by HW acceleration!\n\n\n\n");
            sts = MFX_ERR_NONE;
        }
    }

done:
    delete[] (mfxU8*)vih2;
    m_OK = (sts == MFX_ERR_NONE);
    return (m_OK) ? S_OK : E_FAIL;
}

void CQuickSync::SetAspectRatio(VIDEOINFOHEADER2& vih2, mfxFrameInfo& FrameInfo)
{
    DWORD alignedWidth = MSDK_ALIGN16(FrameInfo.CropW);
    
    // fix small aspect ratio errors
    if (MSDK_ALIGN16(vih2.dwPictAspectRatioX) == alignedWidth)
    {
        vih2.dwPictAspectRatioX = alignedWidth;
    }

    // AR is not always contained in the header (it's optional for MFX decoder initialization though)
    if (FrameInfo.AspectRatioW * FrameInfo.AspectRatioH == 0)
    {
        //convert display aspect ratio (is in VIDEOINFOHEADER2) to pixel aspect ratio (is accepted by MediaSDK components)
        mfxStatus sts = DARtoPAR(vih2.dwPictAspectRatioX, vih2.dwPictAspectRatioY, 
            FrameInfo.CropW, FrameInfo.CropH,
            FrameInfo.AspectRatioW, FrameInfo.AspectRatioH);

        if (sts != MFX_ERR_NONE)
        {
            FrameInfo.AspectRatioW = FrameInfo.AspectRatioH = 1;
        }
    }

    // use image size as aspect ratio when PAR is 1x1
    if (FrameInfo.AspectRatioW == FrameInfo.AspectRatioH)
    {
        m_FrameData.dwPictAspectRatioX = alignedWidth;
        m_FrameData.dwPictAspectRatioY = FrameInfo.CropH;
    }
    else
    {
        m_FrameData.dwPictAspectRatioX = vih2.dwPictAspectRatioX;
        m_FrameData.dwPictAspectRatioY = vih2.dwPictAspectRatioY;
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

    MSDK_TRACE("QSDcoder: Decode\n");

    // We haven't flushed since the last BeginFlush call - probably a DVD playback scenario
    // where NewSegment/OnSeek isn't issued.
    if (m_bNeedToFlush)
    {
        OnSeek(0);
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

    m_TimeManager.AddTimeStamp(pSample);

    // Manipulate bitstream for decoder
    sts = m_pFrameConstructor->ConstructFrame(pSample, &mfxBS);
    ASSERT(mfxBS.DataLength <= mfxBS.MaxLength);
    MSDK_CHECK_ERROR(sts, MFX_ERR_MORE_DATA, S_OK); // not an error
    MSDK_CHECK_NOT_EQUAL(sts, MFX_ERR_NONE, E_FAIL);

    // Decode mfxBitstream until all data is taken by decoder
    while (mfxBS.DataLength > 0 && !m_bNeedToFlush)
    {
        // Decode the bitstream
        sts = m_pDecoder->Decode(&mfxBS, pSurfaceOut);                

        if (MFX_ERR_NONE == sts)
        {
            DeliverSurface(pSurfaceOut);            
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

            sts = OnVideoParamsChanged();
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
            if (!m_pDecoder->GetOutputQueue()->empty())
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
    return hr;
}

HRESULT CQuickSync::DeliverSurface(mfxFrameSurface1* pSurface)
{
    if (m_bNeedToFlush)
        return S_OK;

    MSDK_TRACE("QSDcoder: DeliverSurface\n");

    MSDK_CHECK_POINTER(m_DeliverSurfaceCallback, E_UNEXPECTED);

    // Got a new surface. A NULL surface means to get a surface from the output queue
    if (pSurface != NULL)
    {
        // Initial frames with invalid times are discarded
        REFERENCE_TIME rtStart = m_TimeManager.ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
        if (m_pDecoder->GetOutputQueue()->empty() &&  rtStart == INVALID_REFTIME)
            return S_OK;

        PushSurface(pSurface);

        // Not enough surfaces for proper time stamp correction
        DWORD queueSize = (m_bDvdDecoding) ? 0 :
            m_Config.dwOutputQueueLength;

        if (!m_bForceOutput && m_pDecoder->GetOutputQueue()->size() < queueSize)
            return S_OK;
    }

    // Get oldest surface from queue
    pSurface = PopSurface();
    MSDK_CHECK_POINTER(pSurface, E_POINTER);

    if (pSurface->Data.Corrupted)
    {
        // Do something with a corrupted surface?
        pSurface->Data.Corrupted = pSurface->Data.Corrupted;
    }

    ++m_nSegmentFrameCount;

    // Result is in m_FrameData
    // False return value means that the frame has a negative time stamp and should not be displayed.
    if (!SetTimeStamp(pSurface))
        return S_OK;

    m_FrameData.bFilm = false;

    // Setup interlacing info
    m_FrameData.dwInterlaceFlags = PicStructToDsFlags(pSurface->Info.PicStruct);
    
    // TODO: find actual frame type I/P/B
    // Media sample isn't reliable as it referes to an older frame!
    m_FrameData.frameType = QsFrameData::I;

    // Obtain surface data and copy it to temp buffer.
    mfxFrameData frameData;
    m_pDecoder->LockFrame(pSurface, &frameData);
    
    // Setup output buffer
    m_FrameData.dwStride = frameData.Pitch;
    size_t outSize = 4096 + // Adding 4K for page alignment optimizations
        m_FrameData.dwStride * pSurface->Info.CropH * 3 / 2;

    if (!m_OutputBuffer || m_OutputBufferSize != outSize)
    {
        if (m_OutputBuffer)
        {
            _aligned_free(m_OutputBuffer);
        }

        m_OutputBufferSize = outSize; 
        // malloc with page alignment (easier to offset)
        m_OutputBuffer = (BYTE*)_aligned_malloc(m_OutputBufferSize, 16);
    }

    size_t height = pSurface->Info.CropH; // Cropped image height
    size_t pitch  = frameData.Pitch;      // Image line + padding in bytes --> set by the driver

    // Fill output size
    m_FrameData.dwHeight = pSurface->Info.CropH;
    m_FrameData.dwWidth  = MSDK_ALIGN16(pSurface->Info.CropW + pSurface->Info.CropX);

    // Offset output buffer's address for fastest SSE4 copy.
    // Page offset (12 lsb of addresses) sould be 2K apart from source buffer
    size_t offset = ((size_t)frameData.Y & PAGE_MASK) ^ (1 << 11);
    m_FrameData.y = m_OutputBuffer + offset;
    m_FrameData.u = m_FrameData.y + (pitch * height);

    // Copy Y
    MEMCPY(m_FrameData.y, frameData.Y + (pSurface->Info.CropY * pitch), height * pitch);

    // Copy UV
    MEMCPY(m_FrameData.u, frameData.CbCr + (pSurface->Info.CropY * pitch), pitch * height / 2);

    // Add borders for non standard images
    if (pSurface->Info.CropW < m_FrameData.dwWidth)
    {
        AddBorders(pSurface);
    }

    // Unlock the frame
    m_pDecoder->UnlockFrame(pSurface, &frameData);

    // Send the surface out - return code from dshow filter is ignored.
    if (m_bNeedToFlush)
        return S_OK;

    MSDK_TRACE("QSDcoder: DeliverSurfaceCallback (%I64d, %I64d)\n", m_FrameData.rtStart, m_FrameData.rtStop);
    m_DeliverSurfaceCallback(m_ObjParent, &m_FrameData);
    return S_OK;
}

void CQuickSync::AddBorders(mfxFrameSurface1* pSurface)
{
    DWORD h = m_FrameData.dwHeight;

    // Left border
    if (pSurface->Info.CropX > 0)
    {
        for (DWORD y = 0; y < h; ++y)
        {
            memset(m_FrameData.y + m_FrameData.dwStride * y, 16, pSurface->Info.CropX);

            if (y < h / 2)
            {
                memset(m_FrameData.u + m_FrameData.dwStride * y, 128, pSurface->Info.CropX);
            }
        }
    }

    // Right border
    int rBorderWidth = (int)m_FrameData.dwWidth - (pSurface->Info.CropX + pSurface->Info.CropW);
    if (rBorderWidth)
    {
        for (DWORD y = 0; y < m_FrameData.dwHeight; ++y)
        {
            memset(m_FrameData.y + m_FrameData.dwStride * y + pSurface->Info.CropX + pSurface->Info.CropW,
                16, rBorderWidth);

            if (y < h / 2)
            {
                memset(m_FrameData.u + m_FrameData.dwStride * y + pSurface->Info.CropX + pSurface->Info.CropW,
                    128, rBorderWidth);
            }
        }
    }
}

mfxU32 CQuickSync::PicStructToDsFlags(mfxU32 picStruct)
{
    if (m_TimeManager.GetInverseTelecine())
    {
        return AM_VIDEO_FLAG_WEAVE;
    }

    mfxU32 flags = 0;
    // progressive frame - note that sometimes interlaced content has the MFX_PICSTRUCT_PROGRESSIVE in combination with other flags
    if (picStruct == MFX_PICSTRUCT_PROGRESSIVE)
    {
        return AM_VIDEO_FLAG_WEAVE;
    }

    // top field first
    if (picStruct & MFX_PICSTRUCT_FIELD_TFF)
    {
        flags |= AM_VIDEO_FLAG_FIELD1FIRST;
    }

    // telecine flag
    if (picStruct & MFX_PICSTRUCT_FIELD_REPEATED)
    {
        flags |= AM_VIDEO_FLAG_REPEAT_FIELD;
    }

    return flags;
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

    // recieved a BeginFlush that wasn't handled for some reason
    // this overrides the request to output the remaining frames
    deliverFrames = deliverFrames && !m_bNeedToFlush;
    m_bForceOutput = deliverFrames;

    // Flush internal queue
    FlushOutputQueue(deliverFrames);

    // flush HW decoder by sending NULL bitstreams.
    while (MFX_ERR_NONE == sts || MFX_ERR_MORE_SURFACE == sts)
    {
        mfxFrameSurface1* pSurf = NULL;
        sts = m_pDecoder->Decode(NULL, pSurf);

        if (MFX_ERR_NONE == sts && deliverFrames)
        {
            hr = DeliverSurface(pSurf);
            if (FAILED(hr))
                break;
        }
    }

    // Flush internal queue again
    FlushOutputQueue(deliverFrames);

    ASSERT(m_pDecoder->GetOutputQueue()->empty());

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

    MSDK_TRACE("QSDcoder: FlushOutputQueue (deliverFrames=%s)\n", (m_bForceOutput) ? "TRUE" : "FALSE");

    while (!m_pDecoder->GetOutputQueue()->empty() && deliverFrames && !m_bNeedToFlush)
    {
        DeliverSurface(NULL);
    }

    // clear the internal frame queue - either failure in flushing or no need to deliver the frames.
    while (!m_pDecoder->GetOutputQueue()->empty())
    {
        PopSurface();
    }

    m_bForceOutput = bForceOutputSave;
}

HRESULT CQuickSync::OnSeek(REFERENCE_TIME segmentStart)
{
    MSDK_TRACE("QSDcoder: OnSeek\n");

    CQsAutoLock cObjectLock(&m_csLock);
    mfxStatus sts = MFX_ERR_NONE;

    m_nSegmentFrameCount = 0;

    if (m_pDecoder)
    {
        m_TimeManager.Reset();
        sts = m_pDecoder->Reset(&m_mfxParamsVideo, m_nPitch);
        ASSERT(sts == MFX_ERR_NONE);
        m_pFrameConstructor->Reset();

        FlushOutputQueue(false);
    }

    m_bNeedToFlush = false;
    return (sts == MFX_ERR_NONE) ? S_OK : E_FAIL;
}

bool CQuickSync::SetTimeStamp(mfxFrameSurface1* pSurface)
{
    if (!m_TimeManager.GetSampleTimeStamp(pSurface, *m_pDecoder->GetOutputQueue(), m_FrameData.rtStart, m_FrameData.rtStop))
        return false;

    //TODO: force BOB DI after loss of IVTC for a few frames
    return m_FrameData.rtStart >= 0;
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
    bool bFrameStructChange = curInfo.PicStruct != newInfo.PicStruct;

    if (!(bResChange || bAspectRatioChange || bFrameRateChange || bFrameStructChange))
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
        DWORD alignedWidth = MSDK_ALIGN16(newInfo.CropW);        
        PARtoDAR(newInfo.AspectRatioW, newInfo.AspectRatioH, alignedWidth, newInfo.CropH, m_FrameData.dwPictAspectRatioX, m_FrameData.dwPictAspectRatioY);
    }

    if (bFrameRateChange || bFrameStructChange)
    {
        double frameRate = (double)curInfo.FrameRateExtN / (double)curInfo.FrameRateExtD;
        m_TimeManager.OnVideoParamsChanged(frameRate);
    }

    // Might be needed for DVD menus - currently not working as expected :(
//    Flush(true);
//    OnSeek(0);
    return MFX_ERR_NONE;
}

void CQuickSync::PushSurface(mfxFrameSurface1* pSurface)
{
    m_pDecoder->PushSurface(pSurface);

    // Note - no need to lock as all API functions are already exclusive.
    m_TimeManager.AddOutputTimeStamp(pSurface);
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
