#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <cstdint>
#include <mutex>
#include <thread>

#include "config.hpp"
#include "shader.hpp"
#include "upload_buffer.hpp"

static_assert(FRAMES == 2);

static SDL_Window* window;
static SDL_GPUDevice* device;
static SDL_GPUComputePipeline* updatePipeline;
static SDL_GPUComputePipeline* clearPipeline;
static SDL_GPUComputePipeline* uploadPipeline;
static SDL_GPUGraphicsPipeline* renderPipeline;
static SDL_GPUTexture* updateTextures[FRAMES];
static SDL_GPUTexture* renderTexture;
static ComputeUploadBuffer<uint32_t> uploadBuffer;
static volatile uint64_t speed = 1000;
static volatile int readFrame = 0;
static volatile int writeFrame = 1;

static bool Init()
{
    SDL_SetAppMetadata("Falling Sand", nullptr, nullptr);
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("Failed to initialize SDL: %s", SDL_GetError());
        return false;
    }
    window = SDL_CreateWindow("Falling Sand", WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        SDL_Log("Failed to create window: %s", SDL_GetError());
        return false;
    }
#if defined(SDL_PLATFORM_WIN32)
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    /* TODO: */
    // device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXIL, true, nullptr);
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
    SDL_GPUShader* fragShader = LoadShader(device, "render.frag");
    SDL_GPUShader* vertShader = LoadShader(device, "render.vert");
    if (!fragShader || !vertShader)
    {
        SDL_Log("Failed to create shader(s)");
        return false;
    }
    SDL_GPUColorTargetDescription target{};
    SDL_GPUGraphicsPipelineCreateInfo info{};
    target.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.vertex_shader = vertShader;
    info.fragment_shader = fragShader;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    renderPipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    updatePipeline = LoadComputePipeline(device, "update.comp");
    clearPipeline = LoadComputePipeline(device, "clear.comp");
    uploadPipeline = LoadComputePipeline(device, "upload.comp");
    if (!renderPipeline || !updatePipeline || !clearPipeline || !uploadPipeline)
    {
        SDL_Log("Failed to create pipelines(s): %s", SDL_GetError());
        return false;
    }
    SDL_ReleaseGPUShader(device, fragShader);
    SDL_ReleaseGPUShader(device, vertShader);
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

static bool CreateResources()
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
    return true;
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
    uint32_t width;
    uint32_t height;
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
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize.x = width;
        io.DisplaySize.y = height;
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("Settings");
        ImGui::End();
        ImGui::Render();
        ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), commandBuffer);
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
        /* TODO: which is better, read or write? */
        SDL_BindGPUFragmentStorageTextures(renderPass, 0, &updateTextures[readFrame], 1);
        SDL_DrawGPUPrimitives(renderPass, 4, 1, 0, 0);
        SDL_EndGPURenderPass(renderPass);
    }
    {
        SDL_GPUBlitInfo info{};
        info.load_op = SDL_GPU_LOADOP_DONT_CARE;
        info.source.texture = renderTexture;
        info.source.w = WIDTH;
        info.source.h = HEIGHT;
        info.destination.texture = swapchainTexture;
        info.destination.w = width;
        info.destination.h = height;
        info.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commandBuffer, &info);
    }
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

static void HandleMotion(SDL_Event* event)
{
    int x1 = event->motion.x - event->motion.xrel;
    int y1 = event->motion.y - event->motion.yrel;
    int x2 = event->motion.x;
    int y2 = event->motion.y;
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
    // for (uint32_t offset = 0; offset < 4; offset++)
    // {
    //     SDL_GPUStorageTextureReadWriteBinding binding{};
    //     binding.texture = updateTextures[writeFrame];
    //     SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &binding, 1, nullptr, 0);
    //     if (!computePass)
    //     {
    //         SDL_Log("Failed to begin compute pass: %s", SDL_GetError());
    //         SDL_CancelGPUCommandBuffer(commandBuffer);
    //         return;
    //     }
    //     int groupsX = (WIDTH / 2 + UPDATE_THREADS - 1) / UPDATE_THREADS;
    //     int groupsY = (HEIGHT / 2 + UPDATE_THREADS - 1) / UPDATE_THREADS;
    //     SDL_BindGPUComputePipeline(computePass, updatePipeline);
    //     SDL_PushGPUComputeUniformData(commandBuffer, 0, &offset, sizeof(offset));
    //     SDL_DispatchGPUCompute(computePass, groupsX, groupsY, 1);
    //     SDL_EndGPUComputePass(computePass);
    // }
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
    if (!CreateResources())
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
                if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)
                {
                    std::lock_guard lock{mutex};
                    HandleMotion(&event);
                }
                break;
            }
        }
        if (!running)
        {
            break;
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
    SDL_ReleaseGPUComputePipeline(device, updatePipeline);
    SDL_ReleaseGPUComputePipeline(device, clearPipeline);
    SDL_ReleaseGPUComputePipeline(device, uploadPipeline);
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