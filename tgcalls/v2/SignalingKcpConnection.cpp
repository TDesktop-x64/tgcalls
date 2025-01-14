#include "v2/SignalingKcpConnection.h"

#include <random>
#include <sys/time.h>

#include "rtc_base/async_tcp_socket.h"
#include "p2p/base/basic_packet_socket_factory.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "p2p/base/packet_transport_internal.h"

#include "FieldTrialsConfig.h"

/* get system time */
static inline void itimeofday(long *sec, long *usec) {
    struct timeval time;
    gettimeofday(&time, NULL);
    if (sec) *sec = time.tv_sec;
    if (usec) *usec = time.tv_usec;
}

/* get clock in millisecond 64 */
static inline IINT64 iclock64(void) {
    long s, u;
    IINT64 value;
    itimeofday(&s, &u);
    value = ((IINT64)s) * 1000 + (u / 1000);
    return value;
}

static inline IUINT32 iclock() {
    return (IUINT32)(iclock64() & 0xfffffffful);
}

namespace tgcalls {

SignalingKcpConnection::SignalingKcpConnection(std::shared_ptr<Threads> threads, std::function<void(const std::vector<uint8_t> &)> onIncomingData, std::function<void(const std::vector<uint8_t> &)> emitData) :
_threads(threads),
_emitData(emitData),
_onIncomingData(onIncomingData) {
    _receiveBuffer.resize(512 * 1024);
    
    _kcp = ikcp_create(0, this);
    _kcp->output = &SignalingKcpConnection::udpOutput;
    
    ikcp_wndsize(_kcp, 128, 128);
    ikcp_nodelay(_kcp, 0, 10, 0, 0);
    
    //_onIncomingData
}

int SignalingKcpConnection::udpOutput(const char *buf, int len, ikcpcb *kcp, void *user) {
    SignalingKcpConnection *connection = (SignalingKcpConnection *)user;
    if (connection) {
        connection->_emitData(std::vector<uint8_t>(buf, buf + len));
    }
    return 0;
}

SignalingKcpConnection::~SignalingKcpConnection() {
    
}

void SignalingKcpConnection::start() {
    scheduleInternalUpdate(0);
}

void SignalingKcpConnection::scheduleInternalUpdate(int timeoutMs) {
    const auto weak = std::weak_ptr<SignalingKcpConnection>(shared_from_this());
    _threads->getMediaThread()->PostDelayedTask([weak]() {
        auto strong = weak.lock();
        if (!strong) {
            return;
        }
        
        strong->performInternalUpdate();
        
        strong->scheduleInternalUpdate(10);
    }, webrtc::TimeDelta::Millis(timeoutMs));
}

void SignalingKcpConnection::performInternalUpdate() {
    ikcp_update(_kcp, iclock());
    
    while (true) {
        int result = ikcp_recv(_kcp, (char *)_receiveBuffer.data(), (int)_receiveBuffer.size());
        if (result >= 0) {
            std::vector<uint8_t> packet;
            packet.resize(result);
            std::copy(_receiveBuffer.begin(), _receiveBuffer.begin() + result, packet.begin());
            _onIncomingData(packet);
        } else {
            break;
        }
    }
}

void SignalingKcpConnection::receiveExternal(const std::vector<uint8_t> &data) {
    ikcp_input(_kcp, (const char *)data.data(), (long)data.size());
}

void SignalingKcpConnection::send(const std::vector<uint8_t> &data) {
    ikcp_send(_kcp, (const char *)data.data(), (int)data.size());
}

}
