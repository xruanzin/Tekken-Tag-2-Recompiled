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

// Differential-test entry point: executes exactly one decoded guest
// instruction and returns the next guest address.
uint32_t execute_test_instruction(PPCContext& ctx, uint8_t* base,
                                  uint32_t address, uint32_t word);

// Loads a synthetic contiguous PPC program for game-independent control-flow
// differential tests.
void initialize_test_program(uint32_t base_address,
                             const std::vector<uint32_t>& words);

// Prints the current thread's most recent guest instructions. This is kept
// allocation-free so crash/fatal diagnostics can use it safely.
void print_recent_trace();

} // namespace cafe::dynamic
