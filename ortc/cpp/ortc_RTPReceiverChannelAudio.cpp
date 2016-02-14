/*

 Copyright (c) 2015, Hookflash Inc. / Hookflash Inc.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 The views and conclusions contained in the software and documentation are those
 of the authors and should not be interpreted as representing official policies,
 either expressed or implied, of the FreeBSD Project.
 
 */

#include <ortc/internal/ortc_RTPReceiverChannelAudio.h>
#include <ortc/internal/ortc_RTPReceiverChannel.h>
#include <ortc/internal/ortc_MediaStreamTrack.h>
#include <ortc/internal/ortc_RTPPacket.h>
#include <ortc/internal/ortc_RTCPPacket.h>
#include <ortc/internal/ortc_Helper.h>
#include <ortc/internal/ortc_ORTC.h>
#include <ortc/internal/platform.h>

#include <openpeer/services/ISettings.h>
#include <openpeer/services/IHelper.h>
#include <openpeer/services/IHTTP.h>

#include <zsLib/Stringize.h>
#include <zsLib/Log.h>
#include <zsLib/XML.h>

#include <cryptopp/sha.h>

#include <webrtc/system_wrappers/include/cpu_info.h>
#include <webrtc/voice_engine/include/voe_codec.h>
#include <webrtc/voice_engine/include/voe_rtp_rtcp.h>
#include <webrtc/voice_engine/include/voe_network.h>

#ifdef _DEBUG
#define ASSERT(x) ZS_THROW_BAD_STATE_IF(!(x))
#else
#define ASSERT(x)
#endif //_DEBUG


namespace ortc { ZS_DECLARE_SUBSYSTEM(ortclib) }

namespace ortc
{
  ZS_DECLARE_TYPEDEF_PTR(openpeer::services::ISettings, UseSettings)
  ZS_DECLARE_TYPEDEF_PTR(ortc::internal::Helper, UseHelper)
  ZS_DECLARE_TYPEDEF_PTR(openpeer::services::IHelper, UseServicesHelper)
  ZS_DECLARE_TYPEDEF_PTR(openpeer::services::IHTTP, UseHTTP)

  typedef openpeer::services::Hasher<CryptoPP::SHA1> SHA1Hasher;

  namespace internal
  {
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark (helpers)
    #pragma mark


    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IICETransportForSettings
    #pragma mark

