// fh1 - native renderer, parts C3, C4, C5c and C6: drawing with the NFSSPV library shaders onto the
// C2 render targets.
//
// Started as the version that validated the whole chain on PC with the videos and the title screen: 2D
// textures (base level), guest vertices and indices, constants, pipelines per state and render passes.
// What is not covered is rejected with the cause logged once (details in the .cpp).

#pragma once

#include <rex/ui/vulkan/device.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace fh1::native {

struct EntryShader;

// Image of a render target or a texture, in a host format.
struct ImageNative {
  VkImage image = VK_NULL_HANDLE;
  // Only if the image has its own dedicated allocation. Images from the texture pool leave this NULL on
  // purpose: their memory is a chunk of a shared slab and cannot be freed on its own.
  VkDeviceMemory memory_block = VK_NULL_HANDLE;
  // Texture pool slab, or UINT32_MAX if the image does not come from the pool. The literal is used here
  // instead of kBlockPoolInvalid to keep that header out of this one.
  uint32_t pool_block = 0xFFFFFFFFu;
  VkImageView view = VK_NULL_HANDLE;
  // FH1: the same 8-bit color image seen as sRGB (render targets and resolved textures only). Draws into a
  // k_8_8_8_8_GAMMA target and fetches with the gamma sign go through it, so the host converts as the console does.
  VkImageView view_srgb = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
  // fh1_native_shadow_scale: the shadow map is drawn smaller than the guest requests and upscaled when
  // resolved. This holds the size the guest thinks it has; 0 = the same.
  uint32_t width_guest = 0;
  uint32_t height_guest = 0;
  // ZCULL: the scene's depth images are created without TRANSFER_DST so the driver can assign them a ZCULL
  // plane. Without that usage they cannot be cleared with vkCmdClearDepthStencilImage or receive copies:
  // they have to be cleared by opening a pass with loadOp = CLEAR, and they cannot be swapped with their
  // resolved texture.
  bool accepts_target_of_copy = true;
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool prepared = false;  // already in GENERAL and initialized
  // Resolved texture with RB_COPY_DEST_INFO.copy_dest_swap: the guest sees it with red and blue swapped
  // relative to the render target it comes from.
  bool swap_rb = false;
  // FH1: RB_COPY_DEST_INFO.copy_dest_exp_bias of the resolve that filled it: the console stores the picture
  // multiplied by 2^exp_bias; here it is stored as drawn and the scale is applied when it is fetched.
  int32_t exp_bias = 0;
  // fh1_native_resolve_without_copy: when a whole render target is resolved, its image is swapped with the
  // resolved texture's instead of copied. The target keeps that texture's old content: if the game draws
  // on it again without clearing first, the content has to be brought back.
  bool content_invalid = false;
  uint32_t resolved_base = 0;  // where its content is while content_invalid
};

// GPU time categories (ContextTargets::MarkGpu and the C2 report).
inline constexpr uint32_t kGpuOthers = 0;
inline constexpr uint32_t kGpuShadows = 1;   // depth-only targets of 1600 or more
inline constexpr uint32_t kGpuScene = 2;    // 1280
inline constexpr uint32_t kGpuReflection = 3;   // 640
inline constexpr uint32_t kGpu320 = 4;       // 320: cubemap faces and blur
inline constexpr uint32_t kGpuSmaller = 5;   // under 320
inline constexpr uint32_t kGpuCopies = 6;    // C2 copies to resolved textures
inline constexpr uint32_t kGpuClears = 7;  // C2 color and depth clears
// 1280 targets without depth: full-screen post-processing and HUD ("scene" is kept for the ones with
// depth, the geometry).
inline constexpr uint32_t kGpuSceneWithoutDepth = 8;
// Not a pass type but the GPU gap between the end of one submission and the start of the next.
inline constexpr uint32_t kGpuGapBetweenJobs = 9;
inline constexpr uint32_t kGpuCategories = 10;
// Buckets of the histogram of intervals between Swaps (originally <15, 15-18, 18-25, 25-30, 30-36, 36-50,
// >=50 ms). Now 11 buckets: the top ones were all lumped into ">=50 ms", where 83 % of race frames fell,
// so a 55 ms frame could not be told from a 150 ms one, which is exactly the difference between "slow"
// and "stuttering".
inline constexpr uint32_t kBucketsSwap = 11;

