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

using namespace std;
// returns true if value is between upper and lower bounds (inclusive)
#define InRange(val, lb, up) ( (val) <= (up) && (val) >= (lb))
#define GetSampleRefTime(s) CDecTimeManager::ConvertMFXTime2ReferenceTime((s)->Data.TimeStamp)

const double CDecTimeManager::fps2997 = 30.0 * 1000.0 / 1001.0;
const double CDecTimeManager::fps23976 = 24.0 * 1000.0 / 1001.0;

void CDecTimeManager::Reset()
{
    m_nOutputFrames = -1;
    m_nSegmentSampleCount = 0;
    m_OutputTimeStamps.clear();
    m_bValidFrameRate = false;
    m_nLastSeenFieldDoubling = 0;
    m_rtPrevStart = INVALID_REFTIME;
    m_bIsSampleInFields = false;
    SetInverseTelecine(false);
}

bool CDecTimeManager::CalcFrameRate(const mfxFrameSurface1* pSurface,
                                    const std::deque<mfxFrameSurface1*>& queue)
{
    if (m_bValidFrameRate)
        return true;

    m_dFrameRate = m_dOrigFrameRate;

    // first thing, find if the time stamps are PTS (presentation) or DTS (decoding).
    // PTS time stamps are monotonic.
    // this is important for deriving correct time stamp in GetSampleTimeStamp
    REFERENCE_TIME rtCurrent = GetSampleRefTime(pSurface);
    m_bIsPTS = true;
    REFERENCE_TIME prevStart = rtCurrent;
    for (auto it = queue.begin(); it != queue.end(); ++it)
    {
        const REFERENCE_TIME& rtStart = GetSampleRefTime(*it);
        if (INVALID_REFTIME != prevStart)
        {
            // not montonic:
            if (rtStart < prevStart)
            {
                m_bIsPTS = false;
                break;
            }
        }

        prevStart = rtStart;
    }

    // check validity of time stamps
    set<REFERENCE_TIME> sampleTimesSet;
    int duplicates = 0, invalids = 0;

    if (rtCurrent != INVALID_REFTIME)
    {
        sampleTimesSet.insert(rtCurrent);
    }
    else
    {
        ++invalids;
    }

    for (auto it = queue.begin(); it != queue.end(); ++it)
    {
        REFERENCE_TIME rtStart = GetSampleRefTime(*it);
        if (rtStart != INVALID_REFTIME)
        {
            // note: insert return pair<iterator, bool>. the bool field is true when item was added. otherwise false (item exists)
            duplicates += (false == (sampleTimesSet.insert(rtStart)).second);
        }
        else
        {
            ++invalids;
        }
    }

    double avgFrameRate = m_dFrameRate;

    // need enough samples for average frame rate calculation
    if (sampleTimesSet.size() > 8)
    {
        // find average frame rate (ignore current frame - sometimes wrong values):
        REFERENCE_TIME rtStart = *(sampleTimesSet.begin());  // lowest
        REFERENCE_TIME rtStop  = *(sampleTimesSet.rbegin()); // highest time stamp

        // TODO: account for newest frames being invalid or duplicates
        avgFrameRate = (1e7 * (m_OutputTimeStamps.size() - 1)) / // multiply by the count of all frames -1
            (double)(rtStop - rtStart);
    }

    // got something that look like a valid frame rate.
    // check if this is related to the actual time stamps.
    if (m_dFrameRate > 1.0)
    {
        // check if samples are inputted as fields not frames. frame rate might be artificially doubled
        if (InRange(m_dFrameRate, avgFrameRate * 1.9,  avgFrameRate * 2.1) &&
            pSurface->Info.PicStruct != MFX_PICSTRUCT_PROGRESSIVE)
        {
            m_dFrameRate /= 2;
        }

        // need extra handling?
    }
    // use original time stamps
    else if (m_bIsPTS)
    {
        m_dFrameRate = 0;
    }
    // need to calc frame rate :(
    // this isn't optimal.
    else
    {
        // use average frame rate
        m_dFrameRate = avgFrameRate;
    }

    MSDK_TRACE("QsDecoder: frame rate is %0.2f\n", (float)(m_dFrameRate));
    m_bValidFrameRate = true;
    return true;
}

