#include "group/GroupInstanceReferenceImpl.h"

#include "LogSinkImpl.h"
#include "FakeAudioDeviceModule.h"
#include "StaticThreads.h"
#include "ThreadLocalObject.h"
#include "AudioDeviceHelper.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/enable_media.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/jsep.h"
#include "api/jsep_session_description.h"
#include "api/jsep_ice_candidate.h"
#include "api/candidate.h"
#include "api/units/time_delta.h"
#include "pc/session_description.h"

#include "pc/peer_connection.h"
#include "pc/media_session.h"
#include "p2p/client/basic_port_allocator.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/network.h"
#include "rtc_base/rtc_certificate_generator.h"

#include "modules/audio_processing/audio_buffer.h"

#include "platform/PlatformInterface.h"

#include "group/GroupJoinPayloadInternal.h"

#include "third-party/json11.hpp"

#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace tgcalls {

namespace {

// --- PeerConnection observer adapter ---

class GRPeerConnectionObserver : public webrtc::PeerConnectionObserver {
public:
    std::function<void()> onRenegotiationNeeded;
    std::function<void(const webrtc::IceCandidateInterface *)> onIceCandidate;
    std::function<void(webrtc::PeerConnectionInterface::IceConnectionState)> onConnectionChange;
    std::function<void(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> onTrack;
    std::function<void(webrtc::scoped_refptr<webrtc::DataChannelInterface>)> onDataChannel;

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override {}

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        if (onTrack) onTrack(transceiver);
    }

    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> dc) override {
        if (onDataChannel) onDataChannel(dc);
    }

    void OnRenegotiationNeeded() override {
        if (onRenegotiationNeeded) onRenegotiationNeeded();
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override {
        if (onConnectionChange) onConnectionChange(state);
    }

    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

    void OnIceCandidate(const webrtc::IceCandidateInterface *candidate) override {
        if (onIceCandidate) onIceCandidate(candidate);
    }

    void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>&) override {}
    void OnIceSelectedCandidatePairChanged(const cricket::CandidatePairChangeEvent&) override {}
    void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>, const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&) override {}
    void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>) override {}
};

// --- DataChannel observer adapter ---

class GRDataChannelObserver : public webrtc::DataChannelObserver {
public:
    std::function<void()> onStateChange;
    std::function<void(webrtc::DataBuffer const &)> onMessage;

    void OnStateChange() override { if (onStateChange) onStateChange(); }
    void OnMessage(webrtc::DataBuffer const &buffer) override { if (onMessage) onMessage(buffer); }
};

// --- SetSessionDescription observer ---

class GRSetSDPObserver : public webrtc::SetSessionDescriptionObserver {
public:
    GRSetSDPObserver(std::function<void(webrtc::RTCError)> callback) : _callback(std::move(callback)) {}
    void OnSuccess() override { _callback(webrtc::RTCError::OK()); }
    void OnFailure(webrtc::RTCError error) override { _callback(std::move(error)); }
private:
    std::function<void(webrtc::RTCError)> _callback;
};

// --- CreateSessionDescription observer ---

class GRCreateSDPObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    GRCreateSDPObserver(std::function<void(webrtc::SessionDescriptionInterface*)> onSuccess,
                        std::function<void(webrtc::RTCError)> onFailure)
        : _onSuccess(std::move(onSuccess)), _onFailure(std::move(onFailure)) {}

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override { _onSuccess(desc); }
    void OnFailure(webrtc::RTCError error) override { _onFailure(std::move(error)); }

private:
    std::function<void(webrtc::SessionDescriptionInterface*)> _onSuccess;
    std::function<void(webrtc::RTCError)> _onFailure;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// GroupInstanceReferenceInternal
// ---------------------------------------------------------------------------

class GroupInstanceReferenceInternal : public std::enable_shared_from_this<GroupInstanceReferenceInternal> {
public:
    GroupInstanceReferenceInternal(GroupInstanceDescriptor &&descriptor, std::shared_ptr<Threads> threads)
        : _threads(std::move(threads))
        , _networkStateUpdated(std::move(descriptor.networkStateUpdated))
        , _audioLevelsUpdated(std::move(descriptor.audioLevelsUpdated))
        , _createAudioDeviceModule(std::move(descriptor.createAudioDeviceModule))
        , _requestMediaChannelDescriptions(std::move(descriptor.requestMediaChannelDescriptions))
        , _outgoingAudioBitrateKbit(descriptor.outgoingAudioBitrateKbit)
    {
    }

    ~GroupInstanceReferenceInternal() {
        if (_peerConnection) {
            _peerConnection->Close();
        }
    }

    void start() {
        const auto weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this());

        // 1. Create AudioDeviceModule.
        auto taskQueueFactory = webrtc::CreateDefaultTaskQueueFactory();
        _audioDeviceModule = _createAudioDeviceModule(taskQueueFactory.get());

