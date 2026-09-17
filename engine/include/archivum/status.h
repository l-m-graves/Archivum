// Status and Result: the engine's error-handling vocabulary.
//
// The engine never throws. Every fallible operation returns a Status, or a
// Result<T> that holds either a value or a non-ok Status. Callers must check.
#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace archivum {

enum class ErrorCode : std::uint8_t {
  Ok = 0,
  IoError,          // the operating system (or the fault shim) refused an I/O operation
  NotFound,         // file or record does not exist
  AlreadyExists,    // exclusive create on an existing file
  Corrupt,          // checksum or structural validation failed
  InvalidArgument,  // caller error
  Unsupported,      // not implemented on this platform or in this build
  Crashed,          // test-only: the simulated filesystem has crashed and handles are dead
  Busy,             // the operation cannot proceed now (e.g. checkpoint with active readers)
  Constraint,       // a schema constraint rejected the change; message names it
};

std::string_view to_string(ErrorCode code);

class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  static Status io(std::string message) { return Status(ErrorCode::IoError, std::move(message)); }
  static Status not_found(std::string message) {
    return Status(ErrorCode::NotFound, std::move(message));
  }
  static Status already_exists(std::string message) {
    return Status(ErrorCode::AlreadyExists, std::move(message));
  }
  static Status corrupt(std::string message) { return Status(ErrorCode::Corrupt, std::move(message)); }
  static Status invalid_argument(std::string message) {
    return Status(ErrorCode::InvalidArgument, std::move(message));
  }
  static Status unsupported(std::string message) {
    return Status(ErrorCode::Unsupported, std::move(message));
  }
  static Status crashed(std::string message) { return Status(ErrorCode::Crashed, std::move(message)); }
  static Status busy(std::string message) { return Status(ErrorCode::Busy, std::move(message)); }
  static Status constraint(std::string message) {
    return Status(ErrorCode::Constraint, std::move(message));
  }

  bool ok() const { return code_ == ErrorCode::Ok; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {  // NOLINT(google-explicit-constructor)
    assert(!status_.ok() && "Result constructed from an ok Status without a value");
  }

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  T& value() & {
    assert(ok());
    return *value_;
  }
  const T& value() const& {
    assert(ok());
    return *value_;
  }
  T&& value() && {
    assert(ok());
    return std::move(*value_);
  }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace archivum