void CDecTimeManager::SetInverseTelecine(bool bIvtc)
{
    // Nothing changed
    bIvtc = bIvtc && m_bEnableIvtc;

    if (bIvtc == m_bIvtc)
        return;
    
    m_bIvtc = bIvtc;
    if (m_bIvtc)
    {
        FixFrameRate(fps23976);
        MSDK_TRACE("QsDecoder: entering IVTC\n", (float)(m_dFrameRate));
    }
    else
    {
        FixFrameRate(fps2997);
        MSDK_TRACE("QsDecoder: leaving IVTC\n", (float)(m_dFrameRate));
    }
}

bool CDecTimeManager::GetSampleTimeStamp(const mfxFrameSurface1* pSurface,
                                         const std::deque<mfxFrameSurface1*>& queue,
                                         REFERENCE_TIME& rtStart,
                                         REFERENCE_TIME& rtStop)
{
    if (!m_bValidFrameRate)
    {
        CalcFrameRate(pSurface, queue);
    }

    bool bFieldDoubling = (0 != (pSurface->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED));
    const REFERENCE_TIME rtDecoder = GetSampleRefTime(pSurface);
    rtStop = INVALID_REFTIME;

    ++m_nLastSeenFieldDoubling;

    // Enter inverse telecine mode
    if (bFieldDoubling)
    {
        SetInverseTelecine(true);
        m_nLastSeenFieldDoubling = 0;
    }
    // Return to normal frame rate due to content change
    else if (m_nLastSeenFieldDoubling > 1) //m_dFrameRate)
    {
        SetInverseTelecine(false);
    }

    ++m_nOutputFrames;

    // Can't start the sequence - drop frame
    if (rtDecoder == INVALID_REFTIME && m_rtPrevStart == INVALID_REFTIME)
    {
        rtStart = rtStop = INVALID_REFTIME;
        return false;
    }

    // Find smallest valid time stamp for first frame
    rtStart = rtDecoder;

    // First frame in a new frame sequence (first frame after a stop or a seek)
    // should always be a keyframe
    if (m_rtPrevStart == INVALID_REFTIME)
    {
        // Presentation time stamps (PTS).
        // Time stamp should be OK (in presentation order)
        if (m_bIsPTS)
        {
            // Easy case - rtDecoder is the time stamp to use
            if (rtDecoder != INVALID_REFTIME)
            {
                rtStart = rtDecoder;
                auto it = m_OutputTimeStamps.find(rtDecoder);
                
                ASSERT(it != m_OutputTimeStamps.end());
                if (it != m_OutputTimeStamps.end())
                {
                    m_OutputTimeStamps.erase(it);
                }
            }
            // Need to calculate time stamp from future frames
            else if (!m_OutputTimeStamps.empty())
            {
                size_t count = 0;

                // Note - m_OutputTimeStamps contains only valid time stamps - take the smallest
                rtStart = *(m_OutputTimeStamps.begin());

                // Find distance from current sample
                for (auto it = queue.begin(); it != queue.end(); ++it)
                {
                    ++count;
                    REFERENCE_TIME t = GetSampleRefTime(*it);
                    if (rtStart == t)
                    {
                        break;
                    }
                }

                // Take negative offset from this future time stamp
                rtStart = rtStart - (REFERENCE_TIME)(0.5 + (1e7 * count) / m_dFrameRate);
            }
            // Can't derive time stamp, frame will be dropped :(
            else
            {
                return false;
            }
        }
        // Decoding time stamps (DTS) - a little tricky, might not be 100%
        else
        {
            // Note - m_OutputTimeStamps contains only valid time stamps - take the smallest
            if (!m_OutputTimeStamps.empty())
            {
                auto it = m_OutputTimeStamps.begin();
                rtStart = *(it);
                m_OutputTimeStamps.erase(it);
            }
            // Can't derive time stamp, frame will be dropped :(
            else
            {
                return false;
            }
        }
    }
    // 2nd and above frames
    else 
    {
        // Check if frame rate has changed
        double tmpFrameRate;
        if (CalcCurrentFrameRate(tmpFrameRate, 1 + queue.size()))
        {
            FixFrameRate(tmpFrameRate);
        }

        if (m_dFrameRate > 0 || INVALID_REFTIME == rtDecoder)
        {
            rtStart = m_rtPrevStart + (REFERENCE_TIME)(0.5 + 1e7 / m_dFrameRate);
            auto it = m_OutputTimeStamps.begin();
            if (it != m_OutputTimeStamps.end())
            {
                REFERENCE_TIME rtTemp = *(it);
                if (rtTemp < rtStart || abs(rtTemp - rtStart) < 25000) // diff is less than 2.5ms
                {
                    m_OutputTimeStamps.erase(it);
                }
            }
        }
        else
        {
            auto it = m_OutputTimeStamps.begin();
            if (it != m_OutputTimeStamps.end())
            {
                rtStart = *it;
                m_OutputTimeStamps.erase(it);
            }
            else
            {
                rtStart = INVALID_REFTIME;
                return false;
            }
        }
    }   

    rtStop = rtStart + 1;
    m_rtPrevStart = rtStart;
    return true;
}