        // 2. Create PeerConnectionFactory.
        webrtc::PeerConnectionFactoryDependencies deps;
        deps.network_thread = _threads->getNetworkThread();
        deps.signaling_thread = _threads->getMediaThread();
        deps.worker_thread = _threads->getWorkerThread();
        deps.task_queue_factory = std::move(taskQueueFactory);
        deps.adm = _audioDeviceModule;

        webrtc::AudioProcessingBuilder builder;
        deps.audio_processing = builder.Create();

        deps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
        deps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();

        webrtc::EnableMedia(deps);

        deps.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>(deps.task_queue_factory.get());

        _peerConnectionFactory = webrtc::CreateModularPeerConnectionFactory(std::move(deps));
        if (!_peerConnectionFactory) {
            RTC_LOG(LS_ERROR) << "GroupRef: Failed to create PeerConnectionFactory";
            return;
        }

        // Allow loopback network interfaces (needed for localhost SFU).
        {
            webrtc::PeerConnectionFactoryInterface::Options factoryOptions;
            factoryOptions.network_ignore_mask = 0; // Don't ignore loopback.
            _peerConnectionFactory->SetOptions(factoryOptions);
        }

        // 3. Create PeerConnection.
        _peerConnectionObserver = std::make_unique<GRPeerConnectionObserver>();

        _peerConnectionObserver->onConnectionChange = [weak, threads = _threads](
            webrtc::PeerConnectionInterface::IceConnectionState state) {
            threads->getMediaThread()->PostTask([weak, state]() {
                if (auto strong = weak.lock()) {
                    strong->onIceConnectionChange(state);
                }
            });
        };

        _peerConnectionObserver->onTrack = [weak, threads = _threads](
            webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
            threads->getMediaThread()->PostTask([weak, transceiver]() {
                if (auto strong = weak.lock()) {
                    strong->onTrackAdded(transceiver);
                }
            });
        };

        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.type = webrtc::PeerConnectionInterface::IceTransportsType::kAll;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
        config.rtcp_mux_policy = webrtc::PeerConnectionInterface::RtcpMuxPolicy::kRtcpMuxPolicyRequire;
        config.continual_gathering_policy = webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
        config.audio_jitter_buffer_fast_accelerate = true;

        webrtc::PeerConnectionDependencies pcDeps(nullptr);
        pcDeps.observer = _peerConnectionObserver.get();

        _networkMonitorFactory = PlatformInterface::SharedInstance()->createNetworkMonitorFactory();
        _socketFactory = std::make_unique<rtc::BasicPacketSocketFactory>(_threads->getNetworkThread()->socketserver());
        _networkManager = std::make_unique<rtc::BasicNetworkManager>(_networkMonitorFactory.get(), _threads->getNetworkThread()->socketserver());
        pcDeps.allocator = std::make_unique<cricket::BasicPortAllocator>(_networkManager.get(), _socketFactory.get());