// Stages of the C6 report. The last four split the pass change, which on the console is the most
// expensive and most variable stage.
inline constexpr uint32_t kStagesDraw = 12;

// What the C2 render target code provides to the draws. All on the ring thread.
class ContextTargets {
 public:
  virtual ~ContextTargets() = default;
  // Command buffer of the frame's work, recording. nullptr on failure.
  virtual VkCommandBuffer CommandsWork() = 0;
  // Upload command buffer, recording: submitted right before the work one.
  virtual VkCommandBuffer CommandsUpload() = 0;
  // Changes every time the work buffer starts recording again.
  virtual uint64_t GenerationCommands() const = 0;
  // Render targets already in GENERAL. May record commands: call outside a pass.
  virtual ImageNative* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  virtual ImageNative* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  // Texture resolved by C2 at that physical address, or nullptr.
  virtual const ImageNative* TextureResolved(uint32_t address) = 0;
  // FH1: the resolved depth at that address as the bytes the console's resolve writes (red = stencil, then the
  // 24-bit depth from its lowest byte), for draws that fetch it as a color texture. Call it after TextureResolved.
  // nullptr if there is no such image.
  // FH1: a draw fetches the resolved texture at that address as a texture narrower than its image (the image
  // took the resolve's pitch, a multiple of 32): the next resolve there makes the image that wide.
  virtual void HintWidthResolved(uint32_t address, uint32_t width) {
    (void)address;
    (void)width;
  }
  virtual const ImageNative* TextureResolvedBytes(uint32_t address) {
    (void)address;
    return nullptr;
  }
  // FH1: a depth-only rectangle is about to fill that depth buffer (Z written without a test). When an 8-bit color
  // target on the same EDRAM (same base and pitch) is drawn into next, it first receives the depth buffer's
  // bytes, as on the console: the game clears the shadow / headlight mask to white that way (Z = 1, stencil FF).
  // x0..y1: the rectangle in the pixels of that image (the fill only reaches those; several fills add up).
  // word_known: the rectangle has one Z and replaces the stencil, so the EDRAM word it leaves is known (word =
  // depth << 8 | stencil) and is written as it is; otherwise the bytes are taken from the depth image.
  virtual void NoteFillDepth(uint32_t base, uint32_t format, uint32_t pitch, int32_t x0, int32_t y0, int32_t x1,
                             int32_t y1, bool word_known, uint32_t word) {
    (void)word_known;
    (void)word;
    (void)base;
    (void)format;
    (void)pitch;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
  }
  // fh1_native_lazy_depth. While set, the depth textures requested belong to a sample the
  // shader does not take (the final composite without blur) and do not force a copy.
  virtual void ReadsOfDepthDead(bool dead) { (void)dead; }
  // fh1_native_diag_clears. What a pass loads and stores of that render target (its renderArea): the
  // part of what was cleared that is actually used. Called before opening the pass.
  virtual void NoteAreaOfPass(const ImageNative* image, uint32_t width, uint32_t height) {
    (void)image;
    (void)width;
    (void)height;
  }
  // fh1_native_shadow_minimum (see fh1_native_targets.cpp). When a pass opens: true if it is the
  // shadow map car pass that C2 is observing or applying, and then each draw is validated with
  // DrawOfCarsShadow. `only_depth`: the pass has no color targets.
  virtual bool PassOfCarsShadow(const ImageNative* depth, bool only_depth) {
    (void)depth;
    (void)only_depth;
    return false;
  }
  // A draw of that pass: `exact` if it leaves in the depth buffer the minimum of what was there and of
  // its fragments.
  virtual void DrawOfCarsShadow(bool exact, uint32_t control_z) {
    (void)exact;
    (void)control_z;
  }
  // Address of the shadow map texture with cars to watch for when preparing samplers; 0 = none.
  virtual uint32_t AddressShadowCars() const { return 0; }
  // A draw samples that texture. `capable`: its pixel shader has tfetch2DShadowMin on that register; `ps`,
  // its number. Returns the pair to take the minimum with (its view goes in the register's 3D index word),
  // or nullptr.
  virtual const ImageNative* PartnerShadowCars(bool capable, uint32_t ps) {
    (void)capable;
    (void)ps;
    return nullptr;
  }
  // Submits what was recorded and continues in the other slot, with its upload buffer empty.
  virtual bool SendYWait() = 0;
  // Waits for the GPU to finish everything pending and starts recording again. It is expensive (a
  // one-frame stutter), so it is only used as a last resort when memory runs out: with the GPU idle,
  // textures can be released regardless of when they were last used, because none is in use.
  virtual bool WaitGpuOfAll() = 0;
  // GPU timestamp: whatever is recorded from here counts toward that category.
  virtual void MarkGpu(uint32_t category) = 0;
  // Host occlusion query for the game's open one, inside the open pass. Returns its index, or UINT32_MAX
  // if no game query is open or there is no room left. End it before closing the pass.
  virtual uint32_t BeginQueryOcclusion() = 0;
  virtual void FinishQueryOcclusion(uint32_t index) = 0;

