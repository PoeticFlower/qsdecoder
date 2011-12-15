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
#include "TimeManager.h"
#include "QuickSync.h"

#define TO_STRING(s) #s
#ifdef __INTEL_COMPILER
#   define COMPILER "ICL"
#   define COMPILER_VER ((float)__INTEL_COMPILER) / 100)
#elif defined(_MSC_VER)
#   define COMPILER "MSVC"
#   if _MSC_VER==1600
#       define COMPILER_VER "2010"
#   elif _MSC_VER==1500
#       define COMPILER_VER "2008"
#   endif
#else
#   define COMPILER "Unknown and not supported"
#endif

IQuickSyncDecoder* __stdcall createQuickSync()
{
    return new CQuickSync();
}

void __stdcall destroyQuickSync(IQuickSyncDecoder* p)
{
    delete (CQuickSync*)(p);
}

void __stdcall getVersion(char* ver, const char** license)
{
    strcpy(ver, QS_DEC_VERSION " by Eric Gur. Compiled using " COMPILER " " COMPILER_VER  " (" __DATE__ " " __TIME__ ")");
    *license = "(C) 2011 Intel\xae Corp.";
}
