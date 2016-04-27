/*
 * Intel License
 */

#include <vector>
#include "webrtc/base/logging.h"
#include "talk/woogeen/sdk/base/peerconnectionchannel.h"
#include "talk/woogeen/sdk/base/sdputils.h"

namespace woogeen {
namespace base {

PeerConnectionChannel::PeerConnectionChannel(
    PeerConnectionChannelConfiguration configuration)
    : pc_thread_(nullptr),
      configuration_(configuration),
      factory_(nullptr),
      peer_connection_(nullptr) {
  LOG(LS_INFO) << "PeerConnectionChannel video codec: "
               << configuration_.media_codec.video_codec;
}

PeerConnectionChannel::~PeerConnectionChannel() {
  if (peer_connection_ != nullptr) {
    peer_connection_->Close();
  }
  if (pc_thread_ != nullptr)
    delete pc_thread_;
}

bool PeerConnectionChannel::InitializePeerConnection() {
  LOG(LS_INFO) << "Initialize PeerConnection.";
  if (factory_.get() == nullptr)
    factory_ = PeerConnectionDependencyFactory::Get();
  media_constraints_.AddOptional(MediaConstraintsInterface::kEnableDtlsSrtp,
                                 true);
  media_constraints_.SetMandatory(
      webrtc::MediaConstraintsInterface::kOfferToReceiveAudio, true);
  media_constraints_.SetMandatory(
      webrtc::MediaConstraintsInterface::kOfferToReceiveVideo, true);
  peer_connection_ = (factory_->CreatePeerConnection(configuration_,
                                                     &media_constraints_, this))
                         .get();
  if (!peer_connection_.get()) {
    LOG(LS_ERROR) << "Failed to initialize PeerConnection.";
    RTC_DCHECK(false);
    return false;
  }
  if (pc_thread_ == nullptr) {
    pc_thread_ = new rtc::Thread();
    pc_thread_->Start();
  }
  RTC_CHECK(peer_connection_);
  RTC_CHECK(pc_thread_);
  return true;
}

const webrtc::SessionDescriptionInterface*
PeerConnectionChannel::LocalDescription() {
  RTC_CHECK(peer_connection_);
  return peer_connection_->local_description();
}

void PeerConnectionChannel::OnMessage(rtc::Message* msg) {
  if (peer_connection_->signaling_state() ==
      webrtc::PeerConnectionInterface::SignalingState::kClosed) {
    LOG(LS_WARNING)
        << "Attempt to perform PeerConnection operation when it is closed.";
    return;
  }
  switch (msg->message_id) {
    case kMessageTypeClosePeerConnection:
      peer_connection_->Close();
      break;
    case kMessageTypeCreateOffer: {
      rtc::TypedMessageData<
          scoped_refptr<FunctionalCreateSessionDescriptionObserver>>* param =
          static_cast<rtc::TypedMessageData<
              scoped_refptr<FunctionalCreateSessionDescriptionObserver>>*>(
              msg->pdata);
      peer_connection_->CreateOffer(param->data(), &media_constraints_);
      delete param;
      break;
    }
    case kMessageTypeCreateAnswer: {
      rtc::TypedMessageData<
          scoped_refptr<FunctionalCreateSessionDescriptionObserver>>* param =
          static_cast<rtc::TypedMessageData<
              scoped_refptr<FunctionalCreateSessionDescriptionObserver>>*>(
              msg->pdata);
      peer_connection_->CreateAnswer(param->data(), nullptr);
      delete param;
      break;
    }
    case kMessageTypeCreateDataChannel: {
      rtc::TypedMessageData<std::string>* param =
          static_cast<rtc::TypedMessageData<std::string>*>(msg->pdata);
      webrtc::DataChannelInit config;
      data_channel_ =
          peer_connection_->CreateDataChannel(param->data(), &config);
      data_channel_->RegisterObserver(this);
      LOG(LS_INFO) << "Created data channel.";
      delete param;
      break;
    }
    case kMessageTypeSetLocalDescription: {
      SetSessionDescriptionMessage* param =
          static_cast<SetSessionDescriptionMessage*>(msg->pdata);
      // Set codec preference.
      webrtc::SessionDescriptionInterface* desc = param->description;
      std::string sdp_string;
      if (!desc->ToString(&sdp_string)) {
        LOG(LS_ERROR) << "Error parsing local description.";
        ASSERT(false);
      }
      sdp_string = SdpUtils::SetPreferAudioCodec(
          sdp_string, configuration_.media_codec.audio_codec);
      sdp_string = SdpUtils::SetPreferVideoCodec(
          sdp_string, configuration_.media_codec.video_codec);
      webrtc::SessionDescriptionInterface* new_desc(
          webrtc::CreateSessionDescription(desc->type(), sdp_string, nullptr));
      peer_connection_->SetLocalDescription(param->observer, new_desc);
      delete param;
      break;
    }
    case kMessageTypeSetRemoteDescription: {
      SetSessionDescriptionMessage* param =
          static_cast<SetSessionDescriptionMessage*>(msg->pdata);
      // Set codec preference.
      webrtc::SessionDescriptionInterface* desc = param->description;
      std::string sdp_string;
      if (!desc->ToString(&sdp_string)) {
        LOG(LS_ERROR) << "Error parsing local description.";
        ASSERT(false);
      }
      if (configuration_.max_audio_bandwidth != 0) {
        sdp_string = SdpUtils::SetMaximumAudioBandwidth(
            sdp_string, configuration_.max_audio_bandwidth);
      }
      if (configuration_.max_video_bandwidth != 0) {
        sdp_string = SdpUtils::SetMaximumVideoBandwidth(
            sdp_string, configuration_.max_video_bandwidth);
      }
      webrtc::SessionDescriptionInterface* new_desc(
          webrtc::CreateSessionDescription(desc->type(), sdp_string, nullptr));
      peer_connection_->SetRemoteDescription(param->observer, new_desc);
      delete param;
      break;
    }
    case kMessageTypeSetRemoteIceCandidate: {
      rtc::TypedMessageData<webrtc::IceCandidateInterface*>* param =
          static_cast<rtc::TypedMessageData<webrtc::IceCandidateInterface*>*>(
              msg->pdata);
      peer_connection_->AddIceCandidate(param->data());
      delete param;
      break;
    }
    case kMessageTypeAddStream: {
      rtc::ScopedRefMessageData<MediaStreamInterface>* param =
          static_cast<rtc::ScopedRefMessageData<MediaStreamInterface>*>(
              msg->pdata);
      peer_connection_->AddStream(param->data());
      delete param;
      break;
    }
    case kMessageTypeRemoveStream: {
      rtc::ScopedRefMessageData<MediaStreamInterface>* param =
          static_cast<rtc::ScopedRefMessageData<MediaStreamInterface>*>(
              msg->pdata);
      peer_connection_->RemoveStream(param->data());
      delete param;
      break;
    }
    default:
      LOG(LS_WARNING) << "Unknown message type.";
  }
}

void PeerConnectionChannel::OnSetRemoteSessionDescriptionSuccess() {
  LOG(LS_INFO) << "Set remote sdp success.";
  if (peer_connection_->remote_description() &&
      peer_connection_->remote_description()->type() == "offer") {
    CreateAnswer();
  }
}

void PeerConnectionChannel::OnSetRemoteSessionDescriptionFailure(
    const std::string& error) {
  LOG(LS_INFO) << "Set remote sdp failed.";
}

void PeerConnectionChannel::OnCreateSessionDescriptionSuccess(
    webrtc::SessionDescriptionInterface* desc) {
  LOG(LS_INFO) << "Create sdp success.";
}

void PeerConnectionChannel::OnCreateSessionDescriptionFailure(
    const std::string& error) {
  LOG(LS_INFO) << "Create sdp failed.";
}

void PeerConnectionChannel::OnSetLocalSessionDescriptionSuccess() {
  LOG(LS_INFO) << "Set local sdp success.";
}

void PeerConnectionChannel::OnSetLocalSessionDescriptionFailure(
    const std::string& error) {
  LOG(LS_INFO) << "Set local sdp failed.";
}

void PeerConnectionChannel::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {}

void PeerConnectionChannel::OnSignalingChange(
    PeerConnectionInterface::SignalingState new_state) {}

void PeerConnectionChannel::OnAddStream(MediaStreamInterface* stream) {}

void PeerConnectionChannel::OnRemoveStream(MediaStreamInterface* stream) {}

void PeerConnectionChannel::OnDataChannel(
    webrtc::DataChannelInterface* data_channel) {}

void PeerConnectionChannel::OnRenegotiationNeeded() {}

void PeerConnectionChannel::OnIceConnectionChange(
    PeerConnectionInterface::IceConnectionState new_state) {}

void PeerConnectionChannel::OnIceGatheringChange(
    PeerConnectionInterface::IceGatheringState new_state) {}
}
}
