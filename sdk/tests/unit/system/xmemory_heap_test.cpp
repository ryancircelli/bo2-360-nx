/**
 * @file        system/xmemory_heap_test.cpp
 * @brief       Guest heap bounds, double free and write-watch checks
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

using namespace rex::memory;

namespace {
constexpr uint32_t kCommitRW = kMemoryAllocationReserve | kMemoryAllocationCommit;
constexpr uint32_t kRW = kMemoryProtectRead | kMemoryProtectWrite;
}  // namespace

TEST_CASE("guest heaps reject bad fixed allocations and frees", "[xmemory]") {
  Memory memory;
  REQUIRE(memory.Initialize());

  BaseHeap* heap = memory.LookupHeap(0x40000000);
  REQUIRE(heap);
  const uint32_t page = heap->page_size();
  const uint32_t top = heap->heap_base() + heap->heap_size();

  // Last page fits; one page past the end must not.
  CHECK_FALSE(heap->AllocFixed(top - page, page * 2, page, kCommitRW, kRW));
  CHECK(heap->AllocFixed(top - page, page, page, kCommitRW, kRW));
  CHECK(heap->Release(top - page));

  // Double free of a heap's first page used to clear the page table to UINT32_MAX.
  REQUIRE(heap->AllocFixed(heap->heap_base(), page, page, kCommitRW, kRW));
  CHECK(heap->Release(heap->heap_base()));
  CHECK_FALSE(heap->Release(heap->heap_base()));
}

TEST_CASE("write watch covers a host page whose first guest page is free", "[xmemory]") {
  // no_phys_alias moves the 4K physical heap to 0xC0000000; the watch code must work on both.
  const bool no_phys_alias = GENERATE(false, true);
  Memory memory;
  REQUIRE(memory.Initialize(no_phys_alias));

  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeapByType(true, 4096));
  REQUIRE(heap->heap_base() == (no_phys_alias ? 0xC0000000u : 0xE0000000u));
  REQUIRE(heap);
  const uint32_t host_page = uint32_t(page_size());
  const uint32_t offset = heap->host_address_offset();
  // Guest address of the second host page of the heap, and the first guest
  // page inside it. On 16 KB hosts this skips the host page's first 4 KB page.
  const uint32_t host_page_guest = heap->heap_base() + host_page - offset;
  const uint32_t texture =
      host_page_guest + (host_page > heap->page_size() ? heap->page_size() : 0);
  const uint32_t length = host_page_guest + host_page - texture;
  REQUIRE(heap->AllocFixed(texture, length, heap->page_size(), kCommitRW, kRW));

  heap->EnableAccessCallbacks(heap->GetPhysicalAddress(texture), length, true, false);

  size_t query_length = host_page;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(QueryProtect(memory.TranslateVirtual(texture), query_length, access));
  CHECK(access == PageAccess::kReadOnly);

  heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(), texture, length,
                         true, true);
  query_length = host_page;
  REQUIRE(QueryProtect(memory.TranslateVirtual(texture), query_length, access));
  CHECK(access == PageAccess::kReadWrite);
  CHECK(heap->Release(texture));
}

TEST_CASE("no_phys_alias serves 4K physical pages at 0xC0000000", "[xmemory]") {
  Memory memory;
  REQUIRE(memory.Initialize(true));
  REQUIRE(memory.no_phys_alias());

  auto* small = static_cast<PhysicalHeap*>(memory.LookupHeapByType(true, 4096));
  auto* large = static_cast<PhysicalHeap*>(memory.LookupHeapByType(true, 16 * 1024 * 1024));
  REQUIRE(small == large);
  CHECK(small->heap_base() == 0xC0000000u);
  CHECK(small->page_size() == 4096u);

  // A 4K allocation lands below 0xE0000000; its physical address is the plain low bits, and the
  // 0xA0000000 alias of that physical address sees the same bytes (no 0x1000 shift anywhere).
  uint32_t a = 0;
  REQUIRE(small->Alloc(0x1000, 0x1000, kCommitRW, kRW, true, &a));
  CHECK(a >= 0xC0000000u);
  CHECK(a < 0xE0000000u);
  CHECK(small->GetPhysicalAddress(a) == (a & 0x1FFFFFFFu));
  *memory.TranslateVirtual<uint32_t*>(a) = 0x12345678u;
  CHECK(*memory.TranslatePhysical<uint32_t*>(a & 0x1FFFFFFFu) == 0x12345678u);
  CHECK(*memory.TranslateVirtual<uint32_t*>(0xA0000000u + (a & 0x1FFFFFFFu)) == 0x12345678u);

  // 16 MB pages keep their alignment in the now 4K-granular heap (callers round to 16 MB).
  uint32_t b = 0;
  REQUIRE(small->AllocRange(0xC0000000u, 0xDFFFFFFFu, 0x1000000, 0x1000000, kCommitRW, kRW, true, &b));
  CHECK((b & 0xFFFFFFu) == 0u);
  uint32_t size = 0;
  CHECK(small->QuerySize(b, &size));
  CHECK(size == 0x1000000u);

  // The 0xE0000000 view is no-access.
  size_t query_length = page_size();
  PageAccess access = PageAccess::kReadWrite;
  REQUIRE(QueryProtect(memory.virtual_membase() + 0xE0000000ull, query_length, access));
  CHECK(access == PageAccess::kNoAccess);
  CHECK(small->Release(a));
  CHECK(small->Release(b));
}

TEST_CASE("default layout keeps 4K physical pages at 0xE0000000", "[xmemory]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  CHECK_FALSE(memory.no_phys_alias());
  CHECK(memory.LookupHeapByType(true, 4096)->heap_base() == 0xE0000000u);
  CHECK(memory.LookupHeapByType(true, 16 * 1024 * 1024)->page_size() == 16u * 1024 * 1024);
}
