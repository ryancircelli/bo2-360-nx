/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_shader_cache.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#include "xxhash.h"
#include "rex/filesystem.h"
#include "rex/logging.h"
#include "rex/hash.h"

namespace rex {
namespace graphics {
namespace metal {

std::unique_ptr<MetalArtifactStore> g_metal_artifact_store =
    std::make_unique<MetalArtifactStore>();

namespace {

constexpr uint32_t kArtifactFileMagic = 0x46414D58;    // 'XMAF'
constexpr uint32_t kArtifactRecordMagic = 0x52414D58;  // 'XMAR'
constexpr uint32_t kMaxArtifactPayloadSize = 128u * 1024u * 1024u;

static constexpr size_t kDxilBytecodeSerializedKeySize =
    MetalDxilBytecodeKey::kSerializedWordCount * sizeof(uint64_t);

REXPACKEDSTRUCT(ArtifactFileHeader, {
  uint32_t magic;
  uint32_t version;
});

REXPACKEDSTRUCT(ArtifactRecordHeader, {
  uint32_t magic;
  uint32_t version;
  uint64_t primary_hash;
  uint64_t modification;
  uint32_t stage;
  uint32_t kind;
  uint32_t payload_size;
  uint32_t reserved;
  uint64_t payload_hash;
});

REXPACKEDSTRUCT(DxilBytecodePayloadHeader, {
  uint32_t key_size;
  uint32_t dxil_size;
});

template <typename T>
bool ReadPod(const uint8_t*& ptr, const uint8_t* end, T* out) {
  if (size_t(end - ptr) < sizeof(T)) {
    return false;
  }
  std::memcpy(out, ptr, sizeof(T));
  ptr += sizeof(T);
  return true;
}

bool ReadBytes(const uint8_t*& ptr, const uint8_t* end, void* out,
               size_t size) {
  if (size_t(end - ptr) < size) {
    return false;
  }
  std::memcpy(out, ptr, size);
  ptr += size;
  return true;
}

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t size) {
  if (!size) {
    return;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

}  // namespace

size_t MetalArtifactStore::ArtifactKeyHasher::operator()(
    const ArtifactKey& key) const {
  const uint64_t words[] = {key.primary_hash, key.modification,
                            uint64_t(key.stage), uint64_t(key.kind)};
  return size_t(XXH3_64bits(words, sizeof(words)));
}

MetalArtifactStore::ArtifactKey MetalArtifactStore::MakeDxilBytecodeKey(
    const MetalDxilBytecodeKey& dxil_key) {
  const auto words = dxil_key.SerializeWords();
  XXH128_hash_t hash =
      XXH3_128bits(words.data(), words.size() * sizeof(uint64_t));
  ArtifactKey key;
  key.primary_hash = hash.low64;
  key.modification = hash.high64;
  key.stage = 0;
  key.kind = ArtifactKind::kDxilBytecode;
  return key;
}

void MetalArtifactStore::Initialize(
    const std::filesystem::path& artifact_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  Shutdown();
  artifact_path_ = artifact_path;
  rex::filesystem::CreateParentFolder(artifact_path_);
  artifact_file_ = rex::filesystem::OpenFile(artifact_path_, "a+b");
  if (!artifact_file_) {
    REXLOG_WARN("MetalArtifactStore: failed to open {}",
           rex::path_to_utf8(artifact_path_));
    return;
  }
  initialized_ = LoadIndex();
  if (!initialized_) {
    std::fclose(artifact_file_);
    artifact_file_ = nullptr;
    index_.clear();
    artifact_path_.clear();
  }
}

void MetalArtifactStore::Shutdown() {
  if (artifact_file_) {
    std::fflush(artifact_file_);
    std::fclose(artifact_file_);
    artifact_file_ = nullptr;
  }
  index_.clear();
  artifact_path_.clear();
  initialized_ = false;
}

bool MetalArtifactStore::LoadIndex() {
  if (!artifact_file_) {
    return false;
  }
  rex::filesystem::Seek(artifact_file_, 0, SEEK_SET);

  ArtifactFileHeader header = {};
  if (std::fread(&header, sizeof(header), 1, artifact_file_) != 1 ||
      header.magic != kArtifactFileMagic || header.version != kStorageVersion) {
    rex::filesystem::TruncateStdioFile(artifact_file_, 0);
    header.magic = kArtifactFileMagic;
    header.version = kStorageVersion;
    if (std::fwrite(&header, sizeof(header), 1, artifact_file_) != 1) {
      return false;
    }
    std::fflush(artifact_file_);
    return true;
  }

  uint64_t valid_bytes = sizeof(header);
  while (true) {
    ArtifactRecordHeader record = {};
    if (std::fread(&record, sizeof(record), 1, artifact_file_) != 1) {
      break;
    }
    if (record.magic != kArtifactRecordMagic ||
        record.version != kStorageVersion ||
        record.payload_size > kMaxArtifactPayloadSize) {
      break;
    }
    const int64_t payload_offset = rex::filesystem::Tell(artifact_file_);
    if (payload_offset < 0) {
      break;
    }
    if (!rex::filesystem::Seek(artifact_file_, record.payload_size, SEEK_CUR)) {
      break;
    }
    ArtifactKey key;
    key.primary_hash = record.primary_hash;
    key.modification = record.modification;
    key.stage = record.stage;
    key.kind = static_cast<ArtifactKind>(record.kind);
    index_[key] = RecordLocation{uint64_t(payload_offset), record.payload_size,
                                 record.payload_hash};
    valid_bytes = uint64_t(payload_offset) + record.payload_size;
  }

  rex::filesystem::TruncateStdioFile(artifact_file_, valid_bytes);
  rex::filesystem::Seek(artifact_file_, 0, SEEK_END);
  return true;
}

bool MetalArtifactStore::ReadPayload(const RecordLocation& location,
                                     std::vector<uint8_t>* out) {
  if (!artifact_file_ || !out ||
      location.payload_size > kMaxArtifactPayloadSize) {
    return false;
  }
  out->resize(location.payload_size);
  if (!rex::filesystem::Seek(artifact_file_, int64_t(location.payload_offset),
                            SEEK_SET)) {
    return false;
  }
  if (location.payload_size &&
      std::fread(out->data(), location.payload_size, 1, artifact_file_) != 1) {
    return false;
  }
  if (XXH3_64bits(out->data(), out->size()) != location.payload_hash) {
    return false;
  }
  return true;
}

void MetalArtifactStore::StorePayload(const ArtifactKey& key,
                                      const uint8_t* payload,
                                      size_t payload_size) {
  if (!artifact_file_ || !payload || payload_size == 0 ||
      payload_size > kMaxArtifactPayloadSize) {
    return;
  }
  ArtifactRecordHeader record = {};
  record.magic = kArtifactRecordMagic;
  record.version = kStorageVersion;
  record.primary_hash = key.primary_hash;
  record.modification = key.modification;
  record.stage = key.stage;
  record.kind = uint32_t(key.kind);
  record.payload_size = uint32_t(payload_size);
  record.payload_hash = XXH3_64bits(payload, payload_size);

  rex::filesystem::Seek(artifact_file_, 0, SEEK_END);
  if (std::fwrite(&record, sizeof(record), 1, artifact_file_) != 1) {
    return;
  }
  const int64_t payload_offset = rex::filesystem::Tell(artifact_file_);
  if (payload_offset < 0) {
    return;
  }
  if (std::fwrite(payload, payload_size, 1, artifact_file_) != 1) {
    return;
  }
  std::fflush(artifact_file_);
  index_[key] = RecordLocation{uint64_t(payload_offset), uint32_t(payload_size),
                               record.payload_hash};
}

bool MetalArtifactStore::LoadDxilBytecode(const MetalDxilBytecodeKey& key,
                                          std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  std::vector<uint8_t> payload;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return false;
    }
    ArtifactKey artifact_key = MakeDxilBytecodeKey(key);
    auto it = index_.find(artifact_key);
    if (it == index_.end()) {
      return false;
    }
    if (!ReadPayload(it->second, &payload)) {
      return false;
    }
  }

