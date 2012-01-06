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
#include "CodecInfo.h"

static bool CheckForSSE41();

static const bool s_SSE4_1_enabled = CheckForSSE41();

// gpu_memcpy is a memcpy style function that copied data very fast from a
// GPU tiled memory (write back)
// Performance tip: page offset (12 lsb) of both addresses should be different
//  optimally use a 2K offset between them.
void* gpu_memcpy(void* d, const void* s, size_t size)
{
    if (d == NULL || s == NULL) return NULL;

    //if memory is not aligned, use memcpy
    bool isAligned = (((size_t)(s) | (size_t)(d)) & 0xF) == 0;
    if (!(isAligned && s_SSE4_1_enabled))
    {
        return memcpy(d, s, size);
    }

    size_t reminder = size & 127; // copy 128 bytes every loop
    size_t end = 0;

    __m128i* pTrg = (__m128i*)d;
    __m128i* pTrgEnd = pTrg + ((size - reminder) >> 4);
    __m128i* pSrc = (__m128i*)s;
    
    // make sure source is synced - doesn't hurt if not needed.
    _mm_sfence();

// split main loop to 32 and 64 bit code
// 64 doesn't have inline assembly and assembly code is faster.
// TODO: write a pure ASM function for the 64 bit verrsion.
#ifdef _M_X64 
    while (pTrg < pTrgEnd)
    {
        // Emits the Streaming SIMD Extensions 4 (SSE4.1) instruction movntdqa
        // fastest method for copying GPU RAM. Availble since Penryn (45nm Core 2 Dou/Quad)
        pTrg[0] = _mm_stream_load_si128(&pSrc[0]);
        pTrg[1] = _mm_stream_load_si128(&pSrc[1]);
        pTrg[2] = _mm_stream_load_si128(&pSrc[2]);
        pTrg[3] = _mm_stream_load_si128(&pSrc[3]);
        pTrg[4] = _mm_stream_load_si128(&pSrc[4]);
        pTrg[5] = _mm_stream_load_si128(&pSrc[5]);
        pTrg[6] = _mm_stream_load_si128(&pSrc[6]);
        pTrg[7] = _mm_stream_load_si128(&pSrc[7]);
        pSrc += 8;
        pTrg += 8;
    }
#else // end of 64 bit code
    __asm
    {
        mov ecx, pSrc;
        mov edx, pTrg;
        mov eax, pTrgEnd;

        // test ending condition
        cmp edx, eax; // if (pTrgEnd >= pTrg) goto endLoop
        jae endLoop;
startLoop:
        movntdqa  xmm0,    [ecx];
        movdqa    [edx],       xmm0;
        movntdqa  xmm1,    [ecx + 16];
        movdqa    [edx + 16],  xmm1;
        movntdqa  xmm2,       [ecx + 32];
        movdqa    [edx + 32],  xmm2;
        movntdqa  xmm3,       [ecx + 48];
        movdqa    [edx + 48],  xmm3;
        movntdqa  xmm4,       [ecx + 64];
        movdqa    [edx + 64],  xmm4;
        movntdqa  xmm5,       [ecx + 80];
        movdqa    [edx + 80],  xmm5;
        movntdqa  xmm6,       [ecx + 96];
        movdqa    [edx + 96],  xmm6;
        movntdqa  xmm7,       [ecx + 112];
        movdqa    [edx + 112], xmm7;
        add  edx, 128;
        add  ecx, 128;
        cmp edx, eax; // if (pTrgEnd > pTrg) goto startLoop
        jb  startLoop;
endLoop:
    }

    pTrg = pTrgEnd;
    pSrc += (size - reminder) >> 4;

#endif // end of 32 bit code

    // copy in 16 byte steps
    if (reminder >= 16)
    {
        size = reminder;
        reminder = size & 15;
        end = size >> 4;
        for (size_t i = 0; i < end; ++i)
        {
            pTrg[i] = _mm_stream_load_si128(&pSrc[i]);
        }
    }

    // copy last bytes
    if (reminder)
    {
        __m128i temp = _mm_stream_load_si128(pSrc + end);

        char* ps = (char*)(&temp);
        char* pt = (char*)(pTrg + end);

        for (size_t i = 0; i < reminder; ++i)
        {
            pt[i] = ps[i];
        }
    }

    return d;
}

