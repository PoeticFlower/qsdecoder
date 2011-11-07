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

struct mfxAllocatorParams
{
    virtual ~mfxAllocatorParams(){};
};

// this class implements methods declared in mfxFrameAllocator structure
// simply redirecting them to virtual methods which should be overridden in derived classes
class MFXFrameAllocator : public mfxFrameAllocator
{
public:
    MFXFrameAllocator();
    virtual ~MFXFrameAllocator() {}

     // optional method, override if need to pass some parameters to allocator from application
    virtual mfxStatus Init(mfxAllocatorParams* pParams) = 0;
    virtual mfxStatus Close() = 0;

protected:
    virtual mfxStatus AllocFrames(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response) = 0;
    virtual mfxStatus LockFrame(mfxMemId mid, mfxFrameData* ptr) = 0;
    virtual mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData* ptr) = 0;
    virtual mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL* handle) = 0;
    virtual mfxStatus FreeFrames(mfxFrameAllocResponse* response) = 0;  

private:
    static mfxStatus Alloc_(mfxHDL pthis, mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
    static mfxStatus Lock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
    static mfxStatus Unlock_(mfxHDL pthis, mfxMemId mid, mfxFrameData* ptr);
    static mfxStatus GetHDL_(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);
    static mfxStatus Free_(mfxHDL pthis, mfxFrameAllocResponse* response);
};

// This class implements basic logic of memory allocator
// Manages responses for different components according to allocation request type
// External frames of a particular component-related type are allocated in one call
// Further calls return previously allocated response.
// Ex. Preallocated frame chain with type=FROM_ENCODE | FROM_VPPIN will be returned when 
// request type contains either FROM_ENCODE or FROM_VPPIN

// This class does not allocate any actual memory
class BaseFrameAllocator: public MFXFrameAllocator
{    
public:
    BaseFrameAllocator() {}
    virtual ~BaseFrameAllocator() {}

    virtual mfxStatus Init(mfxAllocatorParams* pParams) = 0; 
    virtual mfxStatus Close();

protected:
    virtual mfxStatus AllocFrames(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);   
    virtual mfxStatus FreeFrames(mfxFrameAllocResponse* response);

    typedef std::list<mfxFrameAllocResponse>::iterator Iter;
    static const mfxU32 MEMTYPE_FROM_MASK = MFX_MEMTYPE_FROM_ENCODE | MFX_MEMTYPE_FROM_DECODE | MFX_MEMTYPE_FROM_VPPIN | MFX_MEMTYPE_FROM_VPPOUT;

    struct UniqueResponse
    {
        mfxU32 m_refCount;
        mfxFrameAllocResponse m_response;
        mfxU16 m_type;

        UniqueResponse() { Reset(); } 
        void Reset() { memset(this, 0, sizeof(*this)); }
    };

    std::list<mfxFrameAllocResponse> m_responses;
    UniqueResponse m_externalDecoderResponse;
   
    // checks responses for identity
    virtual bool IsSame(const mfxFrameAllocResponse& l, const mfxFrameAllocResponse& r);

    // checks if request is supported
    virtual mfxStatus CheckRequestType(mfxFrameAllocRequest* request);
    
    // memory type specific methods, must be overridden in derived classes

    virtual mfxStatus LockFrame(mfxMemId mid, mfxFrameData* ptr) = 0;
    virtual mfxStatus UnlockFrame(mfxMemId mid, mfxFrameData* ptr) = 0;
    virtual mfxStatus GetFrameHDL(mfxMemId mid, mfxHDL* handle) = 0;
    
    // frees memory attached to response
    virtual mfxStatus ReleaseResponse(mfxFrameAllocResponse* response) = 0;
    // allocates memory
    virtual mfxStatus AllocImpl(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response) = 0;

    template <class T>
    class safe_array
    {
    public:
        safe_array(T* ptr = 0) : m_ptr(ptr) {} // construct from object pointer            
        ~safe_array() { reset(0); }
        inline T* get()
        { // return wrapped pointer
            return m_ptr;
        }
        inline T* release()
        { // return wrapped pointer and give up ownership
            T* ptr = m_ptr;
            m_ptr = 0;
            return ptr;
        }
        inline void reset(T* ptr) 
        { // destroy designated object and store new pointer
            if (m_ptr)
            {
                delete[] m_ptr;
            }
            m_ptr = ptr;
        }        
    protected:
        T* m_ptr; // the wrapped object pointer
    };
};

class MFXBufferAllocator : public mfxBufferAllocator
{
public:
    MFXBufferAllocator();
    virtual ~MFXBufferAllocator() {}

protected:
    virtual mfxStatus AllocBuffer(mfxU32 nbytes, mfxU16 type, mfxMemId* mid) = 0;
    virtual mfxStatus LockBuffer(mfxMemId mid, mfxU8** ptr) = 0;
    virtual mfxStatus UnlockBuffer(mfxMemId mid) = 0;    
    virtual mfxStatus FreeBuffer(mfxMemId mid) = 0;  

private:
    static mfxStatus Alloc_(mfxHDL pthis, mfxU32 nbytes, mfxU16 type, mfxMemId* mid);
    static mfxStatus Lock_(mfxHDL pthis, mfxMemId mid, mfxU8** ptr);
    static mfxStatus Unlock_(mfxHDL pthis, mfxMemId mid);    
    static mfxStatus Free_(mfxHDL pthis, mfxMemId mid);
};
