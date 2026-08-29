#include "algorithm-trainer/nsjail-python-executor.h"

#include <arpa/inet.h>
#include <json/json.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::size_t maximum_message_size{2 * 1024 * 1024};

bool transfer(int descriptor, void *data, std::size_t size, bool writing) {
  auto *cursor = static_cast<char *>(data);
  while (size > 0) {
    const auto count =
        writing ? ::write(descriptor, cursor, size) : ::read(descriptor, cursor, size);
    if (!writing && count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    cursor += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<Json::Value> receive(int descriptor) {
  std::uint32_t encoded_size{};
  if (!transfer(descriptor, &encoded_size, sizeof(encoded_size), false))
    return std::nullopt;
  const auto size = ntohl(encoded_size);
  if (size == 0 || size > maximum_message_size)
    return std::nullopt;
  std::string body(size, '\0');
  if (!transfer(descriptor, body.data(), body.size(), false))
    return std::nullopt;
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  const auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
  if (!reader->parse(body.data(), body.data() + body.size(), &value, &errors) || !value.isObject())
    return std::nullopt;
  return value;
}

bool send(int descriptor, const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  auto body = Json::writeString(builder, value);
  auto size = htonl(static_cast<std::uint32_t>(body.size()));
  return transfer(descriptor, &size, sizeof(size), true) &&
         transfer(descriptor, body.data(), body.size(), true);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 5 || std::string_view{argv[1]} != "--ipc-fd" ||
      std::string_view{argv[3]} != "--cgroupv2-mount")
    return 2;
  int descriptor{};
  const auto raw = std::string_view{argv[2]};
  const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), descriptor);
  if (error != std::errc{} || end != raw.data() + raw.size() || descriptor < 3)
    return 2;
  const auto request = receive(descriptor);
  if (!request || !(*request)["language"].isString() || !(*request)["source"].isString() ||
      !(*request)["input"].isString())
    return 3;

  auto config = algorithm_trainer::NsJailPythonExecutor::default_config();
  config.cgroupv2_mount = argv[4];
  algorithm_trainer::NsJailPythonExecutor executor{std::move(config)};
  auto result = executor.run({.language = (*request)["language"].asString(),
                              .source_code = (*request)["source"].asString(),
                              .standard_input = (*request)["input"].asString()});
  Json::Value response;
  if (const auto *failure = std::get_if<algorithm_trainer::ExecutorError>(&result)) {
    response["error"] = failure->message;
  } else {
    const auto &execution = std::get<algorithm_trainer::ExecutionResult>(result);
    response["exitCode"] = execution.exit_code;
    response["stdout"] = execution.standard_output;
    response["stderr"] = execution.standard_error;
    response["timedOut"] = execution.timed_out;
    if (execution.error_type)
      response["errorType"] = *execution.error_type;
  }
  return send(descriptor, response) ? 0 : 4;
}
