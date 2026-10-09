#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Extent = 64u;
constexpr std::uint32_t Stride = 16u;
constexpr std::size_t BlockBytes = 65536u;
alignas(256) std::array<std::byte, Extent * Extent * 4u> pixels{};
constexpr std::array<std::array<float, 4>, 3> triangle{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<float, 4> offset{0.0f, 0.0f, 0.0f, 1.0f};
alignas(256) constexpr std::array<std::uint32_t, 8> vertexCode{0xbe822100u, 0x06081104u, 0x060a1305u, 0x060c1506u, 0x060e1707u, 0xf80008cfu, 0x07060504u, 0xbf810000u};
constexpr std::array<std::uint32_t, 5> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};
constexpr std::array<std::size_t, 4> corners{0u, Extent - 1u, (Extent - 1u) * Extent, Extent * Extent - 1u};
constexpr std::size_t center = (Extent / 2u) * Extent + Extent / 2u;

std::byte* AllocateWatched() {
#ifdef _WIN32
    auto* block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, BlockBytes));
    GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
    void* raw = mmap(nullptr, 2u * BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(raw != MAP_FAILED, "cannot map the vertex buffers");
    auto* block = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(raw) + BlockBytes - 1u) & ~static_cast<std::uintptr_t>(BlockBytes - 1u));
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
#endif
    Require(block != nullptr && AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), BlockBytes), "the vertex buffers are not write-watched");
    return block;
}

std::array<std::uint32_t, 4> Descriptor(std::span<const std::byte> storage, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (Stride << 16u), records, 0x01000facu | (77u << 12u)};
}

bool Red(std::size_t pixel) {
    const auto index = pixel * 4u;
    return pixels[index] == std::byte{255} && pixels[index + 1u] == std::byte{0} && pixels[index + 2u] == std::byte{0} && pixels[index + 3u] == std::byte{255};
}

void RequireOffsetApplied(const char* message) {
    Require(Red(center), message);
    for (const auto corner : corners) Require(pixels[corner * 4u + 3u] == std::byte{0}, message);
}

ShaderRecompiler::RecompileResult CompileVertex(AgcDriver::VulkanDevice& device, std::span<const std::byte> positions, std::span<const std::byte> offsets, std::uint32_t offsetRecords) {
    ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
    vertexInfo.fetchEmbedded = true;
    vertexInfo.fetchAttribReg = 4u;
    vertexInfo.fetchBufferReg = 10u;
    vertexInfo.resourcesNum = 2u;
    vertexInfo.resources[0].fields = Descriptor(positions, 6u);
    vertexInfo.resources[1].fields = Descriptor(offsets, offsetRecords);
    vertexInfo.resourcesDst[0] = {4, 4, 0, 0};
    vertexInfo.resourcesDst[1] = {8, 4, 1, 0};
    const std::array<std::uint32_t, 4> userData{0x10000000u, 0x00000001u, 0u, 0u};
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(vertexCode.data()), std::as_bytes(std::span(vertexCode))}}};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, memory}, device.Target(), {0u, 0u, 0u, 96u}};
    request.useCache = false;
    auto vertex = ShaderRecompiler::Recompile(request);
    Require(vertex.vertexAttributes.size() == 2u, "fetch-shader vertex inputs did not become Vulkan vertex attributes");
    return vertex;
}

void Run(AgcDriver::VulkanDevice& device) {
    auto* block = AllocateWatched();
    const std::span<std::byte> positions(block, 6u * Stride);
    const std::span<std::byte> offsets(block + BlockBytes / 2u, 6u * Stride);
    for (std::size_t vertex = 0; vertex < 6u; ++vertex) std::memcpy(positions.data() + vertex * Stride, triangle[vertex % 3u].data(), 16u);
    for (std::size_t vertex = 0; vertex < 6u; ++vertex) std::memcpy(offsets.data() + vertex * Stride, offset.data(), 16u);
    const auto threeRecords = CompileVertex(device, positions, offsets, 3u);
    const auto sixRecords = CompileVertex(device, positions, offsets, 6u);
    const auto pushBytes = static_cast<std::uint32_t>(threeRecords.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.targetOutputMode[0] = 9u;
    pixelInfo.targetExportMapping.fill(0xe4u);
    ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}}, {32u, 0u, {}, std::nullopt, pixelInfo, std::nullopt, {}}, device.Target(), {0u, 0u, pushBytes, 128u - pushBytes}};
    pixelRequest.useCache = false;
    const auto pixel = ShaderRecompiler::Recompile(pixelRequest);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> threeRecordShaders{{{ShaderStage::Vertex, &threeRecords, 0u}, {ShaderStage::Fragment, &pixel, pushBytes}}};
    const std::array<AgcDriver::Graphics::CompiledShader, 2> sixRecordShaders{{{ShaderStage::Vertex, &sixRecords, 0u}, {ShaderStage::Fragment, &pixel, pushBytes}}};
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Extent, Extent}, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Extent, Extent};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, static_cast<float>(Extent), static_cast<float>(Extent), -static_cast<float>(Extent), 0.0f, 1.0f};
    state.scissor = {{0, 0}, {Extent, Extent}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    AgcDriver::Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};
    pixels.fill(std::byte{0});
    device.Draw(state, draw, threeRecordShaders);
    device.WaitIdle();
    RequireOffsetApplied("the in-range offset was not applied");
    draw.firstVertex = 3u;
    pixels.fill(std::byte{0});
    device.Draw(state, draw, threeRecordShaders);
    device.WaitIdle();
    for (std::size_t index = 0; index < Extent * Extent; ++index) Require(Red(index), "a vertex attribute past num_records was not read as zeros");
    pixels.fill(std::byte{0});
    device.Draw(state, draw, sixRecordShaders);
    device.WaitIdle();
    RequireOffsetApplied("an in-range vertex attribute read the zeros of an earlier fetch past num_records at the same address");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "vertex fetch past records tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
