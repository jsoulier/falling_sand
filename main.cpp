#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"
#include "shader.hpp"
#include "upload_buffer.hpp"

static_assert(FRAMES == 2);
static_assert(EMPTY == 0);

static SDL_Window* window;
static SDL_GPUDevice* device;
static SDL_GPUComputePipeline* updatePipeline;
static SDL_GPUComputePipeline* clearPipeline;
static SDL_GPUComputePipeline* uploadPipeline;
static SDL_GPUComputePipeline* blurPipeline;
static SDL_GPUGraphicsPipeline* renderPipeline;
static SDL_GPUTexture* updateTextures[FRAMES];
static SDL_GPUTexture* renderTexture;
static SDL_GPUTexture* blurTexture;
static ComputeUploadBuffer<uint32_t> uploadBuffer;
static uint32_t width;
static uint32_t height;
static volatile int speed = 16;
static volatile int readFrame = 0;
static volatile int writeFrame = 1;
static int red = 230;
static int green = 0;
static int blue = 230;
static int type = SAND;
static float letterboxX;
static float letterboxY;
static float letterboxW = 0.0f;
static float letterboxH = 0.0f;
static int radius = 10;
static bool imguiFocused;

static bool Init()
{
    SDL_SetAppMetadata("Falling Sand", nullptr, nullptr);
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("Falling Sand", WIDTH * 2, HEIGHT * 2, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return false;
    }
#if defined(SDL_PLATFORM_WIN32)
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, true, nullptr);
#elif defined(SDL_PLATFORM_APPLE)
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
#else
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
#endif
    if (!device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return false;
    }
    if (!SDL_ClaimWindowForGPUDevice(device, window))
    {
        SDL_Log("Failed to create swapchain: %s", SDL_GetError());
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    ImGui_ImplSDLGPU3_Init(&info);
    return true;
}

static bool CreatePipelines()
{
    SDL_GPUShader* renderShader = LoadShader(device, "render.frag");
    SDL_GPUShader* fullscreenShader = LoadShader(device, "fullscreen.vert");
    if (!renderShader || !fullscreenShader)
    {
        SDL_Log("Failed to create shader(s)");
        return false;
    }
    SDL_GPUColorTargetDescription target{};
    SDL_GPUGraphicsPipelineCreateInfo info{};
    target.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.vertex_shader = fullscreenShader;
    info.fragment_shader = renderShader;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    renderPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    updatePipeline = LoadComputePipeline(device, "update.comp");
    clearPipeline = LoadComputePipeline(device, "clear.comp");
    uploadPipeline = LoadComputePipeline(device, "upload.comp");
    blurPipeline = LoadComputePipeline(device, "blur.comp");
    if (!renderPipeline || !updatePipeline || !clearPipeline || !uploadPipeline || !blurPipeline)
    {
        SDL_Log("Failed to create pipelines(s): %s", SDL_GetError());
        return false;
    }
    SDL_ReleaseGPUShader(device, renderShader);
    SDL_ReleaseGPUShader(device, fullscreenShader);
    return true;
}

static void ClearTexture(SDL_GPUTexture* texture, SDL_GPUCommandBuffer* inCommandBuffer = nullptr)
{
    SDL_GPUCommandBuffer* commandBuffer = inCommandBuffer;
    if (!inCommandBuffer)
    {
        commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (!commandBuffer)
        {
            SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
            return;
        }
    }
    SDL_GPUStorageTextureReadWriteBinding binding{};
    binding.texture = texture;
    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &binding, 1, nullptr, 0);
    if (!computePass)
    {
        SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
        if (!inCommandBuffer)
        {
            SDL_CancelGPUCommandBuffer(commandBuffer);
        }
        return;
    }
    int groupsX = (WIDTH + UPDATE_THREADS - 1) / UPDATE_THREADS;
    int groupsY = (HEIGHT + UPDATE_THREADS - 1) / UPDATE_THREADS;
    SDL_BindGPUComputePipeline(computePass, clearPipeline);
    SDL_DispatchGPUCompute(computePass, groupsX, groupsY, 1);
    SDL_EndGPUComputePass(computePass);
    if (!inCommandBuffer)
    {
        SDL_SubmitGPUCommandBuffer(commandBuffer);
    }
}

