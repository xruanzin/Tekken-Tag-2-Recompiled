#pragma once

#include "cafe/ppc_context.h"

#include <cstdint>
#include <vector>

namespace cafe::dynamic {

// Initializes the zero-install execution backend from the user's RPX.
// The RPX is still validated by the normal loader before execution.
void initialize(const std::vector<uint8_t>& rpx_bytes);
bool enabled();

// Initializes imported data objects after the system heap exists.
void initialize_data_imports();

// Executes guest PowerPC code beginning at entry. The backend keeps code
// decoded/cached in memory and resolves Cafe OS imports directly to the
// native runtime.
void execute(PPCContext& ctx, uint8_t* base, uint32_t entry);

} // namespace cafe::dynamic
