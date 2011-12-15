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
#include "base_allocator.h"

/////////////////////////////////////////////////////////////////////////////////////////////
//    MFXFrameAllocator
/////////////////////////////////////////////////////////////////////////////////////////////
MFXFrameAllocator::MFXFrameAllocator()
{
    MSDK_ZERO_MEMORY(reserved, sizeof(reserved));
    pthis = this;
    Alloc = Alloc_;
    Lock  = Lock_;
    Free  = Free_;
    Unlock = Unlock_;
    GetHDL = GetHDL_;
}

mfxStatus MFXFrameAllocator::Alloc_(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXFrameAllocator*)(pthis))->AllocFrames(request, response);
}

mfxStatus MFXFrameAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXFrameAllocator*)(pthis))->LockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXFrameAllocator*)(pthis))->UnlockFrame(mid, ptr);
}

mfxStatus MFXFrameAllocator::Free_(mfxHDL pthis, mfxFrameAllocResponse* response)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXFrameAllocator*)(pthis))->FreeFrames(response);
}

mfxStatus MFXFrameAllocator::GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL* handle)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXFrameAllocator*)(pthis))->GetFrameHDL(mid, handle);
}

/////////////////////////////////////////////////////////////////////////////////////////////
//    BaseFrameAllocator
/////////////////////////////////////////////////////////////////////////////////////////////
mfxStatus BaseFrameAllocator::CheckRequestType(mfxFrameAllocRequest* request)
{
    MSDK_CHECK_POINTER(request, MFX_ERR_NULL_PTR);

    // check that Media SDK component is specified in request 
    return ((request->Type & MEMTYPE_FROM_MASK) != 0) ? MFX_ERR_NONE : MFX_ERR_UNSUPPORTED;
}

mfxStatus BaseFrameAllocator::AllocFrames(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
    if (0 == request || 0 == response || 0 == request->NumFrameSuggested)
        return MFX_ERR_MEMORY_ALLOC; 

    if (MFX_ERR_NONE != CheckRequestType(request))
        return MFX_ERR_UNSUPPORTED;

    mfxStatus sts = MFX_ERR_NONE;

    if ((request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) && (request->Type & MFX_MEMTYPE_FROM_DECODE))
    {
        // external decoder allocations
        
        if (m_externalDecoderResponse.m_refCount > 0)
        {            
            // check if enough frames were allocated
            if (request->NumFrameMin > m_externalDecoderResponse.m_response.NumFrameActual)
                return MFX_ERR_MEMORY_ALLOC;

            m_externalDecoderResponse.m_refCount++;
            // return existing response
            *response = m_externalDecoderResponse.m_response;
        }
        else 
        {
            sts = AllocImpl(request, response);
            if (sts == MFX_ERR_NONE)
            {
                m_externalDecoderResponse.m_response = *response;
                m_externalDecoderResponse.m_refCount = 1;
                m_externalDecoderResponse.m_type = request->Type & MEMTYPE_FROM_MASK;
            }           
        }
    }
    else
    {
        // internal allocations

        // reserve space before allocation to avoid memory leak
        m_responses.push_back(mfxFrameAllocResponse());

        sts = AllocImpl(request, response);
        if (sts == MFX_ERR_NONE)
        {
            m_responses.back() = *response;
        }
        else
        {
            m_responses.pop_back();
        }
    }

    return sts;
}

bool BaseFrameAllocator::IsSame(const mfxFrameAllocResponse& l, const mfxFrameAllocResponse& r)
{
    return l.mids != 0 && r.mids != 0 && l.mids[0] == r.mids[0] && l.NumFrameActual == r.NumFrameActual;
}

mfxStatus BaseFrameAllocator::FreeFrames(mfxFrameAllocResponse* response)
{
    if (response == 0)
        return MFX_ERR_INVALID_HANDLE;

    mfxStatus sts = MFX_ERR_NONE;
    
    // check whether response is m_externalDecoderResponse
    if (m_externalDecoderResponse.m_refCount > 0 && IsSame(*response, m_externalDecoderResponse.m_response))
    {
        if (--m_externalDecoderResponse.m_refCount == 0)
        {
            sts = ReleaseResponse(response);
            m_externalDecoderResponse.Reset();
        }
        return sts;
    }

    // if not found so far, then search in internal responses
    for (Iter i = m_responses.begin(); i != m_responses.end(); ++i)
    {
        if (IsSame(*response, *i))
        {
            sts = ReleaseResponse(response);
            m_responses.erase(i);
            return sts;
        }
    }

    // not found anywhere, report an error
    return MFX_ERR_INVALID_HANDLE;
}

mfxStatus BaseFrameAllocator::Close()
{
    ReleaseResponse(&(m_externalDecoderResponse.m_response));
    m_externalDecoderResponse.Reset();
    
    while (!m_responses.empty())
    {        
        ReleaseResponse(&(*m_responses.begin()));
        m_responses.pop_front();
    }
    
    return MFX_ERR_NONE;
}

/////////////////////////////////////////////////////////////////////////////////////////////
//    MFXBufferAllocator
/////////////////////////////////////////////////////////////////////////////////////////////
MFXBufferAllocator::MFXBufferAllocator()
{    
    pthis = this;
    Alloc = Alloc_;
    Lock  = Lock_;
    Free  = Free_;
    Unlock = Unlock_;
}

mfxStatus MFXBufferAllocator::Alloc_(mfxHDL pthis, mfxU32 nbytes, mfxU16 type, mfxMemId* mid)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXBufferAllocator*)pthis)->AllocBuffer(nbytes, type, mid);
}

mfxStatus MFXBufferAllocator::Lock_(mfxHDL pthis, mfxMemId mid, mfxU8** ptr)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXBufferAllocator*)pthis)->LockBuffer(mid, ptr);
}

mfxStatus MFXBufferAllocator::Unlock_(mfxHDL pthis, mfxMemId mid)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXBufferAllocator*)pthis)->UnlockBuffer(mid);
}

mfxStatus MFXBufferAllocator::Free_(mfxHDL pthis, mfxMemId mid)
{
    MSDK_CHECK_POINTER(pthis, MFX_ERR_MEMORY_ALLOC);
    return ((MFXBufferAllocator*)pthis)->FreeBuffer(mid);
}
