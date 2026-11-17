#pragma once
#include <string>

namespace reaweb {
// ReaScript-safe, flat ASCII JSON. count=0 is stat; 1..65536 is a bounded
// binary read encoded as hex. No writable handles or source mutations exist.
std::string shared_read_range_json(const char* path, const char* offset, int count) noexcept;
}
