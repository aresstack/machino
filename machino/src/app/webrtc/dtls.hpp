// DTLS for WebRTC (mbedTLS): self-signed certificate + SHA-256 fingerprint
// for the SDP, a passive (server-role) handshake with use_srtp, and the
// RFC 5764 key exporter that hands the SRTP master keys over. This is the
// ONLY place third-party crypto is used; SRTP itself is Machino's own
// vector-tested code. The transport is socket-free: the owner feeds incoming
// DTLS datagrams in and ships the queued outgoing ones - which is what makes
// the whole handshake host-testable against an in-memory mbedTLS client.
#pragma once
#include "app/webrtc/srtp.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace machino { namespace webrtc {

class DtlsTransport {
public:
    DtlsTransport();
    ~DtlsTransport();
    DtlsTransport(const DtlsTransport&) = delete;
    DtlsTransport& operator=(const DtlsTransport&) = delete;

    // False when certificate or context setup failed; the session must not
    // be offered to a browser then.
    bool ok() const;

    // "AA:BB:..." SHA-256 of the certificate DER, for a=fingerprint.
    std::string fingerprint() const;

    // One incoming DTLS datagram from the demux.
    void feed(const uint8_t* p, size_t n);

    // Advances the handshake; false = fatal alert (tear the session down).
    bool step();
    bool handshake_done() const;

    // Outgoing datagrams produced by feed()/step(); send each as one UDP packet.
    bool take_out(std::vector<uint8_t>& dgram);

    // After the handshake: SRTP keys from the RFC 5764 exporter. As the DTLS
    // server the camera sends with the server key and reads the browser's
    // RTCP with the client key. False when use_srtp was not negotiated.
    bool export_srtp(SrtpKey& our_send, SrtpKey& their_send);

private:
    struct Impl;
    std::unique_ptr<Impl> im_;
};

}} // namespace machino::webrtc
