/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>  // IWYU pragma: keep: struct timeval
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "gflags/gflags.h"

#include "cuttlefish/common/libs/fs/fd.h"
#include "cuttlefish/common/libs/fs/shared_fd.h"
#include "cuttlefish/common/libs/fs/shared_select.h"
#include "cuttlefish/host/libs/config/config_instance_derived.h"
#include "cuttlefish/host/libs/config/cuttlefish_config.h"
#include "cuttlefish/host/libs/config/logging.h"
#include "cuttlefish/posix/strerror.h"
#include "cuttlefish/result/result.h"

DEFINE_int32(console_in_fd, -1,
             "File descriptor for the console's input channel");
DEFINE_int32(console_out_fd, -1,
             "File descriptor for the console's output channel");

namespace cuttlefish {

// Handles forwarding the serial console to a pseudo-terminal (PTY)
// It receives a couple of fds for the console (could be the same fd twice if,
// for example a socket_pair were used).
// Data available in the console's output needs to be read immediately to avoid
// the having the VMM blocked on writes to the pipe. To achieve this one thread
// takes care of (and only of) all read calls (from console output and from the
// socket client), using select(2) to ensure it never blocks. Writes are handled
// in a different thread, the two threads communicate through a buffer queue
// protected by a mutex.
class ConsoleForwarder {
 public:
  ConsoleForwarder(std::string console_path, SharedFD console_in,
                   SharedFD console_out, SharedFD console_log,
                   SharedFD kernel_log)
      : console_path_(std::move(console_path)),
        console_in_(std::move(console_in)),
        console_out_(std::move(console_out)),
        console_log_(std::move(console_log)),
        kernel_log_(std::move(kernel_log)) {}
  [[noreturn]] void StartServer() {
    // Create a new thread to handle writes to the console
    writer_thread_ = std::thread([this]() { WriteLoop(); });
    // Use the calling thread (likely the process' main thread) to handle
    // reading the console's output and input from the client.
    ReadLoop();
  }

 private:
  Fd OpenPTY() {
    // Remove any stale symlink to a pts device
    auto ret = unlink(console_path_.c_str());
    CHECK(!(ret < 0 && errno != ENOENT))
        << "Failed to unlink " << console_path_ << ": " << StrError(errno);

    // posix_openpt(3) only portably accepts O_RDWR and O_NOCTTY (OpenBSD
    // rejects anything else with EINVAL), so make the fd non-blocking with
    // fcntl(2) afterwards.
    auto pty = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(pty >= 0) << "Failed to open a PTY: " << StrError(errno);
    int pty_flags = fcntl(pty, F_GETFL);
    CHECK(pty_flags >= 0 && fcntl(pty, F_SETFL, pty_flags | O_NONBLOCK) == 0)
        << "Failed to make the PTY non-blocking: " << StrError(errno);

    CHECK_EQ(grantpt(pty), 0) << StrError(errno);
    CHECK_EQ(unlockpt(pty), 0) << StrError(errno);

    int packet_mode_enabled = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    CHECK_EQ(ioctl(pty, TIOCPKT, &packet_mode_enabled), 0) << StrError(errno);

    auto pty_dev_name = ptsname(pty);
    CHECK(pty_dev_name != nullptr)
        << "Failed to obtain PTY device name: " << StrError(errno);

    CHECK(symlink(pty_dev_name, console_path_.c_str()) >= 0)
        << "Failed to create symlink to " << pty_dev_name << " at "
        << console_path_ << ": " << StrError(errno);

    Result<Fd> pty_fd = Fd::Dup(pty);
    close(pty);
    CHECK(pty_fd.has_value()) << pty_fd.error();

    return std::move(*pty_fd);
  }

  void EnqueueWrite(std::shared_ptr<std::vector<char>> buf_ptr, SharedFD fd) {
    std::lock_guard<std::mutex> lock(write_queue_mutex_);
    write_queue_.emplace_back(fd, buf_ptr);
    condvar_.notify_one();
  }