        auto pcOrError = _peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(pcDeps));
        if (!pcOrError.ok()) {
            RTC_LOG(LS_ERROR) << "GroupRef: Failed to create PeerConnection: " << pcOrError.error().message();
            return;
        }
        _peerConnection = pcOrError.value();

        // 4. Create data channel.
        webrtc::DataChannelInit dcInit;
        auto dcOrError = _peerConnection->CreateDataChannelOrError("data", &dcInit);
        if (dcOrError.ok()) {
            _dataChannel = dcOrError.value();
            setupDataChannel();
        }

        // 5. Add outgoing audio transceiver.
        cricket::AudioOptions audioOpts;
        auto audioSource = _peerConnectionFactory->CreateAudioSource(audioOpts);
        auto audioTrack = _peerConnectionFactory->CreateAudioTrack("audio0", audioSource.get());

        webrtc::RtpTransceiverInit transceiverInit;
        transceiverInit.stream_ids = {"0"};

        auto result = _peerConnection->AddTransceiver(audioTrack, transceiverInit);
        if (result.ok()) {
            _outgoingAudioTransceiver = result.value();
            _outgoingAudioTrack = audioTrack;

            webrtc::RtpParameters params = _outgoingAudioTransceiver->sender()->GetParameters();
            if (params.encodings.empty()) {
                params.encodings.push_back(webrtc::RtpEncodingParameters());
            }
            params.encodings[0].max_bitrate_bps = _outgoingAudioBitrateKbit * 1024;
            _outgoingAudioTransceiver->sender()->SetParameters(params);

            _outgoingAudioTrack->set_enabled(false); // Muted by default.
        }

        RTC_LOG(LS_INFO) << "GroupRef: PeerConnection created successfully";
    }

    void emitJoinPayload(std::function<void(GroupJoinPayload const &)> completion) {
        _joinCompletion = std::move(completion);

        // Create offer to get local SDP with ICE/DTLS params.
        auto observer = rtc::make_ref_counted<GRCreateSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](
                webrtc::SessionDescriptionInterface* desc) {
                auto strong = weak.lock();
                if (!strong) return;
                strong->_threads->getMediaThread()->PostTask([weak, ownedDesc = std::unique_ptr<webrtc::SessionDescriptionInterface>(desc->Clone())]() mutable {
                    if (auto s = weak.lock()) {
                        s->onLocalOfferCreated(std::move(ownedDesc));
                    }
                });
            },
            [](webrtc::RTCError error) {
                RTC_LOG(LS_ERROR) << "GroupRef: CreateOffer failed: " << error.message();
            }
        );

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offerOptions;
        _peerConnection->CreateOffer(observer.get(), offerOptions);
    }

    void onLocalOfferCreated(std::unique_ptr<webrtc::SessionDescriptionInterface> offer) {
        // Set local description.
        auto* rawOffer = offer.release();
        auto observer = rtc::make_ref_counted<GRSetSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](webrtc::RTCError error) {
                if (!error.ok()) {
                    RTC_LOG(LS_ERROR) << "GroupRef: SetLocalDescription failed: " << error.message();
                    return;
                }
                auto strong = weak.lock();
                if (!strong) return;
                strong->_threads->getMediaThread()->PostTask([weak]() {
                    if (auto s = weak.lock()) {
                        s->onLocalDescriptionSet();
                    }
                });
            }
        );
        _peerConnection->SetLocalDescription(observer.get(), rawOffer);
    }

    void onLocalDescriptionSet() {
        auto localDesc = _peerConnection->local_description();
        if (!localDesc) {
            RTC_LOG(LS_ERROR) << "GroupRef: local_description is null after SetLocalDescription";
            return;
        }

        // Extract ICE/DTLS params from local SDP.
        auto* cricketDesc = localDesc->description();
        if (!cricketDesc || cricketDesc->contents().empty()) {
            RTC_LOG(LS_ERROR) << "GroupRef: empty local description";
            return;
        }

        const auto& firstContent = cricketDesc->contents()[0];
        const auto* transportInfo = cricketDesc->GetTransportInfoByName(firstContent.name);
        if (!transportInfo) {
            RTC_LOG(LS_ERROR) << "GroupRef: no transport info in local description";
            return;
        }

        std::string ufrag = transportInfo->description.ice_ufrag;
        std::string pwd = transportInfo->description.ice_pwd;

        // Get DTLS fingerprint.
        std::string fingerprintHash;
        std::string fingerprintValue;
        if (transportInfo->description.identity_fingerprint) {
            fingerprintHash = transportInfo->description.identity_fingerprint->algorithm;
            fingerprintValue = transportInfo->description.identity_fingerprint->GetRfc4572Fingerprint();
        }

        // Get outgoing audio SSRC from the first audio content.
        uint32_t audioSsrc = 0;
        auto* audioDesc = firstContent.media_description();
        if (audioDesc && !audioDesc->streams().empty()) {
            audioSsrc = audioDesc->streams()[0].first_ssrc();
        }

        _localUfrag = ufrag;
        _localPwd = pwd;
        _outgoingSsrc = audioSsrc;

        // Build join JSON.
        GroupJoinInternalPayload internalPayload;
        internalPayload.audioSsrc = audioSsrc;
        internalPayload.transport.ufrag = ufrag;
        internalPayload.transport.pwd = pwd;

        GroupJoinTransportDescription::Fingerprint fp;
        fp.hash = fingerprintHash;
        fp.fingerprint = fingerprintValue;
        fp.setup = "passive"; // Client is DTLS server (SSL_SERVER).
        internalPayload.transport.fingerprints.push_back(fp);

        GroupJoinPayload payload;
        payload.audioSsrc = audioSsrc;
        payload.json = internalPayload.serialize();

        if (_joinCompletion) {
            _joinCompletion(payload);
            _joinCompletion = nullptr;
        }
    }

    void setJoinResponsePayload(std::string const &payload) {
        // Parse the SFU response JSON.
        auto parsed = GroupJoinResponsePayload::parse(payload);
        if (!parsed) {
            RTC_LOG(LS_ERROR) << "GroupRef: Failed to parse join response";
            return;
        }

        _remoteTransport = parsed->transport;

        // Build remote answer SDP from the parsed transport.
        auto remoteAnswer = buildRemoteAnswer();
        if (!remoteAnswer) {
            RTC_LOG(LS_ERROR) << "GroupRef: Failed to build remote answer";
            return;
        }

        auto observer = rtc::make_ref_counted<GRSetSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](webrtc::RTCError error) {
                if (!error.ok()) {
                    RTC_LOG(LS_ERROR) << "GroupRef: SetRemoteDescription failed: " << error.message();
                    return;
                }
                auto strong = weak.lock();
                if (!strong) return;
                strong->_threads->getMediaThread()->PostTask([weak]() {
                    if (auto s = weak.lock()) {
                        s->addRemoteIceCandidates();
                    }
                });
            }
        );
        _peerConnection->SetRemoteDescription(observer.get(), remoteAnswer.release());
    }

    void addRemoteIceCandidates() {
        if (!_peerConnection) return;

        // Determine the first mid (bundle transport).
        std::string bundleMid = "0";
        auto localDesc = _peerConnection->local_description();
        if (localDesc && !localDesc->description()->contents().empty()) {
            bundleMid = localDesc->description()->contents()[0].name;
        }

        for (const auto& candidate : _remoteTransport.candidates) {
            int port = 0;
            try { port = std::stoi(candidate.port); } catch (...) { continue; }
            int priority = 0;
            try { priority = std::stoi(candidate.priority); } catch (...) {}

            cricket::Candidate c;
            c.set_foundation(candidate.foundation);
            c.set_component(std::stoi(candidate.component));
            c.set_protocol(candidate.protocol);
            c.set_priority(priority);
            c.set_address(rtc::SocketAddress(candidate.ip, port));
            c.set_type(candidate.type);

            auto iceCandidate = webrtc::CreateIceCandidate(bundleMid, 0, c);
            if (iceCandidate) {
                if (!_peerConnection->AddIceCandidate(iceCandidate.get())) {
                    RTC_LOG(LS_WARNING) << "GroupRef: Failed to add ICE candidate " << candidate.ip << ":" << candidate.port;
                } else {
                    RTC_LOG(LS_INFO) << "GroupRef: Added ICE candidate " << candidate.ip << ":" << candidate.port;
                }
            }
        }
    }

    std::unique_ptr<webrtc::SessionDescriptionInterface> buildRemoteAnswer() {
        auto localDesc = _peerConnection->local_description();
        if (!localDesc || !localDesc->description()) {
            RTC_LOG(LS_ERROR) << "GroupRef: No local description available for building answer";
            return nullptr;
        }

        auto* localCricketDesc = localDesc->description();
        auto cricketDesc = std::make_unique<cricket::SessionDescription>();
        std::vector<std::string> bundleMids;

        // Build TransportDescription from SFU response.
        cricket::TransportDescription transportDesc;
        transportDesc.ice_ufrag = _remoteTransport.ufrag;
        transportDesc.ice_pwd = _remoteTransport.pwd;
        transportDesc.ice_mode = cricket::ICEMODE_LITE;

        if (!_remoteTransport.fingerprints.empty()) {
            auto& fp = _remoteTransport.fingerprints[0];
            auto fingerprint = rtc::SSLFingerprint::CreateUniqueFromRfc4572(fp.hash, fp.fingerprint);
            if (fingerprint) {
                transportDesc.identity_fingerprint = std::move(fingerprint);
            }
            // SFU sends setup=active (DTLS client).
            transportDesc.connection_role = cricket::CONNECTIONROLE_ACTIVE;
        }

        // Build a map from mid -> SSRC for remote audio m-lines.
        // We need to find which SSRC corresponds to each recvonly transceiver mid.
        std::map<std::string, uint32_t> midToSsrc;
        for (const auto& [ssrc, info] : _remoteSsrcs) {
            if (info.transceiver) {
                auto mid = info.transceiver->mid();
                if (mid.has_value()) {
                    midToSsrc[mid.value()] = ssrc;
                }
            }
        }

        // Mirror the local offer: for each content in the local description,
        // create a matching content in the remote answer with the SAME mid.
        bool isFirstAudio = true;
        for (const auto& localContent : localCricketDesc->contents()) {
            const std::string& mid = localContent.name;
            auto* localMedia = localContent.media_description();
            if (!localMedia) continue;

            if (localMedia->type() == cricket::MEDIA_TYPE_DATA) {
                // --- Data channel: clone from local offer ---
                auto dataContent = localMedia->Clone();
                dataContent->set_direction(webrtc::RtpTransceiverDirection::kSendRecv);

                cricket::ContentInfo ci(localContent.type);
                ci.name = mid;
                ci.rejected = false;
                ci.bundle_only = false;
                ci.set_media_description(std::move(dataContent));

                cricketDesc->AddContent(std::move(ci));
                cricketDesc->AddTransportInfo(cricket::TransportInfo(mid, transportDesc));
                bundleMids.push_back(mid);

            } else if (localMedia->type() == cricket::MEDIA_TYPE_AUDIO) {
                auto audioContent = std::make_unique<cricket::AudioContentDescription>();

                // Opus codec.
                cricket::AudioCodec opus = cricket::CreateAudioCodec(111, "opus", 48000, 2);
                opus.params["minptime"] = "10";
                opus.params["useinbandfec"] = "1";
                audioContent->AddCodec(opus);
                audioContent->set_rtcp_mux(true);

                if (isFirstAudio) {
                    // --- First audio m-line: sendrecv (our outgoing audio) ---
                    isFirstAudio = false;

                    // Copy RTP header extensions from local offer so PeerConnection
                    // includes them (especially ssrc-audio-level) in outgoing RTP.
                    // Don't copy stream params — the SFU doesn't send audio on the
                    // main m-line. Leaving the receiver without a signaled SSRC allows
                    // PeerConnection to handle incoming RTP via unsignaled streams,
                    // which will assign the correct remote SSRC.
                    for (const auto& ext : localMedia->rtp_header_extensions()) {
                        audioContent->AddRtpHeaderExtension(ext);
                    }

                    audioContent->set_direction(webrtc::RtpTransceiverDirection::kSendRecv);
                } else {
                    // --- Recvonly audio transceiver: answer with sendonly ---
                    // Don't include SSRCs — the SFU forwards raw RTP from CustomImpl
                    // whose mid extension IDs don't match this PeerConnection's mapping,
                    // so packets arrive as unsignaled streams on mid=0. Including SSRCs
                    // here would conflict with those unsignaled stream registrations.
                    audioContent->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAudioLevelUri, 1));
                    audioContent->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kAbsSendTimeUri, 2));
                    audioContent->AddRtpHeaderExtension(webrtc::RtpExtension(webrtc::RtpExtension::kTransportSequenceNumberUri, 3));

                    audioContent->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
                }

                cricket::ContentInfo ci(cricket::MediaProtocolType::kRtp);
                ci.name = mid;
                ci.rejected = false;
                ci.bundle_only = false;
                ci.set_media_description(std::move(audioContent));

                cricketDesc->AddContent(std::move(ci));
                cricketDesc->AddTransportInfo(cricket::TransportInfo(mid, transportDesc));
                bundleMids.push_back(mid);
            }
        }

        // Bundle group.
        if (!bundleMids.empty()) {
            cricket::ContentGroup bundleGroup(cricket::GROUP_TYPE_BUNDLE);
            for (const auto& name : bundleMids) {
                bundleGroup.AddContentName(name);
            }
            cricketDesc->AddGroup(bundleGroup);
        }

        auto jsepAnswer = std::make_unique<webrtc::JsepSessionDescription>(
            webrtc::SdpType::kAnswer,
            std::move(cricketDesc),
            "0", "0");

        // Add ICE candidates.
        if (!bundleMids.empty()) {
            for (const auto& candidate : _remoteTransport.candidates) {
                int port = std::stoi(candidate.port);
                int priority = 0;
                try { priority = std::stoi(candidate.priority); } catch (...) {}

                cricket::Candidate c;
                c.set_foundation(candidate.foundation);
                c.set_component(std::stoi(candidate.component));
                c.set_protocol(candidate.protocol);
                c.set_priority(priority);
                c.set_address(rtc::SocketAddress(candidate.ip, port));
                c.set_type(candidate.type);

                // Add to the first transport (bundled).
                auto iceCandidate = webrtc::CreateIceCandidate(bundleMids[0], 0, c);
                if (iceCandidate) {
                    jsepAnswer->AddCandidate(iceCandidate.get());
                }
            }
        }

        return jsepAnswer;
    }

    void setConnectionMode(GroupConnectionMode mode, bool, bool) {
        // No-op: PeerConnection manages its own connection state.
    }

    void setIsMuted(bool isMuted) {
        if (_outgoingAudioTrack) {
            _outgoingAudioTrack->set_enabled(!isMuted);
        }
    }

    void setVolume(uint32_t ssrc, double volume) {
        // Could adjust receiver gain per SSRC. Not critical for audio-only test.
    }

    void stop(std::function<void()> completion) {
        _isPollingAudioLevels = false;
        if (_peerConnection) {
            _peerConnection->Close();
        }
        if (completion) {
            completion();
        }
    }

    void removeSsrcs(std::vector<uint32_t>) {}
    void removeIncomingVideoSource(uint32_t) {}
    void setIsNoiseSuppressionEnabled(bool) {}
    void setVideoCapture(std::shared_ptr<VideoCaptureInterface>) {}
    void setVideoSource(std::function<webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>()>) {}
    void setAudioOutputDevice(std::string) {}
    void setAudioInputDevice(std::string) {}
    void addExternalAudioSamples(std::vector<uint8_t>&&) {}
    void addOutgoingVideoOutput(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {}
    void addIncomingVideoOutput(std::string const &, std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {}
    void setRequestedVideoChannels(std::vector<VideoChannelDescription>&&) {}
    void getStats(std::function<void(GroupInstanceStats)> completion) {
        if (completion) completion(GroupInstanceStats{});
    }
    void internal_addCustomNetworkEvent(bool) {}

private:
    void setupDataChannel() {
        _dataChannelObserver = std::make_unique<GRDataChannelObserver>();

        _dataChannelObserver->onStateChange = [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())]() {
            auto strong = weak.lock();
            if (!strong) return;
            strong->_threads->getMediaThread()->PostTask([weak]() {
                if (auto s = weak.lock()) {
                    s->onDataChannelStateChanged();
                }
            });
        };

        _dataChannelObserver->onMessage = [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](
            webrtc::DataBuffer const &buffer) {
            if (buffer.binary) return;
            std::string msg(buffer.data.data(), buffer.data.data() + buffer.data.size());
            auto strong = weak.lock();
            if (!strong) return;
            strong->_threads->getMediaThread()->PostTask([weak, msg = std::move(msg)]() {
                if (auto s = weak.lock()) {
                    s->onDataChannelMessage(msg);
                }
            });
        };

        _dataChannel->RegisterObserver(_dataChannelObserver.get());
    }

    void onDataChannelStateChanged() {
        if (_dataChannel && _dataChannel->state() == webrtc::DataChannelInterface::DataState::kOpen) {
            _isDataChannelOpen = true;
            RTC_LOG(LS_INFO) << "GroupRef: Data channel open";
        } else {
            _isDataChannelOpen = false;
        }
    }

    void onDataChannelMessage(std::string const &msg) {
        // Parse JSON message.
        std::string err;
        auto json = json11::Json::parse(msg, err);
        if (!err.empty()) return;

        auto colibriClass = json["colibriClass"].string_value();
        if (colibriClass == "ActiveAudioSsrcs") {
            handleActiveAudioSsrcs(json);
        }
        // Ignore other Colibri messages (SenderVideoConstraints, etc.)
    }

    void handleActiveAudioSsrcs(json11::Json const &json) {
        auto ssrcArray = json["ssrcs"].array_items();

        std::set<uint32_t> newSsrcs;
        for (const auto& item : ssrcArray) {
            uint32_t ssrc = static_cast<uint32_t>(static_cast<int32_t>(item.int_value()));
            if (ssrc != 0) {
                newSsrcs.insert(ssrc);
            }
        }

        // Diff against current set.
        bool changed = false;

        // Add new SSRCs.
        std::vector<uint32_t> ssrcsToRequest;
        for (uint32_t ssrc : newSsrcs) {
            if (_remoteSsrcs.find(ssrc) == _remoteSsrcs.end()) {
                // Assign a new mid. Mids "0" is our audio, "1" might be data channel.
                // Use incrementing counter starting from 10 to avoid collision.
                std::string mid = std::to_string(_nextMid++);

                RemoteSsrcInfo info;
                info.mid = mid;
                _remoteSsrcs[ssrc] = info;
                ssrcsToRequest.push_back(ssrc);
                changed = true;

                RTC_LOG(LS_INFO) << "GroupRef: New remote SSRC " << ssrc << " (mid=" << mid << ")";
            }
        }

        // Remove gone SSRCs.
        for (auto it = _remoteSsrcs.begin(); it != _remoteSsrcs.end(); ) {
            if (newSsrcs.find(it->first) == newSsrcs.end()) {
                RTC_LOG(LS_INFO) << "GroupRef: Removing SSRC " << it->first;
                it = _remoteSsrcs.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        if (changed) {
            renegotiate();
        }

        // Request media channel descriptions for new SSRCs.
        if (!ssrcsToRequest.empty() && _requestMediaChannelDescriptions) {
            _requestMediaChannelDescriptions(ssrcsToRequest,
                [](std::vector<MediaChannelDescription>&&) {
                    // Descriptions received. For audio-only test, we don't need to act on them.
                });
        }
    }

    void renegotiate() {
        // Create new offer (with recvonly transceivers for remote SSRCs),
        // then build a matching remote answer.

        // First, add recvonly transceivers for SSRCs that don't have one yet.
        for (auto& [ssrc, info] : _remoteSsrcs) {
            if (!info.transceiver) {
                webrtc::RtpTransceiverInit init;
                init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
                init.stream_ids = {std::to_string(ssrc)};

                auto result = _peerConnection->AddTransceiver(cricket::MEDIA_TYPE_AUDIO, init);
                if (result.ok()) {
                    info.transceiver = result.value();
                    RTC_LOG(LS_INFO) << "GroupRef: Added recvonly transceiver for SSRC " << ssrc;
                }
            }
        }

        // Create a new offer.
        auto observer = rtc::make_ref_counted<GRCreateSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](
                webrtc::SessionDescriptionInterface* desc) {
                auto strong = weak.lock();
                if (!strong) return;
                strong->_threads->getMediaThread()->PostTask([weak, ownedDesc = std::unique_ptr<webrtc::SessionDescriptionInterface>(desc->Clone())]() mutable {
                    if (auto s = weak.lock()) {
                        s->onRenegotiationOfferCreated(std::move(ownedDesc));
                    }
                });
            },
            [](webrtc::RTCError error) {
                RTC_LOG(LS_ERROR) << "GroupRef: Renegotiation CreateOffer failed: " << error.message();
            }
        );

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
        _peerConnection->CreateOffer(observer.get(), opts);
    }

    void onRenegotiationOfferCreated(std::unique_ptr<webrtc::SessionDescriptionInterface> offer) {
        auto* rawOffer = offer.release();
        auto observer = rtc::make_ref_counted<GRSetSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](webrtc::RTCError error) {
                if (!error.ok()) {
                    RTC_LOG(LS_ERROR) << "GroupRef: Renegotiation SetLocalDescription failed: " << error.message();
                    return;
                }
                if (auto strong = weak.lock()) {
                    strong->_threads->getMediaThread()->PostTask([weak]() {
                        if (auto s = weak.lock()) {
                            s->onRenegotiationLocalDescSet();
                        }
                    });
                }
            }
        );
        _peerConnection->SetLocalDescription(observer.get(), rawOffer);
    }

    void onRenegotiationLocalDescSet() {
        // Now build a matching remote answer with the updated m-lines.
        // Need to update mids to match what PeerConnection generated in the offer.
        auto localDesc = _peerConnection->local_description();
        if (!localDesc) return;

        // Update _remoteSsrcs mids to match the actual mids from the local offer transceivers.
        for (auto& [ssrc, info] : _remoteSsrcs) {
            if (info.transceiver) {
                info.mid = info.transceiver->mid().value_or(info.mid);
            }
        }

        auto remoteAnswer = buildRemoteAnswer();
        if (!remoteAnswer) {
            RTC_LOG(LS_ERROR) << "GroupRef: Failed to build renegotiation answer";
            return;
        }

        auto observer = rtc::make_ref_counted<GRSetSDPObserver>(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())](webrtc::RTCError error) {
                if (!error.ok()) {
                    RTC_LOG(LS_ERROR) << "GroupRef: Renegotiation SetRemoteDescription failed: " << error.message();
                    return;
                }
                auto strong = weak.lock();
                if (!strong) return;
                strong->_threads->getMediaThread()->PostTask([weak]() {
                    if (auto s = weak.lock()) {
                        s->addRemoteIceCandidates();
                    }
                });
            }
        );
        _peerConnection->SetRemoteDescription(observer.get(), remoteAnswer.release());
    }

    void onIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
        bool connected = (state == webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected ||
                         state == webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted);

        if (connected != _isConnected) {
            _isConnected = connected;
            if (connected) {
                startAudioLevelPolling();
            }
            if (_networkStateUpdated) {
                GroupNetworkState netState;
                netState.isConnected = connected;
                netState.connectionMode = GroupConnectionMode::GroupConnectionModeRtc;
                _networkStateUpdated(netState);
            }
        }
    }

    void onTrackAdded(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        RTC_LOG(LS_INFO) << "GroupRef: Remote audio track added (mid=" << transceiver->mid().value_or("?") << ")";
    }

    void startAudioLevelPolling() {
        if (_isPollingAudioLevels) return;
        _isPollingAudioLevels = true;
        scheduleAudioLevelPoll();
    }

    void scheduleAudioLevelPoll() {
        _threads->getMediaThread()->PostDelayedTask(
            [weak = std::weak_ptr<GroupInstanceReferenceInternal>(shared_from_this())]() {
                if (auto strong = weak.lock()) {
                    strong->pollAudioLevels();
                    if (strong->_isPollingAudioLevels) {
                        strong->scheduleAudioLevelPoll();
                    }
                }
            },
            webrtc::TimeDelta::Millis(100));
    }

    void pollAudioLevels() {
        if (!_audioLevelsUpdated || !_peerConnection) return;
        if (_remoteSsrcs.empty()) return;

        // Report a synthetic level for all known remote SSRCs.
        // We know audio is being received and decoded (bitrate stats confirm this)
        // but can't extract actual levels because the SFU forwards RTP with
        // extension IDs that may not match this PeerConnection's mapping.
        GroupLevelsUpdate update;
        for (const auto& [ssrc, info] : _remoteSsrcs) {
            GroupLevelUpdate entry;
            entry.ssrc = ssrc;
            entry.value.level = 0.1f;
            entry.value.voice = true;
            update.updates.push_back(entry);
        }

        if (!update.updates.empty()) {
            _audioLevelsUpdated(update);
        }
    }

