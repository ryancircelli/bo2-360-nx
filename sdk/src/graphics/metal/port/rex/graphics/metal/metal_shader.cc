/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license.                                          *
 * See backends/metal/licenses/XeniOS-LICENSE for the full terms.             *
 ******************************************************************************
 */

#include "rex/graphics/metal/metal_shader.h"

#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <mutex>

#ifndef DISPATCH_DATA_DESTRUCTOR_NONE
#define DISPATCH_DATA_DESTRUCTOR_NONE DISPATCH_DATA_DESTRUCTOR_DEFAULT
#endif

#include "rex/assert.h"
#include "rex/filesystem.h"
#include "rex/logging.h"
#include "rex/math.h"
#include "rex/string.h"
#include "rex/graphics/pipeline/shader/dxbc.h"
#include "rex/graphics/flags.h"
#include "rex/ui/metal/metal_api.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {

constexpr uint32_t kAllTranslatedCbvMask =
    (uint32_t(1)
     << (uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices) +
         1)) -
    1;
constexpr uint32_t kDescriptorIndicesCbvSizeBytes = 4096;

void MarkFetchConstantDword(DxbcShader::FetchConstantDwordMask& mask,
                            uint32_t dword_index) {
  if (dword_index >= DxbcShader::kFetchConstantDwordCount) {
    assert_always();
    return;
  }
  mask[dword_index >> 5] |= uint32_t(1) << (dword_index & 31);
}

void MarkVertexFetchConstant(DxbcShader::FetchConstantDwordMask& mask,
                             uint32_t fetch_constant_index) {
  if (fetch_constant_index >= xenos::kVertexFetchConstantCount) {
    assert_always();
    return;
  }
  const uint32_t dword_index = fetch_constant_index * 2;
  MarkFetchConstantDword(mask, dword_index);
  MarkFetchConstantDword(mask, dword_index + 1);
}

void MarkTextureFetchConstant(DxbcShader::FetchConstantDwordMask& mask,
                              uint32_t fetch_constant_index) {
  if (fetch_constant_index >= xenos::kTextureFetchConstantCount) {
    assert_always();
    return;
  }
  const uint32_t dword_index = fetch_constant_index * 6;
  for (uint32_t i = 0; i < 6; ++i) {
    MarkFetchConstantDword(mask, dword_index + i);
  }
}

}  // namespace

MetalShader::MetalShader(xenos::ShaderType shader_type,
                         uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
                         size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

const MetalShader::DrawConstantMetadata& MetalShader::GetDrawConstantMetadata()
    const {
  std::call_once(draw_constant_metadata_once_, [this]() {
    DrawConstantMetadata metadata = {};

    const Shader::ConstantRegisterMap& constant_map = constant_register_map();
    for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap);
         ++i) {
      uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
      uint32_t bit_index;
      while (rex::bit_scan_forward(vfetch_bits_remaining, &bit_index)) {
        vfetch_bits_remaining = rex::clear_lowest_bit(vfetch_bits_remaining);
        MarkVertexFetchConstant(metadata.shader_fetch_constant_dword_mask,
                                i * 32 + bit_index);
      }
    }

    for (const Shader::VertexBinding& binding : vertex_bindings()) {
      MarkVertexFetchConstant(metadata.shader_fetch_constant_dword_mask,
                              binding.fetch_constant);
    }
    for (const Shader::TextureBinding& binding : texture_bindings()) {
      MarkTextureFetchConstant(metadata.shader_fetch_constant_dword_mask,
                               binding.fetch_constant);
    }

    constexpr uint32_t kMaxDescriptorIndexWords =
        kDescriptorIndicesCbvSizeBytes / sizeof(uint32_t);
    metadata.descriptor_indices_word_count = 1;
    for (const DxbcShader::TextureBinding& binding :
         GetTextureBindingsAfterTranslation()) {
      assert_true(binding.bindless_descriptor_index < kMaxDescriptorIndexWords);
      if (binding.bindless_descriptor_index < kMaxDescriptorIndexWords) {
        metadata.descriptor_indices_word_count =
            std::max(metadata.descriptor_indices_word_count,
                     binding.bindless_descriptor_index + 1);
      }
    }
    for (const DxbcShader::SamplerBinding& binding :
         GetSamplerBindingsAfterTranslation()) {
      assert_true(binding.bindless_descriptor_index < kMaxDescriptorIndexWords);
      if (binding.bindless_descriptor_index < kMaxDescriptorIndexWords) {
        metadata.descriptor_indices_word_count =
            std::max(metadata.descriptor_indices_word_count,
                     binding.bindless_descriptor_index + 1);
      }
    }

    const uint32_t used_cbuffer_mask = GetUsedCbufferMaskAfterTranslation();
    metadata.active_cbv_mask = used_cbuffer_mask
                                   ? (used_cbuffer_mask & kAllTranslatedCbvMask)
                                   : kAllTranslatedCbvMask;

    draw_constant_metadata_ = metadata;
  });
  return draw_constant_metadata_;
}

