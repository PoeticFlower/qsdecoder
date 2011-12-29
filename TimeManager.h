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

#pragma once

#define MFX_TIME_STAMP_FREQUENCY 90000L
#define MFX_TIME_STAMP_INVALID  (mfxU64)(-1)
#define MFX_TIME_STAMP_MAX      ((mfxI64)100000 * (mfxI64)MFX_TIME_STAMP_FREQUENCY)
#define INVALID_REFTIME _I64_MIN

struct TTimeStampInfo
{
    TTimeStampInfo(int _id = 0, REFERENCE_TIME _rtStart = 0) :
        id(_id), rtStart(_rtStart)
    {
    }

    int id;
    REFERENCE_TIME rtStart;
};

typedef std::deque<TTimeStampInfo> TTimeStampQueue;
typedef std::multiset<REFERENCE_TIME> TSortedTimeStamps;


class CDecTimeManager
{
public:
    CDecTimeManager(bool bEnableIvtc = true) : m_dOrigFrameRate(0), m_dFrameRate(0), m_bIvtc(false), m_bEnableIvtc(bEnableIvtc)
    {
        Reset();
    }

    double  GetFrameRate() { return m_dFrameRate; }
    void    SetFrameRate(double frameRate, bool bIsFields)
    {
        m_dFrameRate = (bIsFields && frameRate < 30.0) ? (frameRate / 2) : frameRate;
        m_dOrigFrameRate = m_dFrameRate;
        m_bIsSampleInFields = bIsFields;
        MSDK_TRACE("QsDecoder: frame rate is %0.2f\n", (float)(m_dFrameRate));
    }

    void Reset();
    void SetInverseTelecine(bool bIvtc);
    inline bool GetInverseTelecine() { return m_bIvtc; }
    inline bool HasValidFrameRate() { return m_bValidFrameRate || m_dFrameRate == 0; }

    static mfxU64 ConvertReferenceTime2MFXTime(REFERENCE_TIME rtTime)
    {
        if (-1e7 == rtTime || INVALID_REFTIME == rtTime)
            return MFX_TIME_STAMP_INVALID;

        return (mfxU64)(((double)rtTime / 1e7) * (double)MFX_TIME_STAMP_FREQUENCY);
    }

    static REFERENCE_TIME ConvertMFXTime2ReferenceTime(mfxU64 nTime)
    {
        if (MFX_TIME_STAMP_INVALID == nTime)
            return (REFERENCE_TIME)INVALID_REFTIME;

        mfxI64 t = (mfxI64)nTime; // don't lose the sign

        if (t < -MFX_TIME_STAMP_MAX || t > MFX_TIME_STAMP_MAX)
            return (REFERENCE_TIME)INVALID_REFTIME;

        return (REFERENCE_TIME)(((double)t / (double)MFX_TIME_STAMP_FREQUENCY) * 1e7);
    }

    void AddOutputTimeStamp(mfxFrameSurface1* pSurface);
    bool CalcFrameRate(const mfxFrameSurface1* pSurface,
                       const std::deque<mfxFrameSurface1*>& queue);
    bool GetSampleTimeStamp(const mfxFrameSurface1* pSurface,
                            const std::deque<mfxFrameSurface1*>& queue,
                            REFERENCE_TIME& rtStart,
                            REFERENCE_TIME& rtStop);
    bool IsSampleInFields() { return m_bIsSampleInFields; }
    void OnVideoParamsChanged(double frameRate);

    static const double fps2997;
    static const double fps23976;

protected:
    void FixFrameRate(double frameRate);
    bool CalcCurrentFrameRate(double& tmpFrameRate, size_t nQueuedFrames);

    double m_dOrigFrameRate;
    double m_dFrameRate;
    bool   m_bValidFrameRate;
    bool   m_bIvtc;
    int    m_nLastSeenFieldDoubling;
    bool   m_bIsPTS; // true for Presentation Time Stamps (source). output is always PTS.
    int    m_nSegmentSampleCount;
    int    m_nOutputFrames;
    bool   m_bIsSampleInFields;
    bool   m_bEnableIvtc;
    REFERENCE_TIME m_rtPrevStart;
    TSortedTimeStamps m_OutputTimeStamps;
};