private:
    struct RemoteSsrcInfo {
        std::string mid;
        webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver;
    };

    std::shared_ptr<Threads> _threads;

    // Callbacks from descriptor.
    std::function<void(GroupNetworkState)> _networkStateUpdated;
    std::function<void(GroupLevelsUpdate const &)> _audioLevelsUpdated;
    std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(webrtc::TaskQueueFactory*)> _createAudioDeviceModule;
    std::function<std::shared_ptr<RequestMediaChannelDescriptionTask>(std::vector<uint32_t> const &, std::function<void(std::vector<MediaChannelDescription> &&)>)> _requestMediaChannelDescriptions;
    int _outgoingAudioBitrateKbit = 32;

    // Join flow.
    std::function<void(GroupJoinPayload const &)> _joinCompletion;
    GroupJoinTransportDescription _remoteTransport;
    std::string _localUfrag;
    std::string _localPwd;

    // PeerConnection.
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _peerConnectionFactory;
    std::unique_ptr<GRPeerConnectionObserver> _peerConnectionObserver;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> _peerConnection;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;

    std::unique_ptr<rtc::NetworkMonitorFactory> _networkMonitorFactory;
    std::unique_ptr<rtc::BasicPacketSocketFactory> _socketFactory;
    std::unique_ptr<rtc::BasicNetworkManager> _networkManager;

    // Audio.
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> _outgoingAudioTrack;
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> _outgoingAudioTransceiver;

    // Data channel.
    webrtc::scoped_refptr<webrtc::DataChannelInterface> _dataChannel;
    std::unique_ptr<GRDataChannelObserver> _dataChannelObserver;
    bool _isDataChannelOpen = false;

    // Remote SSRCs.
    std::map<uint32_t, RemoteSsrcInfo> _remoteSsrcs;
    int _nextMid = 10; // Start after reserved mids (0=audio, 1-9=reserved).
    uint32_t _outgoingSsrc = 0;

    // Audio level polling.
    bool _isPollingAudioLevels = false;

    // State.
    bool _isConnected = false;
};

