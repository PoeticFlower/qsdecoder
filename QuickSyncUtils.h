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

// SSE4.1 based memcpy that copies from video memory to system memory
void* gpu_memcpy(void* d, const void* s, size_t _size);
// Finds greatest common divider
mfxU32 GCD(mfxU32 a, mfxU32 b);
// Pixel Aspect Ratio to Display Aspect Ratio conversion
mfxStatus PARtoDAR(DWORD parw, DWORD parh, DWORD w, DWORD h, DWORD& darw, DWORD& darh);
// Display Aspect Ratio to Pixel Aspect Ratio conversion
mfxStatus DARtoPAR(mfxU32 darw, mfxU32 darh, mfxU32 w, mfxU32 h, mfxU16& parw, mfxU16& parh);
// Name from codec identifier (MSDK enum)
const char* GetCodecName(DWORD codec);
// Name of codec's profile - profile identifier is accoding to DirectShow
const char* GetProfileName(DWORD codec, DWORD profile);

#ifdef _DEBUG
// Set current thread name in VS debugger
void SetThreadName(LPCSTR szThreadName, DWORD dwThreadID = -1 /* current thread */);
// Print assert to debugger output
void DebugAssert(const TCHAR *pCondition,const TCHAR *pFileName, int iLine);
#endif

// Wrapper for low level critical section
class CQsLock
{
    friend class CQsAutoLock;
    friend class CQsAutoUnlock;
public:
    CQsLock()     { InitializeCriticalSection(&m_CritSec); }
    ~CQsLock()    { DeleteCriticalSection(&m_CritSec); }

private:
    // Lock and Unlock are private nad can only be called from
    // the CQsAutoLock/CQsAutoUnlock classes
    __forceinline void Lock()   { EnterCriticalSection(&m_CritSec); }
    __forceinline void Unlock() { LeaveCriticalSection(&m_CritSec); }

    // Make copy constructor and assignment operator inaccessible
    DISALLOW_COPY_AND_ASSIGN(CQsLock)
    CRITICAL_SECTION m_CritSec;
};

// Locks a critical section, and unlocks it automatically
// when the auto-lock object goes out of scope
class CQsAutoLock
{
public:
    CQsAutoLock(CQsLock* plock)
    {
        m_pLock = plock;
        if (m_pLock != NULL)
            m_pLock->Lock();
    }

    ~CQsAutoLock()
    {
        if (m_pLock != NULL)
            m_pLock->Unlock();
    }

private:
    DISALLOW_COPY_AND_ASSIGN(CQsAutoLock);
    CQsLock* m_pLock;
};

// Unlocks a critical section, and locks it automatically
// when the auto-unlock object goes out of scope
class CQsAutoUnlock
{
public:
    CQsAutoUnlock(CQsLock* plock)
    {
        m_pLock = plock;
        if (m_pLock != NULL)
            m_pLock->Unlock();
    }

    ~CQsAutoUnlock()
    {
        if (m_pLock != NULL)
            m_pLock->Lock();
    }

private:
    DISALLOW_COPY_AND_ASSIGN(CQsAutoUnlock);
    CQsLock* m_pLock;
};

// Thread safe queue template class
// Assumes a maximum capacity given by the user for
// the HasCapacity method to make sense.
template<class T> class CQsThreadSafeQueue : public CQsLock
{
public:
    CQsThreadSafeQueue() : m_Capacity(0)
    {
    }

    ~CQsThreadSafeQueue()
    {
        // Better safe than sorry
        CQsAutoLock lock(this);
    }

    __forceinline void Push(const T& item)
    {
        CQsAutoLock lock(this);
        m_Queue.push_back(item);
        m_Capacity = max(m_Queue.size(), m_Capacity);
    }

    __forceinline T Pop()
    {
        CQsAutoLock lock(this);
        if (m_Queue.empty())
            return T();

        T res = m_Queue.front();
        m_Queue.pop_front();
        return res;
    }

    __forceinline size_t Size()
    {
        CQsAutoLock lock(this);
        return m_Queue.size();
    }

    __forceinline void SetCapacity(size_t capacity)
    {
        CQsAutoLock lock(this);
        m_Capacity = max(capacity, m_Capacity);
    }

    __forceinline size_t GetCapacity()
    {
        CQsAutoLock lock(this);
        return m_Capacity;
    }

    __forceinline bool HasCapacity()
    {
        CQsAutoLock lock(this);
        return m_Queue.size() < m_Capacity;
    }
    
    __forceinline bool Empty()
    {
        CQsAutoLock lock(this);
        return m_Queue.empty();
    }

private:
    std::deque<T> m_Queue;
    size_t m_Capacity;
};

// Simple SSE friendly buffer class
class CQsAlignedBuffer
{
public:
    CQsAlignedBuffer(size_t bufferSize) : m_BufferSize(bufferSize)
    {
        m_Buffer = (BYTE*)_aligned_malloc(m_BufferSize, 16);
    }

    ~CQsAlignedBuffer()
    {
        _aligned_free(m_Buffer);
    }

    __forceinline size_t GetBufferSize() { return m_BufferSize; }
    __forceinline BYTE* GetBuffer() { return m_Buffer; }

private:
    size_t m_BufferSize;
    BYTE* m_Buffer;
};
