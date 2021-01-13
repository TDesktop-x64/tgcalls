#include "GroupInstanceCustomImpl.h"

#include <memory>

#include "Instance.h"
#include "VideoCaptureInterfaceImpl.h"
#include "VideoCapturerInterface.h"
#include "CodecSelectHelper.h"
#include "Message.h"
#include "platform/PlatformInterface.h"
#include "StaticThreads.h"
#include "GroupNetworkManager.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "system_wrappers/include/field_trial.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "call/call.h"
#include "modules/rtp_rtcp/source/rtp_utility.h"
#include "api/call/audio_sink.h"
#include "modules/audio_processing/audio_buffer.h"
#include "absl/strings/match.h"
#include "modules/audio_processing/agc2/vad_with_level.h"

#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"
#include "CodecSelectHelper.h"

#include <random>
#include <sstream>
#include <iostream>

namespace tgcalls {

namespace {

static int stringToInt(std::string const &string) {
    std::stringstream stringStream(string);
    int value = 0;
    stringStream >> value;
    return value;
}

static std::string intToString(int value) {
    std::ostringstream stringStream;
    stringStream << value;
    return stringStream.str();
}

static std::string uint32ToString(uint32_t value) {
    std::ostringstream stringStream;
    stringStream << value;
    return stringStream.str();
}

static uint32_t stringToUInt32(std::string const &string) {
    std::stringstream stringStream(string);
    uint32_t value = 0;
    stringStream >> value;
    return value;
}

static uint16_t stringToUInt16(std::string const &string) {
    std::stringstream stringStream(string);
    uint16_t value = 0;
    stringStream >> value;
    return value;
}

static std::string formatTimestampMillis(int64_t timestamp) {
    std::ostringstream stringStream;
    stringStream << std::fixed << std::setprecision(3) << (double)timestamp / 1000.0;
    return stringStream.str();
}

static VideoCaptureInterfaceObject *GetVideoCaptureAssumingSameThread(VideoCaptureInterface *videoCapture) {
    return videoCapture
        ? static_cast<VideoCaptureInterfaceImpl*>(videoCapture)->object()->getSyncAssumingSameThread()
        : nullptr;
}

struct OutgoingVideoFormat {
    cricket::VideoCodec videoCodec;
    cricket::VideoCodec rtxCodec;
};

static void addDefaultFeedbackParams(cricket::VideoCodec *codec) {
    // Don't add any feedback params for RED and ULPFEC.
    if (codec->name == cricket::kRedCodecName || codec->name == cricket::kUlpfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamRemb, cricket::kParamValueEmpty));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc, cricket::kParamValueEmpty));
    // Don't add any more feedback params for FLEXFEC.
    if (codec->name == cricket::kFlexfecCodecName) {
        return;
    }
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamCcm, cricket::kRtcpFbCcmParamFir));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kParamValueEmpty));
    codec->AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamNack, cricket::kRtcpFbNackParamPli));
    
    /*if (codec->name == kVp8CodecName &&
        IsEnabled(trials, "WebRTC-RtcpLossNotification")) {
        codec->AddFeedbackParam(FeedbackParam(kRtcpFbParamLntf, kParamValueEmpty));
    }*/
}

static absl::optional<OutgoingVideoFormat> assignPayloadTypes(std::vector<webrtc::SdpVideoFormat> const &formats) {
    if (formats.empty()) {
        return absl::nullopt;
    }

    constexpr int kFirstDynamicPayloadType = 100;
    constexpr int kLastDynamicPayloadType = 127;

    int payload_type = kFirstDynamicPayloadType;

    //formats.list.push_back(webrtc::SdpVideoFormat(cricket::kRedCodecName));
    //formats.list.push_back(webrtc::SdpVideoFormat(cricket::kUlpfecCodecName));

    auto result = OutgoingVideoFormat();
    
    bool codecSelected = false;
    
    for (const auto &format : formats) {
        if (codecSelected) {
            break;
        }
        
        cricket::VideoCodec codec(format);
        codec.id = payload_type;
        addDefaultFeedbackParams(&codec);

        if (!absl::EqualsIgnoreCase(codec.name, cricket::kVp8CodecName)) {
            continue;
        }
        
        result.videoCodec = codec;
        codecSelected = true;

        // Increment payload type.
        ++payload_type;
        if (payload_type > kLastDynamicPayloadType) {
            RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
            break;
        }

        // Add associated RTX codec for non-FEC codecs.
        if (!absl::EqualsIgnoreCase(codec.name, cricket::kUlpfecCodecName) &&
            !absl::EqualsIgnoreCase(codec.name, cricket::kFlexfecCodecName)) {
            result.rtxCodec = cricket::VideoCodec::CreateRtxCodec(payload_type, codec.id);

            // Increment payload type.
            ++payload_type;
            if (payload_type > kLastDynamicPayloadType) {
                RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                break;
            }
        }
    }
    return result;
}

struct VideoSsrcs {
    struct SimulcastLayer {
        uint32_t ssrc = 0;
        uint32_t fidSsrc = 0;
        
        SimulcastLayer(uint32_t ssrc_, uint32_t fidSsrc_) :
            ssrc(ssrc_), fidSsrc(fidSsrc_) {
        }
        
        SimulcastLayer(const SimulcastLayer &other) :
            ssrc(other.ssrc), fidSsrc(other.fidSsrc) {
        }
    };
    
    std::vector<SimulcastLayer> simulcastLayers;
    
    VideoSsrcs() {
    }
    
    VideoSsrcs(const VideoSsrcs &other) :
        simulcastLayers(other.simulcastLayers) {
    }
};

class NetworkInterfaceImpl : public cricket::MediaChannel::NetworkInterface {
public:
    NetworkInterfaceImpl(std::function<void(rtc::CopyOnWriteBuffer const *, rtc::SentPacket)> sendPacket) :
    _sendPacket(sendPacket) {
        
    }
    
    bool SendPacket(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
        rtc::SentPacket sentPacket(options.packet_id, rtc::TimeMillis(), options.info_signaled_after_sent);
        _sendPacket(packet, sentPacket);
        return true;
    }

    bool SendRtcp(rtc::CopyOnWriteBuffer *packet, const rtc::PacketOptions& options) {
        rtc::SentPacket sentPacket(options.packet_id, rtc::TimeMillis(), options.info_signaled_after_sent);
        _sendPacket(packet, sentPacket);
        return true;
    }

    int SetOption(cricket::MediaChannel::NetworkInterface::SocketType, rtc::Socket::Option, int) {
        return -1;
    }

private:
    std::function<void(rtc::CopyOnWriteBuffer const *, rtc::SentPacket)> _sendPacket;
};

static const int kVadResultHistoryLength = 8;

class CombinedVad {
private:
    webrtc::VadLevelAnalyzer _vadWithLevel;
    float _vadResultHistory[kVadResultHistoryLength];

public:
    CombinedVad() {
        for (int i = 0; i < kVadResultHistoryLength; i++) {
            _vadResultHistory[i] = 0.0f;
        }
    }

    ~CombinedVad() {
    }