static bool CreateTextures()
{
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.width = WIDTH;
    info.height = HEIGHT;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    for (int i = 0; i < FRAMES; i++)
    {
        info.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
        info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ |
            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
            SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
        updateTextures[i] = SDL_CreateGPUTexture(device, &info);
        if (!updateTextures[i])
        {
            SDL_Log("Failed to create texture: %s", SDL_GetError());
            return false;
        }
        ClearTexture(updateTextures[i]);
    }
    info.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    renderTexture = SDL_CreateGPUTexture(device, &info);
    if (!renderTexture)
    {
        SDL_Log("Failed to create texture: %s", SDL_GetError());
        return false;
    }
    info.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    info.usage = SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    blurTexture = SDL_CreateGPUTexture(device, &info);
    if (!blurTexture)
    {
        SDL_Log("Failed to create texture: %s", SDL_GetError());
        return false;
    }
    return true;
}

static void RenderImGui(SDL_GPUCommandBuffer* commandBuffer)
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = width;
    io.DisplaySize.y = height;
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Settings");
    imguiFocused = ImGui::IsWindowFocused();
    int delay = speed;
    ImGui::SliderInt("Delay", &delay, 16, 1000);
    speed = delay;
    ImGui::SliderInt("Radius", &radius, 1, 100);
    ImGui::SliderInt("Red", &red, 0, 255);
    ImGui::SliderInt("Green", &green, 0, 255);
    ImGui::SliderInt("Blue", &blue, 0, 255);
    if (ImGui::Button("Empty"))
    {
        type = EMPTY;
    }
    if (ImGui::Button("Sand"))
    {
        type = SAND;
    }
    if (ImGui::Button("Stone"))
    {
        type = STONE;
    }
    ImGui::End();
    ImGui::Render();
    ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), commandBuffer);
}

static void LetterboxBlit(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* swapchainTexture)
{
    SDL_GPUBlitInfo info{};
    const float renderRatio = static_cast<float>(WIDTH) / HEIGHT;
    const float swapchainRatio = static_cast<float>(width) / height;
    float scale = 0.0f;
    if (renderRatio > swapchainRatio)
    {
        scale = static_cast<float>(width) / WIDTH;
        letterboxW = width;
        letterboxH = HEIGHT * scale;
        letterboxX = 0.0f;
        letterboxY = (height - letterboxH) / 2.0f;
    }
    else
    {
        scale = static_cast<float>(height) / HEIGHT;
        letterboxH = height;
        letterboxW = WIDTH * scale;
        letterboxX = (width - letterboxW) / 2.0f;
        letterboxY = 0.0f;
    }
    SDL_FColor clearColor = {0.02f, 0.02f, 0.02f, 1.0f};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.clear_color = clearColor;
    info.source.texture = renderTexture;
    info.source.w = WIDTH;
    info.source.h = HEIGHT;
    info.destination.texture = swapchainTexture;
    info.destination.x = letterboxX;
    info.destination.y = letterboxY;
    info.destination.w = letterboxW;
    info.destination.h = letterboxH;
    info.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commandBuffer, &info);
}