  // fh1_reflection_visibility. Our own occlusion query around a single draw that samples the reflection
  // (or around the witness, the final composite), inside the open pass and with no game query open.
  // Returns its index, or UINT32_MAX if it cannot be measured (the draw counts as visible). End it right
  // after the draw.
  virtual uint32_t BeginQueryVisibility(bool witness) {
    (void)witness;
    return UINT32_MAX;
  }
  virtual void FinishQueryVisibility(uint32_t index) { (void)index; }

  // Pipeline statistics of a whole pass (shaded fragments, vertex invocations and primitives reaching
  // clipping), accumulated per category. UINT32_MAX if not measured.
  virtual uint32_t BeginStatistics(uint32_t category) = 0;
  virtual void FinishStatistics(uint32_t index) = 0;

  // The same per draw, tagged with the pixel shader number, in an occasional diagnostic frame. Returns
  // UINT32_MAX if not measured or if there is no room left.
  virtual uint32_t BeginStatisticsDraw(uint32_t label, uint32_t category) = 0;
  virtual void FinishStatisticsDraw(uint32_t index) = 0;
};

struct RequestDraw {
  const uint32_t* register_values = nullptr;       // register mirror of the ring sink
  const EntryShader* vs = nullptr;
  const EntryShader* ps = nullptr;
  std::span<const uint32_t> vs_microcode;  // patched by D3D (last IM_LOAD)
  uint64_t generation_vs = 0;                // changes with every VS IM_LOAD
  uint64_t generation_constants_vs = 0;     // changes when 0x4000-0x43FF are written
  uint64_t generation_constants_ps = 0;     // changes when 0x4400-0x47FF are written
  /*
   * The two most expensive stages of recording a draw on the console are "textures" (4.1 ms per frame) and
   * the viewport/scissor that the C6 report includes in "pipeline". Both are pure functions of registers
   * that almost never change between consecutive draws, so the ring sink tracks when they really change
   * (it compares the value before writing it, as with the constants) and the count arrives here.
   *
   * A consumer can keep the last value seen next to its result (the 48+16 texture slots and the 32 1/size
   * words of the shared block; the VkViewport, the VkRect2D and ndc[4]) and skip the whole recomputation
   * while it does not change; the framing cache does this with generation_framing. A generation that goes
   * up too often only causes extra work, never a wrong draw.
   */
  uint64_t generation_fetch = 0;             // changes when 0x4800-0x48BF are written (fetch constants)
  uint64_t generation_framing = 0;          // viewport, scissor, clipping, rasterizer mode
  // kVeg* flags (fh1_native_hooks.h) of the record the draw comes with; 0 = no verdict from the game
  // (fh1_d3d_game_vegetation).
  uint16_t vegetation_game = 0;
};