    bool update(webrtc::AudioBuffer *buffer) {
        webrtc::AudioFrameView<float> frameView(buffer->channels(), buffer->num_channels(), buffer->num_frames());
        auto result = _vadWithLevel.AnalyzeFrame(frameView);
        for (int i = 1; i < kVadResultHistoryLength; i++) {
            _vadResultHistory[i - 1] = _vadResultHistory[i];
        }
        _vadResultHistory[kVadResultHistoryLength - 1] = result.speech_probability;

        float movingAverage = 0.0f;
        for (int i = 0; i < kVadResultHistoryLength; i++) {
            movingAverage += _vadResultHistory[i];
        }
        movingAverage /= (float)kVadResultHistoryLength;

        bool vadResult = false;
        if (movingAverage > 0.8f) {
            vadResult = true;
        }

        return vadResult;
    }
};

class AudioSinkImpl: public webrtc::AudioSinkInterface {
public:
    struct Update {
        float level = 0.0f;
        bool hasSpeech = false;
        
        Update(float level_, bool hasSpech_) :
            level(level_), hasSpeech(hasSpech_) {
        }
        
        Update(const Update &other) :
            level(other.level), hasSpeech(other.hasSpeech) {
        }
    };
    
public:
    AudioSinkImpl(std::function<void(Update)> update) :
    _update(update) {
    }

    virtual ~AudioSinkImpl() {
    }

    virtual void OnData(const Data& audio) override {
        if (audio.channels == 1) {
            const int16_t *samples = (const int16_t *)audio.data;
            int numberOfSamplesInFrame = (int)audio.samples_per_channel;

            webrtc::AudioBuffer buffer(audio.sample_rate, 1, 48000, 1, 48000, 1);
            webrtc::StreamConfig config(audio.sample_rate, 1);
            buffer.CopyFrom(samples, config);

            bool vadResult = _vad.update(&buffer);

            for (int i = 0; i < numberOfSamplesInFrame; i++) {
                int16_t sample = samples[i];
                if (sample < 0) {
                    sample = -sample;
                }
                if (_peak < sample) {
                    _peak = sample;
                }
                _peakCount += 1;
            }

            if (_peakCount >= 1200) {
                float level = ((float)(_peak)) / 4000.0f;
                _peak = 0;
                _peakCount = 0;
                _update(Update(level, vadResult));
            }
        }
    }
    
private:
    std::function<void(Update)> _update;

    int _peakCount = 0;
    uint16_t _peak = 0;

    CombinedVad _vad;
    
};

class VideoSinkImpl : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    VideoSinkImpl() {
    }

    virtual ~VideoSinkImpl() {
    }

    virtual void OnFrame(const webrtc::VideoFrame& frame) override {
        _lastFrame = frame;
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnFrame(frame);
            }
        }
    }

    virtual void OnDiscardedFrame() override {
        for (int i = (int)(_sinks.size()) - 1; i >= 0; i--) {
            auto strong = _sinks[i].lock();
            if (!strong) {
                _sinks.erase(_sinks.begin() + i);
            } else {
                strong->OnDiscardedFrame();
            }
        }
    }

    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _sinks.push_back(impl);
        if (_lastFrame) {
            auto strong = impl.lock();
            if (strong) {
                strong->OnFrame(_lastFrame.value());
            }
        }
    }

private:
    std::vector<std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>>> _sinks;
    absl::optional<webrtc::VideoFrame> _lastFrame;
};

class AudioCaptureAnalyzer : public webrtc::CustomAudioAnalyzer {
private:
    void Initialize(int sample_rate_hz, int num_channels) override {

    }
    
    void Analyze(const webrtc::AudioBuffer* buffer) override {
        if (!buffer) {
            return;
        }
        if (buffer->num_channels() != 1) {
            return;
        }

        float peak = 0;
        int peakCount = 0;
        const float *samples = buffer->channels_const()[0];
        for (int i = 0; i < buffer->num_frames(); i++) {
            float sample = samples[i];
            if (sample < 0) {
                sample = -sample;
            }
            if (peak < sample) {
                peak = sample;
            }
            peakCount += 1;
        }

        bool vadStatus = _vad.update((webrtc::AudioBuffer *)buffer);
        
        _peakCount += peakCount;
        if (_peak < peak) {
            _peak = peak;
        }
        if (_peakCount >= 1200) {
            float level = _peak / 4000.0f;
            _peak = 0;
            _peakCount = 0;
            
            _updated(GroupLevelValue{
                level,
                vadStatus,
            });
        }
    }
    
    std::string ToString() const override {
        return "analyzing";
    }
    
private:
    std::function<void(GroupLevelValue const &)> _updated;
    
    CombinedVad _vad;
    int32_t _peakCount = 0;
    float _peak = 0;

public:
    AudioCaptureAnalyzer(std::function<void(GroupLevelValue const &)> updated) :
    _updated(updated) {
    }

    virtual ~AudioCaptureAnalyzer() = default;
};

class IncomingAudioChannel {
public:
    IncomingAudioChannel(
        cricket::MediaEngineInterface *mediaEngine,
        webrtc::Call *call,
        uint32_t ssrc,
        std::function<void(std::vector<uint8_t> const &, rtc::SentPacket)> sendPacket,
        std::function<void(AudioSinkImpl::Update)> &&onAudioLevelUpdated) :
    _ssrc(ssrc) {
        cricket::AudioOptions audioOptions;
        audioOptions.echo_cancellation = true;
        audioOptions.noise_suppression = true;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;
        
        _audioChannel.reset(mediaEngine->voice().CreateMediaChannel(call, cricket::MediaConfig(), audioOptions, webrtc::CryptoOptions::NoGcm()));
        
        _audioInterface.reset(new NetworkInterfaceImpl([sendPacket = std::move(sendPacket)](rtc::CopyOnWriteBuffer const *buffer, rtc::SentPacket sentPacket) {
            std::vector<uint8_t> data;
            
            data.resize(buffer->size());
            memcpy(data.data(), buffer->data(), buffer->size());
            sendPacket(data, sentPacket);
        }));
        _audioChannel->SetInterface(_audioInterface.get());
        
        const uint32_t opusClockrate = 48000;
        const uint16_t opusSdpPayload = 111;
        const char *opusSdpName = "opus";
        const uint8_t opusSdpChannels = 2;
        const uint32_t opusSdpBitrate = 0;

        cricket::AudioRecvParameters audioRecvParameters;
        audioRecvParameters.codecs.emplace_back(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
        audioRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kAudioLevelUri, 1);
        audioRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, 2);
        audioRecvParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 3);
        audioRecvParameters.rtcp.reduced_size = true;
        audioRecvParameters.rtcp.remote_estimate = false;
        
        std::vector<std::string> streamIds;
        streamIds.push_back(std::string("stream") + uint32ToString(ssrc));

        _audioChannel->SetRecvParameters(audioRecvParameters);
        cricket::StreamParams audioRecvStreamParams = cricket::StreamParams::CreateLegacy(ssrc);
        audioRecvStreamParams.set_stream_ids(streamIds);
        _audioChannel->AddRecvStream(audioRecvStreamParams);
        _audioChannel->SetPlayout(true);
        
        std::unique_ptr<AudioSinkImpl> audioLevelSink(new AudioSinkImpl([onAudioLevelUpdated = std::move(onAudioLevelUpdated)](AudioSinkImpl::Update update) {
            onAudioLevelUpdated(update);
        }));
        _audioChannel->SetRawAudioSink(ssrc, std::move(audioLevelSink));
    }
    
    ~IncomingAudioChannel() {
        _audioChannel->SetPlayout(false);
        _audioChannel->RemoveRecvStream(_ssrc);
        _audioChannel->SetInterface(nullptr);
    }
    
    void setVolume(double value) {
        _audioChannel->SetOutputVolume(_ssrc, value);
    }
    