#ifdef _DEBUG
#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO
{
   DWORD dwType;     // Must be 0x1000.
   LPCSTR szName;    // Pointer to name (in user addr space).
   DWORD dwThreadID; // Thread ID (-1=caller thread).
   DWORD dwFlags;    // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

/*
    Renames current thread. Useful for identifying a thread in VS.
*/
void SetThreadName(LPCSTR szThreadName, DWORD dwThreadID)
{
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = szThreadName;
    info.dwThreadID = (DWORD)-1;
    info.dwFlags = 0;

    __try
    {
        #define MS_VC_EXCEPTION 0x406D1388
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(DWORD), (ULONG_PTR*)&info);
    }
    __except(EXCEPTION_CONTINUE_EXECUTION)
    {
    }
} 

void DebugAssert(const TCHAR* pCondition,const TCHAR* pFileName, int iLine)
{
    TCHAR szInfo[2048];
    _sntprintf_s(szInfo, sizeof(szInfo)-1, _T("%s \nAt line %d of %s\nContinue? (Cancel to debug)"), sizeof(szInfo), pCondition, iLine, pFileName);

    int msgId = MessageBox(NULL, szInfo, _T("ASSERT Failed"),
        MB_SYSTEMMODAL | MB_ICONHAND | MB_YESNOCANCEL | MB_SETFOREGROUND);
    switch (msgId)
    {
    case IDNO:              /* Kill the application */
        FatalAppExit(FALSE, _T("Application terminated"));
        break;

    case IDCANCEL:          /* Break into the debugger */
        DebugBreak();
        break;

    case IDYES:             /* Ignore assertion continue execution */
        break;
    }
}

#endif //_DEBUG

// Finds greatest common denominator
mfxU32 GCD(mfxU32 a, mfxU32 b)
{    
    if (0 == a)
        return b;
    else if (0 == b)
        return a;

    mfxU32 a1, b1;

    if (a >= b)
    {
        a1 = a;
        b1 = b;
    }
    else 
    {
        a1 = b;
        b1 = a;
    }

    // a1 >= b1;
    mfxU32 r = a1 % b1;

    while (0 != r)
    {
        a1 = b1;
        b1 = r;
        r = a1 % b1;
    }

    return b1;
}

