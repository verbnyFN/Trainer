#include "algorithm-trainer/nsjail-python-executor.h"

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace algorithm_trainer {
namespace {

constexpr int output_limit_exit_code{120};
constexpr int child_launch_error_exit_code{127};
constexpr int signal_exit_code_offset{128};
constexpr std::chrono::milliseconds supervisor_grace_period{250};
constexpr int supervisor_poll_milliseconds{25};
constexpr std::size_t maximum_log_bytes{std::size_t{64} * 1024};
constexpr std::size_t temporary_path_capacity{64};
constexpr std::size_t pipe_read_buffer_size{4096};
constexpr mode_t writable_file_mode{0600};
constexpr mode_t read_only_file_mode{0400};
constexpr std::string_view seccomp_policy{
    "ERRNO(1) { clone, fork, vfork, unshare, setns, socket, socketpair, connect, bind, "
    "listen, accept, ptrace, process_vm_readv, process_vm_writev, mount, chroot, bpf, "
    "perf_event_open, keyctl, add_key, request_key } DEFAULT ALLOW"}; // NOLINT

class TemporaryWorkspace {
public:
  TemporaryWorkspace(const TemporaryWorkspace &) = delete;
  TemporaryWorkspace &operator=(const TemporaryWorkspace &) = delete;
  TemporaryWorkspace(TemporaryWorkspace &&other) noexcept : path_{std::exchange(other.path_, {})} {}
  TemporaryWorkspace &operator=(TemporaryWorkspace &&) = delete;

  ~TemporaryWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  static std::variant<TemporaryWorkspace, ExecutorError> create() {
    std::array<char, temporary_path_capacity> path_template{};
    constexpr std::string_view pattern{"/tmp/algorithm-trainer-XXXXXX"};
    std::ranges::copy(pattern, path_template.begin());

    const auto *created_path = ::mkdtemp(path_template.data());
    if (created_path == nullptr) {
      return ExecutorError{"Could not create an isolated workspace: " +
                           std::string{std::strerror(errno)}};
    }
    return TemporaryWorkspace{created_path};
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  explicit TemporaryWorkspace(std::filesystem::path path) : path_{std::move(path)} {}

  std::filesystem::path path_;
};

class FileDescriptor {
public:
  FileDescriptor() = default;
  explicit FileDescriptor(int descriptor) : descriptor_{descriptor} {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  FileDescriptor(FileDescriptor &&other) noexcept
      : descriptor_{std::exchange(other.descriptor_, -1)} {}

  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  ~FileDescriptor() { reset(); }

  [[nodiscard]] int get() const { return descriptor_; }
  [[nodiscard]] bool valid() const { return descriptor_ >= 0; }

  int release() { return std::exchange(descriptor_, -1); }

  void reset() {
    if (valid()) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
  }

private:
  int descriptor_{-1};
};

struct Pipe {
  FileDescriptor read_end;
  FileDescriptor write_end;
};

std::variant<Pipe, ExecutorError> make_pipe() {
  std::array<int, 2> descriptors{};
  if (::pipe2(descriptors.data(), O_CLOEXEC) == -1) {
    return ExecutorError{"Could not create executor pipe: " + std::string{std::strerror(errno)}};
  }
  return Pipe{
      .read_end = FileDescriptor{descriptors[0]},
      .write_end = FileDescriptor{descriptors[1]},
  };
}

std::optional<ExecutorError> write_file(const std::filesystem::path &path,
                                        std::string_view contents) {
  FileDescriptor file{
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, writable_file_mode)};
  if (!file.valid()) {
    return ExecutorError{"Could not create executor input file: " +
                         std::string{std::strerror(errno)}};
  }

  while (!contents.empty()) {
    const auto written = ::write(file.get(), contents.data(), contents.size());
    if (written == -1) {
      if (errno == EINTR) {
        continue;
      }
      return ExecutorError{"Could not write executor input file: " +
                           std::string{std::strerror(errno)}};
    }
    contents.remove_prefix(static_cast<std::size_t>(written));
  }

  if (::fchmod(file.get(), read_only_file_mode) == -1) {
    return ExecutorError{"Could not protect executor input file: " +
                         std::string{std::strerror(errno)}};
  }
  return std::nullopt;
}

std::variant<std::vector<std::filesystem::path>, ExecutorError>
read_runtime_closure(const std::filesystem::path &manifest) {
  std::ifstream input{manifest};
  if (!input) {
    return ExecutorError{"Python runtime closure manifest is unavailable"};
  }

  std::vector<std::filesystem::path> paths;
  for (std::string line; std::getline(input, line);) {
    if (!line.starts_with("/nix/store/") || line.find(':') != std::string::npos) {
      return ExecutorError{"Python runtime closure manifest contains an invalid path"};
    }
    paths.emplace_back(std::move(line));
  }
  if (paths.empty()) {
    return ExecutorError{"Python runtime closure manifest is empty"};
  }
  return paths;
}

std::optional<ExecutorError>
prepare_root(const std::filesystem::path &root,
             const std::vector<std::filesystem::path> &runtime_closure) {
  std::error_code error;
  std::filesystem::create_directories(root / "workspace", error);
  if (error) {
    return ExecutorError{"Could not prepare sandbox workspace mount"};
  }

  for (const auto &store_path : runtime_closure) {
    const auto destination = root / store_path.relative_path();
    std::filesystem::create_directories(destination, error);
    if (error) {
      return ExecutorError{"Could not prepare Python runtime mount"};
    }
  }
  return std::nullopt;
}

std::string read_bounded_file(const std::filesystem::path &path, std::size_t limit) {
  std::ifstream input{path, std::ios::binary};
  std::string contents;
  contents.resize(limit);
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  contents.resize(static_cast<std::size_t>(input.gcount()));
  return contents;
}

void set_non_blocking(int descriptor) {
  const auto flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags != -1) {
    static_cast<void>(::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
  }
}

bool drain(FileDescriptor &descriptor, std::string &destination, std::size_t output_limit,
           std::size_t &total_output, bool &limit_exceeded) {
  std::array<char, pipe_read_buffer_size> buffer{};
  while (true) {
    const auto count = ::read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      const auto size = static_cast<std::size_t>(count);
      const auto remaining = total_output < output_limit ? output_limit - total_output : 0;
      destination.append(buffer.data(), std::min(size, remaining));
      total_output += size;
      limit_exceeded = limit_exceeded || total_output > output_limit;
      continue;
    }
    if (count == 0) {
      descriptor.reset();
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    descriptor.reset();
    return false;
  }
}

void terminate_supervisor(pid_t process) { static_cast<void>(::kill(process, SIGTERM)); }

int wait_for_supervisor(pid_t process) {
  int status{};
  while (::waitpid(process, &status, 0) == -1 && errno == EINTR) {
  }
  return status;
}

std::vector<std::string>
nsjail_arguments(const NsJailPythonExecutorConfig &config, const TemporaryWorkspace &workspace,
                 const std::vector<std::filesystem::path> &runtime_closure) {
  const auto root = workspace.path() / "root";
  const auto work = workspace.path() / "work";
  const auto log = workspace.path() / "nsjail.log";
  const auto backup_time_limit =
      std::chrono::duration_cast<std::chrono::seconds>(config.wall_time_limit).count() + 2;

  std::vector<std::string> arguments{
      config.nsjail_path.string(),
      "--mode",
      "o",
      "--chroot",
      root.string(),
      "--cwd",
      "/workspace",
      "--hostname",
      "algorithm-trainer",
      "--time_limit",
      std::to_string(backup_time_limit),
      "--max_cpus",
      "1",
      "--rlimit_as",
      std::to_string(config.address_space_limit_megabytes),
      "--rlimit_cpu",
      std::to_string(config.cpu_time_limit_seconds),
      "--rlimit_fsize",
      "1",
      "--rlimit_nofile",
      "16",
      "--rlimit_nproc",
      std::to_string(config.process_limit),
      "--rlimit_stack",
      "16",
      "--disable_proc",
      "--iface_no_lo",
      "--forward_signals",
      "--env",
      "HOME=/workspace",
      "--env",
      "PATH=",
      "--env",
      "PYTHONHASHSEED=0",
      "--env",
      "PYTHONDONTWRITEBYTECODE=1",
      "--env",
      "PYTHONNOUSERSITE=1",
      "--bindmount",
      work.string() + ":/workspace",
      "--log",
      log.string(),
      "--seccomp_string",
      std::string{seccomp_policy},
  };

  for (const auto &path : runtime_closure) {
    arguments.emplace_back("--bindmount_ro");
    arguments.push_back(path.string() + ':' + path.string());
  }

  arguments.emplace_back("--");
  arguments.push_back(config.python_path.string());
  arguments.emplace_back("-I");
  arguments.emplace_back("-B");
  arguments.emplace_back("/workspace/submission.py");
  return arguments;
}

struct ChildDescriptors {
  int standard_input;
  int standard_output;
  int standard_error;
  int exec_error;
};

[[noreturn]] void execute_nsjail(const NsJailPythonExecutorConfig &config,
                                 const TemporaryWorkspace &workspace,
                                 const std::vector<std::filesystem::path> &runtime_closure,
                                 const ChildDescriptors &descriptors) {
  static_cast<void>(::setpgid(0, 0));
  if (::dup2(descriptors.standard_input, STDIN_FILENO) == -1 ||
      ::dup2(descriptors.standard_output, STDOUT_FILENO) == -1 ||
      ::dup2(descriptors.standard_error, STDERR_FILENO) == -1) {
    const auto error = errno;
    static_cast<void>(::write(descriptors.exec_error, &error, sizeof(error)));
    _exit(child_launch_error_exit_code);
  }

  const auto arguments = nsjail_arguments(config, workspace, runtime_closure);
  std::vector<char *> argument_pointers;
  argument_pointers.reserve(arguments.size() + 1);
  for (const auto &argument : arguments) {
    argument_pointers.push_back(const_cast<char *>(argument.c_str()));
  }
  argument_pointers.push_back(nullptr);

  std::array<char *, 1> environment{nullptr};
  ::execve(config.nsjail_path.c_str(), argument_pointers.data(), environment.data());

  const auto error = errno;
  static_cast<void>(::write(descriptors.exec_error, &error, sizeof(error)));
  _exit(child_launch_error_exit_code);
}

bool nsjail_failed_to_initialize(int status, std::string_view log) {
  return (!WIFEXITED(status) || WEXITSTATUS(status) != 0) &&
         (log.find("[F]") != std::string_view::npos ||
          log.find("Couldn't") != std::string_view::npos);
}

int execution_exit_code(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return signal_exit_code_offset + WTERMSIG(status);
  }
  return 1;
}

} // namespace

NsJailPythonExecutor::NsJailPythonExecutor() : NsJailPythonExecutor(default_config()) {}

NsJailPythonExecutor::NsJailPythonExecutor(NsJailPythonExecutorConfig config)
    : config_{std::move(config)} {}

NsJailPythonExecutorConfig NsJailPythonExecutor::default_config() {
  return {
      .nsjail_path = ALGORITHM_TRAINER_NSJAIL_PATH,
      .python_path = ALGORITHM_TRAINER_PYTHON_PATH,
      .runtime_closure_manifest = ALGORITHM_TRAINER_PYTHON_RUNTIME_CLOSURE,
  };
}

ExecutorResult NsJailPythonExecutor::run( // NOLINT(readability-function-cognitive-complexity)
    const ExecutionRequest &request) {
  if (request.language != "python") {
    return ExecutorError{"NsJailPythonExecutor supports only Python"};
  }

  auto workspace_result = TemporaryWorkspace::create();
  if (const auto *error = std::get_if<ExecutorError>(&workspace_result)) {
    return *error;
  }
  auto &workspace = std::get<TemporaryWorkspace>(workspace_result);

  auto closure_result = read_runtime_closure(config_.runtime_closure_manifest);
  if (const auto *error = std::get_if<ExecutorError>(&closure_result)) {
    return *error;
  }
  const auto &runtime_closure = std::get<std::vector<std::filesystem::path>>(closure_result);

  const auto root = workspace.path() / "root";
  const auto work = workspace.path() / "work";
  std::error_code filesystem_error;
  std::filesystem::create_directories(work, filesystem_error);
  if (filesystem_error) {
    return ExecutorError{"Could not prepare executor workspace"};
  }
  if (const auto error = prepare_root(root, runtime_closure)) {
    return *error;
  }
  if (const auto error = write_file(work / "submission.py", request.source_code)) {
    return *error;
  }
  if (const auto error = write_file(work / "stdin", request.standard_input)) {
    return *error;
  }

  FileDescriptor input{::open((work / "stdin").c_str(), O_RDONLY | O_CLOEXEC)};
  if (!input.valid()) {
    return ExecutorError{"Could not open executor input"};
  }

  auto stdout_result = make_pipe();
  auto stderr_result = make_pipe();
  auto exec_error_result = make_pipe();
  if (std::holds_alternative<ExecutorError>(stdout_result) ||
      std::holds_alternative<ExecutorError>(stderr_result) ||
      std::holds_alternative<ExecutorError>(exec_error_result)) {
    return ExecutorError{"Could not establish executor communication pipes"};
  }
  auto stdout_pipe = std::get<Pipe>(std::move(stdout_result));
  auto stderr_pipe = std::get<Pipe>(std::move(stderr_result));
  auto exec_error_pipe = std::get<Pipe>(std::move(exec_error_result));

  const auto child = ::fork();
  if (child == -1) {
    return ExecutorError{"Could not launch NsJail supervisor: " +
                         std::string{std::strerror(errno)}};
  }
  if (child == 0) {
    execute_nsjail(config_, workspace, runtime_closure,
                   {
                       .standard_input = input.get(),
                       .standard_output = stdout_pipe.write_end.get(),
                       .standard_error = stderr_pipe.write_end.get(),
                       .exec_error = exec_error_pipe.write_end.get(),
                   });
  }

  input.reset();
  stdout_pipe.write_end.reset();
  stderr_pipe.write_end.reset();
  exec_error_pipe.write_end.reset();
  set_non_blocking(stdout_pipe.read_end.get());
  set_non_blocking(stderr_pipe.read_end.get());

  ExecutionResult result;
  std::size_t total_output{};
  bool output_limit_exceeded{};
  bool timed_out{};
  bool supervisor_exited{};
  bool termination_requested{};
  int status{};
  const auto deadline = std::chrono::steady_clock::now() + config_.wall_time_limit;
  auto force_kill_deadline = deadline + supervisor_grace_period;

  while (!supervisor_exited || stdout_pipe.read_end.valid() || stderr_pipe.read_end.valid()) {
    std::array<pollfd, 2> descriptors{{
        {.fd = stdout_pipe.read_end.get(), .events = POLLIN, .revents = 0},
        {.fd = stderr_pipe.read_end.get(), .events = POLLIN, .revents = 0},
    }};
    static_cast<void>(::poll(descriptors.data(), descriptors.size(), supervisor_poll_milliseconds));
    if (stdout_pipe.read_end.valid()) {
      drain(stdout_pipe.read_end, result.standard_output, config_.output_limit_bytes, total_output,
            output_limit_exceeded);
    }
    if (stderr_pipe.read_end.valid()) {
      drain(stderr_pipe.read_end, result.standard_error, config_.output_limit_bytes, total_output,
            output_limit_exceeded);
    }

    const auto now = std::chrono::steady_clock::now();
    if (!termination_requested && (output_limit_exceeded || now >= deadline)) {
      timed_out = !output_limit_exceeded;
      termination_requested = true;
      force_kill_deadline = now + supervisor_grace_period;
      terminate_supervisor(child);
    }
    if (termination_requested && !supervisor_exited && now >= force_kill_deadline) {
      static_cast<void>(::kill(child, SIGKILL));
    }

    if (!supervisor_exited) {
      const auto waited = ::waitpid(child, &status, WNOHANG);
      supervisor_exited = waited == child;
    }
  }

  if (!supervisor_exited) {
    status = wait_for_supervisor(child);
  }

  int exec_error{};
  const auto exec_error_size =
      ::read(exec_error_pipe.read_end.get(), &exec_error, sizeof(exec_error));
  if (exec_error_size > 0) {
    return ExecutorError{"NsJail could not be executed: " + std::string{std::strerror(exec_error)}};
  }

  const auto nsjail_log = read_bounded_file(workspace.path() / "nsjail.log", maximum_log_bytes);
  if (!timed_out && !output_limit_exceeded && nsjail_failed_to_initialize(status, nsjail_log)) {
    return ExecutorError{"NsJail could not establish the sandbox"};
  }

  result.timed_out = timed_out;
  result.exit_code = output_limit_exceeded ? output_limit_exit_code : execution_exit_code(status);
  if (output_limit_exceeded) {
    if (!result.standard_error.empty()) {
      result.standard_error.push_back('\n');
    }
    result.standard_error.append("Output limit exceeded");
  }
  return result;
}

} // namespace algorithm_trainer
