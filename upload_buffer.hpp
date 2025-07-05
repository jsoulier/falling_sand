#pragma once

#include <SDL3/SDL.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

template<typename T, SDL_GPUBufferUsageFlags U>
class UploadBuffer;
template<typename T>
using UploadBufferVertex = UploadBuffer<T, SDL_GPU_BUFFERUSAGE_VERTEX>;
template<typename T>
using UploadBufferIndex = UploadBuffer<T, SDL_GPU_BUFFERUSAGE_INDEX>;
template<typename T>
using UploadBufferCompute = UploadBuffer<T, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ>;

template<typename T, SDL_GPUBufferUsageFlags U>
class UploadBuffer
{
public:
    UploadBuffer()
        : buffer{nullptr}
        , transferBuffer{nullptr}
        , size{0}
        , capacity{0}
        , data{nullptr}
        , resize{false} {}

    void Destroy(SDL_GPUDevice* device)
    {
        SDL_ReleaseGPUBuffer(device, buffer);
        SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
        buffer = nullptr;
        transferBuffer = nullptr;
        data = nullptr;
    }

    template<typename... Args>
    void Emplace(SDL_GPUDevice* device, Args&&... args)
    {
        if (!data && transferBuffer)
        {
            size = 0;
            resize = false;
            data = static_cast<T*>(SDL_MapGPUTransferBuffer(device, transferBuffer, true));
            if (!data)
            {
                SDL_Log("Failed to map transfer buffer: %s", SDL_GetError());
                return;
            }
        }
        if (size == capacity)
        {
            uint32_t startingCapacity = 10;
            uint32_t capacity = std::max(startingCapacity, size * 2);
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = capacity * sizeof(T);
            SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(device, &info);
            if (!transferBuffer)
            {
                SDL_Log("Failed to create transfer buffer: %s", SDL_GetError());
                return;
            }
            T* data = static_cast<T*>(SDL_MapGPUTransferBuffer(device, transferBuffer, false));
            if (!data)
            {
                SDL_Log("Failed to map transfer buffer: %s", SDL_GetError());
                SDL_ReleaseGPUTransferBuffer(device, transferBuffer);
                return;
            }
            if (this->data)
            {
                std::memcpy(data, this->data, size * sizeof(T));
                SDL_UnmapGPUTransferBuffer(device, this->transferBuffer);
            }
            SDL_ReleaseGPUTransferBuffer(device, this->transferBuffer);
            this->capacity = capacity;
            this->transferBuffer = transferBuffer;
            this->data = data;
            resize = true;
        }
        assert(data);
        data[size++] = T{std::forward<Args>(args)...};
    }

    void Upload(SDL_GPUDevice* device, SDL_GPUCopyPass* copyPass)
    {
        if (resize)
        {
            SDL_ReleaseGPUBuffer(device, buffer);
            buffer = nullptr;
            SDL_GPUBufferCreateInfo info{};
            info.usage = U;
            info.size = capacity * sizeof(T);
            buffer = SDL_CreateGPUBuffer(device, &info);
            if (!buffer)
            {
                SDL_Log("Failed to create buffer: %s", SDL_GetError());
                return;
            }
            resize = false;
        }
        if (data)
        {
            SDL_UnmapGPUTransferBuffer(device, transferBuffer);
            data = nullptr;
        }
        if (size)
        {
            SDL_GPUTransferBufferLocation location{};
            SDL_GPUBufferRegion region{};
            location.transfer_buffer = transferBuffer;
            region.buffer = buffer;
            region.size = size * sizeof(T);
            SDL_UploadToGPUBuffer(copyPass, &location, &region, true);
        }
    }

    SDL_GPUBuffer* getBuffer() const
    {
        return buffer;
    }

    uint32_t getSize() const
    {
        return size;
    }

private:
    SDL_GPUBuffer* buffer;
    SDL_GPUTransferBuffer* transferBuffer;
    uint32_t size;
    uint32_t capacity;
    T* data;
    bool resize;
};