private:
    uint32_t _ssrc = 0;
    std::unique_ptr<cricket::VoiceMediaChannel> _audioChannel;
    std::unique_ptr<NetworkInterfaceImpl> _audioInterface;
};

class IncomingVideoChannel {
public:
    IncomingVideoChannel(
        cricket::MediaEngineInterface *mediaEngine,
        webrtc::Call *call,
        std::vector<webrtc::SdpVideoFormat> const &availableVideoFormats,
        GroupParticipantDescription const &description,
        std::function<void(std::vector<uint8_t> const &, rtc::SentPacket)> sendPacket) {
        _videoSink.reset(new VideoSinkImpl());
        
        cricket::VideoOptions videoOptions;
        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
        _videoChannel.reset(mediaEngine->video().CreateMediaChannel(call, cricket::MediaConfig(), videoOptions, webrtc::CryptoOptions::NoGcm(), _videoBitrateAllocatorFactory.get()));
        
        _videoInterface.reset(new NetworkInterfaceImpl([sendPacket = std::move(sendPacket)](rtc::CopyOnWriteBuffer const *buffer, rtc::SentPacket sentPacket) {
            std::vector<uint8_t> data;
            
            data.resize(buffer->size());
            memcpy(data.data(), buffer->data(), buffer->size());
            sendPacket(data, sentPacket);
        }));
        _videoChannel->SetInterface(_videoInterface.get());
        
        auto payloadTypes = assignPayloadTypes(availableVideoFormats);
        if (!payloadTypes.has_value()) {
            return;
        }
        
        cricket::VideoSendParameters videoSendParameters;
        cricket::VideoRecvParameters videoRecvParameters;
        
        videoSendParameters.codecs = { payloadTypes.value().videoCodec, payloadTypes.value().rtxCodec };
        videoRecvParameters.codecs = { payloadTypes.value().videoCodec, payloadTypes.value().rtxCodec };
        
        for (const auto &extension : description.videoExtensionMap) {
            videoSendParameters.extensions.emplace_back(extension.second, extension.first);
            videoRecvParameters.extensions.emplace_back(extension.second, extension.first);
        }
        
        videoSendParameters.rtcp.reduced_size = true;
        videoSendParameters.rtcp.remote_estimate = false;
        _videoChannel->SetSendParameters(videoSendParameters);
        
        cricket::StreamParams videoSendStreamParams;

        videoRecvParameters.rtcp.reduced_size = true;
        videoRecvParameters.rtcp.remote_estimate = false;
        
        std::vector<std::string> streamIds;
        streamIds.push_back(std::string("stream") + uint32ToString(description.audioSsrc));

        _videoChannel->SetRecvParameters(videoRecvParameters);
        
        cricket::StreamParams videoRecvStreamParams;
        
        std::vector<uint32_t> allSsrcs;
        for (const auto &group : description.videoSourceGroups) {
            for (auto ssrc : group.ssrcs) {
                if (std::find(allSsrcs.begin(), allSsrcs.end(), ssrc) == allSsrcs.end()) {
                    allSsrcs.push_back(ssrc);
                }
            }
            
            if (group.semantics == "SIM") {
                if (_mainVideoSsrc == 0) {
                    _mainVideoSsrc = group.ssrcs[0];
                }
            }
            
            cricket::SsrcGroup parsedGroup(group.semantics, group.ssrcs);
            videoRecvStreamParams.ssrc_groups.push_back(parsedGroup);
        }
        videoRecvStreamParams.ssrcs = allSsrcs;
        
        if (_mainVideoSsrc == 0) {
            if (description.videoSourceGroups.size() == 1) {
                _mainVideoSsrc = description.videoSourceGroups[0].ssrcs[0];
            }
        }
        
        videoRecvStreamParams.cname = "cname";
        
        videoRecvStreamParams.set_stream_ids(streamIds);
        _videoChannel->AddRecvStream(videoRecvStreamParams);
        
        _videoChannel->SetSink(_mainVideoSsrc, _videoSink.get());
    }
    
    ~IncomingVideoChannel() {
        _videoChannel->RemoveRecvStream(_mainVideoSsrc);
        _videoChannel->SetInterface(nullptr);
    }
    
    void addSink(std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _videoSink->addSink(impl);
    }
    
private:
    uint32_t _mainVideoSsrc = 0;
    std::unique_ptr<VideoSinkImpl> _videoSink;
    std::vector<GroupJoinPayloadVideoSourceGroup> _ssrcGroups;
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;
    std::unique_ptr<cricket::VideoMediaChannel> _videoChannel;
    std::unique_ptr<NetworkInterfaceImpl> _videoInterface;
};

struct SsrcMappingInfo {
    uint32_t ssrc = 0;
    bool isVideo = false;
};

class MissingSsrcPacketBuffer {
public:
    MissingSsrcPacketBuffer(int limit) :
    _limit(limit) {
    }
    
    ~MissingSsrcPacketBuffer() {
    }
    
    void add(uint32_t ssrc, rtc::CopyOnWriteBuffer const &packet) {
        if (_packets.size() == _limit) {
            _packets.erase(_packets.begin());
        }
        _packets.push_back(std::make_pair(ssrc, packet));
    }
    
    std::vector<rtc::CopyOnWriteBuffer> get(uint32_t ssrc) {
        std::vector<rtc::CopyOnWriteBuffer> result;
        for (auto it = _packets.begin(); it != _packets.end(); ) {
            if (it->first == ssrc) {
                result.push_back(it->second);
                _packets.erase(it);
            } else {
                it++;
            }
        }
        return result;
    }
    
private:
    int _limit = 0;
    std::vector<std::pair<uint32_t, rtc::CopyOnWriteBuffer>> _packets;
    
};

} // namespace

