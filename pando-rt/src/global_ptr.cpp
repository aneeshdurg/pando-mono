// SPDX-License-Identifier: MIT
/* Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved. */
/* Copyright (c) 2023 University of Washington */

#include "pando-rt/memory/global_ptr.hpp"

#include <atomic>
#include <cstring>
#include <type_traits>

#include "pando-rt/locality.hpp"
#include "pando-rt/memory/address_translation.hpp"
#include "pando-rt/stdlib.hpp"
#include "pando-rt/benchmark/counters.hpp"

#if defined(PANDO_RT_USE_BACKEND_PREP)
#include "prep/cores.hpp"
#include "prep/hart_context_fwd.hpp"
#include "prep/log.hpp"
#if PANDO_MEM_TRACE_OR_STAT
#include "prep/memtrace_log.hpp"
#endif
#include "prep/memory.hpp"
#include "prep/nodes.hpp"
#include "prep/status.hpp"
#elif defined(PANDO_RT_USE_BACKEND_DRVX)
#include "drvx/drvx.hpp"
#endif // PANDO_RT_USE_BACKEND_PREP

constexpr bool POINTER_TIMER_ENABLE = false;
counter::Record<std::int64_t> pointerCount = counter::Record<std::int64_t>();

namespace pando {

namespace detail {

GlobalAddress createGlobalAddress(void* nativePtr) noexcept {
#if defined(PANDO_RT_USE_BACKEND_PREP)

  if (nativePtr == nullptr) {
    return GlobalAddress{};
  }

  const auto thisPlace = getCurrentPlace();

  // check if it is in L2SP or main memory
  const auto& memoryInformation = Memory::findInformation(nativePtr);
  switch (memoryInformation.type) {
    case MemoryType::L2SP: {
      const auto offset = reinterpret_cast<std::byte*>(nativePtr) - memoryInformation.baseAddress;
      return encodeL2SPAddress(thisPlace.node, thisPlace.pod, offset);
    }
    case MemoryType::Main: {
      const auto offset = reinterpret_cast<std::byte*>(nativePtr) - memoryInformation.baseAddress;
      return encodeMainAddress(thisPlace.node, offset);
    }
    default: {
      // it is potentially variable at the stack; check if it is in L1SP
      const auto offset = Cores::getL1SPOffset(nativePtr);
      if (offset < 0) {
        // pointer could not be found in any memory
        PANDO_ABORT("Could not translate native pointer to global pointer");
      }
      return encodeL1SPAddress(thisPlace.node, thisPlace.pod, thisPlace.core, offset);
    }
  }

#elif defined(PANDO_RT_USE_BACKEND_DRVX)

  // only stack variable to L1SP conversion is allowed in DrvX
  GlobalAddress addr = 0;
  std::size_t size = 0;
  DrvAPI::DrvAPINativeToAddress(nativePtr, &addr, &size);
  if (extractMemoryType(addr) != MemoryType::L1SP) {
    PANDO_ABORT("GlobalPtr to native conversion possible only for L1SP in DrvX");
  }

  return addr;

#endif // PANDO_RT_USE_BACKEND_PREP
}

void* asNativePtr(GlobalAddress globalAddr) noexcept {
#if defined(PANDO_RT_USE_BACKEND_PREP)

  // yield to other hart and then issue the operation
  //hartYield();

  if (globalAddr == 0) {
    return nullptr;
  }

  const auto nodeIdx = extractNodeIndex(globalAddr);
  if (nodeIdx != Nodes::getCurrentNode()) {
    // object is not in this host
    PANDO_ABORT("Object is in remote native address");
  }
  return Memory::getNativeAddress(globalAddr);

#elif defined(PANDO_RT_USE_BACKEND_DRVX)

  const auto nodeIdx = extractNodeIndex(globalAddr);
  if (nodeIdx != NodeIndex(DrvAPI::myPXNId())) {
    // object is not in this host
    PANDO_ABORT("Object is in remote native address");
  }
  void* nativePtr{nullptr};
  std::size_t size = 0;
  DrvAPI::DrvAPIAddressToNative(globalAddr, &nativePtr, &size);
  return nativePtr;

#else

#error "Not implemented"

#endif // PANDO_RT_USE_BACKEND_PREP
}

void load(GlobalAddress srcGlobalAddr, std::size_t n, void* dstNativePtr) {
#if defined(PANDO_RT_USE_BACKEND_PREP)

  const auto nodeIdx = extractNodeIndex(srcGlobalAddr);
  if (nodeIdx == Nodes::getCurrentNode()) {
    counter::HighResolutionCount<POINTER_TIMER_ENABLE> pointerTimer;
    pointerTimer.start();
    // yield to other hart and then issue the operation
    //hartYield();

    const void* srcNativePtr = Memory::getNativeAddress(srcGlobalAddr);
    // we read from shared memory
    std::memcpy(dstNativePtr, srcNativePtr, n);

#if PANDO_MEM_TRACE_OR_STAT
    // if the level of mem-tracing is ALL (2), log intra-pxn memory operations
    MemTraceLogger::log("LOAD", nodeIdx, nodeIdx, n, dstNativePtr, srcGlobalAddr);
#endif
    counter::recordHighResolutionEvent(pointerCount, pointerTimer);
  } else {
    // remote load; send remote load request and wait for it to finish
    Nodes::LoadHandle handle(dstNativePtr);
    if (auto status = Nodes::load(nodeIdx, srcGlobalAddr, n, handle); status != Status::Success) {
      SPDLOG_ERROR("Load error: {}", status);
      PANDO_ABORT("Load error");
    }

    hartYieldUntil([&handle] {
      return handle.ready();
    });
  }

#elif defined(PANDO_RT_USE_BACKEND_DRVX)

  using BlockType = std::uint64_t;
  const auto blockSize = sizeof(BlockType);
  const auto numBlocks = n / blockSize;
  const auto remainderBytes = n % blockSize;

  auto blockDst = static_cast<BlockType*>(dstNativePtr);
  auto blockSrc = srcGlobalAddr;

  const auto bytesRead = numBlocks * blockSize;
  auto byteDst = static_cast<std::byte*>(dstNativePtr) + bytesRead;
  auto byteSrc = srcGlobalAddr + bytesRead;

#if defined(PANDO_RT_BYPASS)
  if (getBypassFlag()) {
    for (std::size_t i = 0; i < numBlocks; i++) {
      const auto bytesOffset = i * blockSize;

      void *addr_native = nullptr;
      std::size_t size = 0;
      DrvAPI::DrvAPIAddressToNative(blockSrc + bytesOffset, &addr_native, &size);
      BlockType* as_native_pointer = reinterpret_cast<BlockType*>(addr_native);
      *(blockDst + i) = *as_native_pointer;
      // hartYield
      DrvAPI::nop(1u);
    }

    for (std::size_t i = 0; i < remainderBytes; i++) {
      void *addr_native = nullptr;
      std::size_t size = 0;
      DrvAPI::DrvAPIAddressToNative(byteSrc + i, &addr_native, &size);
      std::byte* as_native_pointer = reinterpret_cast<std::byte*>(addr_native);
      *(byteDst + i) = *as_native_pointer;
      // hartYield
      DrvAPI::nop(1u);
    }
  } else {
    for (std::size_t i = 0; i < numBlocks; i++) {
      const auto bytesOffset = i * blockSize;
      *(blockDst + i) = DrvAPI::read<BlockType>(blockSrc + bytesOffset);
    }

    for (std::size_t i = 0; i < remainderBytes; i++) {
      *(byteDst + i) = DrvAPI::read<std::byte>(byteSrc + i);
    }
  }
#else
  for (std::size_t i = 0; i < numBlocks; i++) {
    const auto bytesOffset = i * blockSize;
    *(blockDst + i) = DrvAPI::read<BlockType>(blockSrc + bytesOffset);
  }

  for (std::size_t i = 0; i < remainderBytes; i++) {
    *(byteDst + i) = DrvAPI::read<std::byte>(byteSrc + i);
  }
#endif

// Adding an extra layer for message payload granularity
// Currently does not work for types more than 64 bits
// Kept for future usage
/*
  using ChunkType = std::uint64_t;
  using BlockType = std::uint16_t;
  const auto chunkSize = sizeof(ChunkType);
  const auto blockSize = sizeof(BlockType);
  const auto numChunks = n / chunkSize;
  const auto remainderBlocks = n % chunkSize;
  const auto numBlocks = remainderBlocks / blockSize;
  const auto remainderBytes = remainderBlocks % blockSize;

  if (numChunks > 0) {
    auto chunkDst = static_cast<ChunkType*>(dstNativePtr);
    auto chunkSrc = reinterpret_cast<std::uint64_t>(srcGlobalAddr);
    for (std::size_t i = 0; i < numChunks; i++) {
      const auto offset = i * chunkSize;
      *(chunkDst + i) = DrvAPI::read<ChunkType>(chunkSrc + offset);
    }
  }

  const auto blocksRead = numChunks * chunkSize;
  if (numBlocks > 0) {
    auto blockDst = static_cast<BlockType*>(dstNativePtr) + blocksRead;
    auto blockSrc = reinterpret_cast<std::uint64_t>(srcGlobalAddr) + blocksRead;
    for (std::size_t i = 0; i < numBlocks; i++) {
      const auto offset = i * blockSize;
      *(blockDst + i) = DrvAPI::read<BlockType>(blockSrc + offset);
    }
  }

  const auto bytesRead = blocksRead + numBlocks * blockSize;
  if (remainderBytes > 0) {
    auto byteDst = static_cast<std::byte*>(dstNativePtr) + bytesRead;
    auto byteSrc = reinterpret_cast<std::uint64_t>(srcGlobalAddr) + bytesRead;
    for (std::size_t i = 0; i < remainderBytes; i++) {
      *(byteDst + i) = DrvAPI::read<std::byte>(byteSrc + i);
    }
  }
*/
#endif // PANDO_RT_USE_BACKEND_PREP
}

struct Data64Bytes {
  std::uint64_t data[8];
};

void store(GlobalAddress dstGlobalAddr, std::size_t n, const void* srcNativePtr) {
#if defined(PANDO_RT_USE_BACKEND_PREP)

  const auto nodeIdx = extractNodeIndex(dstGlobalAddr);
  if (nodeIdx == Nodes::getCurrentNode()) {
    counter::HighResolutionCount<POINTER_TIMER_ENABLE> pointerTimer;
    pointerTimer.start();
    // yield to other hart and then issue the operation
    //hartYield();

    void* dstNativePtr = Memory::getNativeAddress(dstGlobalAddr);
    std::memcpy(dstNativePtr, srcNativePtr, n);
    // we write to shared memory

#if PANDO_MEM_TRACE_OR_STAT
    // if the level of mem-tracing is ALL (2), log intra-pxn memory operations
    MemTraceLogger::log("STORE", nodeIdx, nodeIdx, n, dstNativePtr, dstGlobalAddr);
#endif
    counter::recordHighResolutionEvent(pointerCount, pointerTimer);
  } else {
    // remote store; send remote store request and wait for it to finish
    Nodes::AckHandle handle;
    if (auto status = Nodes::store(nodeIdx, dstGlobalAddr, n, srcNativePtr, handle);
        status != Status::Success) {
      SPDLOG_ERROR("Store error: {}", status);
      PANDO_ABORT("Store error");
    }

    hartYieldUntil([&handle] {
      return handle.ready();
    });
  }

#elif defined(PANDO_RT_USE_BACKEND_DRVX)

  using BlockType = std::uint64_t;
  const auto blockSize = sizeof(BlockType);
  const auto numBlocks = n / blockSize;
  const auto remainderBytes = n % blockSize;

  auto blockSrc = static_cast<const BlockType*>(srcNativePtr);
  auto blockDst = reinterpret_cast<std::uint64_t>(dstGlobalAddr);

  const auto bytesWritten = numBlocks * blockSize;
  auto byteSrc = static_cast<const std::byte*>(srcNativePtr) + bytesWritten;
  auto byteDst = reinterpret_cast<std::uint64_t>(dstGlobalAddr) + bytesWritten;

#if defined(PANDO_RT_BYPASS)
  if (getBypassFlag()) {
    for (std::size_t i = 0; i < numBlocks; i++) {
      auto blockData = *(blockSrc + i);
      const auto offset = i * blockSize;

      void *addr_native = nullptr;
      std::size_t size = 0;
      DrvAPI::DrvAPIAddressToNative(blockDst + offset, &addr_native, &size);
      BlockType* as_native_pointer = reinterpret_cast<BlockType*>(addr_native);
      *as_native_pointer = blockData;
      // hartYield
      DrvAPI::nop(1u);
    }

    for (std::size_t i = 0; i < remainderBytes; i++) {
      auto byteData = *(byteSrc + i);

      void *addr_native = nullptr;
      std::size_t size = 0;
      DrvAPI::DrvAPIAddressToNative(byteDst + i, &addr_native, &size);
      std::byte* as_native_pointer = reinterpret_cast<std::byte*>(addr_native);
      *as_native_pointer = byteData;
      // hartYield
      DrvAPI::nop(1u);
    }
  } else {
    for (std::size_t i = 0; i < numBlocks; i++) {
      auto blockData = *(blockSrc + i);
      const auto offset = i * blockSize;
      DrvAPI::write<BlockType>(blockDst + offset, blockData);
    }

    for (std::size_t i = 0; i < remainderBytes; i++) {
      auto byteData = *(byteSrc + i);
      DrvAPI::write<std::byte>(byteDst + i, byteData);
    }
  }
#else
  for (std::size_t i = 0; i < numBlocks; i++) {
    auto blockData = *(blockSrc + i);
    const auto offset = i * blockSize;
    DrvAPI::write<BlockType>(blockDst + offset, blockData);
  }

  for (std::size_t i = 0; i < remainderBytes; i++) {
    auto byteData = *(byteSrc + i);
    DrvAPI::write<std::byte>(byteDst + i, byteData);
  }
#endif

// Adding an extra layer for message payload granularity
// Currently does not work for types more than 64 bits
// Kept for future usage
/*
  using ChunkType = std::uint64_t;
  using BlockType = std::uint16_t;
  const auto chunkSize = sizeof(ChunkType);
  const auto blockSize = sizeof(BlockType);
  const auto numChunks = n / chunkSize;
  const auto remainderBlocks = n % chunkSize;
  const auto numBlocks = remainderBlocks / blockSize;
  const auto remainderBytes = remainderBlocks % blockSize;

  if (numChunks > 0) {
    auto chunkSrc = static_cast<const ChunkType*>(srcNativePtr);
    auto chunkDst = reinterpret_cast<std::uint64_t>(dstGlobalAddr);
    for (std::size_t i = 0; i < numChunks; i++) {
      auto chunkData = *(chunkSrc + i);
      const auto offset = i * chunkSize;
      DrvAPI::write<ChunkType>(chunkDst + offset, chunkData);
    }
  }

  const auto blocksWritten = numChunks * chunkSize;
  if (numBlocks > 0) {
    auto blockSrc = static_cast<const BlockType*>(srcNativePtr) + blocksWritten;
    auto blockDst = reinterpret_cast<std::uint64_t>(dstGlobalAddr) + blocksWritten;
    for (std::size_t i = 0; i < numBlocks; i++) {
      auto blockData = *(blockSrc + i);
      const auto offset = i * blockSize;
      DrvAPI::write<BlockType>(blockDst + offset, blockData);
    }
  }

  const auto bytesWritten = blocksWritten + numBlocks * blockSize;
  if (remainderBytes > 0) {
    auto byteSrc = static_cast<const std::byte*>(srcNativePtr) + bytesWritten;
    auto byteDst = reinterpret_cast<std::uint64_t>(dstGlobalAddr) + bytesWritten;
    for (std::size_t i = 0; i < remainderBytes; i++) {
      auto byteData = *(byteSrc + i);
      DrvAPI::write<std::byte>(byteDst + i, byteData);
    }
  }
*/
#endif // PANDO_RT_USE_BACKEND_PREP
}

} // namespace detail

MemoryType memoryTypeOf(GlobalPtr<const void> ptr) noexcept {
  return extractMemoryType(ptr.address);
}

Place localityOf(GlobalPtr<const void> ptr) noexcept {
  if (ptr == nullptr) {
    return anyPlace;
  }
  const auto memoryType = extractMemoryType(ptr.address);
  const auto nodeIdx = extractNodeIndex(ptr.address);
  const auto podIdx = (memoryType != MemoryType::Main) ? extractPodIndex(ptr.address) : anyPod;
  const auto coreIdx = (memoryType == MemoryType::L1SP) ? extractCoreIndex(ptr.address) : anyCore;
  return Place{nodeIdx, podIdx, coreIdx};
}

} // namespace pando