static void Upload(SDL_GPUCommandBuffer* commandBuffer)
{
    uploadBuffer.Upload(device, commandBuffer);
    if (!uploadBuffer.GetBufferSize() || imguiFocused)
    {
        return;
    }
    SDL_GPUStorageTextureReadWriteBinding binding{};
    binding.texture = updateTextures[readFrame];
    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &binding, 1, nullptr, 0);
    if (!computePass)
    {
        SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
        return;
    }
    int groupsX = (uploadBuffer.GetBufferSize() + UPLOAD_THREADS - 1) / UPLOAD_THREADS;
    SDL_GPUBuffer* buffer = uploadBuffer.GetBuffer();
    uint32_t bufferSize = uploadBuffer.GetBufferSize();
    uint32_t particle = 0;
    particle |= red << 0;
    particle |= green << 8;
    particle |= blue << 16;
    particle |= type << 24;
    int32_t time = SDL_GetTicks();
    SDL_BindGPUComputePipeline(computePass, uploadPipeline);
    SDL_BindGPUComputeStorageBuffers(computePass, 0, &buffer, 1);
    SDL_PushGPUComputeUniformData(commandBuffer, 0, &bufferSize, sizeof(bufferSize));
    SDL_PushGPUComputeUniformData(commandBuffer, 1, &particle, sizeof(particle));
    SDL_PushGPUComputeUniformData(commandBuffer, 2, &radius, sizeof(radius));
    SDL_PushGPUComputeUniformData(commandBuffer, 3, &time, sizeof(time));
    SDL_DispatchGPUCompute(computePass, groupsX, 1, 1);
    SDL_EndGPUComputePass(computePass);
}

static void Render()
{
    SDL_WaitForGPUSwapchain(device, window);
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return;
    }
    SDL_GPUTexture* swapchainTexture;
    if (!SDL_AcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &width, &height))
    {
        SDL_Log("Failed to acquire swapchain texture: %s", SDL_GetError());
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return;
    }
    if (!swapchainTexture || !width || !height)
    {
        /* happens on minimize */
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return;
    }
    RenderImGui(commandBuffer);
    Upload(commandBuffer);
    {
        SDL_GPUStorageTextureReadWriteBinding binding{};
        binding.texture = blurTexture;
        SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &binding, 1, nullptr, 0);
        if (!computePass)
        {
            SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return;
        }
        int groupsX = (WIDTH + UPDATE_THREADS - 1) / UPDATE_THREADS;
        int groupsY = (HEIGHT + UPDATE_THREADS - 1) / UPDATE_THREADS;
        SDL_BindGPUComputePipeline(computePass, blurPipeline);
        SDL_BindGPUComputeStorageTextures(computePass, 0, &updateTextures[readFrame], 1);
        SDL_DispatchGPUCompute(computePass, groupsX, groupsY, 1);
        SDL_EndGPUComputePass(computePass);
    }
    {
        SDL_GPUColorTargetInfo info{};
        info.texture = renderTexture;
        info.load_op = SDL_GPU_LOADOP_DONT_CARE;
        info.store_op = SDL_GPU_STOREOP_STORE;
        info.cycle = true;
        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &info, 1, nullptr);
        if (!renderPass)
        {
            SDL_Log("Failed to begin render pass: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return;
        }
        SDL_BindGPUGraphicsPipeline(renderPass, renderPipeline);
        SDL_BindGPUFragmentStorageTextures(renderPass, 0, &blurTexture, 1);
        SDL_DrawGPUPrimitives(renderPass, 4, 1, 0, 0);
        SDL_EndGPURenderPass(renderPass);
    }
    LetterboxBlit(commandBuffer, swapchainTexture);
    {
        SDL_GPUColorTargetInfo info{};
        info.texture = swapchainTexture;
        info.load_op = SDL_GPU_LOADOP_LOAD;
        info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &info, 1, nullptr);
        if (!renderPass)
        {
            SDL_Log("Failed to begin render pass: %s", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(commandBuffer);
            return;
        }
        ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), commandBuffer, renderPass);
        SDL_EndGPURenderPass(renderPass);
    }
    SDL_SubmitGPUCommandBuffer(commandBuffer);
}

static void Upload(float x1, float y1, float x2, float y2)
{
    static constexpr float Step = 1.0f;
    float dx = x2 - x1;
    float dy = y2 - y1;
    float distance = std::sqrtf(dx * dx + dy * dy);
    if (distance < std::numeric_limits<float>::epsilon())
    {
        distance = 1.0f;
    }
    float scale = static_cast<float>(letterboxW) / WIDTH;
    for (float i = 0.0f; i < distance; i += Step)
    {
        float t = i / distance;
        float screenX = x1 + t * dx;
        float screenY = y1 + t * dy;
        float x = (screenX - letterboxX) / scale;
        float y = (screenY - letterboxY) / scale;
        uint32_t position = 0;
        position |= static_cast<uint32_t>(x) << 0;
        position |= static_cast<uint32_t>(y) << 16;
        uploadBuffer.Emplace(device, position);
    }
}

