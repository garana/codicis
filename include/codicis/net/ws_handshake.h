#ifndef CODICIS_NET_WS_HANDSHAKE_H
#define CODICIS_NET_WS_HANDSHAKE_H

/**
 * @file ws_handshake.h
 * @brief WebSocket opening-handshake helpers (RFC 6455 section 4).
 */

#include <string>

#include "codicis/net/http_message.h"

namespace codicis {

/**
 * @brief Compute the Sec-WebSocket-Accept value for a client key.
 *
 * accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
 * @param client_key The client's Sec-WebSocket-Key header value.
 * @return The accept token.
 */
std::string WsComputeAcceptKey(const std::string& client_key);

/**
 * @brief Check whether @p req is a valid WebSocket upgrade request.
 * @param req      The parsed HTTP request.
 * @param key_out  On success, receives the Sec-WebSocket-Key value.
 * @return True if the request is a well-formed version-13 upgrade.
 */
bool WsIsUpgradeRequest(const HttpRequest& req, std::string* key_out);

/**
 * @brief Build the 101 Switching Protocols response for @p client_key.
 * @param client_key The client's Sec-WebSocket-Key value.
 * @return The full response bytes to send.
 */
std::string WsBuildHandshakeResponse(const std::string& client_key);

}  // namespace codicis

#endif  // CODICIS_NET_WS_HANDSHAKE_H