// ---------------------------------------------------------------------------
// GroupInstanceReferenceImpl (public wrapper)
// ---------------------------------------------------------------------------

GroupInstanceReferenceImpl::GroupInstanceReferenceImpl(GroupInstanceDescriptor &&descriptor) {
    if (descriptor.config.need_log) {
        _logSink = std::make_unique<LogSinkImpl>(descriptor.config.logPath);
    }

    _threads = descriptor.threads;

    _internal.reset(new ThreadLocalObject<GroupInstanceReferenceInternal>(_threads->getMediaThread(), [descriptor = std::move(descriptor), threads = _threads]() mutable {
        return std::make_shared<GroupInstanceReferenceInternal>(std::move(descriptor), threads);
    }));
    _internal->perform([](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->start();
    });
}

GroupInstanceReferenceImpl::~GroupInstanceReferenceImpl() {
    if (_logSink) {
        rtc::LogMessage::RemoveLogToStream(_logSink.get());
    }
    _internal.reset();
    _threads->getMediaThread()->BlockingCall([] {});
}

void GroupInstanceReferenceImpl::stop(std::function<void()> completion) {
    _internal->perform([completion = std::move(completion)](GroupInstanceReferenceInternal *unwrapped) mutable {
        unwrapped->stop(std::move(completion));
    });
}

