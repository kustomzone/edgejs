#include <cstdint>
#include <cstdlib>

extern "C" __attribute__((used)) std::uint32_t ubi_guest_malloc(std::uint32_t size) {
  void* ptr = std::malloc(static_cast<size_t>(size));
  return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(ptr));
}
