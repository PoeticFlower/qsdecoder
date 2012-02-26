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

// forward declarations
class CQuickSyncDecoder;
class CFrameConstructor;
class MFXFrameAllocator;

class CQuickSync : public IQuickSyncDecoder
{
public:
    CQuickSync();
    virtual ~CQuickSync();

protected:
    virtual bool getOK() { return m_OK; }
    virtual HRESULT TestMediaType(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC);
    virtual HRESULT InitDecoder(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC);
    virtual HRESULT Decode(IMediaSample* pIn);
    virtual HRESULT Flush(bool deliverFrames = true);
    HRESULT DecodeHeader(
        const AM_MEDIA_TYPE* mtIn,
        FOURCC fourCC,
        CFrameConstructor*& pFrameConstructor,
        VIDEOINFOHEADER2*& vih2,
        size_t nSampleSize,
        size_t& nVideoInfoSize,
        mfxVideoParam& videoParams);
    virtual HRESULT CheckCodecProfileSupport(DWORD codec, DWORD profile, DWORD level);

    // Marks the beginning of a flush (discard frames or reset).
    // Any samples recieved during the flush period are discarded internally.
    // Can't do anything but change flags to avoid deadlocks.
    // Called asynchroniously from the application thread.
    virtual HRESULT BeginFlush()
    {
        // Don't use a lock - causes deadlocks!
        MSDK_TRACE("QSDcoder: BeginFlush\n");
        m_bFlushing = true; 
        m_bNeedToFlush = true; // Need to flush buffers (reset) in the future (within the decoding thread!).
                               // Set to false during OnSeek or Flush.
        return S_OK;
    }

    // Marks the end of the flush period - from now on, Decode receives valid samples.
    // Called asynchroniously from the application thread
    virtual HRESULT EndFlush()
    {
        // Don't use a lock - causes deadlocks!
        MSDK_TRACE("QSDcoder: EndFlush\n");
        m_bFlushing = false;
        return S_OK;
    }

    virtual void SetD3DDeviceManager(IDirect3DDeviceManager9* pDeviceManager);
    HRESULT HandleSubType(const AM_MEDIA_TYPE* mtIn, FOURCC fourCC, mfxVideoParam& videoParams, CFrameConstructor*& pFrameContructor);
    HRESULT CopyMediaTypeToVIDEOINFOHEADER2(const AM_MEDIA_TYPE* mtIn, VIDEOINFOHEADER2*& vih2, size_t& nVideoInfoSize, size_t& nSampleSize);
    HRESULT QueueSurface(mfxFrameSurface1* pSurface, bool async);
    HRESULT ProcessDecodedFrame(mfxFrameSurface1* pSurface);
    void    ClearQueue();
    int DeliverSurface(bool bWaitForCompletion);
    virtual void SetDeliverSurfaceCallback(void* obj, TQS_DeliverSurfaceCallback func)
    {
        CQsAutoLock cObjectLock(&m_csLock);
        m_ObjParent = obj;
        m_DeliverSurfaceCallback = func;
    }
    virtual HRESULT OnSeek(REFERENCE_TIME segmentStart);
    virtual void GetConfig(CQsConfig* pConfig);
    virtual void SetConfig(CQsConfig* pConfig);

    bool SetTimeStamp(mfxFrameSurface1* pSurface, QsFrameData& frameData);
    void SetAspectRatio(VIDEOINFOHEADER2& vih2, mfxFrameInfo& FrameInfo);
    void UpdateAspectRatio(mfxFrameSurface1* pSurface, QsFrameData& frameData);
    mfxStatus ConvertFrameRate(mfxF64 dFrameRate, mfxU32& nFrameRateExtN, mfxU32& nFrameRateExtD);
    mfxStatus OnVideoParamsChanged();
    void PicStructToDsFlags(mfxU32 picStruct, DWORD& flags, QsFrameData::QsFrameStructure& frameStructure);
    inline void PushSurface(mfxFrameSurface1* pSurface);
    inline mfxFrameSurface1* PopSurface();
    void FlushOutputQueue();
    unsigned ProcessorWorkerThreadMsgLoop();

    // statics
    static void OnDecodeComplete(mfxFrameSurface1* pSurface, void* obj);
    static unsigned  __stdcall ProcessorWorkerThreadProc(void* pThis);

    // data members
    bool m_OK;
    bool m_bInitialized;
    void* m_ObjParent;
    TQS_DeliverSurfaceCallback m_DeliverSurfaceCallback;
    CQsLock             m_csLock;
    CQuickSyncDecoder*  m_pDecoder;
    mfxVideoParam       m_mfxParamsVideo;      // video params
    mfxU32              m_nPitch;
    CDecTimeManager     m_TimeManager;
    CFrameConstructor*  m_pFrameConstructor;
    size_t              m_nSegmentFrameCount; // used for debugging mostly
    volatile bool       m_bFlushing;          // like in DirectShow - current frame and data should be discarded
    volatile bool       m_bNeedToFlush;       // a flush was seen but not handled yet
    bool                m_bDvdDecoding;       // support DVD decoding
    CQsConfig           m_Config;
    HANDLE              m_hProcessorWorkerThread;
    unsigned            m_ProcessorWorkerThreadId;

    typedef std::pair<QsFrameData*, CQsAlignedBuffer*> TQsQueueItem;

    CQsThreadSafeQueue<mfxFrameSurface1*> m_DecodedFramesQueue;
    CQsThreadSafeQueue<TQsQueueItem> m_ProcessedFramesQueue; // holds frame buffers after processing
    CQsThreadSafeQueue<TQsQueueItem> m_FreeFramesPool;       // holds free frame buffers
};
