#ifndef TGCALLS_STREAMING_MEDIA_CONTEXT_H
#define TGCALLS_STREAMING_MEDIA_CONTEXT_H

#include "GroupInstanceImpl.h"
#include <stdint.h>
#include "../StaticThreads.h"

namespace tgcalls {

class StreamingMediaContextPrivate;

class StreamingMediaContext {
public:
    struct VideoChannel {
        VideoChannelDescription::Quality quality = VideoChannelDescription::Quality::Thumbnail;
        std::string endpoint;

        VideoChannel(VideoChannelDescription::Quality quality_, std::string endpoint_) :
        quality(quality_),
        endpoint(endpoint_) {
        }
    };

public:
    struct StreamingMediaContextArguments {
        std::shared_ptr<Threads> threads;
        std::function<std::shared_ptr<BroadcastPartTask>(int64_t, int64_t, std::function<void(BroadcastPart &&)>)> requestAudioBroadcastPart;
        std::function<std::shared_ptr<BroadcastPartTask>(int64_t, int64_t, int32_t, VideoChannelDescription::Quality, std::function<void(BroadcastPart &&)>)> requestVideoBroadcastPart;
        std::function<void(std::string const &, webrtc::VideoFrame const &)> displayVideoFrame;
        std::function<void(uint32_t, float, bool)> updateAudioLevel;
    };

public:
    StreamingMediaContext(StreamingMediaContextArguments &&arguments);
    ~StreamingMediaContext();
    
    StreamingMediaContext& operator=(const StreamingMediaContext&) = delete;
    StreamingMediaContext& operator=(StreamingMediaContext&&) = delete;

    void setActiveVideoChannels(std::vector<VideoChannel> const &videoChannels);

    void getAudio(int16_t *audio_samples, const size_t num_samples, const uint32_t samples_per_sec);
    
private:
    std::shared_ptr<StreamingMediaContextPrivate> _private;
};

}

#endif