    //-------------------------------------------------------------------------
    void IRTPReceiverChannelAudioForSettings::applyDefaults()
    {
//      UseSettings::setUInt(ORTC_SETTING_SCTP_TRANSPORT_MAX_MESSAGE_SIZE, 5*1024);
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IRTPReceiverChannelAudioForRTPReceiverChannel
    #pragma mark

    //-------------------------------------------------------------------------
    ElementPtr IRTPReceiverChannelAudioForRTPReceiverChannel::toDebug(ForRTPReceiverChannelPtr object)
    {
      if (!object) return ElementPtr();
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object)->toDebug();
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr IRTPReceiverChannelAudioForRTPReceiverChannel::create(
                                                                                     RTPReceiverChannelPtr receiverChannel,
                                                                                     MediaStreamTrackPtr track,
                                                                                     const Parameters &params
                                                                                     )
    {
      return internal::IRTPReceiverChannelAudioFactory::singleton().create(receiverChannel, track, params);
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IRTPReceiverChannelAudioForMediaStreamTrack
    #pragma mark

    //-------------------------------------------------------------------------
    ElementPtr IRTPReceiverChannelAudioForMediaStreamTrack::toDebug(ForMediaStreamTrackPtr object)
    {
      if (!object) return ElementPtr();
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object)->toDebug();
    }


    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio
    #pragma mark
    
    //---------------------------------------------------------------------------
    const char *RTPReceiverChannelAudio::toString(States state)
    {
      switch (state) {
        case State_Pending:       return "pending";
        case State_Ready:         return "ready";
        case State_ShuttingDown:  return "shutting down";
        case State_Shutdown:      return "shutdown";
      }
      return "UNDEFINED";
    }
    
    //-------------------------------------------------------------------------
    RTPReceiverChannelAudio::RTPReceiverChannelAudio(
                                                     const make_private &,
                                                     IMessageQueuePtr queue,
                                                     UseChannelPtr receiverChannel,
                                                     UseMediaStreamTrackPtr track,
                                                     const Parameters &params
                                                     ) :
      MessageQueueAssociator(queue),
      SharedRecursiveLock(SharedRecursiveLock::create()),
      mReceiverChannel(receiverChannel),
      mTrack(track),
      mParameters(make_shared<Parameters>(params)),
      mModuleProcessThread(webrtc::ProcessThread::Create("RTPReceiverChannelAudioThread"))
    {
      ZS_LOG_DETAIL(debug("created"))

      ORTC_THROW_INVALID_PARAMETERS_IF(!receiverChannel)
    }

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::init()
    {
      AutoRecursiveLock lock(*this);
      IWakeDelegateProxy::create(mThisWeak.lock())->onWake();

      mVoiceEngine = rtc::scoped_ptr<webrtc::VoiceEngine, VoiceEngineDeleter>(webrtc::VoiceEngine::Create());
      mCallStats = rtc::scoped_ptr<webrtc::CallStats>(new webrtc::CallStats());
      mCongestionController = 
        rtc::scoped_ptr<webrtc::CongestionController>(new webrtc::CongestionController(
                                                                                       mModuleProcessThread.get(),
                                                                                       mCallStats.get())
                                                                                       );

      mModuleProcessThread->Start();
      mModuleProcessThread->RegisterModule(mCallStats.get());

      webrtc::VoEBase::GetInterface(mVoiceEngine.get())->Init(mTrack->getAudioDeviceModule());

      mChannel = webrtc::VoEBase::GetInterface(mVoiceEngine.get())->CreateChannel();
      webrtc::VoERTP_RTCP::GetInterface(mVoiceEngine.get())->SetLocalSSRC(mChannel, 2010);

      int numCpuCores = webrtc::CpuInfo::DetectNumberOfCores();

      mTransport = Transport::create(mThisWeak.lock());

      webrtc::AudioReceiveStream::Config config;
      config.rtp.remote_ssrc = 2000;
      config.rtp.local_ssrc = 2010;
      config.rtp.extensions.push_back(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTime, 1));
      config.voe_channel_id = mChannel;
      config.receive_transport = mTransport.get();
      config.rtcp_send_transport = mTransport.get();
      config.combined_audio_video_bwe = true;

      int ncodecs = webrtc::VoECodec::GetInterface(mVoiceEngine.get())->NumOfCodecs();
      for (int i = 0; i < ncodecs; ++i) {
        webrtc::CodecInst codec;
        webrtc::VoECodec::GetInterface(mVoiceEngine.get())->GetCodec(i, codec);
        webrtc::VoECodec::GetInterface(mVoiceEngine.get())->SetRecPayloadType(mChannel, codec);
      }

      mReceiveStream = rtc::scoped_ptr<webrtc::AudioReceiveStream>(
          new webrtc::internal::AudioReceiveStream(
                                                   mCongestionController->GetRemoteBitrateEstimator(false),
                                                   config,
                                                   mVoiceEngine.get()
                                                   ));

      webrtc::VoENetwork::GetInterface(mVoiceEngine.get())->RegisterExternalTransport(mChannel, *mTransport);

      mTrack->start();

      webrtc::VoEBase::GetInterface(mVoiceEngine.get())->StartReceive(mChannel);
      webrtc::VoEBase::GetInterface(mVoiceEngine.get())->StartPlayout(mChannel);
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudio::~RTPReceiverChannelAudio()
    {
      if (isNoop()) return;

      webrtc::VoEBase::GetInterface(mVoiceEngine.get())->StopPlayout(mChannel);
      webrtc::VoEBase::GetInterface(mVoiceEngine.get())->StopReceive(mChannel);
      webrtc::VoENetwork::GetInterface(mVoiceEngine.get())->DeRegisterExternalTransport(mChannel);

      mTrack->stop();

      mModuleProcessThread->Stop();

      ZS_LOG_DETAIL(log("destroyed"))
      mThisWeak.reset();

      cancel();
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::convert(ForSettingsPtr object)
    {
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object);
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::convert(ForReceiverChannelFromMediaBasePtr object)
    {
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object);
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::convert(ForRTPReceiverChannelPtr object)
    {
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object);
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::convert(ForMediaStreamTrackFromMediaBasePtr object)
    {
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object);
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::convert(ForMediaStreamTrackPtr object)
    {
      return ZS_DYNAMIC_PTR_CAST(RTPReceiverChannelAudio, object);
    }


    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => IRTPReceiverChannelMediaBaseForRTPReceiverChannel
    #pragma mark

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::handlePacket(RTPPacketPtr packet)
    {
      {
        AutoRecursiveLock lock(*this);
      }
      webrtc::PacketTime time(packet->timestamp(), 0);
      //mReceiveStream->DeliverRtp(packet->buffer()->data(), packet->buffer()->size(), time);
      webrtc::VoENetwork::GetInterface(mVoiceEngine.get())->ReceivedRTPPacket(
        mChannel, packet->buffer()->data(), packet->buffer()->size(), time);
      return true;
    }

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::handlePacket(RTCPPacketPtr packet)
    
    {
      {
        AutoRecursiveLock lock(*this);
      }
      //mReceiveStream->DeliverRtcp(packet->buffer()->data(), packet->buffer()->size());
      webrtc::VoENetwork::GetInterface(mVoiceEngine.get())->ReceivedRTCPPacket(
        mChannel, packet->buffer()->data(), packet->buffer()->size());
      return true;
    }
    
    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::handleUpdate(ParametersPtr params)
    {
#define TODO_UPDATE_PARAMETERS 1
#define TODO_UPDATE_PARAMETERS 2
      {
        AutoRecursiveLock lock(*this);
        mParameters = make_shared<Parameters>(*params);
      }
    }
    

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => IRTPReceiverChannelAudioForRTPReceiverChannel
    #pragma mark
    
    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr RTPReceiverChannelAudio::create(
                                                               RTPReceiverChannelPtr receiverChannel,
                                                               MediaStreamTrackPtr track,
                                                               const Parameters &params
                                                               )
    {
      RTPReceiverChannelAudioPtr pThis(make_shared<RTPReceiverChannelAudio>(make_private {}, IORTCForInternal::queueORTC(), receiverChannel, track, params));
      pThis->mThisWeak = pThis;
      pThis->init();
      return pThis;
    }

    //-------------------------------------------------------------------------
    int32_t RTPReceiverChannelAudio::getAudioSamples(
                                                     const size_t numberOfSamples,
                                                     const uint8_t numberOfChannels,
                                                     void *audioSamples,
                                                     size_t& numberOfSamplesOut
                                                     )
    {
#define TODO_IMPLEMENT_THIS 1
#define TODO_IMPLEMENT_THIS 2
      return 0;
    }
    
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => IRTPReceiverChannelAudioForMediaStreamTrack
    #pragma mark

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => IWakeDelegate
    #pragma mark

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::onWake()
    {
      ZS_LOG_DEBUG(log("wake"))

      AutoRecursiveLock lock(*this);
      step();
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => ITimerDelegate
    #pragma mark

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::onTimer(TimerPtr timer)
    {
      ZS_LOG_DEBUG(log("timer") + ZS_PARAM("timer id", timer->getID()))

      AutoRecursiveLock lock(*this);
#define TODO 1
#define TODO 2
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => IRTPReceiverChannelAudioAsyncDelegate
    #pragma mark

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPSenderChannelAudio => friend Transport
    #pragma mark

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::SendRtcp(const uint8_t* packet, size_t length)
    {
      auto channel = mReceiverChannel.lock();
      if (!channel) return false;
      return channel->sendPacket(RTCPPacket::create(packet, length));
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPSenderChannelAudio::Transport
    #pragma mark

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudio::Transport::Transport(
                                                  const make_private &,
                                                  RTPReceiverChannelAudioPtr outer
                                                  ) :
      mOuter(outer)
    {
    }
        
    //-------------------------------------------------------------------------
    RTPReceiverChannelAudio::Transport::~Transport()
    {
      mThisWeak.reset();
    }
    
    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::Transport::init()
    {
    }
    
    //-------------------------------------------------------------------------
    RTPReceiverChannelAudio::TransportPtr RTPReceiverChannelAudio::Transport::create(RTPReceiverChannelAudioPtr outer)
    {
      TransportPtr pThis(make_shared<Transport>(make_private{}, outer));
      pThis->mThisWeak = pThis;
      pThis->init();
      return pThis;
    }
    
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPSenderChannelAudio::Transport => webrtc::Transport
    #pragma mark

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::Transport::SendRtp(
                                                     const uint8_t* packet,
                                                     size_t length,
                                                     const webrtc::PacketOptions& options
                                                     )
    {
      return true;
    }
    
    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::Transport::SendRtcp(const uint8_t* packet, size_t length)
    {
      auto outer = mOuter.lock();
      if (!outer) return false;
      return outer->SendRtcp(packet, length);
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark RTPReceiverChannelAudio => (internal)
    #pragma mark

    //-------------------------------------------------------------------------
    Log::Params RTPReceiverChannelAudio::log(const char *message) const
    {
      ElementPtr objectEl = Element::create("ortc::RTPReceiverChannelAudio");
      UseServicesHelper::debugAppend(objectEl, "id", mID);
      return Log::Params(message, objectEl);
    }

    //-------------------------------------------------------------------------
    Log::Params RTPReceiverChannelAudio::debug(const char *message) const
    {
      return Log::Params(message, toDebug());
    }

    //-------------------------------------------------------------------------
    ElementPtr RTPReceiverChannelAudio::toDebug() const
    {
      AutoRecursiveLock lock(*this);

      ElementPtr resultEl = Element::create("ortc::RTPReceiverChannelAudio");

      UseServicesHelper::debugAppend(resultEl, "id", mID);

      UseServicesHelper::debugAppend(resultEl, "graceful shutdown", (bool)mGracefulShutdownReference);

      UseServicesHelper::debugAppend(resultEl, "state", toString(mCurrentState));

      UseServicesHelper::debugAppend(resultEl, "error", mLastError);
      UseServicesHelper::debugAppend(resultEl, "error reason", mLastErrorReason);

      auto receiverChannel = mReceiverChannel.lock();
      UseServicesHelper::debugAppend(resultEl, "receiver channel", receiverChannel ? receiverChannel->getID() : 0);

      return resultEl;
    }

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::isShuttingDown() const
    {
      return State_ShuttingDown == mCurrentState;
    }

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::isShutdown() const
    {
      return State_Shutdown == mCurrentState;
    }

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::step()
    {
      ZS_LOG_DEBUG(debug("step"))

      if ((isShuttingDown()) ||
          (isShutdown())) {
        ZS_LOG_DEBUG(debug("step forwarding to cancel"))
        cancel();
        return;
      }

      // ... other steps here ...
      if (!stepBogusDoSomething()) goto not_ready;
      // ... other steps here ...

      goto ready;

    not_ready:
      {
        ZS_LOG_TRACE(debug("not ready"))
        return;
      }

    ready:
      {
        ZS_LOG_TRACE(log("ready"))
        setState(State_Ready);
      }
    }

    //-------------------------------------------------------------------------
    bool RTPReceiverChannelAudio::stepBogusDoSomething()
    {
      if ( /* step already done */ false ) {
        ZS_LOG_TRACE(log("already completed do something"))
        return true;
      }

      if ( /* cannot do step yet */ false) {
        ZS_LOG_DEBUG(log("waiting for XYZ to complete before continuing"))
        return false;
      }

      ZS_LOG_DEBUG(log("doing step XYZ"))

      // ....
#define TODO 1
#define TODO 2

      return true;
    }

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::cancel()
    {
      //.......................................................................
      // try to gracefully shutdown

      if (isShutdown()) return;

      if (!mGracefulShutdownReference) mGracefulShutdownReference = mThisWeak.lock();

      if (mGracefulShutdownReference) {
//        return;
      }

      //.......................................................................
      // final cleanup

      setState(State_Shutdown);

      // make sure to cleanup any final reference to self
      mGracefulShutdownReference.reset();
    }

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::setState(States state)
    {
      if (state == mCurrentState) return;

      ZS_LOG_DETAIL(debug("state changed") + ZS_PARAM("new state", toString(state)) + ZS_PARAM("old state", toString(mCurrentState)))

      mCurrentState = state;

//      RTPReceiverChannelAudioPtr pThis = mThisWeak.lock();
//      if (pThis) {
//        mSubscriptions.delegate()->onRTPReceiverChannelAudioStateChanged(pThis, mCurrentState);
//      }
    }

    //-------------------------------------------------------------------------
    void RTPReceiverChannelAudio::setError(WORD errorCode, const char *inReason)
    {
      String reason(inReason);
      if (reason.isEmpty()) {
        reason = UseHTTP::toString(UseHTTP::toStatusCode(errorCode));
      }

      if (0 != mLastError) {
        ZS_LOG_WARNING(Detail, debug("error already set thus ignoring new error") + ZS_PARAM("new error", errorCode) + ZS_PARAM("new reason", reason))
        return;
      }

      mLastError = errorCode;
      mLastErrorReason = reason;

      ZS_LOG_WARNING(Detail, debug("error set") + ZS_PARAM("error", mLastError) + ZS_PARAM("reason", mLastErrorReason))
    }

    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    #pragma mark
    #pragma mark IRTPReceiverChannelAudioFactory
    #pragma mark

    //-------------------------------------------------------------------------
    IRTPReceiverChannelAudioFactory &IRTPReceiverChannelAudioFactory::singleton()
    {
      return RTPReceiverChannelAudioFactory::singleton();
    }

    //-------------------------------------------------------------------------
    RTPReceiverChannelAudioPtr IRTPReceiverChannelAudioFactory::create(
                                                                       RTPReceiverChannelPtr receiverChannel,
                                                                       MediaStreamTrackPtr track,
                                                                       const Parameters &params
                                                                       )
    {
      if (this) {}
      return internal::RTPReceiverChannelAudio::create(receiverChannel, track, params);
    }

  } // internal namespace
}