  const uint8_t* ptr = payload.data();
  const uint8_t* end = ptr + payload.size();
  DxilBytecodePayloadHeader header = {};
  if (!ReadPod(ptr, end, &header)) {
    return false;
  }
  if (header.key_size != kDxilBytecodeSerializedKeySize ||
      header.dxil_size > kMaxArtifactPayloadSize) {
    return false;
  }

  std::array<uint64_t, MetalDxilBytecodeKey::kSerializedWordCount>
      stored_key_words = {};
  if (!ReadBytes(ptr, end, stored_key_words.data(),
                 kDxilBytecodeSerializedKeySize) ||
      stored_key_words != key.SerializeWords()) {
    return false;
  }

  out->resize(header.dxil_size);
  if (header.dxil_size && !ReadBytes(ptr, end, out->data(), header.dxil_size)) {
    return false;
  }
  return ptr == end;
}

void MetalArtifactStore::StoreDxilBytecode(
    const MetalDxilBytecodeKey& key, const std::vector<uint8_t>& dxil_data) {
  if (dxil_data.empty()) {
    return;
  }

  const auto key_words = key.SerializeWords();
  DxilBytecodePayloadHeader header = {};
  header.key_size = uint32_t(key_words.size() * sizeof(uint64_t));
  header.dxil_size = uint32_t(dxil_data.size());

  std::vector<uint8_t> payload;
  payload.reserve(sizeof(header) + header.key_size + dxil_data.size());
  AppendBytes(payload, &header, sizeof(header));
  AppendBytes(payload, key_words.data(), header.key_size);
  AppendBytes(payload, dxil_data.data(), dxil_data.size());

  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    StorePayload(MakeDxilBytecodeKey(key), payload.data(), payload.size());
  }
}

MetalArtifactStore::Stats MetalArtifactStore::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats;
  stats.entry_count = index_.size();
  for (const auto& [key, location] : index_) {
    stats.total_payload_bytes += location.payload_size;
  }
  return stats;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