class GroupInstanceCustomInternal : public std::enable_shared_from_this<GroupInstanceCustomInternal> {
public:
    GroupInstanceCustomInternal(GroupInstanceDescriptor &&descriptor) :
    _networkStateUpdated(descriptor.networkStateUpdated),
    _audioLevelsUpdated(descriptor.audioLevelsUpdated),
    _incomingVideoSourcesUpdated(descriptor.incomingVideoSourcesUpdated),
    _participantDescriptionsRequired(descriptor.participantDescriptionsRequired),
    _videoCapture(descriptor.videoCapture),
    _eventLog(std::make_unique<webrtc::RtcEventLogNull>()),
    _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory()),
    _missingPacketBuffer(100) {
        assert(StaticThreads::getMediaThread()->IsCurrent());
        
        auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        do {
            _outgoingAudioSsrc = distribution(generator);
        } while (!_outgoingAudioSsrc);
        
        uint32_t outgoingVideoSsrcBase = _outgoingAudioSsrc + 1;
        int numVideoSimulcastLayers = 1;
        for (int layerIndex = 0; layerIndex < numVideoSimulcastLayers; layerIndex++) {
            _outgoingVideoSsrcs.simulcastLayers.push_back(VideoSsrcs::SimulcastLayer(outgoingVideoSsrcBase + layerIndex * 2 + 0, outgoingVideoSsrcBase + layerIndex * 2 + 1));
        }
    }

    ~GroupInstanceCustomInternal() {
        _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
        _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);

        _outgoingAudioChannel->SetAudioSend(_outgoingAudioSsrc, false, nullptr, &_audioSource);
        _outgoingAudioChannel->OnReadyToSend(false);
        _outgoingAudioChannel->SetSend(false);
        _outgoingAudioChannel->SetInterface(nullptr);
        
        _outgoingVideoChannel->SetVideoSend(_outgoingVideoSsrcs.simulcastLayers[0].ssrc, NULL, nullptr);
        _outgoingVideoChannel->OnReadyToSend(false);
        _outgoingVideoChannel->SetSend(false);
        _outgoingVideoChannel->SetInterface(nullptr);
    }

    void start() {
        const auto weak = std::weak_ptr<GroupInstanceCustomInternal>(shared_from_this());
        
        webrtc::field_trial::InitFieldTrialsFromString(
            "WebRTC-Audio-Allocation/min:32kbps,max:32kbps/"
            "WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
        );
        
        _networkManager.reset(new ThreadLocalObject<GroupNetworkManager>(StaticThreads::getNetworkThread(), [weak] () mutable {
            return new GroupNetworkManager(
                [=](const GroupNetworkManager::State &state) {
                    StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [=] {
                        const auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        bool mappedState = false;
                        if (state.isFailed) {
                            mappedState = false;
                        } else {
                            mappedState = state.isReadyToSendData
                                ? true
                                : false;
                        }
                        
                        if (strong->_networkStateUpdated) {
                            strong->_networkStateUpdated(mappedState);
                        }

                        strong->setIsConnected(mappedState);
                    });
                },
                [=](rtc::CopyOnWriteBuffer const &message) {
                    StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, message]() mutable {
                        if (const auto strong = weak.lock()) {
                            strong->receivePacket(message);
                        }
                    });
                });
        }));
        
        _networkManager->perform(RTC_FROM_HERE, [](GroupNetworkManager *networkManager) {
            networkManager->start();
        });

        PlatformInterface::SharedInstance()->configurePlatformAudio();

        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = _taskQueueFactory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
        mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();

        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();
        
        auto analyzer = new AudioCaptureAnalyzer([weak](GroupLevelValue const &level) {
            StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, level](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->_myAudioLevel = level;
            });
        });

        webrtc::AudioProcessingBuilder builder;
        builder.SetCaptureAnalyzer(std::unique_ptr<AudioCaptureAnalyzer>(analyzer));
        
        mediaDeps.audio_processing = builder.Create();

        _audioDeviceModule = createAudioDeviceModule();
        if (!_audioDeviceModule) {
            return;
        }
        mediaDeps.adm = _audioDeviceModule;
        
        _availableVideoFormats = mediaDeps.video_encoder_factory->GetSupportedFormats();

        _mediaEngine = cricket::CreateMediaEngine(std::move(mediaDeps));
        _mediaEngine->Init();

        webrtc::Call::Config callConfig(_eventLog.get());
        callConfig.task_queue_factory = _taskQueueFactory.get();
        callConfig.trials = &_fieldTrials;
        callConfig.audio_state = _mediaEngine->voice().GetAudioState();
        _call.reset(webrtc::Call::Create(callConfig));

        cricket::AudioOptions audioOptions;
        audioOptions.echo_cancellation = true;
        audioOptions.noise_suppression = true;
        audioOptions.audio_jitter_buffer_fast_accelerate = true;

        std::vector<std::string> streamIds;
        streamIds.push_back("1");

        _outgoingAudioChannel.reset(_mediaEngine->voice().CreateMediaChannel(_call.get(), cricket::MediaConfig(), audioOptions, webrtc::CryptoOptions::NoGcm()));

        const uint32_t opusClockrate = 48000;
        const uint16_t opusSdpPayload = 111;
        const char *opusSdpName = "opus";
        const uint8_t opusSdpChannels = 2;
        const uint32_t opusSdpBitrate = 0;

        const uint8_t opusMinBitrateKbps = 32;
        const uint8_t opusMaxBitrateKbps = 32;
        const uint8_t opusStartBitrateKbps = 32;
        const uint8_t opusPTimeMs = 120;

        cricket::AudioCodec opusCodec(opusSdpPayload, opusSdpName, opusClockrate, opusSdpBitrate, opusSdpChannels);
        opusCodec.AddFeedbackParam(cricket::FeedbackParam(cricket::kRtcpFbParamTransportCc));
        opusCodec.SetParam(cricket::kCodecParamMinBitrate, opusMinBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamStartBitrate, opusStartBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamMaxBitrate, opusMaxBitrateKbps);
        opusCodec.SetParam(cricket::kCodecParamUseInbandFec, 1);
        opusCodec.SetParam(cricket::kCodecParamPTime, opusPTimeMs);

        cricket::AudioSendParameters audioSendPrameters;
        audioSendPrameters.codecs.push_back(opusCodec);
        audioSendPrameters.extensions.emplace_back(webrtc::RtpExtension::kAudioLevelUri, 1);
        audioSendPrameters.extensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, 2);
        audioSendPrameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 3);
        audioSendPrameters.options.echo_cancellation = true;
        audioSendPrameters.options.noise_suppression = true;
        audioSendPrameters.options.auto_gain_control = true;
        audioSendPrameters.options.typing_detection = false;
        audioSendPrameters.rtcp.reduced_size = true;
        audioSendPrameters.rtcp.remote_estimate = false;
        _outgoingAudioChannel->SetSendParameters(audioSendPrameters);
        _outgoingAudioChannel->AddSendStream(cricket::StreamParams::CreateLegacy(_outgoingAudioSsrc));
        
        _outgoingAudioInterface.reset(new NetworkInterfaceImpl([weak](rtc::CopyOnWriteBuffer const *buffer, rtc::SentPacket sentPacket) {
            rtc::CopyOnWriteBuffer packet(*buffer);
                
            StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, packet, sentPacket]() mutable {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->_networkManager->perform(RTC_FROM_HERE, [weak, packet, sentPacket](GroupNetworkManager *networkManager) mutable {
                    networkManager->sendMessage(packet);
                    
                    StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sentPacket]() mutable {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        
                        strong->_call->OnSentPacket(sentPacket);
                    });
                });
            });
        }));
        _outgoingAudioChannel->SetInterface(_outgoingAudioInterface.get());
        
        _videoBitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
        _outgoingVideoChannel.reset(_mediaEngine->video().CreateMediaChannel(_call.get(), cricket::MediaConfig(), cricket::VideoOptions(), webrtc::CryptoOptions::NoGcm(), _videoBitrateAllocatorFactory.get()));
        _outgoingVideoInterface.reset(new NetworkInterfaceImpl([weak](rtc::CopyOnWriteBuffer const *buffer, rtc::SentPacket sentPacket) {
            rtc::CopyOnWriteBuffer packet(*buffer);
                
            StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, packet, sentPacket]() mutable {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->_networkManager->perform(RTC_FROM_HERE, [weak, packet, sentPacket](GroupNetworkManager *networkManager) mutable {
                    networkManager->sendMessage(packet);
                    
                    StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sentPacket]() mutable {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        
                        strong->_call->OnSentPacket(sentPacket);
                    });
                });
            });
        }));
        _outgoingVideoChannel->SetInterface(_outgoingVideoInterface.get());
        
        configureSendVideo();
        
        beginLevelsTimer(50);
        
        adjustBitratePreferences(true);
    }

    void stop() {
    }
    
    void beginLevelsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<GroupInstanceCustomInternal>(shared_from_this());
        StaticThreads::getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }

            GroupLevelsUpdate levelsUpdate;
            levelsUpdate.updates.reserve(strong->_audioLevels.size() + 1);
            for (auto &it : strong->_audioLevels) {
                if (it.second.level > 0.001f) {
                    levelsUpdate.updates.push_back(GroupLevelUpdate{
                        it.first,
                        it.second,
                        });
                }
            }
            auto myAudioLevel = strong->_myAudioLevel;
            if (strong->_isMuted) {
                myAudioLevel.level = 0.0f;
                myAudioLevel.voice = false;
            }
            levelsUpdate.updates.push_back(GroupLevelUpdate{ 0, myAudioLevel });

            strong->_audioLevels.clear();
            if (strong->_audioLevelsUpdated) {
                strong->_audioLevelsUpdated(levelsUpdate);
            }

            strong->beginLevelsTimer(50);
        }, timeoutMs);
    }
    
    void configureSendVideo() {
        cricket::VideoSendParameters videoSendParameters;
        
        auto payloadTypes = assignPayloadTypes(_availableVideoFormats);
        if (!payloadTypes.has_value()) {
            return;
        }
        
        GroupJoinPayloadVideoPayloadType vp8Payload;
        vp8Payload.id = payloadTypes.value().videoCodec.id;
        vp8Payload.name = payloadTypes.value().videoCodec.name;
        vp8Payload.clockrate = payloadTypes.value().videoCodec.clockrate;
        vp8Payload.channels = 0;
        
        std::vector<GroupJoinPayloadVideoPayloadFeedbackType> vp8FeedbackTypes;
        
        GroupJoinPayloadVideoPayloadFeedbackType fbGoogRemb;
        fbGoogRemb.type = "goog-remb";
        vp8FeedbackTypes.push_back(fbGoogRemb);
        
        GroupJoinPayloadVideoPayloadFeedbackType fbTransportCc;
        fbTransportCc.type = "transport-cc";
        vp8FeedbackTypes.push_back(fbTransportCc);
        
        GroupJoinPayloadVideoPayloadFeedbackType fbCcmFir;
        fbCcmFir.type = "ccm";
        fbCcmFir.subtype = "fir";
        vp8FeedbackTypes.push_back(fbCcmFir);
        
        GroupJoinPayloadVideoPayloadFeedbackType fbNack;
        fbNack.type = "nack";
        vp8FeedbackTypes.push_back(fbNack);
        
        GroupJoinPayloadVideoPayloadFeedbackType fbNackPli;
        fbNackPli.type = "nack";
        fbNackPli.subtype = "pli";
        vp8FeedbackTypes.push_back(fbNackPli);
        
        vp8Payload.feedbackTypes = vp8FeedbackTypes;
        vp8Payload.parameters = {};
        
        _videoPayloadTypes.push_back(std::move(vp8Payload));
        
        GroupJoinPayloadVideoPayloadType rtxPayload;
        rtxPayload.id = payloadTypes.value().rtxCodec.id;
        rtxPayload.name = payloadTypes.value().rtxCodec.name;
        rtxPayload.clockrate = payloadTypes.value().rtxCodec.clockrate;
        rtxPayload.parameters.push_back(std::make_pair("apt", intToString(payloadTypes.value().videoCodec.id)));
        _videoPayloadTypes.push_back(std::move(rtxPayload));
        
        videoSendParameters.codecs = { payloadTypes.value().videoCodec, payloadTypes.value().rtxCodec };
        
        videoSendParameters.extensions.emplace_back(webrtc::RtpExtension::kAbsSendTimeUri, 2);
        videoSendParameters.extensions.emplace_back(webrtc::RtpExtension::kTransportSequenceNumberUri, 3);
        videoSendParameters.extensions.emplace_back(webrtc::RtpExtension::kVideoRotationUri, 13);
        
        for (const auto &extension : videoSendParameters.extensions) {
            _videoExtensionMap.push_back(std::make_pair(extension.id, extension.uri));
        }
        
        videoSendParameters.rtcp.reduced_size = true;
        videoSendParameters.rtcp.remote_estimate = false;
        _outgoingVideoChannel->SetSendParameters(videoSendParameters);
        
        cricket::StreamParams videoSendStreamParams;
        
        std::vector<uint32_t> simulcastGroupSsrcs;
        std::vector<cricket::SsrcGroup> fidGroups;
        for (const auto &layer : _outgoingVideoSsrcs.simulcastLayers) {
            simulcastGroupSsrcs.push_back(layer.ssrc);
            
            videoSendStreamParams.ssrcs.push_back(layer.ssrc);
            videoSendStreamParams.ssrcs.push_back(layer.fidSsrc);
            
            cricket::SsrcGroup fidGroup(cricket::kFidSsrcGroupSemantics, { layer.ssrc, layer.fidSsrc });
            fidGroups.push_back(fidGroup);
        }
        if (simulcastGroupSsrcs.size() > 1) {
            cricket::SsrcGroup simulcastGroup(cricket::kSimSsrcGroupSemantics, simulcastGroupSsrcs);
            videoSendStreamParams.ssrc_groups.push_back(simulcastGroup);
            
            GroupJoinPayloadVideoSourceGroup payloadSimulcastGroup;
            payloadSimulcastGroup.semantics = "SIM";
            payloadSimulcastGroup.ssrcs = simulcastGroupSsrcs;
            _videoSourceGroups.push_back(payloadSimulcastGroup);
        }
        
        for (auto fidGroup : fidGroups) {
            videoSendStreamParams.ssrc_groups.push_back(fidGroup);
            
            GroupJoinPayloadVideoSourceGroup payloadFidGroup;
            payloadFidGroup.semantics = "FID";
            payloadFidGroup.ssrcs = fidGroup.ssrcs;
            _videoSourceGroups.push_back(payloadFidGroup);
        }
        
        videoSendStreamParams.cname = "cname";
        _outgoingVideoChannel->AddSendStream(videoSendStreamParams);
    }
    
    void adjustBitratePreferences(bool resetStartBitrate) {
        webrtc::BitrateConstraints preferences;
        if (_videoCapture) {
            preferences.min_bitrate_bps = 64000;
            if (resetStartBitrate) {
                preferences.start_bitrate_bps = 400000;
            }
            preferences.max_bitrate_bps = 900000;
        } else {
            preferences.min_bitrate_bps = 32000;
            if (resetStartBitrate) {
                preferences.start_bitrate_bps = 32000;
            }
            preferences.max_bitrate_bps = 32000;
        }

        _call->GetTransportControllerSend()->SetSdpBitrateParameters(preferences);
    }
    
    void setIsConnected(bool isConnected) {
        if (_isConnected == isConnected) {
            return;
        }
        _isConnected = isConnected;
        
        RTC_LOG(LS_INFO) << formatTimestampMillis(rtc::TimeMillis()) << ": " << "setIsConnected: " << _isConnected;
        
        if (_isConnected) {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
        } else {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);
        }
        if (_outgoingAudioChannel) {
            _outgoingAudioChannel->OnReadyToSend(_isConnected);
            _outgoingAudioChannel->SetSend(_isConnected && !_isMuted);
            _outgoingAudioChannel->SetAudioSend(_outgoingAudioSsrc, _isConnected && !_isMuted, nullptr, &_audioSource);
        }
        if (_outgoingVideoChannel) {
            if (_isConnected && _videoCapture) {
                _outgoingVideoChannel->SetVideoSend(_outgoingVideoSsrcs.simulcastLayers[0].ssrc, NULL, GetVideoCaptureAssumingSameThread(_videoCapture.get())->source());
                _outgoingVideoChannel->OnReadyToSend(true);
                _outgoingVideoChannel->SetSend(true);
                
            } else {
                _outgoingVideoChannel->SetVideoSend(_outgoingVideoSsrcs.simulcastLayers[0].ssrc, NULL, nullptr);
                _outgoingVideoChannel->OnReadyToSend(false);
                _outgoingVideoChannel->SetSend(false);
            }
        }
    }
    
    void receivePacket(rtc::CopyOnWriteBuffer const &packet) {
        webrtc::RtpUtility::RtpHeaderParser rtpParser(packet.data(), packet.size());
        
        webrtc::RTPHeader header;
        if (rtpParser.RTCP()) {
            if (!rtpParser.ParseRtcp(&header)) {
                RTC_LOG(LS_INFO) << "Could not parse rtcp header";
                return;
            }
            
            _call->Receiver()->DeliverPacket(webrtc::MediaType::ANY, packet, -1);
        } else {
            if (!rtpParser.Parse(&header)) {
                RTC_LOG(LS_INFO) << "Could not parse rtp header";
                return;
            }
            
            if (header.ssrc == _outgoingAudioSsrc) {
                return;
            }
            
            auto it = _ssrcMapping.find(header.ssrc);
            if (it == _ssrcMapping.end()) {
                maybeReportUnknownSsrc(header.ssrc);
                _missingPacketBuffer.add(header.ssrc, packet);
            } else {
                if (it->second.isVideo) {
                    _call->Receiver()->DeliverPacket(webrtc::MediaType::VIDEO, packet, -1);
                } else {
                    _call->Receiver()->DeliverPacket(webrtc::MediaType::AUDIO, packet, -1);
                }
            }
        }
    }
    
    void maybeReportUnknownSsrc(uint32_t ssrc) {
        if (_reportedUnknownSsrcs.find(ssrc) == _reportedUnknownSsrcs.end()) {
            _reportedUnknownSsrcs.insert(ssrc);
            
            _pendingUnknownSsrcs.insert(ssrc);
            
            auto timestamp = rtc::TimeMillis();
            if (_lastUnknownSsrcsReport < timestamp - 100) {
                doReportPendingUnknownSsrcs();
            } else if (!_isUnknownSsrcsScheduled) {
                _isUnknownSsrcsScheduled = true;
                
                const auto weak = std::weak_ptr<GroupInstanceCustomInternal>(shared_from_this());
                StaticThreads::getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }

                    strong->_isUnknownSsrcsScheduled = false;
                    strong->doReportPendingUnknownSsrcs();
                }, 100);
            }
        }
    }
    
    void doReportPendingUnknownSsrcs() {
        if (_participantDescriptionsRequired) {
            std::vector<uint32_t> ssrcs;
            for (auto ssrc : _pendingUnknownSsrcs) {
                ssrcs.push_back(ssrc);
            }
            _pendingUnknownSsrcs.clear();
            
            if (ssrcs.size() != 0) {
                _lastUnknownSsrcsReport = rtc::TimeMillis();
                _participantDescriptionsRequired(ssrcs);
            }
        }
    }
    
    void maybeDeliverBufferedPackets(uint32_t ssrc) {
        auto packets = _missingPacketBuffer.get(ssrc);
        if (packets.size() != 0) {
            auto it = _ssrcMapping.find(ssrc);
            if (it != _ssrcMapping.end()) {
                for (const auto &packet : packets) {
                    if (it->second.isVideo) {
                        _call->Receiver()->DeliverPacket(webrtc::MediaType::VIDEO, packet, -1);
                    } else {
                        _call->Receiver()->DeliverPacket(webrtc::MediaType::AUDIO, packet, -1);
                    }
                }
            }
        }
    }
    
    void emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
        _networkManager->perform(RTC_FROM_HERE, [outgoingAudioSsrc = _outgoingAudioSsrc, videoPayloadTypes = _videoPayloadTypes, videoExtensionMap = _videoExtensionMap, videoSourceGroups = _videoSourceGroups, completion](GroupNetworkManager *networkManager) {
            GroupJoinPayload payload;
            
            payload.ssrc = outgoingAudioSsrc;
            
            payload.videoPayloadTypes = videoPayloadTypes;
            payload.videoExtensionMap = videoExtensionMap;
            payload.videoSourceGroups = videoSourceGroups;
            
            auto localIceParameters = networkManager->getLocalIceParameters();
            payload.ufrag = localIceParameters.ufrag;
            payload.pwd = localIceParameters.pwd;
            
            completion(payload);
        });
    }
    
    void setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture, std::function<void(GroupJoinPayload)> completion) {
        bool resetBitrate = (_videoCapture == nullptr) != (videoCapture == nullptr);
        if (_videoCapture == videoCapture) {
            return;
        }
        
        _videoCapture = videoCapture;
        
        if (_isConnected && _videoCapture) {
            _outgoingVideoChannel->SetVideoSend(_outgoingVideoSsrcs.simulcastLayers[0].ssrc, NULL, GetVideoCaptureAssumingSameThread(_videoCapture.get())->source());
            _outgoingVideoChannel->OnReadyToSend(true);
            _outgoingVideoChannel->SetSend(true);
            
        } else {
            _outgoingVideoChannel->SetVideoSend(_outgoingVideoSsrcs.simulcastLayers[0].ssrc, NULL, nullptr);
            _outgoingVideoChannel->OnReadyToSend(false);
            _outgoingVideoChannel->SetSend(false);
        }
        
        if (resetBitrate) {
            adjustBitratePreferences(true);
        }
        
        //emitJoinPayload(completion);
    }
    
    void setJoinResponsePayload(GroupJoinResponsePayload payload, std::vector<tgcalls::GroupParticipantDescription> &&participants) {
        RTC_LOG(LS_INFO) << formatTimestampMillis(rtc::TimeMillis()) << ": " << "setJoinResponsePayload";
        
        _networkManager->perform(RTC_FROM_HERE, [payload](GroupNetworkManager *networkManager) {
            PeerIceParameters remoteIceParameters;
            remoteIceParameters.ufrag = payload.ufrag;
            remoteIceParameters.pwd = payload.pwd;
            
            std::vector<cricket::Candidate> iceCandidates;
            for (auto const &candidate : payload.candidates) {
                rtc::SocketAddress address(candidate.ip, stringToInt(candidate.port));
                
                cricket::Candidate parsedCandidate(
                    /*component=*/stringToInt(candidate.component),
                    /*protocol=*/candidate.protocol,
                    /*address=*/address,
                    /*priority=*/stringToUInt32(candidate.priority),
                    /*username=*/payload.ufrag,
                    /*password=*/payload.pwd,
                    /*type=*/candidate.type,
                    /*generation=*/stringToUInt32(candidate.generation),
                    /*foundation=*/candidate.foundation,
                    /*network_id=*/stringToUInt16(candidate.network),
                    /*network_cost=*/0
                );
                iceCandidates.push_back(parsedCandidate);
            }
            
            networkManager->setRemoteParams(remoteIceParameters, iceCandidates);
        });
        
        addParticipants(std::move(participants));
    }
    
    void addParticipants(std::vector<GroupParticipantDescription> &&participants) {
        for (const auto &participant : participants) {
            if (participant.audioSsrc == _outgoingAudioSsrc) {
                continue;
            }
            
            if (_incomingAudioChannels.find(participant.audioSsrc) == _incomingAudioChannels.end()) {
                addIncomingAudioChannel(participant.audioSsrc);
            }
            if (participant.videoPayloadTypes.size() != 0 && participant.videoSourceGroups.size() != 0) {
                if (_incomingVideoChannels.find(participant.audioSsrc) == _incomingVideoChannels.end()) {
                    addIncomingVideoChannel(participant);
                }
            }
        }
    }
    
    void removeSsrcs(std::vector<uint32_t> ssrcs) {
        bool updatedIncomingVideoChannels = false;
        
        for (auto ssrc : ssrcs) {
            auto it = _ssrcMapping.find(ssrc);
            if (it != _ssrcMapping.end()) {
                auto mainSsrc = it->second.ssrc;
                auto audioChannel = _incomingAudioChannels.find(mainSsrc);
                if (audioChannel != _incomingAudioChannels.end()) {
                    _incomingAudioChannels.erase(audioChannel);
                }
                auto videoChannel = _incomingVideoChannels.find(mainSsrc);
                if (videoChannel != _incomingVideoChannels.end()) {
                    _incomingVideoChannels.erase(videoChannel);
                    updatedIncomingVideoChannels = true;
                }
            }
        }
        
        if (updatedIncomingVideoChannels) {
            updateIncomingVideoSources();
        }
    }
    
    void setIsMuted(bool isMuted) {
        if (_isMuted == isMuted) {
            return;
        }
        _isMuted = isMuted;
        
        _outgoingAudioChannel->SetSend(_isConnected && !_isMuted);
        _outgoingAudioChannel->SetAudioSend(_outgoingAudioSsrc, _isConnected && !_isMuted, nullptr, &_audioSource);
    }
    
    void addIncomingVideoOutput(uint32_t ssrc, std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
        auto it = _incomingVideoChannels.find(ssrc);
        if (it != _incomingVideoChannels.end()) {
            it->second->addSink(sink);
        }
    }
    
    void addIncomingAudioChannel(uint32_t ssrc) {
        if (_incomingAudioChannels.find(ssrc) != _incomingAudioChannels.end()) {
            return;
        }
        
        const auto weak = std::weak_ptr<GroupInstanceCustomInternal>(shared_from_this());
        
        std::unique_ptr<IncomingAudioChannel> channel(new IncomingAudioChannel(
            _mediaEngine.get(),
            _call.get(),
            ssrc,
            [weak](std::vector<uint8_t> const &data, rtc::SentPacket sentPacket) {
                rtc::CopyOnWriteBuffer packet(data.data(), data.size());
            
                StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, packet, sentPacket]() {
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->_networkManager->perform(RTC_FROM_HERE, [weak, packet, sentPacket](GroupNetworkManager *networkManager) {
                        networkManager->sendMessage(packet);
                        StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sentPacket]() {
                            auto strong = weak.lock();
                            if (!strong) {
                                return;
                            }
                            
                            strong->_call->OnSentPacket(sentPacket);
                        });
                    });
                });
            },
            [weak, ssrc = ssrc](AudioSinkImpl::Update update) {
                StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ssrc, update]() {
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    GroupLevelValue mappedUpdate;
                    mappedUpdate.level = update.level;
                    mappedUpdate.voice = update.hasSpeech;
                    strong->_audioLevels[ssrc] = mappedUpdate;
                });
            }
        ));
        _incomingAudioChannels.insert(std::make_pair(ssrc, std::move(channel)));
        
        SsrcMappingInfo mapping;
        mapping.ssrc = ssrc;
        mapping.isVideo = false;
        _ssrcMapping.insert(std::make_pair(ssrc, mapping));
        
        if (_isConnected) {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
        } else {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);
        }
        
        maybeDeliverBufferedPackets(ssrc);
    }
    
    void addIncomingVideoChannel(GroupParticipantDescription const &participant) {
        if (_incomingVideoChannels.find(participant.audioSsrc) != _incomingVideoChannels.end()) {
            return;
        }
        
        const auto weak = std::weak_ptr<GroupInstanceCustomInternal>(shared_from_this());
        
        std::unique_ptr<IncomingVideoChannel> channel(new IncomingVideoChannel(
            _mediaEngine.get(),
            _call.get(),
            _availableVideoFormats,
            participant,
            [weak](std::vector<uint8_t> const &data, rtc::SentPacket sentPacket) {
                rtc::CopyOnWriteBuffer packet(data.data(), data.size());
            
                StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, packet, sentPacket]() {
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->_networkManager->perform(RTC_FROM_HERE, [weak, packet, sentPacket](GroupNetworkManager *networkManager) {
                        networkManager->sendMessage(packet);
                        StaticThreads::getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sentPacket]() {
                            auto strong = weak.lock();
                            if (!strong) {
                                return;
                            }
                            
                            strong->_call->OnSentPacket(sentPacket);
                        });
                    });
                });
            }
        ));
        _incomingVideoChannels.insert(std::make_pair(participant.audioSsrc, std::move(channel)));
        
        std::vector<uint32_t> allSsrcs;
        for (const auto &group : participant.videoSourceGroups) {
            for (auto ssrc : group.ssrcs) {
                if (_ssrcMapping.find(ssrc) == _ssrcMapping.end()) {
                    allSsrcs.push_back(ssrc);
                    
                    SsrcMappingInfo mapping;
                    mapping.ssrc = participant.audioSsrc;
                    mapping.isVideo = true;
                    _ssrcMapping.insert(std::make_pair(ssrc, mapping));
                }
            }
        }
        
        if (_isConnected) {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkUp);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkUp);
        } else {
            _call->SignalChannelNetworkState(webrtc::MediaType::AUDIO, webrtc::kNetworkDown);
            _call->SignalChannelNetworkState(webrtc::MediaType::VIDEO, webrtc::kNetworkDown);
        }
        
        updateIncomingVideoSources();
        
        for (auto ssrc : allSsrcs) {
            maybeDeliverBufferedPackets(ssrc);
        }
    }
    
    void updateIncomingVideoSources() {
        if (_incomingVideoSourcesUpdated) {
            std::vector<uint32_t> videoChannelSsrcs;
            for (const auto &it : _incomingVideoChannels) {
                videoChannelSsrcs.push_back(it.first);
            }
            _incomingVideoSourcesUpdated(videoChannelSsrcs);
        }
    }
    
    void setVolume(uint32_t ssrc, double volume) {
        auto it = _incomingAudioChannels.find(ssrc);
        if (it != _incomingAudioChannels.end()) {
            it->second->setVolume(volume);
        }
    }
    
