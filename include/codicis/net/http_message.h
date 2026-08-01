#ifndef CODICIS_NET_HTTP_MESSAGE_H
#define CODICIS_NET_HTTP_MESSAGE_H

/**
 * @file http_message.h
 * @brief HTTP/1.1 request and response value types.
 *
 * These are plain data types shared by the hand-rolled parser, router, and
 * connection. Header lookups are case-insensitive per RFC 7230.
 */

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codicis {

/** @brief A single HTTP header field (name preserved as received). */
struct HttpHeader {
  std::string name;
  std::string value;
};

/**
 * @brief A parsed HTTP request.
 */
struct HttpRequest {
  std::string method;   /**< e.g. "GET", "POST". */
  std::string target;   /**< Raw request target, e.g. "/x?y=1". */
  std::string path;     /**< Path portion of the target, e.g. "/x". */
  std::string query;    /**< Query portion (after '?'), empty if none. */
  int http_minor = 1;   /**< Minor version of HTTP/1.x. */
  std::vector<HttpHeader> headers;
  std::string body;
  bool keep_alive = true;  /**< Resolved from version + Connection header. */

  /**
   * @brief Case-insensitive header lookup.
   * @param name The header name.
   * @return Pointer to the value, or nullptr if absent.
   */
  const std::string* header(std::string_view name) const;
};

/**
 * @brief An HTTP response to be serialized onto the wire.
 */
struct HttpResponse {
  int status = 200;
  std::string reason = "OK";
  std::vector<HttpHeader> headers;
  std::string body;

  /**
   * @brief Set the status code and its standard reason phrase.
   * @param code The HTTP status code.
   */
  void set_status(int code);

  /**
   * @brief Append or replace a header.
   * @param name  Header name.
   * @param value Header value.
   */
  void set_header(std::string name, std::string value);

  /**
   * @brief Serialize the full response (status line, headers, body).
   *
   * A Content-Length header is added automatically, and a Connection header
   * reflecting @p keep_alive, unless already present.
   * @param keep_alive Whether the connection should be kept alive.
   * @return The complete response bytes.
   */
  std::string serialize(bool keep_alive) const;
};

/**
 * @brief The standard reason phrase for a status code.
 * @param code The HTTP status code.
 * @return A static reason phrase ("OK", "Not Found", ...).
 */
std::string_view HttpReasonPhrase(int code);

}  // namespace codicis

#endif  // CODICIS_NET_HTTP_MESSAGE_H