struct StatisticsDraws {
  uint64_t drawn = 0;
  uint64_t rejected = 0;
  uint64_t pipelines = 0;
  uint64_t textures = 0;
  uint64_t uploads_texture = 0;
  uint64_t megabytes_uploaded = 0;  // vertices, indices, constants and textures
  uint64_t megabytes_textures = 0;  // texture images created (base level, no eviction)
  uint64_t ms_pipelines = 0;  // creating pipelines, accumulated
  // Accumulated for "C6 counters" (the system prints the differences per report).
  uint64_t passes = 0;             // render passes started
  uint64_t submissions_full = 0;     // submissions due to a full upload buffer
  uint64_t ns_submissions_full = 0;  // inside those submissions, including the wait for the GPU
  uint64_t bytes_vertices = 0;
  // Deduplication of vertex uploads within the frame.
  uint64_t dedupe_hits = 0;
  uint64_t dedupe_bytes = 0;
  uint64_t dedupe_collisions = 0;
  uint64_t bytes_indices = 0;
  uint64_t samplers = 0;          // samplers prepared
  uint64_t samplers_cache = 0;    // of those, resolved by the register cache
  uint64_t ns_passes = 0;          // inside FinishPass + BeginPass on a pass change
  uint64_t ns_vertices = 0;       // copying vertices with byte swap
  uint64_t entries_computed = 0;   // ComputeEntry due to another VS generation
  uint64_t ns_entries = 0;           // inside ComputeEntry
  uint64_t entries_reused = 0; // EntryOf with the same generation and VS
  uint64_t passes_by_generation = 0;  // pass change due to a new command buffer
  uint64_t passes_by_target = 0;     // pass change due to other render targets
  uint64_t passes_resumed = 0;      // the same pass, closed earlier by a copy or a clear
  uint64_t ns_render_pass = 0;        // in vkCmdBeginRenderPass + vkCmdEndRenderPass
  uint64_t texels_passes = 0;          // area opened, summed over every pass opening
  // The same area, split by render target type (indices kGpuShadows..kGpuSmaller).
  std::array<uint64_t, kGpuCategories> texels_by_category{};
  // Pass openings per render target type, to know the average area of each.
  std::array<uint64_t, kGpuCategories> passes_by_category{};
  // Draws and triangles per render target type. With each one's GPU time (C2 report) it is possible to
  // tell whether an expensive pass is geometry-bound or pixel-bound: the counts do not depend on the
  // machine, so what is measured on PC holds for the console.
  std::array<uint64_t, kGpuCategories> draws_by_category{};
  // Draws with no color to write, by whether their pixel shader is needed.
  uint64_t draws_ps_useless = 0;
  uint64_t draws_ps_needed = 0;
  // Shadow map draws by their constant c1 (g_bShadowMapAlphaEnabled).
  uint64_t shadows_alpha_active = 0;
  uint64_t shadows_alpha_off = 0;
  std::array<uint64_t, kGpuCategories> triangles_by_category{};
  // Submissions (upload slot changes) with constants through UBOs and in total, to separate the modes of
  // fh1_native_constants_ubo_toggle_s in the report.
  uint64_t submissions_ubo = 0;
  uint64_t submissions = 0;
  // How many draws really change the 488 bytes of shared constants. It decides whether the comparison or
  // the double memcpy is what needs to get cheaper.
  uint64_t shared_looked = 0;
  uint64_t shared_changed = 0;
  uint64_t bytes_repeated_frame = 0;  // fh1_native_diag_repeated_vertices
  uint64_t bytes_equal_previous = 0;
  uint64_t ns_hash_vertices = 0;
  // Rejections by cause (Reject in the .cpp), most frequent first.
  std::vector<std::pair<uint32_t, uint64_t>> causes;
  // Accumulated nanoseconds per stage of the draws that get recorded: state, indices, textures, pass,
  // uploads, pipeline and recording.
  std::array<uint64_t, kStagesDraw> stages_ns{};
  // Scene draws that allow or prevent early depth rejection.
  uint64_t scene_with_discard = 0;
  uint64_t scene_without_discard = 0;
  // Recorded draws with the stopwatch (1 in kStopwatchEvery): the divisor of stages_ns and ns_vertices.
  uint64_t drawn_timed = 0;
  // Shadow map vegetation dropped right on entry, before paying for indices, textures, upload and pass. It
  // was counted but never printed anywhere, so the gain could not be verified at first. Now it is printed.
  uint64_t vegetation_soon = 0;
};