private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> createAudioDeviceModule() {
        const auto check = [&](webrtc::AudioDeviceModule::AudioLayer layer) {
            auto result = webrtc::AudioDeviceModule::Create(
                layer,
                _taskQueueFactory.get());
            return (result && (result->Init() == 0)) ? result : nullptr;
        };
        if (auto result = check(webrtc::AudioDeviceModule::kPlatformDefaultAudio)) {
            return result;
    #ifdef WEBRTC_LINUX
        } else if (auto result = check(webrtc::AudioDeviceModule::kLinuxAlsaAudio)) {
            return result;
    #endif // WEBRTC_LINUX
        }
        return nullptr;
    }

private:
    std::function<void(bool)> _networkStateUpdated;
    std::function<void(GroupLevelsUpdate const &)> _audioLevelsUpdated;
    std::function<void(std::vector<uint32_t> const &)> _incomingVideoSourcesUpdated;
    std::function<void(std::vector<uint32_t> const &)> _participantDescriptionsRequired;
    std::shared_ptr<VideoCaptureInterface> _videoCapture;
    
    int64_t _lastUnknownSsrcsReport = 0;
    std::set<uint32_t> _pendingUnknownSsrcs;
    bool _isUnknownSsrcsScheduled = false;
    std::set<uint32_t> _reportedUnknownSsrcs;
    
    std::unique_ptr<ThreadLocalObject<GroupNetworkManager>> _networkManager;
    
    std::unique_ptr<webrtc::RtcEventLogNull> _eventLog;
    std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
    std::unique_ptr<cricket::MediaEngineInterface> _mediaEngine;
    std::unique_ptr<webrtc::Call> _call;
    webrtc::FieldTrialBasedConfig _fieldTrials;
    webrtc::LocalAudioSinkAdapter _audioSource;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;
    
    std::unique_ptr<cricket::VoiceMediaChannel> _outgoingAudioChannel;
    uint32_t _outgoingAudioSsrc = 0;
    std::unique_ptr<NetworkInterfaceImpl> _outgoingAudioInterface;
    
    std::vector<webrtc::SdpVideoFormat> _availableVideoFormats;
    
    std::vector<GroupJoinPayloadVideoPayloadType> _videoPayloadTypes;
    std::vector<std::pair<uint32_t, std::string>> _videoExtensionMap;
    std::vector<GroupJoinPayloadVideoSourceGroup> _videoSourceGroups;
    
    std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> _videoBitrateAllocatorFactory;
    std::unique_ptr<cricket::VideoMediaChannel> _outgoingVideoChannel;
    VideoSsrcs _outgoingVideoSsrcs;
    std::unique_ptr<NetworkInterfaceImpl> _outgoingVideoInterface;
    
    std::map<uint32_t, GroupLevelValue> _audioLevels;
    GroupLevelValue _myAudioLevel;
    
    bool _isMuted = true;
    
    MissingSsrcPacketBuffer _missingPacketBuffer;
    std::map<uint32_t, SsrcMappingInfo> _ssrcMapping;
    std::map<uint32_t, std::unique_ptr<IncomingAudioChannel>> _incomingAudioChannels;
    std::map<uint32_t, std::unique_ptr<IncomingVideoChannel>> _incomingVideoChannels;
    
    bool _isConnected = false;
};

