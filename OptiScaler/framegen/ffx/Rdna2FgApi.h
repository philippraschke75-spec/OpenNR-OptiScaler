// Rdna2FgApi.h - C ABI of Rdna2FgRuntime.dll (RDNA2 frame generation: D3D12 recording + HIP worker), ABI 2.
// Host: OptiScaler fork, FSR-FG output path (FSRFG_Dx12). Copy of this header: OptiScaler/framegen/ffx/Rdna2FgApi.h.
//
// Per real frame (game thread):
//   1. Rdna2FgRecordInputs  - in FSRFG_Dx12::Dispatch(), before FFX's Present: records AND submits list A on the game queue
//                             (inputs -> shared io slot, flag_in), signals its own fence, enqueues the HIP job.
//                             RDNA2FG_SKIP: FG is off (auto-off / broken / busy allocator); the callback will then record nothing.
//   2. Rdna2FgRecordOutput  - in FSR-FG's frameGenerationCallback: records list B into FFX's command list (bounded GPU spin on the
//                             HIP job, then the generated frame(s) -> FFX's output(s); the real frame on timeout / abandon).
//      Returns the number of frames recorded (1..num_outputs), 0: nothing recorded (caller sets numGeneratedFrames = 0), < 0: error.
// Rule (INTEGRATION.md R1): A must be its own submission, before the list B lands in.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RDNA2FG_ABI 2
#define RDNA2FG_MAX_FRAMES 4
enum { RDNA2FG_OK = 0, RDNA2FG_SKIP = 1, RDNA2FG_ERROR = -1 };

typedef struct Rdna2FgInputs {
    uint32_t struct_size;
    uint64_t frame_id;             // the frameID FFX will hand to the callback for this frame
    void* color;                   // ID3D12Resource*: hudless colour if the game gives one, else the real frame (back buffer)
    uint32_t color_state;          // D3D12_RESOURCE_STATES now (restored after use)
    uint32_t color_is_hudless;     // 1: the output adds the current frame's UI (present - hudless)
    void* motion;                  // ID3D12Resource* (RG), may be null
    uint32_t motion_state;
    uint32_t mv_width, mv_height;  // extent the vectors' pixel units refer to (render size, or display size for display-res MVs)
    float mv_scale_x, mv_scale_y;  // vector * scale = offset in mv_width x mv_height pixels, current -> previous position
    void* depth;                   // ID3D12Resource*, may be null (sampled over depth_width x depth_height from the origin)
    uint32_t depth_state;
    uint32_t depth_width, depth_height;
    uint32_t reset;                // camera cut: no blend with the previous frame
    uint32_t out_width, out_height;  // swap chain size (= FFX output size)
    uint32_t num_frames;           // generated frames per real frame, 1 (2x) .. RDNA2FG_MAX_FRAMES; 0 = 1
} Rdna2FgInputs;

typedef struct Rdna2FgOutputs {
    uint32_t struct_size;
    uint64_t frame_id;
    void* cmd_list;                // ID3D12GraphicsCommandList* of FFX's callback
    void* present_color;           // ID3D12Resource*: the real frame (FFX presentColor)
    uint32_t present_state;
    uint32_t num_outputs;          // FFX numGeneratedFrames (1..RDNA2FG_MAX_FRAMES); outputs[i] shows time (i+1)/(n+1)
    void* output[RDNA2FG_MAX_FRAMES];          // ID3D12Resource*: FFX outputs[i] (back buffer format, UAV)
    uint32_t output_state[RDNA2FG_MAX_FRAMES];
    uint32_t rect_left, rect_top, rect_width, rect_height;  // generation rect (0 size = whole output)
} Rdna2FgOutputs;

typedef int32_t (*PFN_Rdna2FgCreate)(void* device, void* game_queue, void** ctx);
typedef int32_t (*PFN_Rdna2FgRecordInputs)(void* ctx, const Rdna2FgInputs* in);
typedef int32_t (*PFN_Rdna2FgRecordOutput)(void* ctx, const Rdna2FgOutputs* out);
typedef int32_t (*PFN_Rdna2FgGetStatus)(void* ctx, char* buf, uint32_t n);
typedef void (*PFN_Rdna2FgDestroy)(void* ctx);
typedef const char* (*PFN_Rdna2FgLastError)(void);

// ---------------------------------------------------------------- network plug-in
// Optional Rdna2FgNet.dll beside Rdna2FgRuntime.dll; without it the built-in PLACEHOLDER runs (motion-vector warp of the previous
// and current frame to time t, blended). Called on the runtime's HIP worker thread with the runtime's HIP device current.
// Input slots, per pixel (row-major, width x height, 16 bytes): uint4 { rg f16x2, ba f16x2, mv f16x2, depth f32 }
//   colour in the swap chain's encoding (hudless when the game gives one); mv in uv units, current -> previous position
//   (prev_uv = uv + mv); depth = raw device depth (0 when the game gives none).
// Outputs: RGBA16F (uint2 per pixel), one per generated frame, at time t[i] (0 = previous frame, 1 = current frame).
typedef struct Rdna2FgNetArgs {
    uint32_t struct_size;
    uint32_t width, height;
    const void* cur;               // device pointer, input slot of the current frame
    const void* prev;              // device pointer, input slot of the previous frame (valid only if has_prev)
    int32_t has_prev;              // 0: first frame / reset - output the current frame
    uint32_t num_frames;           // 1..RDNA2FG_MAX_FRAMES
    void* out[RDNA2FG_MAX_FRAMES];  // device pointers
    float t[RDNA2FG_MAX_FRAMES];
    void* stream;                  // hipStream_t: enqueue only; never synchronize, never wait on anything D3D12 writes
} Rdna2FgNetArgs;
typedef int32_t (*PFN_Rdna2FgNetCreate)(int32_t hip_device, void** net);  // exported as Rdna2FgNetCreate; 0 = ok
typedef int32_t (*PFN_Rdna2FgNetRun)(void* net, const Rdna2FgNetArgs* a);  // Rdna2FgNetRun; 0 = ok
typedef void (*PFN_Rdna2FgNetDestroy)(void* net);                           // Rdna2FgNetDestroy

#ifdef RDNA2FG_EXPORTS
#if defined(__HIP_DEVICE_COMPILE__)
#define RDNA2FG_API
#else
#define RDNA2FG_API __declspec(dllexport)
#endif
RDNA2FG_API int32_t Rdna2FgCreate(void* device, void* game_queue, void** ctx);
RDNA2FG_API int32_t Rdna2FgRecordInputs(void* ctx, const Rdna2FgInputs* in);
RDNA2FG_API int32_t Rdna2FgRecordOutput(void* ctx, const Rdna2FgOutputs* out);
RDNA2FG_API int32_t Rdna2FgGetStatus(void* ctx, char* buf, uint32_t n);
RDNA2FG_API void Rdna2FgDestroy(void* ctx);
RDNA2FG_API const char* Rdna2FgLastError(void);
#endif
#ifdef __cplusplus
}
#endif
