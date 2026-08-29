#include "algorithm-trainer/isolated-process-executor.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <json/json.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace algorithm_trainer {
namespace {

constexpr std::size_t maximum_message_size{2 * 1024 * 1024};

struct DelegationState {
  std::filesystem::path mount;
  std::optional<std::string> error;
};

DelegationState prepare_cgroup_delegation() {
  std::ifstream membership{"/proc/self/cgroup"};
  std::string relative;
  for (std::string line; std::getline(membership, line);) {
    if (line.starts_with("0::/") && line.find("..") == std::string::npos) {
      relative = line.substr(4);
      break;
    }
  }
  if (relative.empty())
    return {.error = "The judge requires a delegated cgroup v2 hierarchy"};
  const auto root = std::filesystem::path{"/sys/fs/cgroup"} / relative;
  if (::access(root.c_str(), W_OK) != 0)
    return {.error = "The judge service requires Delegate=cpu memory pids"};

  const auto manager = root / ("backend-" + std::to_string(::getpid()));
  std::error_code filesystem_error;
  std::filesystem::create_directory(manager, filesystem_error);
  if (filesystem_error)
    return {.error = "Could not create the trusted backend cgroup"};
  {
    std::ofstream processes{manager / "cgroup.procs"};
    processes << ::getpid();
    if (!processes)
      return {.error = "Could not separate the backend from judge cgroups"};
  }
  {
    std::ofstream controllers{root / "cgroup.subtree_control"};
    controllers << "+cpu +memory +pids";
    if (!controllers)
      return {.error = "The delegated cgroup lacks cpu, memory, or pids control"};
  }
  return {.mount = root};
}

const DelegationState &delegation() {
  static const DelegationState state = prepare_cgroup_delegation();
  return state;
}

bool write_all(int descriptor, std::string_view data) {
  while (!data.empty()) {
    const auto count = ::write(descriptor, data.data(), data.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}

bool read_all(int descriptor, void *destination, std::size_t size) {
  auto *output = static_cast<char *>(destination);
  while (size > 0) {
    const auto count = ::read(descriptor, output, size);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    output += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool send_message(int descriptor, const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const auto body = Json::writeString(builder, value);
  if (body.size() > maximum_message_size)
    return false;
  const auto encoded_size = htonl(static_cast<std::uint32_t>(body.size()));
  return write_all(descriptor,
                   {reinterpret_cast<const char *>(&encoded_size), sizeof(encoded_size)}) &&
         write_all(descriptor, body);
}

std::variant<Json::Value, std::string> receive_message(int descriptor) {
  std::uint32_t encoded_size{};
  if (!read_all(descriptor, &encoded_size, sizeof(encoded_size)))
    return std::string{"Judge worker closed its response channel"};
  const auto size = ntohl(encoded_size);
  if (size == 0 || size > maximum_message_size)
    return std::string{"Judge worker returned an invalid response size"};
  std::string body(size, '\0');
  if (!read_all(descriptor, body.data(), body.size()))
    return std::string{"Judge worker returned a truncated response"};
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  const auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
  if (!reader->parse(body.data(), body.data() + body.size(), &value, &errors) || !value.isObject())
    return std::string{"Judge worker returned malformed JSON"};
  return value;
}

void close_untrusted_descriptors(int preserved, long maximum) {
  for (int descriptor = 3; descriptor < maximum; ++descriptor) {
    if (descriptor != preserved)
      ::close(descriptor);
  }
}

} // namespace

IsolatedProcessExecutor::IsolatedProcessExecutor(std::filesystem::path worker_path)
    : worker_path_{std::move(worker_path)}, cgroupv2_mount_{delegation().mount},
      delegation_error_{delegation().error} {}

const std::optional<std::string> &IsolatedProcessExecutor::initialization_error() const {
  return delegation_error_;
}

ExecutorResult IsolatedProcessExecutor::run(const ExecutionRequest &request) {
  if (delegation_error_)
    return ExecutorError{*delegation_error_};
  std::array<int, 2> sockets{};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) != 0)
    return ExecutorError{"Could not create the judge worker channel"};

  const auto descriptor = std::to_string(sockets[1]);
  const auto cgroup_mount = cgroupv2_mount_.string();
  std::array<char *, 6> arguments{
      const_cast<char *>(worker_path_.c_str()), const_cast<char *>("--ipc-fd"),
      const_cast<char *>(descriptor.c_str()),   const_cast<char *>("--cgroupv2-mount"),
      const_cast<char *>(cgroup_mount.c_str()), nullptr};
  std::array<char *, 1> environment{nullptr};
  const auto configured_maximum = ::sysconf(_SC_OPEN_MAX);
  const auto maximum_descriptor = configured_maximum > 3 ? configured_maximum : 65536;

  const auto child = ::fork();
  if (child < 0) {
    ::close(sockets[0]);
    ::close(sockets[1]);
    return ExecutorError{"Could not launch the judge worker"};
  }
  if (child == 0) {
    ::close(sockets[0]);
    const auto flags = ::fcntl(sockets[1], F_GETFD);
    if (flags < 0 || ::fcntl(sockets[1], F_SETFD, flags & ~FD_CLOEXEC) < 0)
      _exit(126);
    close_untrusted_descriptors(sockets[1], maximum_descriptor);
    ::execve(worker_path_.c_str(), arguments.data(), environment.data());
    _exit(127);
  }

  ::close(sockets[1]);
  Json::Value payload;
  payload["language"] = request.language;
  payload["source"] = request.source_code;
  payload["input"] = request.standard_input;
  if (!send_message(sockets[0], payload)) {
    ::close(sockets[0]);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    return ExecutorError{"Could not send work to the judge worker"};
  }
  auto response = receive_message(sockets[0]);
  ::close(sockets[0]);
  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  if (const auto *error = std::get_if<std::string>(&response))
    return ExecutorError{*error};
  const auto &json = std::get<Json::Value>(response);
  if (json["error"].isString())
    return ExecutorError{json["error"].asString()};
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !json["exitCode"].isInt() ||
      !json["stdout"].isString() || !json["stderr"].isString() || !json["timedOut"].isBool())
    return ExecutorError{"Judge worker failed or returned an invalid result"};
  ExecutionResult result{.exit_code = json["exitCode"].asInt(),
                         .standard_output = json["stdout"].asString(),
                         .standard_error = json["stderr"].asString(),
                         .timed_out = json["timedOut"].asBool()};
  if (json["errorType"].isString())
    result.error_type = json["errorType"].asString();
  return result;
}

} // namespace algorithm_trainer
