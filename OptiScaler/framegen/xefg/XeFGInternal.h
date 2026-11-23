#pragma once

// XeFG's unexported, lower level C API, called without its proxy swap chain.
//
// libxess_fg.dll exports only the xefgSwapChain* API. Internally that swap chain drives xefgD3D12CreateContext /
// Init / Execute / Destroy, which this module locates inside a loaded libxess_fg.dll. Calling it directly lets
// XeFG generate frames into textures the caller owns, so the Flip Meter output can present them through AMD's
// driver flip metering instead of XeFG's own swap chain and pacer.
//
// Struct layouts and function addresses are for libxess_fg 1.3.1 (XeSS SDK 3.0.2) and 1.3.4. The layouts come
// from dlssg-to-xefg (GPL-3.0), which reverse engineered them; this port keeps the same sizes and offsets.
//
// Other builds are refused: the addresses and the settings layout are per build, and guessing them would write
// into structures we only partly understand.

#include <windows.h>

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace XeFGInternal
{
using result_t = int32_t;

constexpr result_t RESULT_SUCCESS = 0;
constexpr result_t RESULT_ERROR_NOT_IMPLEMENTED = -7;

// Log levels and the callback XeFG reports through, the same values as xefgd_logging_level_t
enum LogLevel : uint32_t
{
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARNING = 2,
    LOG_LEVEL_ERROR = 3
};

// Called while the context is used, message first
using LogCallback = void (*)(const char* message, uint32_t level, void* userData);

// XeFG's default network. Above one interpolated frame Init picks its multi frame network (16) instead, which
// libxess_fg.dll doesn't ship, so this one has to be forced before CreateContext.
constexpr int32_t DEFAULT_MODEL = 18;

// Init flags, the same values as xefg_swapchain_init_flags_t
constexpr uint32_t INIT_FLAG_INVERTED_DEPTH = 1u << 0;
constexpr uint32_t INIT_FLAG_JITTERED_MV = 1u << 4;

// UI modes, the same values as xefg_swapchain_ui_mode_t
constexpr uint32_t UI_MODE_BACKBUFFER_HUDLESS = 4;

#pragma pack(push, 8)

struct Point2D
{
    uint32_t x;
    uint32_t y;
};

struct InitParams
{
    // Size of the generated frames, normally the back buffer size
    Point2D outputResolution;
    // DXGI_FORMAT of the outputs, it has to be usable as a typed UAV
    uint32_t backBufferFormat;
    // INIT_FLAG_*
    uint32_t initFlags;
    // Above 1 Init picks XeFG's multi frame network instead of its 2x one
    uint32_t maxInterpolatedFrames;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    uint32_t reserved0;
    ID3D12Heap* pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap* pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary* pPipelineLibrary;
    // UI_MODE_*, applied only when ExecParams::uiCompositionEnabled is set
    uint32_t uiMode;
    uint32_t reserved1;
};

// One input texture, in NON_PIXEL_SHADER_RESOURCE during Execute (XeFG transitions nothing)
struct ResourceDesc
{
    ID3D12Resource* pResource;
    Point2D base; // top left of the valid region
    Point2D size; // must be non zero
};

// One real frame
struct Frame
{
    ResourceDesc backBuffer; // final color, with UI
    ResourceDesc depth;
    ResourceDesc motionVectors; // only next's are read
    ResourceDesc uiTexture;
    ResourceDesc hudlessColor;
    float viewMatrix[16];      // row major, all zero disables scene change detection
    float projectionMatrix[16]; // row major, no jitter
    float outputResolution[2];
    float jitterOffset[2];
    float motionVectorScale[2]; // must be non zero
    uint32_t resetHistory;
    float frameRenderTime; // ms
};

// Generated frame i is at t = (i + 1) / (numInterpolatedFrames + 1) between prev and next.
// A call writes frames [firstFrameIndex, firstFrameIndex + frameCount), frameCount 0 meaning all of them. The
// expensive per pair work only runs in a call starting at 0, later calls reuse it and must pass the same frames
// and the same ppOutputs array.
struct ExecParams
{
    Frame prev;
    Frame next;
    // numInterpolatedFrames textures in the init format, ALLOW_UNORDERED_ACCESS, in UNORDERED_ACCESS
    ID3D12Resource* const* ppOutputs;
    const Point2D* pOutputBases;
    uint8_t numInterpolatedFrames;
    uint8_t reserved0[3];
    uint32_t networkScales; // 0 = default
    ID3D12DescriptorHeap* pDescriptorHeap;
    uint32_t descriptorHeapOffsetInBytes;
    uint32_t uiCompositionEnabled;
    uint32_t firstFrameIndex;
    uint32_t frameCount;
};

#pragma pack(pop)

static_assert(sizeof(InitParams) == 80);
static_assert(offsetof(InitParams, pTempBufferHeap) == 32);
static_assert(offsetof(InitParams, uiMode) == 72);
static_assert(sizeof(ResourceDesc) == 24);
static_assert(sizeof(Frame) == 280);
static_assert(offsetof(Frame, viewMatrix) == 120);
static_assert(offsetof(Frame, jitterOffset) == 256);
static_assert(offsetof(Frame, motionVectorScale) == 264);
static_assert(offsetof(ExecParams, ppOutputs) == 560);
static_assert(offsetof(ExecParams, numInterpolatedFrames) == 576);
static_assert(offsetof(ExecParams, networkScales) == 580);
static_assert(offsetof(ExecParams, pDescriptorHeap) == 584);
static_assert(offsetof(ExecParams, uiCompositionEnabled) == 596);
static_assert(offsetof(ExecParams, firstFrameIndex) == 600);
static_assert(sizeof(ExecParams) == 608);

// Picks a libxess_fg.dll the internal API can be driven through: already loaded ones first, then the one next to
// this module. Only searches once, the result is kept.
bool Load();

bool Available();

// Why Load() failed, for the log and the menu
const char* Status();

// Process wide, applies to contexts created afterwards. Must be set before CreateContext picks its network.
void SetMaxInterpolatedFrames(uint32_t maxGeneratedFrames);

// Forces a network instead of Init's pick. The default network (18) is the only one libxess_fg.dll ships for
// more than one interpolated frame, without it Init selects the missing multi frame network (16).
void SetModel(int32_t modelId);

bool CreateContext(ID3D12Device* device, void** context);
result_t Init(void* context, const InitParams& params);
result_t Execute(void* context, ID3D12GraphicsCommandList* cmdList, const ExecParams& params);
void Destroy(void* context);

// Wires XeFG's own log to the caller's, the context keeps the callback until Destroy
void SetLoggingCallback(void* context, uint32_t level, LogCallback callback);
} // namespace XeFGInternal