void CDecTimeManager::AddOutputTimeStamp(mfxFrameSurface1* pSurface)
{
    REFERENCE_TIME rtStart = ConvertMFXTime2ReferenceTime(pSurface->Data.TimeStamp);
    if (rtStart != INVALID_REFTIME)
    {
        m_OutputTimeStamps.insert(rtStart);
    }
}

void CDecTimeManager::OnVideoParamsChanged(double frameRate)
{
    if (frameRate < 1)
        return;

    FixFrameRate(frameRate);

    // When this event happens we leave ivtc.
    SetInverseTelecine(false);
}

void CDecTimeManager::FixFrameRate(double frameRate)
{
    // Too close - probably inaccurate calculations
    if (fabs(m_dFrameRate - frameRate) < 0.001)
        return;

    // Modify previous time stamp to reflect frame rate change
    if (m_dFrameRate > 1 && m_rtPrevStart != INVALID_REFTIME)
    {
        m_rtPrevStart += (REFERENCE_TIME)(0.5 + 1e7 / m_dFrameRate);
        m_rtPrevStart -= (REFERENCE_TIME)(0.5 + 1e7 / frameRate);
    }

    m_dFrameRate = frameRate;
    MSDK_TRACE("QsDecoder: frame rate is %0.3f\n", (float)(m_dFrameRate));
}

bool CDecTimeManager::CalcCurrentFrameRate(double& tmpFrameRate, size_t nQueuedFrames)
{
    tmpFrameRate = 0;
    size_t len = m_OutputTimeStamps.size();

    // Need enough frames and that the time stamps are all valid.
    if (len < 4 || nQueuedFrames > len)
        return false;

    vector<REFERENCE_TIME> deltaTimes;
    REFERENCE_TIME prev = INVALID_REFTIME;
    for (auto it = m_OutputTimeStamps.begin(); it != m_OutputTimeStamps.end(); ++it)
    {
        if (INVALID_REFTIME == prev)
        {
            prev = *it;
        }
        else
        {
            REFERENCE_TIME t = *it ;
            deltaTimes.push_back(t - prev);
            prev = t;
        }
    }

    // Check for consistency
    len = deltaTimes.size();
    REFERENCE_TIME d = deltaTimes[0];
    bool accurateFR = true;
    for (size_t i = 1; i < len; ++i)
    {
        // Check if less than 1ms apart
        // Note: many times time stamps are rounded to the nearest ms.
        if (abs(d - deltaTimes[i]) > 12000)
        {
            accurateFR = false;
            break;
        }
    }

    // Can't measure accurately...
    if (!accurateFR)
        return false;

    tmpFrameRate = (1e7 * len) / (*m_OutputTimeStamps.rbegin() - *m_OutputTimeStamps.begin());
    if (fabs(tmpFrameRate - m_dFrameRate) > 1)
    {
        // Fine tune the frame rate
        // Try NTSC ranges
        if (InRange(m_dFrameRate, 59.93, 59.95) || InRange(m_dFrameRate, 29.96, 29.98) || InRange(m_dFrameRate, 23.96, 23.98))
        {
            if (InRange(tmpFrameRate, 28.0, 32.0))
            {
                tmpFrameRate = fps2997;
            }
            else if (InRange(tmpFrameRate, 22.0, 26.0))
            {
                tmpFrameRate = fps23976;
            }
            else
            {
                tmpFrameRate = floor(tmpFrameRate + 0.5);
            }
        }
        // PC/PAL ranges
        else
        {
            tmpFrameRate = floor(tmpFrameRate + 0.5);
        }

        return true;
    }

    return false;
}
