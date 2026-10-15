#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace stratakv::testing {

struct ProcessResult {
  bool exited_normally = false;
  int exit_code = -1;
  bool killed_by_signal = false;
  int signal = 0;
  std::string stdout_text;
};

// Runs `program` with `arguments`, with one extra environment variable set, and
// captures its standard output.
//
// fork + exec rather than system(): the child must inherit a controlled
// environment and the parent must see output written right up to the moment the
// child dies.
inline ProcessResult RunWithEnv(const std::string& program,
                                const std::vector<std::string>& arguments,
                                const std::string& env_name, const std::string& env_value) {
  ProcessResult result;

  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) return result;

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return result;
  }

  if (pid == 0) {
    // Child: wire stdout to the pipe, set the variable, and exec. Nothing here
    // may allocate or lock in ways that matter, because exec replaces it all.
    ::close(pipe_fds[0]);
    ::dup2(pipe_fds[1], 1);
    ::close(pipe_fds[1]);

    if (!env_name.empty()) ::setenv(env_name.c_str(), env_value.c_str(), 1);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(program.c_str(), argv.data());
    ::_exit(127);  // exec failed.
  }

  // Parent: drain the pipe until the child closes it, then reap.
  ::close(pipe_fds[1]);
  char buffer[4096];
  while (true) {
    const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (count <= 0) break;
    result.stdout_text.append(buffer, static_cast<size_t>(count));
  }
  ::close(pipe_fds[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    result.exited_normally = true;
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.killed_by_signal = true;
    result.signal = WTERMSIG(status);
  }
  return result;
}

// Splits captured stdout into lines, dropping a trailing empty one.
inline std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

}  // namespace stratakv::testing