MetalShader::MetalTranslation::~MetalTranslation() {
  if (MTL::Function* f = optimized_function_.exchange(nullptr)) {
    f->release();
  }
  if (optimized_library_) {
    optimized_library_->release();
    optimized_library_ = nullptr;
  }
  if (metal_function_) {
    metal_function_->release();
    metal_function_ = nullptr;
  }
  if (metal_library_) {
    metal_library_->release();
    metal_library_ = nullptr;
  }
}

bool MetalShader::MetalTranslation::InstallOptimizedMetal(
    MTL::Device* device, const std::vector<uint8_t>& metallib) {
  if (!device || metallib.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (!metal_function_ || optimized_function_.load(std::memory_order_relaxed)) {
    return false;
  }
  dispatch_data_t data = dispatch_data_create(
      metallib.data(), metallib.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NS::Error* error = nullptr;
  MTL::Library* library = device->newLibrary(data, &error);
  dispatch_release(data);
  if (!library) {
    return false;
  }
  MTL::Function* function = library->newFunction(
      NS::String::string(function_name_.c_str(), NS::UTF8StringEncoding));
  if (!function) {
    library->release();
    return false;
  }
  static const bool shader_labels = [] {
    const char* v = MetalDebugEnv("SHADER_LABELS");
    return v && v[0] == '1';
  }();
  if (shader_labels) {
    char label[64];
    std::snprintf(label, sizeof(label), "ps:%016llX/%llX/O2",
                  (unsigned long long)shader().ucode_data_hash(),
                  (unsigned long long)modification());
    library->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  optimized_library_ = library;
  optimized_function_.store(function, std::memory_order_release);
  return true;
}

void MetalShader::MetalTranslation::MarkInstalledAsOptimized() {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (metal_function_ && !optimized_function_.load(std::memory_order_relaxed)) {
    metal_function_->retain();
    optimized_function_.store(metal_function_, std::memory_order_release);
  }
}

bool MetalShader::MetalTranslation::InstallNativeMetal(
    MTL::Library* library, MTL::Function* function,
    const std::string& function_name) {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (!library || !function || !metal_function_ ||
      optimized_function_.load(std::memory_order_relaxed)) {
    return false;
  }
  static const bool shader_labels = [] {
    const char* v = MetalDebugEnv("SHADER_LABELS");
    return v && v[0] == '1';
  }();
  if (shader_labels) {
    char label[64];
    std::snprintf(label, sizeof(label), "ps:%016llX/%llX/NX",
                  (unsigned long long)shader().ucode_data_hash(),
                  (unsigned long long)modification());
    library->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  native_function_name_ = function_name;
  optimized_library_ = library;
  native_xlate_.store(true, std::memory_order_release);
  optimized_function_.store(function, std::memory_order_release);
  return true;
}

bool MetalShader::MetalTranslation::InstallNativePrimary(
    MTL::Library* library, MTL::Function* function,
    const std::string& function_name) {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  if (!library || !function || metal_function_ ||
      optimized_function_.load(std::memory_order_relaxed)) {
    return false;
  }
  static const bool shader_labels = [] {
    const char* v = MetalDebugEnv("SHADER_LABELS");
    return v && v[0] == '1';
  }();
  if (shader_labels) {
    char label[64];
    std::snprintf(label, sizeof(label), "%s:%016llX/%llX/NF",
                  shader().type() == xenos::ShaderType::kVertex ? "vs" : "ps",
                  (unsigned long long)shader().ucode_data_hash(),
                  (unsigned long long)modification());
    library->setLabel(NS::String::string(label, NS::UTF8StringEncoding));
  }
  metal_library_ = library;
  metal_function_ = function;
  function_name_ = function_name;
  native_function_name_ = function_name;
  // optimized_library() then returns metal_library_ (as after a cache hit at
  // install), and best_function() the native function.
  function->retain();
  native_xlate_.store(true, std::memory_order_release);
  native_primary_.store(true, std::memory_order_release);
  optimized_function_.store(function, std::memory_order_release);
  return true;
}

void MetalShader::MetalTranslation::SetDxilData(
    std::vector<uint8_t> dxil_data) {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  dxil_data_ = std::move(dxil_data);
}

std::vector<uint8_t> MetalShader::MetalTranslation::GetDxilDataCopy() const {
  std::lock_guard<std::mutex> lock(metal_translation_mutex_);
  return dxil_data_;
}

uint64_t MetalShader::OffsetFetchConstantMasks() const {
  std::call_once(offset_fetch_masks_once_, [this] {
    uint64_t masks = 0;
    // One binding per fetch instruction (Shader::GatherTextureBindingInformation).
    for (const Shader::TextureBinding& b : Shader::texture_bindings()) {
      const ParsedTextureFetchInstruction& f = b.fetch_instr;
      if (f.dimension != xenos::FetchOpDimension::k2D ||
          b.fetch_constant >= 32) {
        continue;
      }
      if (f.attributes.offset_x != 0.0f) {
        masks |= uint64_t(1) << b.fetch_constant;
      }
      if (f.attributes.offset_y != 0.0f) {
        masks |= uint64_t(1) << (32 + b.fetch_constant);
      }
    }
    offset_fetch_masks_ = masks;
  });
  return offset_fetch_masks_;
}

Shader::Translation* MetalShader::CreateTranslationInstance(
    uint64_t modification) {
  return new MetalTranslation(*this, modification);
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
