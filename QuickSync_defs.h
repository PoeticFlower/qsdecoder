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

#define MSDK_DEC_WAIT_INTERVAL 60000
#define MSDK_ENC_WAIT_INTERVAL 10000
#define MSDK_VPP_WAIT_INTERVAL 60000
#define MSDK_WAIT_INTERVAL MSDK_DEC_WAIT_INTERVAL + 3 * MSDK_VPP_WAIT_INTERVAL + MSDK_ENC_WAIT_INTERVAL // an estimate for the longest pipeline we have in samples

#define MSDK_INVALID_SURF_IDX 0xFFFF

#define MSDK_MAX_FILENAME_LEN 1024

#ifndef countof
#   define countof(array) (size_t)(sizeof(array)/sizeof(array[0]))
#endif

#define MSDK_PRINT_RET_MSG(ERR) \
{\
    TCHAR msg[1024];    \
    _stprintf_s(msg, countof(msg), _T("\nReturn on error: error code %d,\t%s\t%d\n\n"), ERR, _T(__FILE__), __LINE__); \
    OutputDebugString(msg); \
    _tperror(msg);\
}
#define MSDK_CHECK_ERROR(P, X, ERR)              { if ((X) == (P)) { MSDK_PRINT_RET_MSG(ERR); return ERR; } }
#define MSDK_CHECK_NOT_EQUAL(P, X, ERR)          { if ((X) != (P)) { MSDK_PRINT_RET_MSG(ERR); return ERR; } }
#define MSDK_CHECK_RESULT_P_RET(P, X)            { if ((X) != (P)) { return P; } }
#define MSDK_CHECK_POINTER(P, ERR)               { if (!(P))       { return ERR;}}
#define MSDK_CHECK_POINTER_NO_RET(P)             { if (!(P))       { return; } }
#define MSDK_IGNORE_MFX_STS(P, X)                { if ((X) == (P)) { P = MFX_ERR_NONE; } }

#define MSDK_SAFE_DELETE(P)                      { delete P; P = NULL; }
#define MSDK_SAFE_DELETE_ARRAY(P)                { delete[] P; P = NULL; }
#define MSDK_SAFE_RELEASE(P)                     { if (P) { P->Release(); P = NULL; } }

#define MSDK_ZERO_MEMORY(P, S)                   { memset(P, 0, S);}
#define MSDK_ZERO_VAR(VAR)                       { memset(&VAR, 0, sizeof(VAR)); }
#define MSDK_ALIGN16(SZ)                         ((SZ + 15) & (~15)) // round up to a multiple of 16
#define MSDK_ALIGN32(SZ)                         ((SZ + 31) & (~31)) // round up to a multiple of 32
#define MSDK_ARRAY_LEN(value)                    (sizeof(value) / sizeof(value[0]))

#ifdef _DEBUG
#   define ASSERT(_x_)  { if (!(_x_)) DebugAssert(TEXT(#_x_),TEXT(__FILE__),__LINE__); }
#else
#   define ASSERT(x)
#endif

#ifdef _DEBUG
#   define MSDK_TRACE(_format, ...)                                                        \
{                                                                                          \
    char msg[256];                                                                         \
    _snprintf_s(msg, MSDK_ARRAY_LEN(msg), MSDK_ARRAY_LEN(msg) - 1, _format, __VA_ARGS__);  \
    OutputDebugStringA(msg);                                                               \
}
// MSDK_VTRACE should be used for very frequent prints
#   ifdef VERBOSE
#       define MSDK_VTRACE MSDK_TRACE
#   else
#       define MSDK_VTRACE
#   endif
#else
#   define MSDK_TRACE(_format, ...)
#   define MSDK_VTRACE(_format, ...)
#endif

// A macro to disallow the copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
    TypeName(const TypeName&);             \
    void operator=(const TypeName&);   

//
// FourCC
//

// VC1
#define FOURCC_VC1  mmioFOURCC('W','V','C','1')
#define FOURCC_WMV3 mmioFOURCC('W','M','V','3')

//MPEG2
#define FOURCC_mpg2 mmioFOURCC('m','p','g','2')
#define FOURCC_MPG2 mmioFOURCC('M','P','G','2')

//H264
#define FOURCC_H264 mmioFOURCC('H','2','6','4')
#define FOURCC_X264 mmioFOURCC('X','2','6','4')
#define FOURCC_h264 mmioFOURCC('h','2','6','4')
#define FOURCC_avc1 mmioFOURCC('a','v','c','1')
#define FOURCC_VSSH mmioFOURCC('V','S','S','H')
#define FOURCC_DAVC mmioFOURCC('D','A','V','C')
#define FOURCC_PAVC mmioFOURCC('P','A','V','C')
#define FOURCC_AVC1 mmioFOURCC('A','V','C','1')

// Output formats
#define FOURCC_NV12 mmioFOURCC('N','V','1','2')

// compatibility with SDK v2.0
#ifndef MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET
#   define MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET MFX_MEMTYPE_DXVA2_DECODER_TARGET
#endif

#ifndef MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET
#   define MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET MFX_MEMTYPE_DXVA2_PROCESSOR_TARGET
#endif

#ifndef MFX_HANDLE_D3D9_DEVICE_MANAGER
#   define MFX_HANDLE_D3D9_DEVICE_MANAGER MFX_HANDLE_DIRECT3D_DEVICE_MANAGER9
#endif

