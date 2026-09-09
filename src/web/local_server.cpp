#include "web/local_server.h"

#include "web/api.h"

#include <WS2tcpip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cctype>
#include <sstream>
#include <string_view>

namespace vanityforge {
namespace {

constexpr std::size_t maximum_header_size = 16 * 1024;
constexpr std::size_t maximum_body_size = 16 * 1024;
constexpr std::string_view expected_host = "127.0.0.1:8787";
constexpr std::string_view expected_origin = "http://127.0.0.1:8787";

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string json_error_body(const std::string_view error) {
  std::string body = "{\"error\":\"";
  for (const char character : error) {
    if (character == '"' || character == '\\') body.push_back('\\');
    if (character == '\r' || character == '\n') body.push_back(' ');
    else body.push_back(character);
  }
  body += "\"}";
  return body;
}

}  // namespace

struct LocalServer::Request final {
  std::string method;
  std::string target;
  std::string host;
  std::string origin;
  std::string content_type;
  std::string body;
};

LocalServer::LocalServer(TaskManager& task_manager, std::string dashboard_html,
                         std::string horizontal_logo_svg, std::string icon_svg)
    : task_manager_(task_manager),
      dashboard_html_(std::move(dashboard_html)),
      horizontal_logo_svg_(std::move(horizontal_logo_svg)),
      icon_svg_(std::move(icon_svg)) {}

LocalServer::~LocalServer() { stop(); }

bool LocalServer::start(std::string& error) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    error = "WSAStartup failed";
    return false;
  }
  winsock_started_ = true;
  listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_socket_ == INVALID_SOCKET) {
    error = "Unable to create local server socket";
    stop();
    return false;
  }
  BOOL exclusive = TRUE;
  if (setsockopt(listen_socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
    error = "Unable to reserve local server port exclusively";
    stop();
    return false;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(8787);
  if (InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr) != 1 ||
      bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
    error = "Unable to bind 127.0.0.1:8787";
    stop();
    return false;
  }
  if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
    error = "Unable to listen on 127.0.0.1:8787";
    stop();
    return false;
  }
  stopping_.store(false, std::memory_order_release);
  accept_thread_ = std::thread(&LocalServer::accept_loop, this);
  return true;
}

void LocalServer::stop() {
  stopping_.store(true, std::memory_order_release);
  if (listen_socket_ != INVALID_SOCKET) {
    shutdown(listen_socket_, SD_BOTH);
    closesocket(listen_socket_);
    listen_socket_ = INVALID_SOCKET;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard lock(clients_mutex_);
    for (auto& client_thread : client_threads_) {
      if (client_thread.joinable()) client_thread.join();
    }
    client_threads_.clear();
    client_completed_.clear();
  }
  if (winsock_started_) {
    WSACleanup();
    winsock_started_ = false;
  }
}

void LocalServer::accept_loop() {
  while (!stopping_.load(std::memory_order_acquire)) {
    sockaddr_in peer{};
    int peer_size = sizeof(peer);
    const SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (client == INVALID_SOCKET) {
      if (stopping_.load(std::memory_order_acquire)) break;
      continue;
    }
    if (peer.sin_family != AF_INET || ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
      closesocket(client);
      continue;
    }
    std::lock_guard lock(clients_mutex_);
    for (std::size_t index = client_threads_.size(); index > 0; --index) {
      const std::size_t worker = index - 1;
      if (!client_completed_[worker]->load(std::memory_order_acquire)) continue;
      if (client_threads_[worker].joinable()) client_threads_[worker].join();
      client_threads_.erase(client_threads_.begin() + static_cast<std::ptrdiff_t>(worker));
      client_completed_.erase(client_completed_.begin() + static_cast<std::ptrdiff_t>(worker));
    }
    auto completed = std::make_shared<std::atomic<bool>>(false);
    client_completed_.push_back(completed);
    try {
      client_threads_.emplace_back([this, client, completed]() {
        handle_client(client);
        completed->store(true, std::memory_order_release);
      });
    } catch (...) {
      client_completed_.pop_back();
      closesocket(client);
    }
  }
}

void LocalServer::handle_client(const SOCKET client) {
  const DWORD timeout = 5000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  Request request;
  if (read_request(client, request)) route_request(client, request);
  shutdown(client, SD_BOTH);
  closesocket(client);
}