class DrawsVulkan {
 public:
  // nullptr if the device lacks capabilities (the reason is logged).
  static std::unique_ptr<DrawsVulkan> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                              rex::memory::Memory* memory_block,
                                              ContextTargets* context_id);
  virtual ~DrawsVulkan() = default;

  virtual bool Draw(const RequestDraw& request) = 0;
  // Before copies, clears and any command outside a pass.
  virtual void FinishPass() = 0;
  // FH1: C2 recorded a pass of its own with another pipeline, descriptor set and viewport in the work command
  // buffer (the depth-as-bytes pass): nothing the draws had bound or set there is valid any more.
  virtual void ForgetStateBound() {}
  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR, instead of with
  // vkCmdClearDepthStencilImage. Needed for images created without TRANSFER_DST, the only ones the driver
  // can give a ZCULL plane. Returns false if it could not.
  virtual bool ClearDepthInPass(VkCommandBuffer commands, const ImageNative& image,
                                       float depth, uint32_t stencil) = 0;
  // fh1_native_clear_useful_area. Clears only the `area` rectangle of a color image by opening a pass
  // with loadOp = CLEAR. Returns false if it could not.
  virtual bool ClearColorInPass(VkCommandBuffer commands, const ImageNative& image, const VkClearColorValue& color,
                                 const VkRect2D& area) {
    (void)commands;
    (void)image;
    (void)color;
    (void)area;
    return false;
  }
  // Right before submitting: closes the pass and publishes the upload buffer.
  virtual void BeforeOfSend() = 0;
  // Starts work in that work slot. Its upload buffer starts over: the GPU has finished the last work
  // submitted with it.
  virtual void UseSlot(uint32_t slot) = 0;
  // A C2 image is about to be destroyed: it is removed from the descriptors.
  virtual void ForgetImage(VkImage image) = 0;
  // A C2 view is about to be destroyed (Destroy, in fh1_native_targets.cpp, with the GPU idle for that
  // image): the FramebufferDe cache framebuffers that use it are destroyed. Without this, if Vulkan gave
  // the same handle to another view, the cache returned a framebuffer created on the dead view.
  virtual void ForgetView(VkImageView view) { (void)view; }
  // The GPU is out of memory. Stops the GPU, releases half the texture cache and reports whether anything
  // was freed. Called by both texture and render target allocations: whichever runs out of memory first
  // calls this before giving up.
  virtual bool ReleaseTexturesByMissingOfMemory() = 0;
  // Before every C2 copy: a resolved texture may change image or channels.
  virtual void InvalidateTextures() = 0;

  // Invalidates in the texture caches only what points to these two images. Used by the image swap of
  // fh1_native_resolve_without_copy: dropping the whole cache twice per frame costs more than the copy it
  // saves (measured on PC).
  virtual void InvalidateImages(VkImage a, VkImage b) = 0;
  virtual StatisticsDraws Statistics() const = 0;
  // Draws recorded since start-up; C2 counts those that fall between two copies.
  virtual uint64_t Drawn() const = 0;
  // Waits for the copy thread to finish the pending vertex copies (fh1_native_uploads_thread).
  virtual void WaitUploads() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t CopiesPending() const { return 0; }
  // With a game occlusion query open, each span of draws inside a pass is counted with a host query
  // (ContextTargets::BeginQueryOcclusion), which closes with the pass or when the game query closes.
  // FH1: the pipeline list (this PC's and the shipped one) starts being built now, from any thread, before the
  // game's code runs and so before the ring's first submission would start it (fh1_app.h, LaunchModule).
  // false = nothing to wait for (no list, no cache, no library, or switched off).
  virtual bool StartPrewarmEarly() { return false; }
  // How far that is, from any thread: pipelines walked, pipelines in the list, and whether it has ended.
  virtual void ProgressPrewarm(uint32_t& done, uint32_t& total, bool& finished) const {
    done = 0;
    total = 0;
    finished = true;
  }
  virtual void OcclusionOpen(bool open) = 0;
};

/*
 * Incremented every time the ring thread handles a guest wait for the GPU (WAIT_REG_MEM). That is the
 * only point at which the game may legally rewrite a vertex range it has already referenced in this
 * frame, so it is where upload deduplication must forget what it recorded. See
 * fh1_native_vertex_dedupe.h.
 */
extern std::atomic<uint32_t> g_synchronizations_ring;

/*
 * The ring thread's reports are written on another thread (fh1_native_deferred_reports).
 *
 * The ring's periodic dump (the system report, every 20 s) took 29-32 ms, and not because of formatting:
 * the log is synchronous (log_async = false) and every 2-4 lines the SD FILE is flushed to the card. In
 * one race there were 6 stutters of 60 ms or more: 3 ended 40-70 ms after a dump started, and 1 within
 * 0.12 s of the C6 substages line (0.5 would be expected by chance).
 * The line is formatted on the calling thread (it reads the ring's state, which is only consistent on its
 * own thread) and queued; the "FH1 reports" thread (priority 0x3B) writes it. The queue never blocks
 * the producer: if it fills up, the line is dropped and counted. Defined in fh1_native_system.cpp.
 */
void ReportDeferred(std::string line);

// Like REXLOG_INFO, but the report thread does the SD write (ReportDeferred).
#define FH1_REPORT_RING(...) ::fh1::native::ReportDeferred(fmt::format(__VA_ARGS__))

}  // namespace fh1::native
