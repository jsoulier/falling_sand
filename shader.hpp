#pragma once

#include <SDL3/SDL.h>

#include <string_view>

SDL_GPUShader* ShaderLoad(SDL_GPUDevice* device, const std::string_view& name);
SDL_GPUComputePipeline* ShaderLoadCompute(SDL_GPUDevice* device, const std::string_view& name);