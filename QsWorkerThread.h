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

// Forward declarations
class CQsThreadPool;

class CQsWorkerThread
{
public:
    CQsWorkerThread(int instanceID, int nPriority, DWORD dwCreateFlags);
    ~CQsWorkerThread();

    unsigned int GetThreadId() { return m_ThreadID; }
    BOOL SetThreadPriority(int nPriority)
    {
        return ::SetThreadPriority(m_hThread, nPriority);
    }

    void StartTask() { m_StartTaskEvent.Unlock(); }
    
private:
    DWORD Run();
    void RunTask();
    static unsigned  __stdcall ProcessorWorkerThreadProc(void* lpParameter);

    //instance ID of thread 
    int          m_InstanceID;
    unsigned     m_ThreadID;
    HANDLE       m_hThread;

    // Thread pool resources
    CQsEvent       m_StartTaskEvent;
    CQsThreadPool* m_pThreadPool;
};

