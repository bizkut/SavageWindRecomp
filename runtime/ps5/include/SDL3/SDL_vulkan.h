// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan.h>
#include "../../sdl_shim.h"
// runtime/ps5/window.c: the instance extensions of a VK_KHR_display surface, and that surface on VideoOut
const char *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *count);
bool SDL_Vulkan_CreateSurface(SDL_Window *window, VkInstance instance, const VkAllocationCallbacks *allocator,
                              VkSurfaceKHR *surface);