bool LocalServer::read_request(const SOCKET client, Request& request) {
  std::string data;
  data.reserve(2048);
  std::array<char, 2048> buffer{};
  std::size_t header_end = std::string::npos;
  while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
    const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) return false;
    data.append(buffer.data(), static_cast<std::size_t>(received));
    if (data.size() > maximum_header_size) return false;
  }

  const std::string_view header(data.data(), header_end);
  const std::size_t first_line_end = header.find("\r\n");
  if (first_line_end == std::string_view::npos) return false;
  const std::string_view first_line = header.substr(0, first_line_end);
  const std::size_t first_space = first_line.find(' ');
  const std::size_t second_space = first_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      first_line.substr(second_space + 1) != "HTTP/1.1") return false;
  request.method = std::string(first_line.substr(0, first_space));
  request.target = std::string(first_line.substr(first_space + 1, second_space - first_space - 1));

  std::size_t content_length = 0;
  std::size_t position = first_line_end + 2;
  while (position < header.size()) {
    const std::size_t end = header.find("\r\n", position);
    const std::string_view line = header.substr(position, end == std::string_view::npos ? header.size() - position : end - position);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return false;
    std::string name = lower_ascii(std::string(line.substr(0, colon)));
    std::string value(line.substr(colon + 1));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    if (name == "host") request.host = value;
    else if (name == "origin") request.origin = value;
    else if (name == "content-type") request.content_type = lower_ascii(value);
    else if (name == "content-length") {
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), content_length);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
    } else if (name == "transfer-encoding") {
      return false;
    }
    if (end == std::string_view::npos) break;
    position = end + 2;
  }
  if (content_length > maximum_body_size) return false;
  request.body.assign(data.data() + header_end + 4, data.size() - header_end - 4);
  while (request.body.size() < content_length) {
    const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (received <= 0) return false;
    request.body.append(buffer.data(), static_cast<std::size_t>(received));
    if (request.body.size() > content_length) return false;
  }
  return request.body.size() == content_length;
}

void LocalServer::route_request(const SOCKET client, const Request& request) {
  if (request.host != expected_host) {
    send_response(client, 403, "Forbidden", "application/json", "{\"error\":\"Invalid Host\"}");
    return;
  }
  if (request.method == "GET" && (request.target == "/" || request.target == "/index.html")) {
    send_response(client, 200, "OK", "text/html; charset=utf-8", dashboard_html_);
    return;
  }
  if (request.method == "GET" &&
      request.target == "/assets/branding/vanylith-horizontal.svg") {
    send_response(client, 200, "OK", "image/svg+xml; charset=utf-8", horizontal_logo_svg_);
    return;
  }
  if (request.method == "GET" && request.target == "/assets/branding/vanylith-icon.svg") {
    send_response(client, 200, "OK", "image/svg+xml; charset=utf-8", icon_svg_);
    return;
  }
  if (request.method == "GET" && request.target == "/favicon.ico") {
    send_response(client, 204, "No Content", "image/x-icon", "");
    return;
  }
  if (request.method == "GET" && request.target == "/api/events") {
    handle_sse(client);
    return;
  }
  if (request.method == "GET" && (request.target == "/api/status" ||
      request.target == "/api/devices" || request.target == "/api/task" ||
      request.target == "/api/results")) {
    send_response(client, 200, "OK", "application/json", snapshot_to_json(task_manager_.snapshot()));
    return;
  }
  if (request.method != "POST") {
    send_response(client, 404, "Not Found", "application/json", "{\"error\":\"Not found\"}");
    return;
  }
  if (request.origin != expected_origin || request.content_type.find("application/json") != 0) {
    send_response(client, 403, "Forbidden", "application/json", "{\"error\":\"Invalid request origin or type\"}");
    return;
  }

  std::string error;
  bool success = false;
  if (request.target == "/api/task/start") {
    SearchTask task;
    success = parse_search_task_json(request.body, task, error) && task_manager_.start(task, error);
  } else if (request.target == "/api/task/pause") {
    success = task_manager_.pause(error);
  } else if (request.target == "/api/task/resume") {
    success = task_manager_.resume(error);
  } else if (request.target == "/api/task/stop") {
    success = task_manager_.stop(error);
  } else if (request.target == "/api/task/reset") {
    success = task_manager_.reset(error);
  } else if (request.target == "/api/devices/config") {
    std::string id;
    bool enabled = false;
    success = parse_device_config_json(request.body, id, enabled, error) &&
              task_manager_.set_device_enabled(id, enabled, error);
  } else {
    send_response(client, 404, "Not Found", "application/json", "{\"error\":\"Not found\"}");
    return;
  }
  if (!success) {
    send_response(client, 400, "Bad Request", "application/json", json_error_body(error));
    return;
  }
  send_response(client, 200, "OK", "application/json", snapshot_to_json(task_manager_.snapshot()));
}

void LocalServer::handle_sse(const SOCKET client) {
  const std::string headers =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
      "Connection: keep-alive\r\nX-Content-Type-Options: nosniff\r\n\r\n";
  if (!send_all(client, headers)) return;
  while (!stopping_.load(std::memory_order_acquire)) {
    const std::string event = "data: " + snapshot_to_json(task_manager_.snapshot()) + "\n\n";
    if (!send_all(client, event)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
  }
}

bool LocalServer::send_all(const SOCKET socket, const std::string& data) {
  std::size_t sent_total = 0;
  while (sent_total < data.size()) {
    const int sent = send(socket, data.data() + sent_total,
                          static_cast<int>(std::min<std::size_t>(data.size() - sent_total, INT_MAX)), 0);
    if (sent <= 0) return false;
    sent_total += static_cast<std::size_t>(sent);
  }
  return true;
}

void LocalServer::send_response(const SOCKET socket, const int status, const std::string_view reason,
                                const std::string_view content_type, const std::string_view body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\nContent-Type: " << content_type
           << "\r\nContent-Length: " << body.size()
           << "\r\nConnection: close\r\nCache-Control: no-store\r\n"
           << "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; "
              "style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
              "object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n"
           << "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
           << "Referrer-Policy: no-referrer\r\n\r\n" << body;
  send_all(socket, response.str());
}

}  // namespace vanityforge
