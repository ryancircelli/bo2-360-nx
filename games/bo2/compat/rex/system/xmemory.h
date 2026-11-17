// Compat: v0.10 rex::memory::Memory facade over v0.11 kernel::GuestAddressSpace.
#pragma once
#include <rex/kernel/heap.h>
#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>
#include <rex/memory.h>
namespace rex::memory {
using kernel::kMemoryProtectRead;
using kernel::kMemoryProtectWrite;
using kernel::BaseHeap;
class Memory {
 public:
  explicit Memory(kernel::GuestAddressSpace* as) : as_(as) {}
  uint8_t* virtual_membase() const { return as_->GetVirtualMembase(); }
  uint8_t* physical_membase() const { return as_->GetPhysicalMembase(); }
  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t a) const { return as_->TranslateVirtual<T>(a); }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t a) const { return as_->TranslatePhysical<T>(a); }
  kernel::BaseHeap* LookupHeap(uint32_t a) const { return as_->LookupHeap(a); }
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t align = 0x20) { return as_->SystemHeapAlloc(size, align); }
  void SystemHeapFree(uint32_t a) { as_->SystemHeapFree(a); }
  uint32_t GetPhysicalAddress(uint32_t a) const { return as_->GetPhysicalAddress(a); }
  kernel::GuestAddressSpace* address_space() const { return as_; }
 private:
  kernel::GuestAddressSpace* as_;
};
}  // namespace rex::memory
