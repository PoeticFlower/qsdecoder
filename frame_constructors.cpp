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
#include "QuickSync_defs.h"
#include "QuickSyncUtils.h"
#include "TimeManager.h"
#include "frame_constructors.h"
#include "H264Nalu.h"

static inline mfxU32 GetValue32(mfxU8* pBuf)
{
    MSDK_CHECK_POINTER(pBuf, 0);
    return ((mfxU32)(pBuf[0]) << 24) | ((mfxU32)(pBuf[1]) << 16) | ((mfxU32)(pBuf[2]) << 8) | ((mfxU32)(pBuf[3]));
}

static inline void SetValue(mfxU32 nValue, mfxU8* pBuf)
{
    MSDK_CHECK_POINTER_NO_RET(pBuf);
    for (mfxU32 i = 0; i < 4; ++i)
    {
        *pBuf++ = (mfxU8)(nValue >> 8 * i);
    }

    return;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
//                                      CFrameConstructor
//////////////////////////////////////////////////////////////////////////////////////////////////////
CFrameConstructor::CFrameConstructor() 
{
    m_bSeqHeaderInserted = false;
    m_bDvdStripPackets = false;
    MSDK_ZERO_VAR(m_ResidialBS);
    MSDK_ZERO_VAR(m_Headers);
    m_ResidialBS.MaxLength = 100;
    m_ResidialBS.Data = new mfxU8[m_ResidialBS.MaxLength];
}

CFrameConstructor::~CFrameConstructor() 
{
    delete[] m_ResidialBS.Data;
    delete[] m_Headers.Data;
}

void CFrameConstructor::Reset()
{
    m_ResidialBS.DataOffset = m_ResidialBS.DataLength = 0;
    m_bSeqHeaderInserted = false;
}

void CFrameConstructor::UpdateTimeStamp(IMediaSample* pSample, mfxBitstream* pBS)
{
    REFERENCE_TIME rtStart, rtEnd;
    HRESULT hr = pSample->GetTime(&rtStart, &rtEnd);
    pBS->TimeStamp = (S_OK == hr || VFW_S_NO_STOP_TIME == hr) ?
         CDecTimeManager::ConvertReferenceTime2MFXTime(rtStart) :
         MFX_TIME_STAMP_INVALID;         
}

void CFrameConstructor::SaveResidialData(mfxBitstream* pBS)
{
    MSDK_CHECK_POINTER_NO_RET(pBS);
    // m_ResidialBS must be empty
    ASSERT(m_ResidialBS.DataLength == 0);
    m_ResidialBS.DataOffset = 0;
    m_ResidialBS.DataLength = pBS->DataLength;
    
    // Check if a bigger buffer is needed
    if (pBS->DataLength > m_ResidialBS.MaxLength)
    {
        delete[] m_ResidialBS.Data;
        mfxU32 newSize = pBS->DataLength;
        m_ResidialBS.Data = new mfxU8[newSize];
        MSDK_CHECK_POINTER_NO_RET(m_ResidialBS.Data);
        m_ResidialBS.MaxLength = newSize;
    }

    ASSERT(pBS->DataOffset + pBS->DataLength <= pBS->MaxLength);
    memcpy(m_ResidialBS.Data, pBS->Data + pBS->DataOffset, pBS->DataLength);
}

mfxStatus CFrameConstructor::ConstructHeaders(
    VIDEOINFOHEADER2* vih,
    const GUID& guidFormat,
    size_t nMtSize,
    size_t nVideoInfoSize)
{
    MSDK_SAFE_DELETE_ARRAY(m_Headers.Data);
    MSDK_ZERO_VAR(m_Headers);

    // nothing to do here...
    if (nMtSize <= nVideoInfoSize)
    {
        return MFX_ERR_MORE_DATA;
    }

    // splitter adds sequence headers to the end of the vih
    size_t nSeqHeaderSize;
    mfxU8* pSeqHeader;
    MPEG2VIDEOINFO* mp2 = NULL;
    if (FORMAT_MPEG2_VIDEO == guidFormat)
    {
        mp2 = (MPEG2VIDEOINFO*)(vih);
        MSDK_CHECK_POINTER(mp2, MFX_ERR_NULL_PTR);
        pSeqHeader = (mfxU8*)mp2->dwSequenceHeader;
        nSeqHeaderSize = mp2->cbSequenceHeader;
    }
    else
    {
        pSeqHeader = (mfxU8*)vih + nVideoInfoSize;
        nSeqHeaderSize = nMtSize - nVideoInfoSize;
    }

    m_Headers.Data = new mfxU8[nSeqHeaderSize];
    m_Headers.MaxLength = m_Headers.DataLength = (mfxU32)nSeqHeaderSize;

    // Copy the sequence header
    memcpy(m_Headers.Data, pSeqHeader, nSeqHeaderSize);
    return MFX_ERR_NONE;
}

mfxStatus CFrameConstructor::ConstructFrame(IMediaSample* pSample, mfxBitstream* pBS)
{
    mfxStatus sts = MFX_ERR_NONE;
    mfxU8*          pDataBuffer = NULL;
    int             nDataSize   = 0;

    MSDK_CHECK_POINTER(pSample, MFX_ERR_NULL_PTR);
    nDataSize = pSample->GetActualDataLength();
    MSDK_CHECK_ERROR(nDataSize, 0, MFX_ERR_MORE_DATA);
    
    pSample->GetPointer(&pDataBuffer);
    MSDK_CHECK_POINTER(pDataBuffer, MFX_ERR_NULL_PTR);

    MSDK_SAFE_DELETE_ARRAY(pBS->Data);

    if (m_bDvdStripPackets)
    {
        StripDvdPacket(pDataBuffer, nDataSize);
        MSDK_CHECK_ERROR(nDataSize, 0, MFX_ERR_MORE_DATA);
    }

    // prefix the sequence headers if needed
    size_t newDataSize = nDataSize + m_ResidialBS.DataLength +
                ((m_bSeqHeaderInserted) ? 0 : m_Headers.DataLength);

    pBS->MaxLength = pBS->DataLength = (mfxU32)newDataSize;

    mfxU8* pData = pBS->Data = new mfxU8[newDataSize];
    MSDK_CHECK_POINTER(pData, MFX_ERR_NULL_PTR);

    // write data left from previous samples
    WriteResidualData(pData);

    // write sequence headers
    WriteHeaders(pData);

    // append new data
    WriteSampleData(pData, pDataBuffer, nDataSize);
    pBS->DataLength = (mfxU32)(pData - pBS->Data);

    UpdateTimeStamp(pSample, pBS);
    return sts;
}

void CFrameConstructor::StripDvdPacket(BYTE*& p, int& len)
{
    if (len > 0 && *(DWORD*)p == 0xba010000)
    { // MEDIATYPE_*_PACK
        len -= 14;
        p += 14;
        if(int stuffing = (p[-1]&7))
        {
            len -= stuffing;
            p += stuffing;
        }
    }

    if (len > 0 && *(DWORD*)p == 0xbb010000)
    {
        len -= 4;
        p += 4;
        int hdrlen = ((p[0]<<8)|p[1]) + 2;
        len -= hdrlen;
        p += hdrlen;
    }

    if (len > 0 &&
        ((*(DWORD*)p&0xf0ffffff) == 0xe0010000 ||
        (*(DWORD*)p&0xe0ffffff) == 0xc0010000 ||
        (*(DWORD*)p&0xbdffffff) == 0xbd010000))
    { // PES
        bool ps1 = (*(DWORD*)p&0xbdffffff) == 0xbd010000;

        len -= 4;
        p += 4;
        int expected = ((p[0]<<8)|p[1]);
        len -= 2;
        p += 2;
        BYTE* p0 = p;

        for (int i = 0; i < 16 && *p == 0xff; i++, len--, p++)
        {
            ;
        }

        if ((*p&0xc0) == 0x80)
        { // mpeg2
            len -= 2;
            p += 2;
            len -= *p+1;
            p += *p+1;
        }
        else
        { // mpeg1
            if((*p&0xc0) == 0x40)
            {
                len -= 2;
                p += 2;
            }

            if ((*p&0x30) == 0x30 || (*p&0x30) == 0x20)
            {
                bool pts = !!(*p&0x20), dts = !!(*p&0x10);
                if (pts)
                {
                    len -= 5;
                }
                p += 5;
                if (dts)
                {
                    ASSERT((*p&0xf0) == 0x10);
                    len -= 5;
                    p += 5;
                }
            }
            else
            {
                len--;
                p++;
            }
        }

        if(ps1)
        {
            len--;
            p++;
        }

        if (expected > 0)
        {
            expected -= (int)(p - p0);
            len = min(expected, len);
        }
    }

    if(len < 0)
    {
        ASSERT(len < 0);
        len = 0;
    }
}

void CFrameConstructor::WriteResidualData(mfxU8*& pData)
{
    if (m_ResidialBS.DataLength)
    {
        memcpy(pData, m_ResidialBS.Data, m_ResidialBS.DataLength);
        pData += m_ResidialBS.DataLength;
        m_ResidialBS.DataLength = 0;
    }
}

void CFrameConstructor::WriteHeaders(mfxU8*& pData)
{
    if (!m_bSeqHeaderInserted)
    {
        memcpy(pData, m_Headers.Data, m_Headers.DataLength); 
        pData += m_Headers.DataLength;
        m_bSeqHeaderInserted = true;
    }
}

void CFrameConstructor::WriteSampleData(mfxU8*& pData, const mfxU8* pSrc, size_t nDataSize)
{
    memcpy(pData, pSrc, nDataSize);
    pData += nDataSize;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
//                                      CVC1FrameConstructor
//////////////////////////////////////////////////////////////////////////////////////////////////////
CVC1FrameConstructor::CVC1FrameConstructor() :
    m_FourCC(0), m_Width(0), m_Height(0)
{
}

void CVC1FrameConstructor::Reset()
{
    if (m_FourCC == FOURCC_WMV3)
    {
        CFrameConstructor::Reset();
        return;
    }

    bool bHeaderInitialized = m_bSeqHeaderInserted;
    CFrameConstructor::Reset();
    m_bSeqHeaderInserted = bHeaderInitialized;
}

mfxStatus CVC1FrameConstructor::ConstructHeaders(
    VIDEOINFOHEADER2* vih,
    const GUID& guidFormat,
    size_t nMtSize,
    size_t nVideoInfoSize)
{
    m_FourCC = vih->bmiHeader.biCompression;
    m_Width  = vih->bmiHeader.biWidth;
    m_Height = vih->bmiHeader.biHeight;
    
    // All other formats except WMV3
    if (FOURCC_WMV3 != m_FourCC)
    {
        if (FOURCC_VC1 == m_FourCC)
        {
            ++nVideoInfoSize;
        }

        return CFrameConstructor::ConstructHeaders(vih, guidFormat, nMtSize, nVideoInfoSize);
    }

    // Special case WMV3
    MSDK_SAFE_DELETE_ARRAY(m_Headers.Data);
    MSDK_ZERO_VAR(m_Headers);

    // nothing to do here...
    if (nMtSize <= nVideoInfoSize)
    {
        return MFX_ERR_MORE_DATA;
    }

    //header should be additionally constructed for main&simple profile in asf data type
    m_Headers.MaxLength = m_Headers.DataLength = (mfxU32)(nMtSize - nVideoInfoSize + 20);
    m_Headers.Data = new mfxU8[m_Headers.MaxLength];
    mfxStatus sts = ConstructHeaderSM(m_Headers.Data,
                                      m_Headers.DataLength,
                                      (mfxU8*)(vih) + nVideoInfoSize,
                                      (mfxU32)(nMtSize - nVideoInfoSize));
    return sts;
}

mfxStatus CVC1FrameConstructor::ConstructFrame(IMediaSample* pSample, mfxBitstream* pBS)
{
    mfxU8* pDataBuffer = NULL;
    mfxU32 nDataSize   = 0;
    MSDK_CHECK_POINTER(pSample, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(pBS, MFX_ERR_NULL_PTR);

    nDataSize = pSample->GetActualDataLength();
    MSDK_CHECK_ERROR(nDataSize, 0, MFX_ERR_MORE_DATA);

    pSample->GetPointer(&pDataBuffer);
    MSDK_CHECK_POINTER(pDataBuffer, MFX_ERR_NULL_PTR);

    UpdateTimeStamp(pSample, pBS);

    // prefix the sequence headers if needed
    size_t newDataSize = nDataSize + m_ResidialBS.DataLength +
        ((m_bSeqHeaderInserted) ? 0 : m_Headers.DataLength) + // add headers size 
        8; // add upto 8 bytes for extra start codes

    pBS->MaxLength = (mfxU32)newDataSize;

    mfxU8* pData = pBS->Data = new mfxU8[newDataSize];
    MSDK_CHECK_POINTER(pData, MFX_ERR_NULL_PTR);

    // write data left from previous samples
    WriteResidualData(pData);

    // write sequence headers
    WriteHeaders(pData);

    if (FOURCC_VC1 != m_FourCC)
    {
        SetValue(nDataSize, pData); 
        SetValue(0, pData + 4);
        pData += 8;
    }
    else if (false == StartCodeExist(pDataBuffer))
    {
        // set start code to first 4 bytes
        SetValue(0x0D010000, pData);
        pData += 4;
    }

    // append new data
    WriteSampleData(pData, pDataBuffer, nDataSize);
    pBS->DataLength = (mfxU32)(pData - pBS->Data);

    return MFX_ERR_NONE;
}

mfxStatus CVC1FrameConstructor::ConstructHeaderSM(mfxU8* pHeaderSM, mfxU32 nHeaderSize, mfxU8* pDataBuffer, mfxU32 nDataSize)
{
    MSDK_CHECK_POINTER(pHeaderSM, MFX_ERR_NULL_PTR);
    MSDK_CHECK_POINTER(pDataBuffer, MFX_ERR_NULL_PTR);
    if (nHeaderSize < nDataSize + 20)
    {
        return MFX_ERR_NOT_ENOUGH_BUFFER;
    }

    // set start code
    SetValue(0xC5000000, pHeaderSM);

    // set size of sequence header is 4 bytes
    SetValue(nDataSize, pHeaderSM + 4);

    // copy saved data back
    memcpy(pHeaderSM + 8, pDataBuffer, nDataSize);

    // set sizes to the end of data
    SetValue(m_Height, pHeaderSM + 8  + nDataSize);
    SetValue(m_Width,  pHeaderSM + 12 + nDataSize);

    // set 0 to the last 4 bytes
    SetValue(0,  pHeaderSM + 16 + nDataSize);

    return MFX_ERR_NONE;
}

bool CVC1FrameConstructor::StartCodeExist(mfxU8* pStart)
{
    MSDK_CHECK_POINTER(pStart, false);

    //check first 4 bytes to be start code
    mfxU32 value = GetValue32(pStart);
    switch (value)
    {
    case 0x010A:
    case 0x010B:
    case 0x010C:
    case 0x010D:
    case 0x010E:
    case 0x010F:
    case 0x011B:
    case 0x011C:
    case 0x011D:
    case 0x011E:
    case 0x011F:
        // start code found
        return true;
    default:
        // start code not found
        return false; 
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////
//                                      CAVCFrameConstructor
//////////////////////////////////////////////////////////////////////////////////////////////////////
CAVCFrameConstructor::CAVCFrameConstructor()
{
    m_HeaderNalSize  = 2;  //MSDN - MPEG2VideoInfo->dwSequenceHeader delimited by 2 byte length fields
    SetValue(0x01000000, m_H264StartCode);
    m_TempBuffer.reserve(1<<20);
}

CAVCFrameConstructor::~CAVCFrameConstructor()
{
}

mfxStatus CAVCFrameConstructor::ConstructHeaders(VIDEOINFOHEADER2* vih,
        const GUID& guidFormat,
        size_t nMtSize,
        size_t nVideoInfoSize)
{
    MSDK_SAFE_DELETE_ARRAY(m_Headers.Data);
    m_Headers.DataLength = m_Headers.MaxLength = 0;

    // nothing to do here...
    if (nMtSize <= nVideoInfoSize || FORMAT_MPEG2_VIDEO != guidFormat)
    {
        return MFX_ERR_MORE_DATA;
    }

    MPEG2VIDEOINFO* mp2 = (MPEG2VIDEOINFO*)(vih);
    MSDK_CHECK_POINTER(mp2, MFX_ERR_NULL_PTR);

    // SPS and/or PPS Data will be present
    mfxStatus sts = MFX_ERR_NONE; 
    mfxU32 nNalDataLen;          
    mfxU8* pNalDataBuff; 
    CH264Nalu itStartCode;
    MSDK_ZERO_VAR(m_Headers);
    m_TempBuffer.clear();
    m_NalSize = mp2->dwFlags;     

    itStartCode.SetBuffer((BYTE*)mp2->dwSequenceHeader, mp2->cbSequenceHeader, m_HeaderNalSize); //Nal size = 2 
    while (itStartCode.ReadNext())
    {    
        nNalDataLen = itStartCode.GetDataLength(); 
        pNalDataBuff = itStartCode.GetDataBuffer();

        switch(itStartCode.GetType())
        {
        case NALU_TYPE_SPS:
        case NALU_TYPE_PPS: 
            m_TempBuffer.insert(m_TempBuffer.end(), m_H264StartCode, m_H264StartCode + 4);
            m_TempBuffer.insert(m_TempBuffer.end(), pNalDataBuff, pNalDataBuff+nNalDataLen);
            break; 
        default: 
            sts = MFX_ERR_MORE_DATA; 
            break;
        }
    }

    if (m_TempBuffer.size())
    {
        //Keep a copy of the SPS/PPS to be placed into the decode stream (after each new segment).
        MSDK_SAFE_DELETE_ARRAY(m_Headers.Data);
        m_Headers.Data = new mfxU8[m_TempBuffer.size()];
        m_Headers.DataLength = m_Headers.MaxLength = (mfxU32)m_TempBuffer.size();
        memcpy(m_Headers.Data, &m_TempBuffer.front(), m_TempBuffer.size());
    }

    return sts; 
}

mfxStatus CAVCFrameConstructor::ConstructFrame(IMediaSample* pSample, mfxBitstream* pBS)
{
    mfxU32 nDataSize = 0; 
    mfxU8* pDataBuffer = NULL;    
    CH264Nalu itStartCode;
    int nNalDataLen; 
    mfxU8* pNalDataBuff; 
    m_TempBuffer.clear();
    MSDK_CHECK_POINTER(pSample, MFX_ERR_NULL_PTR); 
    MSDK_CHECK_POINTER(pBS, MFX_ERR_NULL_PTR); 

    UpdateTimeStamp(pSample, pBS);

    nDataSize = pSample->GetActualDataLength();
    MSDK_CHECK_ERROR(nDataSize, 0, MFX_ERR_MORE_DATA); 
    m_TempBuffer.reserve(nDataSize * 3 / 2);

    pSample->GetPointer(&pDataBuffer);
    MSDK_CHECK_POINTER(pDataBuffer,  MFX_ERR_NULL_PTR);

    itStartCode.SetBuffer(pDataBuffer, nDataSize, m_NalSize); //Nal size = 4

    // Iterate over the NALUs and convert them to have start codes.
    while (itStartCode.ReadNext())
    {
        NALU_TYPE naluType = itStartCode.GetType();

        // Discard audio NALUs
        if (NALU_TYPE_AUD == naluType)
            continue;

        nNalDataLen =  itStartCode.GetDataLength(); 
        pNalDataBuff = itStartCode.GetDataBuffer();

        ASSERT(nNalDataLen > 0); // Shouldn't fail!
        if (nNalDataLen > 0)
        {
            m_TempBuffer.insert(m_TempBuffer.end(), m_H264StartCode, m_H264StartCode + 4);
            m_TempBuffer.insert(m_TempBuffer.end(), pNalDataBuff, pNalDataBuff + nNalDataLen);
        }
    }

    if (m_TempBuffer.empty())
    {
        return MFX_ERR_NONE;
    }

    // Update data size
    nDataSize = (mfxU32)m_TempBuffer.size();
    pDataBuffer = &m_TempBuffer.front();
    size_t newDataSize = nDataSize + m_ResidialBS.DataLength +
        ((m_bSeqHeaderInserted) ? 0 : m_Headers.DataLength);

    mfxU8* pData = pBS->Data = new mfxU8[newDataSize];
    pBS->MaxLength = (mfxU32)newDataSize;

    // write data left from previous samples
    WriteResidualData(pData);

    // Write sequence headers if needed
    WriteHeaders(pData);

    // Append new data
    WriteSampleData(pData, pDataBuffer, nDataSize);
    pBS->DataLength = (mfxU32)(pData - pBS->Data);

    return MFX_ERR_NONE;
}
