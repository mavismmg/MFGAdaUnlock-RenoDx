#pragma once
#include "cuda_route.h"
#include "xefg_cuda_params.h"
#include <array>
#include <cstring>
#include <iterator>
#include <string_view>

namespace xefg_cuda
{
// ---- the layer table ------------------------------------------------------------------------------
// One entry per Intel program the kernels reproduce byte for byte (tests/xess_cuda). The SHA-256 is of
// the DXIL container the provider passes to CreateComputePipelineState: the 11 U-Net programs and 2 flow
// programs from libxess_fg 3.0.2 (the same bytes in OptiScaler's provider), and 4 of RTXMFG's embedded
// ported flow programs. Every program binds root parameter 0 = table (t0, [t4,] u0), 1 = table (t1, t2,
// t3), 2 = root constants b0. Five layers also match RTXMFG's own program (xess/port/unet_conv.hlsl, the
// pins in xess/generate_xess_shaders.py), which ShaderPort puts in place of Intel's: same root signature,
// same Dispatch, byte-identical output.
enum class Family : uint8_t { Pool, Decoder, Flow };
inline constexpr const char* kKernels[] = {"L_aa016078", "L_198d6bf8", "L_dcd8becf", "L_efbe3788", "L_50f0c205", "L_a3c3caa1",
    "L_9481d452", "L_06e97933", "L_3ed03daf", "L_748b0baf", "L_cb391582", "L_fc1b8a04", "L_3459485", "L_3452733",
    "L_flow16_acc17", "L_3453044"};
inline constexpr uint32_t kFlowConstants = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 9) | (1u << 10);
struct Layer
{
    LayerInfo info;
    uint8_t kernel;            // index into kKernels
    Family family;
    uint16_t intelW, intelH;   // output positions per Intel thread group (x, y)
    uint16_t groupsZ;          // the Intel Dispatch's z
    uint16_t tileW, tileH;     // kernel tile (output positions)
    uint16_t gridZ;            // kernel grid z
    uint32_t constants;        // root constant words the program reads (mask)
    // The byte sizes of the program and of the own program (0: none). A program of any other size cannot
    // have the layer's SHA-256, so the route hashes only these sizes, whoever creates the pipeline.
    uint32_t bytes, ownBytes;
};
inline constexpr Layer kLayers[] = {
    {{"aa016078", "8c2efb9e955f39379a91887ed3d90412c4b7ae2c142a874e3da5f699de197f7c", "L_aa016078",
        "ce847b58086ce7e9d25f7d8b90e989247cd4582226506d9552112f1ef4130643"}, 0, Family::Pool, 16, 16, 1, 8, 16, 1, 0, 47728, 92932},
    {{"198d6bf8", "aac1354a8004fe0aae02947a372f622a84b8d61c1691fc77d7fddedaf74ad36b", "L_198d6bf8"}, 1, Family::Pool, 4, 16, 1, 8, 8, 1, 0, 53304, 0},
    {{"dcd8becf", "ba5c8b9ea1aa3aa3ce12acb4c2ff4c7c32d2bccab304c539246ad83f737523a0", "L_dcd8becf"}, 2, Family::Pool, 4, 8, 1, 8, 16, 2, 0, 54196, 0},
    {{"efbe3788", "40e159414d5b839ba7e0e07cbf8ff4ef425ee026c216b2928ef3bb6dd0e20d40", "L_efbe3788",
        "efa8da5ef5f90c62b789b0fed8e77e2ce043e558d164cc552ca7fb9127cedb00"}, 3, Family::Pool, 8, 4, 1, 8, 8, 4, 0, 54224, 54388},
    {{"50f0c205", "fea66ef764b267613725fd37be22dc279b23280ffa132e53c285a798ac27df5e", "L_50f0c205"}, 4, Family::Pool, 16, 16, 1, 16, 4, 4, 0, 56256, 0},
    {{"a3c3caa1", "51f5754a8f4dd34791351f2156f121c4cf119a5fae63bce76266a66b7a097d9a", "L_a3c3caa1"}, 5, Family::Pool, 16, 8, 1, 16, 8, 1, 0, 56280, 0},
    {{"9481d452", "8cffea0f51779e7ddea756043802e9635d6b853d6aee2176ffb1f25a605750c0", "L_9481d452"}, 6, Family::Pool, 8, 8, 1, 16, 16, 1, 0, 28764, 0},
    {{"06e97933", "3cdf1a4746fc91bf161f320e0256dbbccc954e774add6ac53caf50b7d1be69f7", "L_06e97933"}, 7, Family::Pool, 8, 8, 1, 16, 16, 1, 0, 35316, 0},
    {{"3ed03daf", "01c8beac27d3f3f82ecba8f3dc2d9d8b60d4c175a41df3c24a24e8643f5578f5", "L_3ed03daf",
        "9900c30febf95bfa81b4da1178e5773863116beba4d3f032897da4254b6d8926"}, 8, Family::Decoder, 16, 32, 1, 16, 16, 1, 3, 35852, 37760},
    {{"748b0baf", "8291bbc36b1100b07d0788f35a3a9fadfbfcc2267ff0ccb7afb114d76f5d69fe", "L_748b0baf",
        "e78063f97ad2c0913412f07589cd17d0f2a6ebeeb6edd70647a69cca6b6a02bf"}, 9, Family::Decoder, 8, 16, 1, 16, 8, 1, 3, 57376, 38104},
    {{"cb391582", "a60f7488872a675d66afd2bec02b0b89e3cc38c1c4ed1d25d745a7b2fe677412", "L_cb391582",
        "7a29fbf311cd8dee5b8c1cfdadf31ee983092a512cf9c10845a079b457eb62a9"}, 10, Family::Decoder, 32, 8, 1, 16, 16, 1, 3, 57372, 38124},
    {{"fc1b8a04", "8c1d7667722c8692b376ed7511723b5e58efd61d4a4b8c4b61a1233f75df76ee", "L_fc1b8a04"}, 11, Family::Flow, 8, 8, 28, 16, 8, 7, kFlowConstants, 13704, 0},
    {{"3459485", "51667b6d9955036a10cbd4e363881252de8ca5d3b6f210a7a3ee6cab719629a6", "L_3459485"}, 12, Family::Flow, 8, 8, 28, 16, 8, 7, kFlowConstants, 15948, 0},
    {{"3452733", "f4aa535a6fc74410ae8b684eb881b9fd511c6a1ac1c784cc95bd8f7fd8ea0f61", "L_3452733"}, 13, Family::Flow, 8, 8, 28, 16, 8, 7, kFlowConstants, 15948, 0},
    {{"55e5d6b9", "09887269759ef4d7588315f29e71126c899fde79f9edb336eedd598c662d4aea", "L_flow16_acc17"}, 14, Family::Flow, 8, 8, 4, 16, 8, 2, kFlowConstants, 23912, 0},
    {{"34517926", "1bebcab193714ee264da7dc0a96e313d74ad4c71017fdea5314c8b07f6b56394", "L_flow16_acc17"}, 14, Family::Flow, 8, 8, 4, 16, 8, 2, kFlowConstants, 26164, 0},
    {{"3453044", "13abff5d769ef56f99cb9ee7825968872e28395d77a6d344053c06cb08b8064c", "L_3453044"}, 15, Family::Flow, 8, 8, 4, 16, 8, 2, kFlowConstants, 26164, 0},
};
inline constexpr bool LayerSizes()
{
    for (const auto& layer : kLayers)
        if (!layer.bytes || bool(layer.ownBytes) != bool(layer.info.ownSha256)) return false;
    return true;
}
static_assert(LayerSizes(), "every program has a size, and every own program (only those) has one");
inline constexpr std::array<LayerInfo, std::size(kLayers)> MakeInfos()
{
    std::array<LayerInfo, std::size(kLayers)> out{};
    for (size_t i = 0; i < std::size(kLayers); ++i) out[i] = kLayers[i].info;
    return out;
}
inline constexpr auto kLayerInfos = MakeInfos();

inline uint8_t LayerForSha256(std::string_view sha256) noexcept
{
    for (size_t i = 0; i < std::size(kLayers); ++i)
        if (sha256 == kLayers[i].info.sha256 || (kLayers[i].info.ownSha256 && sha256 == kLayers[i].info.ownSha256))
            return uint8_t(i + 1);
    return 0;
}
inline bool DispatchShapeOk(const Layer& layer, UINT x, UINT y, UINT z) noexcept
{
    return z == layer.groupsZ && x && y && x <= 65535 && y <= 65535;
}
inline void FillLaunch(const Layer& layer, UINT x, UINT y, const uint32_t constants[12],
                       XefgCudaParams& out, uint32_t& gridX, uint32_t& gridY) noexcept
{
    out.limitW = x * layer.intelW;
    out.limitH = y * layer.intelH;
    std::memcpy(out.constants, constants, sizeof(out.constants));
    gridX = (out.limitW + layer.tileW - 1) / layer.tileW;
    gridY = (out.limitH + layer.tileH - 1) / layer.tileH;
}
}
