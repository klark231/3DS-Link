#pragma once
// Asserts in the UDS helpers only guard against inconsistent emulator state; here they log.
#include "common/logging/log.h"
#define ASSERT(x) do { if (!(x)) LOG_ERROR(Service_NWM, "assert failed: {}", #x); } while (0)
#define ASSERT_MSG(x, ...) do { if (!(x)) LOG_ERROR(Service_NWM, __VA_ARGS__); } while (0)
#define UNREACHABLE() do {} while (0)
