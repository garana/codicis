/**
 * @file http_message.cc
 * @brief Implementation of HTTP message types (see http_message.h).
 */

#include "codicis/net/http_message.h"

#include <cctype>
#include <string>

namespace codicis {
namespace {

/**
 * @brief ASCII case-insensitive equality of two views.
 * @param a First view.
 * @param b Second view.
 * @return True if equal ignoring ASCII case.
 */
bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

const std::string* HttpRequest::header(std::string_view name) const {
  for (const HttpHeader& h : headers) {
    if (IEquals(h.name, name)) {
      return &h.value;
    }
  }
  return nullptr;
}

void HttpResponse::set_status(int code) {
  status = code;
  reason = std::string(HttpReasonPhrase(code));
}

void HttpResponse::set_header(std::string name, std::string value) {
  for (HttpHeader& h : headers) {
    if (IEquals(h.name, name)) {
      h.value = std::move(value);
      return;
    }
  }
  headers.push_back(HttpHeader{std::move(name), std::move(value)});
}

std::string HttpResponse::serialize(bool keep_alive) const {
  std::string out;
  out.reserve(64 + body.size());
  out += "HTTP/1.1 ";
  out += std::to_string(status);
  out += ' ';
  out += reason;
  out += "\r\n";

  bool have_content_length = false;
  bool have_connection = false;
  for (const HttpHeader& h : headers) {
    if (IEquals(h.name, "Content-Length")) {
      have_content_length = true;
    } else if (IEquals(h.name, "Connection")) {
      have_connection = true;
    }
    out += h.name;
    out += ": ";
    out += h.value;
    out += "\r\n";
  }

  if (!have_content_length) {
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
  }
  if (!have_connection) {
    out += "Connection: ";
    out += keep_alive ? "keep-alive" : "close";
    out += "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

std::string_view HttpReasonPhrase(int code) {
  switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
  }
}

}  // namespace codicis