  [[noreturn]] void WriteLoop() {
    while (true) {
      while (!write_queue_.empty()) {
        std::shared_ptr<std::vector<char>> buf_ptr;
        SharedFD fd;
        {
          std::lock_guard<std::mutex> lock(write_queue_mutex_);
          auto& front = write_queue_.front();
          buf_ptr = front.second;
          fd = front.first;
          write_queue_.pop_front();
        }
        // Write all bytes to the file descriptor. Writes may block, so the
        // mutex lock should NOT be held while writing to avoid blocking the
        // other thread.
        Result<uint64_t> bytes_written = 0;
        ssize_t bytes_to_write = buf_ptr->size();
        while (bytes_to_write > 0) {
          bytes_written =
              fd->Write(buf_ptr->data() + *bytes_written, bytes_to_write);
          if (!bytes_written.has_value()) {
            // It is expected for writes to the PTY to fail if nothing is
            // connected
            if (fd->GetErrno() != EAGAIN) {
              LOG(ERROR) << "Error writing to fd: " << fd->StrError();
            }

            // Don't try to write from this buffer anymore, error handling will
            // be done on the reading thread (failed client will be
            // disconnected, on serial console failure this process will abort).
            break;
          }
          bytes_to_write -= *bytes_written;
        }
      }
      {
        std::unique_lock<std::mutex> lock(write_queue_mutex_);
        // Check again before sleeping, state may have changed
        if (write_queue_.empty()) {
          condvar_.wait(lock);
        }
      }
    }
  }

  [[noreturn]] void ReadLoop() {
    SharedFD client_fd;
    // On BSD-derived systems (including macOS) the controller side of a PTY is
    // readable and returns EOF for as long as no process has the terminal side
    // open; selecting on it in that state would turn this loop into a busy loop
    // re-creating the PTY. Linux never reports EOF there (reads fail with
    // EAGAIN before the first open of the terminal side and with EIO after a
    // hangup), so this only changes behavior on those systems. While no peer is
    // connected the client fd is left out of the select set and probed again
    // every kClientProbeInterval.
    constexpr auto kClientProbeInterval = std::chrono::milliseconds(500);
    bool client_connected = true;
    std::chrono::steady_clock::time_point next_client_probe;
    while (true) {
      if (!client_fd->IsOpen()) {
        client_fd = OpenPTY();
        client_connected = true;
      }

      auto now = std::chrono::steady_clock::now();
      bool poll_client = client_connected || now >= next_client_probe;

      SharedFDSet read_set;
      read_set.Set(console_out_);
      SharedFDSet error_set;
      // NOLINTNEXTLINE(misc-include-cleaner): <sys/time.h>
      struct timeval probe_timeout = {};
      if (poll_client) {
        read_set.Set(client_fd);
        error_set.Set(client_fd);
      } else {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            next_client_probe - now);
        probe_timeout.tv_sec = remaining.count() / 1000000;
        probe_timeout.tv_usec = remaining.count() % 1000000;
      }

      Select(&read_set, nullptr, &error_set,
             poll_client ? nullptr : &probe_timeout);
      if (poll_client && !read_set.IsSet(client_fd) &&
          !error_set.IsSet(client_fd)) {
        // Not readable means not at EOF, so a peer has the terminal side open.
        client_connected = true;
      }
      if (read_set.IsSet(console_out_)) {
        std::shared_ptr<std::vector<char>> buf_ptr =
            std::make_shared<std::vector<char>>(4096);
        uint64_t bytes_read =
            console_out_->Read(buf_ptr->data(), buf_ptr->size()).value_or(0);
        // This is likely unrecoverable, so exit here
        CHECK(bytes_read > 0) << "Error reading from console output: "
                              << console_out_->StrError();
        buf_ptr->resize(bytes_read);
        EnqueueWrite(buf_ptr, console_log_);
        if (client_fd->IsOpen() && client_connected) {
          EnqueueWrite(buf_ptr, client_fd);
        }
        EnqueueWrite(buf_ptr, kernel_log_);
      }
      if (poll_client &&
          (read_set.IsSet(client_fd) || error_set.IsSet(client_fd))) {
        std::shared_ptr<std::vector<char>> buf_ptr =
            std::make_shared<std::vector<char>>(4096);
        uint64_t bytes_read =
            client_fd->Read(buf_ptr->data(), buf_ptr->size()).value_or(0);
        if (bytes_read == 0 && client_fd->GetErrno() == 0) {
          // EOF: nothing has the terminal side open (BSD semantics, see above).
          // Keep the PTY so the published path stays valid and check again
          // later.
          client_connected = false;
          next_client_probe =
              std::chrono::steady_clock::now() + kClientProbeInterval;
        } else if (bytes_read <= 0) {
          // If this happens, it's usually because the PTY controller went away
          // e.g. the user closed minicom, or killed screen, or closed kgdb. In
          // such a case, we will just re-create the PTY
          LOG(ERROR) << "Error reading from client fd: "
                     << client_fd->StrError();
          client_fd->Close();
        } else if (bytes_read == 1) {  // Control message
          client_connected = true;
          VLOG(0) << "pty control message: " << (int)(*buf_ptr)[0];
        } else {
          client_connected = true;
          buf_ptr->resize(bytes_read);
          buf_ptr->erase(buf_ptr->begin());
          EnqueueWrite(buf_ptr, console_in_);
        }
      }
    }
  }

