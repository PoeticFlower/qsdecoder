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
size_t CQsThreadPool::s_nRefCount        = 0;

CQsThreadPool::CQsThreadPool() :
    m_WorkFinishedEvent(false, false)
{
    s_Instance = this;

    MSDK_ZERO_VAR(m_pThreads);
    m_nThreadCount = m_nRunningThreadsCount = 2;

    // Allocate all worker threads
    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        // The following line creates a worker thread
        m_pThreads[i] = new CQsWorkerThread((int)i, THREAD_PRIORITY_HIGHEST, 0);

        if (NULL == m_pThreads[i])
        {
            MSDK_TRACE("QsDecoder: thread pool init failed!\n");
            return;
        }
    }

    // Wait for all threads to initialize
    m_WorkFinishedEvent.Wait(INFINITE);

    SetState(stRunTask);
}

CQsThreadPool::~CQsThreadPool()
{
    CQsAutoLock lock(&m_csLock);
    
    SetState(stQuit);

    for (size_t i = 0; i < m_nThreadCount; ++i)
    {
        delete m_pThreads[i];
    }
}

HRESULT CQsThreadPool::Run(IQsTask* pTask)
{
    CQsThreadPool* p = GetInstance();
    MSDK_CHECK_POINTER(p, E_UNEXPECTED);
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

    // Dispatch the tasks
    {
        CQsAutoLock lock(&m_csLock);

        ASSERT(m_nRunningThreadsCount == 0);

        SetTask(pTask);

        m_nRunningThreadsCount = m_nThreadCount;

        // Signal threads to start working
        for (size_t i = 0; i < m_nThreadCount; ++i)
        {
            m_pThreads[i]->StartTask();
        }

        // Wait for all threads to finish
        m_WorkFinishedEvent.Wait(INFINITE);
    }
    
    return S_OK;
}

void CQsThreadPool::OnTaskFinished()
{
    // Last worker thread
    if (0 == InterlockedDecrement(&m_nRunningThreadsCount))
    {
        m_WorkFinishedEvent.Unlock();
    }
}

HRESULT CQsThreadPool::SetThreadPoolPriority(int nPriority)
{
    if (nPriority < THREAD_PRIORITY_IDLE || nPriority > THREAD_PRIORITY_TIME_CRITICAL)
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
        p->m_pThreads[i]->SetThreadPriority(nPriority);
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

    ++s_nRefCount;
}

void CQsThreadPool::DestroyThreadPool()
{
    if (0 == --s_nRefCount)
    {
        MSDK_SAFE_DELETE(s_Instance);
    }
}