GroupInstanceCustomImpl::GroupInstanceCustomImpl(GroupInstanceDescriptor &&descriptor) :
    _logSink(std::make_unique<LogSinkImpl>(descriptor.config.logPath)) {
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    if (_logSink) {
        rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
    }
    
    _internal.reset(new ThreadLocalObject<GroupInstanceCustomInternal>(StaticThreads::getMediaThread(), [descriptor = std::move(descriptor)]() mutable {
        return new GroupInstanceCustomInternal(std::move(descriptor));
    }));
    _internal->perform(RTC_FROM_HERE, [](GroupInstanceCustomInternal *internal) {
        internal->start();
    });
}

GroupInstanceCustomImpl::~GroupInstanceCustomImpl() {
    if (_logSink) {
        rtc::LogMessage::RemoveLogToStream(_logSink.get());
    }
    _internal.reset();

    // Wait until _internal is destroyed
    StaticThreads::getMediaThread()->Invoke<void>(RTC_FROM_HERE, [] {});
}

void GroupInstanceCustomImpl::stop() {
    _internal->perform(RTC_FROM_HERE, [](GroupInstanceCustomInternal *internal) {
        internal->stop();
    });
}

void GroupInstanceCustomImpl::emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
    _internal->perform(RTC_FROM_HERE, [completion](GroupInstanceCustomInternal *internal) {
        internal->emitJoinPayload(completion);
    });
}

