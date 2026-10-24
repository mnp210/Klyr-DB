#pragma once

#include "klyrdb/common/status.hpp"
#include <string>
#include <string_view>

namespace klyrdb {

/// Error representation for public KlyrDB APIs, wrapping StatusCode and diagnostic message.
using Error = Status;

} // namespace klyrdb
