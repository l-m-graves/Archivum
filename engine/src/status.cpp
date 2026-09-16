#include "archivum/status.h"

namespace archivum {

std::string_view to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::IoError:
      return "io_error";
    case ErrorCode::NotFound:
      return "not_found";
    case ErrorCode::AlreadyExists:
      return "already_exists";
    case ErrorCode::Corrupt:
      return "corrupt";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::Crashed:
      return "crashed";
    case ErrorCode::Busy:
      return "busy";
  }
  return "unknown";
}

std::string Status::to_string() const {
  std::string out(archivum::to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace archivum