static void Update()
{
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!commandBuffer)
    {
        SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
        return;
    }
    ClearTexture(updateTextures[writeFrame], commandBuffer);
    for (uint32_t offset = 0; offset < 9; offset++)
    {
        SDL_GPUStorageTextureReadWriteBinding binding{};
        binding.texture = updateTextures[writeFrame];
        SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &binding, 1, nullptr, 0);
        if (!computePass)
        {
            SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
            SDL_CancelGPUCommandBuffer(commandBuffer);
            return;
        }
        int groupsX = (WIDTH / 3 + UPDATE_THREADS - 1) / UPDATE_THREADS;
        int groupsY = (HEIGHT / 3 + UPDATE_THREADS - 1) / UPDATE_THREADS;
        int32_t time = SDL_GetTicks();
        SDL_BindGPUComputePipeline(computePass, updatePipeline);
        SDL_BindGPUComputeStorageTextures(computePass, 0, &updateTextures[readFrame], 1);
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &offset, sizeof(offset));
        SDL_PushGPUComputeUniformData(commandBuffer, 1, &time, sizeof(time));
        SDL_DispatchGPUCompute(computePass, groupsX, groupsY, 1);
        SDL_EndGPUComputePass(computePass);
    }
    SDL_SubmitGPUCommandBuffer(commandBuffer);
    readFrame = (readFrame + 1) % FRAMES;
    writeFrame = (writeFrame + 1) % FRAMES;
}

int main(int argc, char** argv)
{
    if (!Init())
    {
        SDL_Log("Failed to initialize");
        return 1;
    }
    if (!CreatePipelines())
    {
        SDL_Log("Failed to create pipelines");
        return 1;
    }
    if (!CreateTextures())
    {
        SDL_Log("Failed to create resources");
        return 1;
    }
    bool running = true;
    std::mutex mutex;
    std::thread thread = std::thread([&]()
    {
        uint64_t time1 = SDL_GetTicks();
        uint64_t time2 = 0;
        while (running)
        {
            time2 = SDL_GetTicks();
            uint64_t delta = time2 - time1;
            time1 = time2;
            if (delta < speed)
            {
                SDL_Delay(speed - delta);
            }
            std::lock_guard lock{mutex};
            Update();
        }
    });
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.state & SDL_BUTTON_LMASK)
                {
                    float x1 = event.motion.x - event.motion.xrel;
                    float y1 = event.motion.y - event.motion.yrel;
                    float x2 = event.motion.x;
                    float y2 = event.motion.y;
                    Upload(x1, y1, x2, y2);
                }
                break;
            }
        }
        if (!running)
        {
            break;
        }
        float x;
        float y;
        if (SDL_GetMouseState(&x, &y) & SDL_BUTTON_LMASK)
        {
            Upload(x, y, x, y);
        }
        std::lock_guard lock{mutex};
        Render();
    }
    SDL_HideWindow(window);
    thread.join();
    uploadBuffer.Destroy(device);
    for (int i = 0; i < FRAMES; i++)
    {
        SDL_ReleaseGPUTexture(device, updateTextures[i]);
    }
    SDL_ReleaseGPUTexture(device, renderTexture);
    SDL_ReleaseGPUTexture(device, blurTexture);
    SDL_ReleaseGPUComputePipeline(device, updatePipeline);
    SDL_ReleaseGPUComputePipeline(device, clearPipeline);
    SDL_ReleaseGPUComputePipeline(device, uploadPipeline);
    SDL_ReleaseGPUComputePipeline(device, blurPipeline);
    SDL_ReleaseGPUGraphicsPipeline(device, renderPipeline);
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}