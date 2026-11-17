// SPDX-License-Identifier: BSD-3-Clause
// The SDK's Vulkan host layer as the native renderer sees it (v0.11: rex::gpu::vulkan).
// xenos_export.h comes first: renderdoc_api.h, included by vulkan_instance.h, needs REX_GPU_XENOS_API on macOS.
#pragma once

#include "gpu/xenos/xenos_export.h"
#include "gpu/xenos/host/presenter.h"
#include "gpu/xenos/host/vulkan/vulkan_device.h"
#include "gpu/xenos/host/vulkan/vulkan_objects.h"
#include "gpu/xenos/host/vulkan/vulkan_presenter.h"
#include "gpu/xenos/host/vulkan/vulkan_provider.h"
