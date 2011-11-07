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

void* gpu_memcpy(void* d, const void* s, size_t _size);
mfxU32 GCD(mfxU32 a, mfxU32 b);
mfxStatus PARtoDAR(DWORD parw, DWORD parh, DWORD w, DWORD h, DWORD& darw, DWORD& darh);
mfxStatus DARtoPAR(mfxU32 darw, mfxU32 darh, mfxU32 w, mfxU32 h, mfxU16& parw, mfxU16& parh);
void CopyBitstream(const mfxBitstream& src,  mfxBitstream& trg);
const char* GetCodecName(DWORD codec);
const char* GetProfileName(DWORD codec, DWORD profile);

#ifdef _DEBUG
void SetThreadName(LPCSTR szThreadName, DWORD dwThreadID = -1 /* current thread */);
void DebugAssert(const TCHAR *pCondition,const TCHAR *pFileName, int iLine);
#endif

// wrapper for whatever critical section we have
class CQsLock
{
public:
    CQsLock()     { InitializeCriticalSection(&m_CritSec); }
    ~CQsLock()    { DeleteCriticalSection(&m_CritSec); }
    void Lock()   { EnterCriticalSection(&m_CritSec); }
    void Unlock() { LeaveCriticalSection(&m_CritSec); }

protected:
    // make copy constructor and assignment operator inaccessible
    DISALLOW_COPY_AND_ASSIGN(CQsLock)
//    CQsLock(const CQsLock&);
//    CQsLock& operator=(const CQsLock &);

    CRITICAL_SECTION m_CritSec;
};

// locks a critical section, and unlocks it automatically
// when the lock goes out of scope
class CQsAutoLock
{
protected:
    CQsLock * m_pLock;

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

    // make copy constructor and assignment operator inaccessible
    CQsAutoLock(const CQsAutoLock &refAutoLock);
    CQsAutoLock &operator=(const CQsAutoLock &refAutoLock);
};
