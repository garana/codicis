/**
 * @file ws_handshake.cc
 * @brief Implementation of the WebSocket handshake helpers.
 */

#include "codicis/net/ws_handshake.h"

#include <cctype>

#include "codicis/util/base64.h"
#include "codicis/util/sha1.h"

namespace codicis {
namespace {

/** @brief The RFC 6455 GUID appended to the client key. */
constexpr char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** @brief ASCII case-insensitive substring search. */
bool IContains(const std::string& haystack, const char* needle) {
  std::string h;
  h.reserve(haystack.size());
  for (char c : haystack) {
    h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return h.find(needle) != std::string::npos;
}

/** @brief ASCII case-insensitive equality. */
bool IEquals(const std::string& a, const char* b) {
  std::size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return i == a.size() && b[i] == '\0';
}

}  // namespace

std::string WsComputeAcceptKey(const std::string& client_key) {
  const Sha1Digest digest = Sha1(client_key + kWsGuid);
  return Base64Encode(digest.data(), digest.size());
}

bool WsIsUpgradeRequest(const HttpRequest& req, std::string* key_out) {
  const std::string* upgrade = req.header("Upgrade");
  const std::string* connection = req.header("Connection");
  const std::string* key = req.header("Sec-WebSocket-Key");
  const std::string* version = req.header("Sec-WebSocket-Version");
  if (upgrade == nullptr || connection == nullptr || key == nullptr ||
      version == nullptr) {
    return false;
  }
  if (!IEquals(*upgrade, "websocket")) {
    return false;
  }
  if (!IContains(*connection, "upgrade")) {
    return false;
  }
  if (*version != "13") {
    return false;
  }
  if (key->empty()) {
    return false;
  }
  *key_out = *key;
  return true;
}

std::string WsBuildHandshakeResponse(const std::string& client_key) {
  const std::string accept = WsComputeAcceptKey(client_key);
  std::string out;
  out += "HTTP/1.1 101 Switching Protocols\r\n";
  out += "Upgrade: websocket\r\n";
  out += "Connection: Upgrade\r\n";
  out += "Sec-WebSocket-Accept: ";
  out += accept;
  out += "\r\n\r\n";
  return out;
}

}  // namespace codicis
