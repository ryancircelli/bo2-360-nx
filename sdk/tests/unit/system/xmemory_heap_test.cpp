/**
 * @file        system/xmemory_heap_test.cpp
 * @brief       Guest heap bounds, double free and write-watch checks
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

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
  Memory memory;
  REQUIRE(memory.Initialize());

  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(0xE0000000));
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