void GroupInstanceReferenceImpl::setConnectionMode(GroupConnectionMode mode, bool keep, bool unified) {
    _internal->perform([mode, keep, unified](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->setConnectionMode(mode, keep, unified);
    });
}

void GroupInstanceReferenceImpl::emitJoinPayload(std::function<void(GroupJoinPayload const &)> completion) {
    _internal->perform([completion = std::move(completion)](GroupInstanceReferenceInternal *unwrapped) mutable {
        unwrapped->emitJoinPayload(std::move(completion));
    });
}

void GroupInstanceReferenceImpl::setJoinResponsePayload(std::string const &payload) {
    auto payloadCopy = payload;
    _internal->perform([payloadCopy = std::move(payloadCopy)](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->setJoinResponsePayload(payloadCopy);
    });
}

void GroupInstanceReferenceImpl::removeSsrcs(std::vector<uint32_t> ssrcs) {
    _internal->perform([ssrcs = std::move(ssrcs)](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->removeSsrcs(ssrcs);
    });
}

void GroupInstanceReferenceImpl::removeIncomingVideoSource(uint32_t ssrc) {}

void GroupInstanceReferenceImpl::setIsMuted(bool isMuted) {
    _internal->perform([isMuted](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->setIsMuted(isMuted);
    });
}

