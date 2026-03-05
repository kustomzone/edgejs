#ifndef UBI_COMPAT_EXEC_H_
#define UBI_COMPAT_EXEC_H_

#include <string>
#include <string_view>

bool UbiShouldWrapCompatCommand(std::string_view command);
int UbiRunCompatCommand(int argc, const char* const* argv, std::string* error_out);

#endif  // UBI_COMPAT_EXEC_H_