  std::string console_path_;
  SharedFD console_in_;
  SharedFD console_out_;
  SharedFD console_log_;
  SharedFD kernel_log_;
  std::thread writer_thread_;
  std::mutex write_queue_mutex_;
  std::condition_variable condvar_;
  std::deque<std::pair<SharedFD, std::shared_ptr<std::vector<char>>>>
      write_queue_;
};

int ConsoleForwarderMain(int argc, char** argv) {
  DefaultSubprocessLogging(argv);
  ::gflags::ParseCommandLineFlags(&argc, &argv, true);

  CHECK(!(FLAGS_console_in_fd < 0 || FLAGS_console_out_fd < 0))
      << "Invalid file descriptors: " << FLAGS_console_in_fd << ", "
      << FLAGS_console_out_fd;

  Result<Fd> console_in = Fd::Dup(FLAGS_console_in_fd);
  CHECK(console_in.has_value()) << console_in.error();
  close(FLAGS_console_in_fd);

  Result<Fd> console_out = Fd::Dup(FLAGS_console_out_fd);
  CHECK(console_out.has_value()) << console_out.error();
  close(FLAGS_console_out_fd);

  const CuttlefishConfig* config = CuttlefishConfig::Get();
  CHECK(config) << "Unable to get config object";

  const CuttlefishConfig::InstanceSpecific& instance =
      config->ForDefaultInstance();
  const std::string console_log = instance.PerInstancePath("console_log");
  Fd console_log_fd =
      Fd::Open(console_log, O_CREAT | O_APPEND | O_WRONLY, 0666).value_or(Fd());
  Fd kernel_log_fd =
      Fd::Open(KernelLogPipeName(instance), O_APPEND | O_WRONLY, 0666)
          .value_or(Fd());
  ConsoleForwarder console_forwarder(
      ConsolePath(instance), std::move(*console_in), std::move(*console_out),
      std::move(console_log_fd), std::move(kernel_log_fd));

  // Don't get a SIGPIPE from the clients
  CHECK(sigaction(SIGPIPE, nullptr, nullptr) == 0)
      << "Failed to set SIGPIPE to be ignored: " << StrError(errno);

  console_forwarder.StartServer();
}

}  // namespace cuttlefish

int main(int argc, char** argv) {
  return cuttlefish::ConsoleForwarderMain(argc, argv);
}