void GroupInstanceReferenceImpl::setIsNoiseSuppressionEnabled(bool) {}
void GroupInstanceReferenceImpl::setVideoCapture(std::shared_ptr<VideoCaptureInterface>) {}
void GroupInstanceReferenceImpl::setVideoSource(std::function<webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>()>) {}
void GroupInstanceReferenceImpl::setAudioOutputDevice(std::string) {}
void GroupInstanceReferenceImpl::setAudioInputDevice(std::string) {}
void GroupInstanceReferenceImpl::addExternalAudioSamples(std::vector<uint8_t>&&) {}
void GroupInstanceReferenceImpl::addOutgoingVideoOutput(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {}
void GroupInstanceReferenceImpl::addIncomingVideoOutput(std::string const &, std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>) {}

void GroupInstanceReferenceImpl::setVolume(uint32_t ssrc, double volume) {
    _internal->perform([ssrc, volume](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->setVolume(ssrc, volume);
    });
}

void GroupInstanceReferenceImpl::setRequestedVideoChannels(std::vector<VideoChannelDescription>&&) {}

void GroupInstanceReferenceImpl::getStats(std::function<void(GroupInstanceStats)> completion) {
    _internal->perform([completion = std::move(completion)](GroupInstanceReferenceInternal *unwrapped) {
        unwrapped->getStats(std::move(completion));
    });
}

void GroupInstanceReferenceImpl::internal_addCustomNetworkEvent(bool) {}

} // namespace tgcalls
