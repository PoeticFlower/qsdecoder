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
#include "QsWorkerThread.h"
#include "QsThreadPool.h"

//comment this to disable CPU binding
//#define BIND_QS_THREAD_TO_CPU

//makes sure pool is used well
#ifdef _DEBUG
#   define CHECK_FOR_DEADLOCKS
#endif // _DEBUG

// CQsWorkerThread
CQsWorkerThread::CQsWorkerThread(int instanceID, int nPriority, DWORD dwCreateFlags,
    CQsEvent& workStartedEvent, CQsEvent& workFinishedEvent) :
    m_WorkStartedEvent(workStartedEvent),
    m_WorkFinishedEvent(workFinishedEvent)

{
    m_pThreadPool = CQsThreadPool::GetInstance();
    ASSERT(m_pThreadPool != NULL);

    m_State = stReady;
    m_InstanceID = instanceID;
    m_hThread = (HANDLE)_beginthreadex(NULL, 0, &WorkerThreadProc, this, dwCreateFlags, &m_ThreadID);
    ::SetThreadPriority(m_hThread, nPriority);

    // Set thread name (for visual studio debugger)
    char threadName[256];
    sprintf_s(threadName, 256, "Qs Worker Thread #%i", instanceID);
    SetThreadName(threadName, m_ThreadID);
}

CQsWorkerThread::~CQsWorkerThread()
{
    //wait for thread to finish
    WaitForSingleObject(m_hThread, INFINITE);
    CloseHandle(m_hThread);
}

void CQsWorkerThread::SetState(TState state)
{
    m_State = state;
}

DWORD CQsWorkerThread::Run()
{
    // Signal that thread is initialized
    m_pThreadPool->OnThreadFinished();
    
    bool bStop = false;
    while (!bStop)
    {
        m_WorkStartedEvent.Wait(INFINITE);

        switch (m_State)
        {
        case stRunTask:
            RunTask();
            break;

        case stQuit:
            bStop = true;
            break;

        default:
            MSDK_TRACE("CQsWorkerThread:\nUnknown event!\n");
        }
    }

    return 0;
}

void CQsWorkerThread::SetTask(IQsTask* pTask)
{
    m_pTask = pTask;
}

// CQsWorkerThread message handlers

/*******************************************************************
     Asynchronous run of a block 
 *******************************************************************/
void CQsWorkerThread::RunTask()
{
    if (NULL == m_pTask)
        return;

    size_t taskCount = m_pTask->TaskCount();

    // No work for this thread - just decrement the active thread count
    if (m_InstanceID < (int)taskCount)
    {
        m_pTask->RunTask(m_InstanceID);
    }   

    // Signal that this thread is done with the task
    m_pThreadPool->OnThreadFinished();
}

unsigned  __stdcall CQsWorkerThread::WorkerThreadProc(void* lpParameter)
{
    CQsWorkerThread* pObject = (CQsWorkerThread*)lpParameter;
    
    //enter message loop
    ASSERT(pObject);
    DWORD rc = pObject->Run();
    _endthreadex(rc);
    return rc;
}
