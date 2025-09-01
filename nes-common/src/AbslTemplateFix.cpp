// Template fixes for Abseil compatibility issues
// Provides explicit instantiation of missing Abseil logging templates

#include <absl/log/internal/log_message.h>
#include <type_traits>

namespace absl {
namespace lts_20250127 {
namespace log_internal {

// Explicit instantiation of the missing integer logging template
// This fixes the undefined symbol for LogMessage::operator<<(int const&)
template LogMessage& LogMessage::operator<<(int const&);

// Additional common template instantiations that might be needed
template LogMessage& LogMessage::operator<<(unsigned int const&);
template LogMessage& LogMessage::operator<<(long const&);
template LogMessage& LogMessage::operator<<(unsigned long const&);

}  // namespace log_internal
}  // namespace lts_20250127
}  // namespace absl