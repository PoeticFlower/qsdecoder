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
#include "QuickSyncUtils.h"
#include "QsWorkerThread.h"
#include "QsThreadPool.h"

CQsThreadPool* CQsThreadPool::s_Instance = NULL;
size_t CQsThreadPool::s_nRefCount = 0;

CQsThreadPool::CQsThreadPool() :
    m_WorkStartedEvent(false),
    m_WorkFinishedEvent(false)
{
    s_Instance = this;
    s_nRefCount = 1;

    MSDK_ZERO_VAR(m_pThreads);
    m_nThreadCount = m_nRunningThreadsCount = 2; //GetCoreCount();

    // Allocate all worker threads
    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        //the following line creates a thread with a message map
        m_pThreads[i] = new CQsWorkerThread((int)i, THREAD_PRIORITY_HIGHEST, 0, m_WorkStartedEvent, m_WorkFinishedEvent);

        if (NULL == m_pThreads[i])
        {
            MSDK_TRACE("QsDecoder: thread pool init failed!\n");
            return;
        }
    }

    // Wait for all threads to initialize
    m_WorkFinishedEvent.Wait(INFINITE);

    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        m_pThreads[i]->SetState(CQsWorkerThread::stRunTask);
    }
}

CQsThreadPool::~CQsThreadPool()
{
    CQsAutoLock lock(&m_csLock);
    
    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        m_pThreads[i]->SetState(CQsWorkerThread::stQuit);
    }

    // Free thread from their lock
    m_WorkStartedEvent.Unlock();

    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        delete m_pThreads[i];
    }
}

HRESULT CQsThreadPool::Run(IQsTask* pTask)
{
    CQsThreadPool* p = GetInstance();
    if (NULL == p)
    {
        return E_UNEXPECTED;
    }

    return p->doRun(pTask);
}

HRESULT CQsThreadPool::doRun(IQsTask* pTask)
{
    size_t taskCount = pTask->TaskCount();
    if (pTask == NULL || taskCount < 1)
    {
        return E_INVALIDARG;
    }

    // Special case - do not use thread pool for single threaded tasks
    if (taskCount == 1 || m_nThreadCount == 1)
    {
        for (size_t i = 0; i < taskCount; ++i)
        {
            pTask->RunTask(i);
        }
    }

#ifdef CHECK_FOR_DEADLOCKS
    if (IsBusy())
    {
        MSDK_TRACE(__FUNCTION__ " was called on a locked object.\n"
            "Possible deadlock!\n");
    }
#endif

    // Dispatch the tasks
    {
        CQsAutoLock lock(&m_csLock);

        // Wait for worker threads to become ready
        while (m_nRunningThreadsCount != m_nThreadCount)
        {
             _mm_pause();
             _mm_pause();
             _mm_pause();
             _mm_pause();
        }

        m_WorkFinishedEvent.Lock();
        for (size_t i = 0; i < m_nThreadCount; ++i)
        {
            m_pThreads[i]->SetTask(pTask);
            m_pThreads[i]->SetState(CQsWorkerThread::stRunTask);
        }
        
        // Signal threads to start working
        m_WorkStartedEvent.Unlock();

        // Wait for all threads to finish
        m_WorkFinishedEvent.Wait(INFINITE);
    }
    
    return S_OK;
}

void CQsThreadPool::OnThreadFinished()
{
    // Last worker thread
    if (0 == InterlockedDecrement(&m_nRunningThreadsCount))
    {
        m_WorkStartedEvent.Lock();
        m_WorkFinishedEvent.Unlock();
    }
    // Other worker threads
    else
    {
        m_WorkFinishedEvent.Wait(INFINITE);
    }

    InterlockedIncrement(&m_nRunningThreadsCount);
}

HRESULT CQsThreadPool::SetThreadPoolPriority(int nPriority)
{
    if (nPriority < THREAD_PRIORITY_IDLE  || nPriority > THREAD_PRIORITY_TIME_CRITICAL)
        return E_INVALIDARG;

    CQsThreadPool* p = CQsThreadPool::GetInstance();
    ASSERT(p != NULL);
    if (p == NULL)
    {
        return E_UNEXPECTED;
    }

    if (p->m_nThreadCount < 1)
        return E_FAIL;

    for (size_t i = 0; i < p->m_nThreadCount; ++i)
    {
        ::SetThreadPriority(p->m_pThreads[i]->GetThreadHandle(), nPriority);
    }

    return S_OK;
}

CQsThreadPool* CQsThreadPool::GetInstance()
{
    ASSERT(s_Instance != NULL);
    return s_Instance;
}

void CQsThreadPool::CreateThreadPool()
{
    if (NULL == s_Instance)
    {
        new CQsThreadPool();
    }
    else
    {
        ++s_nRefCount;
    }
}

void CQsThreadPool::DestroyThreadPool()
{
    if (0 == --s_nRefCount)
    {
        MSDK_SAFE_DELETE(s_Instance);
    }
}
