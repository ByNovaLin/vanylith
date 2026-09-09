#pragma once

#include "controller/task_manager.h"

#include <WinSock2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vanityforge {

class LocalServer final {
 public:
  LocalServer(TaskManager& task_manager, std::string dashboard_html,
              std::string horizontal_logo_svg, std::string icon_svg);
  ~LocalServer();
  LocalServer(const LocalServer&) = delete;
  LocalServer& operator=(const LocalServer&) = delete;

  bool start(std::string& error);
  void stop();

 private:
  struct Request;
  void accept_loop();
  void handle_client(SOCKET client);
  bool read_request(SOCKET client, Request& request);
  void handle_sse(SOCKET client);
  void route_request(SOCKET client, const Request& request);
  static bool send_all(SOCKET socket, const std::string& data);
  static void send_response(SOCKET socket, int status, std::string_view reason,
                            std::string_view content_type, std::string_view body);

  TaskManager& task_manager_;
  std::string dashboard_html_;
  std::string horizontal_logo_svg_;
  std::string icon_svg_;
  SOCKET listen_socket_ = INVALID_SOCKET;
  std::atomic<bool> stopping_{false};
  std::thread accept_thread_;
  std::mutex clients_mutex_;
  std::vector<std::thread> client_threads_;
  std::vector<std::shared_ptr<std::atomic<bool>>> client_completed_;
  bool winsock_started_ = false;
};

}  // namespace vanityforge