mfxStatus PARtoDAR(DWORD parw, DWORD parh, DWORD w, DWORD h, DWORD& darw, DWORD& darh)
{
    MSDK_CHECK_ERROR(parw, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(parh, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(w, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(h, 0, MFX_ERR_UNDEFINED_BEHAVIOR);

    mfxU32 reduced_w = 0, reduced_h = 0;
    mfxU32 gcd = GCD(w, h);

    // Divide by greatest common divisor
    reduced_w =  w / gcd;
    reduced_h =  h / gcd;

    darw = reduced_w * parw;
    darh = reduced_h * parh;
    gcd = GCD(darw, darh);

    darh /= gcd;
    darw /= gcd;

    return MFX_ERR_NONE;
}

mfxStatus DARtoPAR(mfxU32 darw, mfxU32 darh, mfxU32 w, mfxU32 h, mfxU16& parw, mfxU16& parh)
{
    MSDK_CHECK_ERROR(darw, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(darh, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(w, 0, MFX_ERR_UNDEFINED_BEHAVIOR);
    MSDK_CHECK_ERROR(h, 0, MFX_ERR_UNDEFINED_BEHAVIOR);

    mfxU16 reduced_w = 0, reduced_h = 0;
    mfxU32 gcd = GCD(w, h);

    // Divide by greatest common divisor to fit into mfxU16
    reduced_w =  (mfxU16) (w / gcd);
    reduced_h =  (mfxU16) (h / gcd);

    // for mpeg2 we need to set exact values for par (standard supports only dar 4:3, 16:9, 221:100, 1:1)
    if (darw * 3 == darh * 4)
    {
        parw = 4 * reduced_h;
        parh = 3 * reduced_w;
    }
    else if (darw * 9 == darh * 16)
    {
        parw = 16 * reduced_h;
        parh = 9  * reduced_w;
    }
    else if (darw * 100 == darh * 221)
    {
        parw = 221 * reduced_h;
        parh = 100 * reduced_w;
    }
    else if (darw == darh)
    {
        parw = reduced_h;
        parh = reduced_w;
    }
    else
    {
        parw = (mfxU16)((DOUBLE)(darw * reduced_h) / (darh * reduced_w) * 1000);
        parh = 1000;
    }

    // Reduce ratio
    gcd = GCD(parw, parh);
    parw /= (mfxU16)gcd;
    parh /= (mfxU16)gcd;

    return MFX_ERR_NONE;
}

const char* GetCodecName(DWORD codec)
{
    switch(codec)
    {
    case MFX_CODEC_AVC:   return "AVC";
    case MFX_CODEC_MPEG2: return "MPEG2";
    case MFX_CODEC_VC1:   return "VC1";
    default:
        return "Unknown";
    }
}

const char* GetProfileName(DWORD codec, DWORD profile)
{
    switch(codec)
    {
    case MFX_CODEC_AVC:
        switch(profile)
        {
        case QS_PROFILE_H264_BASELINE:             return "Baseline";
        case QS_PROFILE_H264_CONSTRAINED_BASELINE: return "Constrained Baseline";
        case QS_PROFILE_H264_MAIN:                 return "Main";
        case QS_PROFILE_H264_EXTENDED:             return "Extended";
        case QS_PROFILE_H264_HIGH:                 return "High";
        case QS_PROFILE_H264_HIGH_10:              return "High 10";
        case QS_PROFILE_H264_HIGH_10_INTRA:        return "High 10 Intra";
        case QS_PROFILE_H264_HIGH_422:             return "High 4:2:2";
        case QS_PROFILE_H264_HIGH_422_INTRA:       return "High 4:2:2 Intra";
        case QS_PROFILE_H264_HIGH_444:             return "High 4:4:4";
        case QS_PROFILE_H264_HIGH_444_PREDICTIVE:  return "High 4:4:4 Preditive";
        case QS_PROFILE_H264_HIGH_444_INTRA:       return "High 4:4:4 Intra";
        case QS_PROFILE_H264_CAVLC_444:            return "CAVLC 4:4:4 Intra";
        case QS_PROFILE_H264_MULTIVIEW_HIGH:       return "Multi View High";
        case QS_PROFILE_H264_STEREO_HIGH:          return "Stereo High"; 
        }
        break;

    case MFX_CODEC_MPEG2:
        switch(profile)
        {
        case QS_PROFILE_MPEG2_422:                return "4:2:2";
        case QS_PROFILE_MPEG2_HIGH:               return "High";
        case QS_PROFILE_MPEG2_SPATIALLY_SCALABLE: return "Spatially Scalable";
        case QS_PROFILE_MPEG2_SNR_SCALABLE:       return "SNR Scalable";
        case QS_PROFILE_MPEG2_MAIN:               return "Main";
        case QS_PROFILE_MPEG2_SIMPLE:             return "Simple";
        }
        break;

    case MFX_CODEC_VC1:
        switch(profile)
        {
        case QS_PROFILE_VC1_SIMPLE:   return "Simple";
        case QS_PROFILE_VC1_MAIN:     return "Main";
        case QS_PROFILE_VC1_COMPLEX:  return "Complex";
        case QS_PROFILE_VC1_ADVANCED: return "Advanced";
        }
        break;

    default: break;
    }

    return "Unknown";
}

static bool CheckForSSE41()
{
   int CPUInfo[4];
    __cpuid(CPUInfo, 1);

    return 0 != (CPUInfo[2] & (1<<19)); //19th bit of 2nd reg means sse4.1 is enabled
}