void GroupInstanceCustomImpl::setJoinResponsePayload(GroupJoinResponsePayload payload, std::vector<tgcalls::GroupParticipantDescription> &&participants) {
    _internal->perform(RTC_FROM_HERE, [payload, participants = std::move(participants)](GroupInstanceCustomInternal *internal) mutable {
        internal->setJoinResponsePayload(payload, std::move(participants));
    });
}

void GroupInstanceCustomImpl::addParticipants(std::vector<GroupParticipantDescription> &&participants) {
    _internal->perform(RTC_FROM_HERE, [participants = std::move(participants)](GroupInstanceCustomInternal *internal) mutable {
        internal->addParticipants(std::move(participants));
    });
}

void GroupInstanceCustomImpl::removeSsrcs(std::vector<uint32_t> ssrcs) {
    _internal->perform(RTC_FROM_HERE, [ssrcs = std::move(ssrcs)](GroupInstanceCustomInternal *internal) mutable {
        internal->removeSsrcs(ssrcs);
    });
}

void GroupInstanceCustomImpl::setIsMuted(bool isMuted) {
    _internal->perform(RTC_FROM_HERE, [isMuted](GroupInstanceCustomInternal *internal) {
        internal->setIsMuted(isMuted);
    });
}

void GroupInstanceCustomImpl::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture, std::function<void(GroupJoinPayload)> completion) {
    _internal->perform(RTC_FROM_HERE, [videoCapture, completion](GroupInstanceCustomInternal *internal) {
        internal->setVideoCapture(videoCapture, completion);
    });
}

void GroupInstanceCustomImpl::setAudioOutputDevice(std::string id) {
    
}

void GroupInstanceCustomImpl::setAudioInputDevice(std::string id) {
    
}

void GroupInstanceCustomImpl::addIncomingVideoOutput(uint32_t ssrc, std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _internal->perform(RTC_FROM_HERE, [ssrc, sink](GroupInstanceCustomInternal *internal) mutable {
        internal->addIncomingVideoOutput(ssrc, sink);
    });
}

void GroupInstanceCustomImpl::setVolume(uint32_t ssrc, double volume) {
    _internal->perform(RTC_FROM_HERE, [ssrc, volume](GroupInstanceCustomInternal *internal) {
        internal->setVolume(ssrc, volume);
    });
}

void GroupInstanceCustomImpl::setFullSizeVideoSsrc(uint32_t ssrc) {
    
}

} // namespace tgcalls
