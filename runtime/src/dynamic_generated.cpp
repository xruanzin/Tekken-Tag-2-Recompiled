#include "cafe/dynamic.h"
#include "cafe/generated.h"

extern "C" const cafe::GuestFunction cafe_guest_functions[] = {{0, nullptr}};
extern "C" const size_t cafe_guest_function_count = 0;

extern "C" const cafe::GuestImport cafe_guest_imports[] = {{"", "", 0, 0, nullptr, false}};
extern "C" const size_t cafe_guest_import_count = 0;

extern "C" const cafe::ProgramInfo cafe_program_info = {
    "fc0270465d384386f67804e48716e30bfbe85a5a1004e3a6bfe3ac75f9de5019",
    0, 0, 0, 0, 0, 0, 0
};

extern "C" void sub_0864107C_orig(cafe::PPCContext& ctx, uint8_t* base) {
    cafe::dynamic::execute(ctx, base, 0x0864107Cu);
}
