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

enum TState
{
    stQuit,
    stRunTask
};

class CQsThreadPool
{
    friend class CQsWorkerThread;
public:
    // Constructs a thread pool
    static void CreateThreadPool();

    // Destroys the pool
    static void DestroyThreadPool();

    // Run a task object
    static HRESULT Run(IQsTask* pTask);

    // Sets the threads priority - default is NORMAL 
    static HRESULT SetThreadPoolPriority(int nPriority);

private:
    CQsThreadPool();
    ~CQsThreadPool();

    // Callback used by the calling threads to signal end of task
    void OnTaskFinished();

    // Runs the task on the singelton pool
    HRESULT doRun(IQsTask* pTask);

    TState GetState() const { return m_State; }
    void   SetState(TState state) { m_State = state; }

    IQsTask* GetTask() const { return m_pTask; }
    void     SetTask(IQsTask* pTask) { m_pTask = pTask; }

    // Get's the one and only instance
    static CQsThreadPool* GetInstance();

    CQsWorkerThread* m_pThreads[QS_MAX_CPU_CORES];
    size_t           m_nThreadCount;
    volatile size_t  m_nRunningThreadsCount;
    TState           m_State;
    IQsTask*         m_pTask;

    CQsLock  m_csLock;              // Object lock
    CQsEvent m_WorkFinishedEvent;   // Event to signal end of run

    // Static members
    static CQsThreadPool* s_Instance;
    static size_t s_nRefCount;
};
