// Genesis Plus GX PS5: the GPU renderer (ProsperoGpu.h).
//
// Vulkan 1.0 only, so that RADV on the PS5 and any desktop driver (lavapipe in the host tests) run the same code:
//   - on the PS5: VK_KHR_display (PS5_Mesa's VideoOut WSI): the display's 60 Hz mode, a plane surface, a FIFO
//     swapchain; the entry points come from the linked driver (vk_icdGetInstanceProcAddr), no loader;
//   - on the host: an off-screen target, read back after each frame and handed to the host's flip dump.
// One frame is in flight: Present waits for the previous frame's fence before it touches the staging buffers.
//
// The slang runtime follows RetroArch's spec (docs: libretro's slang_shaders.md): each pass renders into its own
// image (8-bit, sRGB or 16-bit float), sized from the source, the viewport or absolutely; the last pass renders
// into the game's rectangle on the target. Textures by name (Original, Source, OriginalHistory#, PassOutput#,
// PassFeedback#, aliases, LUTs) and the uniform semantics (MVP, OutputSize, SourceSize, OriginalSize,
// FinalViewportSize, FrameCount, FrameDirection, Rotation, <texture>Size, the parameters) are bound from the
// reflection slang_compile.py made at build time.
//
// SPDX-License-Identifier: MIT

#include "ProsperoGpu.h"

#include "OrbisPaths.h"

#if defined(__PROSPERO__) && !defined(GENPLUS_GPU)
// A PS5 build without RADV (make ps5 without PS5_VULKAN): no GPU renderer, the CPU draws the picture.
namespace ps5gpu
{
bool Init(int, int, bool, int, int)
{
	OrbisLog("[gpu] this build has no GPU renderer (built without PS5_VULKAN)");
	return false;
}
bool Active() { return false; }
void Shutdown() {}
int OutWidth() { return 0; }
int OutHeight() { return 0; }
const char* DeviceName() { return ""; }
int ShaderCount() { return 1; }
const char* ShaderName(int) { return "Off"; }
int PassCount(int) { return 0; }
bool ShaderFullscreen(int) { return false; }
void SetGame(const uint32_t*, int, int, Rect, int, bool, bool) {}
void HideGame() {}
bool Present(const uint32_t*, Rect, bool) { return false; }
double TakeShaderMs() { return 0; }
bool Building() { return false; }
int ShaderParamCount(int) { return 0; }
bool ShaderParam(int, int, ParamInfo*) { return false; }
void SetShaderParam(int, const char*, float) {}
void ResetShaderParams(int) {}
bool SaveShaderParams() { return true; }
bool SceneAvailable() { return false; }
bool SceneTexture(uint64_t, const uint32_t*, int, int, bool) { return false; }
bool HasSceneTexture(uint64_t) { return false; }
void SetScene(const std::vector<SceneQuad>&) {}
void ClearScene() {}
} // namespace ps5gpu
#else
#include "ProsperoSlang.h"
#include "ProsperoThread.h"

#if defined(__PROSPERO__)
// RADV's compiler names mkstemp (aco_print_asm.cpp, only for its debug dumps). No system module gives a title one;
// the SDK fork's platform layer has mkstemps (bound by the link recipe), so mkstemp is that with no suffix. The
// symbol stays local (localize.map).
extern "C" int mkstemps(char* path, int suffix_len);
extern "C" int mkstemp(char* path)
{
	return mkstemps(path, 0);
}
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <chrono>
#include <string>
#include <vector>

// the presets' look-up textures are PNGs: a private copy of stb_image's PNG decoder (fe_covers.cpp has its own)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../frontend/third_party/stb_image.h"
#pragma GCC diagnostic pop

#ifdef __PROSPERO__
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
#define GPU_GET_INSTANCE_PROC vk_icdGetInstanceProcAddr
#else
extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name);
#define GPU_GET_INSTANCE_PROC vkGetInstanceProcAddr
extern "C" void HostGpuFlip(const uint8_t* bgra, int w, int h, bool wait_vsync); // host/sce_host.cpp
#endif

namespace ps5gpu
{
namespace
{
// ---- entry points ------------------------------------------------------------------------------------------
#define GPU_GLOBAL_FNS(X) X(CreateInstance) X(EnumerateInstanceExtensionProperties)
#define GPU_INSTANCE_FNS(X)                                                                                          \
	X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)                                      \
	X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice)                     \
	X(GetDeviceProcAddr) X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceFormatProperties)
#define GPU_DISPLAY_FNS(X)                                                                                           \
	X(GetPhysicalDeviceDisplayPropertiesKHR) X(GetDisplayModePropertiesKHR) X(CreateDisplayPlaneSurfaceKHR)            \
	X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR)              \
	X(GetPhysicalDeviceSurfaceFormatsKHR)
#define GPU_DEVICE_FNS(X)                                                                                            \
	X(DestroyDevice) X(GetDeviceQueue) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)             \
	X(BeginCommandBuffer) X(EndCommandBuffer) X(ResetCommandBuffer) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle)  \
	X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore)              \
	X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateImage) X(DestroyImage) \
	X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)       \
	X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateRenderPass)                      \
	X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule)        \
	X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout)        \
	X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool)                     \
	X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdPipelineBarrier) X(CmdCopyBufferToImage)                   \
	X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline)               \
	X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdPushConstants) X(CmdSetViewport) X(CmdSetScissor)           \
	X(CmdDraw) X(CmdClearColorImage) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool)                       \
	X(CmdWriteTimestamp) X(GetQueryPoolResults) X(FreeDescriptorSets) X(FreeCommandBuffers) X(CreatePipelineCache)          \
	X(DestroyPipelineCache) X(GetPipelineCacheData) X(CmdBlitImage)
#define GPU_SWAPCHAIN_FNS(X)                                                                                         \
	X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR)

#define GPU_DECLARE(name) PFN_vk##name vk##name = nullptr;
GPU_GLOBAL_FNS(GPU_DECLARE)
GPU_INSTANCE_FNS(GPU_DECLARE)
GPU_DISPLAY_FNS(GPU_DECLARE)
GPU_DEVICE_FNS(GPU_DECLARE)
GPU_SWAPCHAIN_FNS(GPU_DECLARE)
#undef GPU_DECLARE

// ---- resources ---------------------------------------------------------------------------------------------
struct Buffer
{
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	void* map = nullptr;
	VkDeviceSize size = 0;
};

struct Image
{
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	int w = 0, h = 0;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkFramebuffer fb = VK_NULL_HANDLE; // when it is a pass's render target
};

struct PassRt
{
	const ps5slang::Pass* def = nullptr;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkRenderPass render_pass = VK_NULL_HANDLE; // its own (intermediate passes); the last pass uses the final one
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	Image out[2]; // out[cur]: this frame's output; out[cur ^ 1]: last frame's (PassFeedback)
	int cur = 0;
	bool feedback = false;
	Buffer ubo;
	VkDescriptorSet set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	int out_w = 0, out_h = 0;
};

struct Chain
{
	int preset = -1;
	std::vector<PassRt> passes;
	std::vector<Image> luts;
	std::vector<float> params; // the preset's parameter values
	int history = 0; // OriginalHistory# used (the deepest)
	std::vector<Image> hist; // hist[0] = OriginalHistory1 ...
	bool built = false;
};

struct State
{
	bool ok = false;
	bool offscreen = false;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memprops = {};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	char device_name[256] = {};
	float ts_period = 0;
	bool timestamps = false;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool in_flight = false;
	VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
	VkQueryPool queries = VK_NULL_HANDLE;
	bool queries_written = false;
	// the target
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat target_format = VK_FORMAT_B8G8R8A8_UNORM;
	std::vector<VkImage> swap_images;
	std::vector<VkImageView> swap_views;
	std::vector<VkFramebuffer> swap_fbs;
	Image offscreen_target;
	Buffer readback;
	int out_w = 0, out_h = 0;
	VkRenderPass final_pass = VK_NULL_HANDLE;
	// the surface and the game frame
	int ui_w = 0, ui_h = 0;
	Image ui;
	Buffer ui_staging;
	bool ui_full = true;
	Image original;
	Buffer game_staging;
	Image dummy;
	// draw state
	Buffer vertices;
	VkSampler samplers[2][4] = {};
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	Chain ui_chain;
	Chain game_chain;
	// shaders built off the frame loop: the old one stays on screen meanwhile
	VkPipelineCache pcache = VK_NULL_HANDLE; // saved to <root>/cache/gpu-pipelines.bin: a shader built once loads fast
	std::mutex queue_mx; // the queue: the frame's submit and present, the builder's uploads
	std::mutex upload_mx; // the upload command pool
	std::mutex dpool_mx; // the descriptor pool
	VkCommandPool upload_pool = VK_NULL_HANDLE;
	size_t pcache_saved = 0; // the size of the cache file last written (or read)
	ps5::BigThread builder; // a big stack: RADV's shader compiler (ACO) recurses deep, and the SDK's default stack is small
	std::atomic<bool> building{false};
	std::atomic<bool> built_ready{false};
	int building_preset = -1;
	Chain* built = nullptr; // the builder's result (nullptr: it failed)
	// the shader parameters set in the menu: preset name -> parameter -> value
	std::mutex param_mx; // param_overrides: the menu writes, the builder reads
	std::map<std::string, std::map<std::string, float>> param_overrides;
	// the shelf's scene (SetScene): its quad pipeline, its textures (mip-mapped), the quads
	VkDescriptorSetLayout scene_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout scene_layout = VK_NULL_HANDLE;
	VkPipeline scene_pipe = VK_NULL_HANDLE;
	VkSampler scene_sampler = VK_NULL_HANDLE;
	VkDescriptorSet scene_dummy_set = VK_NULL_HANDLE;
	struct SceneTex
	{
		VkImage img = VK_NULL_HANDLE;
		VkDeviceMemory mem = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		uint64_t used = 0; // the present it was last drawn in
		size_t bytes = 0;
	};
	std::map<uint64_t, SceneTex> scene_tex;
	std::vector<SceneQuad> scene;
	Buffer scene_vb; // the quads' vertices (written each frame, after the last one's fence)
	bool scene_on = false;
	uint64_t presents = 0;
	// the next frame's game picture
	bool game = false;
	int presents_without_game = 0; // the shelf draws no game: after a moment the GPU stops drawing the last one
	bool game_new = false;
	int game_w = 0, game_h = 0;
	Rect game_dst = {0, 0, 0, 0};
	int shader = 0;
	bool smooth = false, scanlines = false;
	uint64_t frame_count = 0;
	double gpu_ms = 0;
	int gpu_frames = 0;
};
State g;

const float kMvp[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1}; // [0,1] -> [-1,1], column-major

bool Check(VkResult r, const char* what)
{
	if (r != VK_SUCCESS)
	{
		OrbisLog("[gpu] %s -> %d", what, int(r));
		return false;
	}
	return true;
}

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0)
{
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
		{
			const VkMemoryPropertyFlags f = g.memprops.memoryTypes[i].propertyFlags;
			if ((bits & (1u << i)) && (f & want) == want && (pass == 1 || (f & nice) == nice))
				return i;
		}
	return UINT32_MAX;
}

bool MakeBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage;
	if (!Check(vkCreateBuffer(g.device, &bi, nullptr, &b.buf), "vkCreateBuffer"))
		return false;
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(g.device, b.buf, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX || !Check(vkAllocateMemory(g.device, &ai, nullptr, &b.mem), "vkAllocateMemory"))
		return false;
	vkBindBufferMemory(g.device, b.buf, b.mem, 0);
	if (!Check(vkMapMemory(g.device, b.mem, 0, size, 0, &b.map), "vkMapMemory"))
		return false;
	b.size = size;
	return true;
}

void FreeBuffer(Buffer& b)
{
	if (b.buf)
		vkDestroyBuffer(g.device, b.buf, nullptr);
	if (b.mem)
		vkFreeMemory(g.device, b.mem, nullptr);
	b = Buffer();
}

bool MakeImage(Image& im, int w, int h, VkFormat format, VkImageUsageFlags usage)
{
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = format;
	ii.extent = {uint32_t(w), uint32_t(h), 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!Check(vkCreateImage(g.device, &ii, nullptr, &im.img), "vkCreateImage"))
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, im.img, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
	if (!Check(vkAllocateMemory(g.device, &ai, nullptr, &im.mem), "vkAllocateMemory (image)"))
		return false;
	vkBindImageMemory(g.device, im.img, im.mem, 0);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = im.img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (!Check(vkCreateImageView(g.device, &vi, nullptr, &im.view), "vkCreateImageView"))
		return false;
	im.format = format;
	im.w = w;
	im.h = h;
	im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	return true;
}

void FreeImage(Image& im)
{
	if (im.fb)
		vkDestroyFramebuffer(g.device, im.fb, nullptr);
	if (im.view)
		vkDestroyImageView(g.device, im.view, nullptr);
	if (im.img)
		vkDestroyImage(g.device, im.img, nullptr);
	if (im.mem)
		vkFreeMemory(g.device, im.mem, nullptr);
	im = Image();
}

void Barrier(VkCommandBuffer c, Image& im, VkImageLayout to)
{
	if (im.layout == to)
		return;
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = im.layout;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = im.img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
		nullptr, 1, &b);
	im.layout = to;
}

VkRenderPass MakeRenderPass(VkFormat format, VkImageLayout final_layout, bool clear)
{
	VkAttachmentDescription a = {};
	a.format = format;
	a.samples = VK_SAMPLE_COUNT_1_BIT;
	a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	a.finalLayout = final_layout;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sp = {};
	sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sp.colorAttachmentCount = 1;
	sp.pColorAttachments = &ref;
	VkSubpassDependency dep = {};
	dep.srcSubpass = VK_SUBPASS_EXTERNAL;
	dep.dstSubpass = 0;
	dep.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dep.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	dep.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	dep.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	VkRenderPassCreateInfo ri = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	ri.attachmentCount = 1;
	ri.pAttachments = &a;
	ri.subpassCount = 1;
	ri.pSubpasses = &sp;
	ri.dependencyCount = 1;
	ri.pDependencies = &dep;
	VkRenderPass rp = VK_NULL_HANDLE;
	Check(vkCreateRenderPass(g.device, &ri, nullptr, &rp), "vkCreateRenderPass");
	return rp;
}

VkFramebuffer MakeFramebuffer(VkRenderPass rp, VkImageView view, int w, int h)
{
	VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fi.renderPass = rp;
	fi.attachmentCount = 1;
	fi.pAttachments = &view;
	fi.width = uint32_t(w);
	fi.height = uint32_t(h);
	fi.layers = 1;
	VkFramebuffer fb = VK_NULL_HANDLE;
	Check(vkCreateFramebuffer(g.device, &fi, nullptr, &fb), "vkCreateFramebuffer");
	return fb;
}

VkShaderModule Module(const uint32_t* code, unsigned bytes)
{
	VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	si.codeSize = bytes;
	si.pCode = code;
	VkShaderModule m = VK_NULL_HANDLE;
	Check(vkCreateShaderModule(g.device, &si, nullptr, &m), "vkCreateShaderModule");
	return m;
}

VkFormat PassFormat(int fb)
{
	return fb == 2 ? VK_FORMAT_R16G16B16A16_SFLOAT : fb == 1 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
}

// ---- a chain: its passes' pipelines ------------------------------------------------------------------------
bool BuildPass(PassRt& p, VkRenderPass rp, bool blend)
{
	const ps5slang::Pass& d = *p.def;
	std::vector<VkDescriptorSetLayoutBinding> binds;
	if (d.ubo_binding >= 0)
		binds.push_back({uint32_t(d.ubo_binding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	for (int i = 0; i < d.sampler_count; i++)
		binds.push_back({uint32_t(d.samplers[i].binding), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = uint32_t(binds.size());
	li.pBindings = binds.data();
	if (!Check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &p.set_layout), "vkCreateDescriptorSetLayout"))
		return false;
	VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, uint32_t(d.push_size)};
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &p.set_layout;
	pli.pushConstantRangeCount = d.push_size > 0 ? 1 : 0;
	pli.pPushConstantRanges = &pc;
	if (!Check(vkCreatePipelineLayout(g.device, &pli, nullptr, &p.layout), "vkCreatePipelineLayout"))
		return false;

	VkShaderModule vs = Module(d.vs, d.vs_bytes), fs = Module(d.fs, d.fs_bytes);
	if (!vs || !fs)
		return false;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vs;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fs;
	st[1].pName = "main";
	VkVertexInputBindingDescription vb = {0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = 0xf;
	if (blend)
	{
		// premultiplied alpha: the surface over the game
		ba.blendEnable = VK_TRUE;
		ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.colorBlendOp = VK_BLEND_OP_ADD;
		ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		ba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = p.layout;
	pi.renderPass = rp;
	const bool ok = Check(vkCreateGraphicsPipelines(g.device, g.pcache, 1, &pi, nullptr, &p.pipeline),
		"vkCreateGraphicsPipelines");
	vkDestroyShaderModule(g.device, vs, nullptr);
	vkDestroyShaderModule(g.device, fs, nullptr);
	if (!ok)
		return false;
	if (d.ubo_binding >= 0 && !MakeBuffer(p.ubo, std::max(d.ubo_size, 16), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
		return false;
	for (int k = 0; k < 2; k++)
	{
		VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = g.dpool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &p.set_layout;
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		if (!Check(vkAllocateDescriptorSets(g.device, &ai, &p.set[k]), "vkAllocateDescriptorSets"))
			return false;
	}
	return true;
}

void FreeChain(Chain& c)
{
	for (PassRt& p : c.passes)
	{
		if (p.pipeline)
			vkDestroyPipeline(g.device, p.pipeline, nullptr);
		if (p.layout)
			vkDestroyPipelineLayout(g.device, p.layout, nullptr);
		if (p.set_layout)
			vkDestroyDescriptorSetLayout(g.device, p.set_layout, nullptr);
		if (p.render_pass)
			vkDestroyRenderPass(g.device, p.render_pass, nullptr);
		{
			std::lock_guard<std::mutex> lock(g.dpool_mx);
			for (VkDescriptorSet& set : p.set)
				if (set)
					vkFreeDescriptorSets(g.device, g.dpool, 1, &set);
		}
		FreeImage(p.out[0]);
		FreeImage(p.out[1]);
		FreeBuffer(p.ubo);
	}
	for (Image& im : c.luts)
		FreeImage(im);
	for (Image& im : c.hist)
		FreeImage(im);
	c = Chain();
}

bool UploadImage(Image& im, const uint8_t* rgba, int w, int h);

// the values set in the menu (Shader parameters) over the preset's own; the builder thread calls this too
void ApplyOverrides(Chain& c)
{
	if (c.preset < 0 || c.preset >= ps5slang::PresetCount())
		return;
	const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
	std::lock_guard<std::mutex> lk(g.param_mx);
	const auto ov = g.param_overrides.find(pr.name);
	for (int i = 0; i < pr.param_count && size_t(i) < c.params.size(); i++)
	{
		float v = pr.params[i].value;
		if (ov != g.param_overrides.end())
		{
			const auto it = ov->second.find(pr.params[i].name);
			if (it != ov->second.end())
				v = it->second;
		}
		c.params[size_t(i)] = v;
	}
}

bool BuildChain(Chain& c, int preset, bool ui)
{
	FreeChain(c);
	if (preset < 0 || preset >= ps5slang::PresetCount())
		return false;
	// a preset of the user's own: compiled the first time (here, on the build thread)
	if (ps5slang::IsExternal(preset) && !ps5slang::EnsureCompiled(preset, nullptr))
		return false;
	const ps5slang::Preset& pr = ps5slang::PresetAt(preset);
	c.preset = preset;
	c.passes.resize(size_t(pr.pass_count));
	for (int i = 0; i < pr.param_count; i++)
		c.params.push_back(pr.params[i].value);
	// what the passes read: feedback and history
	for (int i = 0; i < pr.pass_count; i++)
	{
		const ps5slang::Pass& d = pr.passes[i];
		for (int s = 0; s < d.sampler_count; s++)
		{
			const std::string n = d.samplers[s].name;
			if (n.compare(0, 15, "OriginalHistory") == 0)
				c.history = std::max(c.history, atoi(n.c_str() + 15));
			if (n.compare(0, 12, "PassFeedback") == 0)
			{
				const int k = atoi(n.c_str() + 12);
				if (k >= 0 && k < pr.pass_count)
					c.passes[size_t(k)].feedback = true;
			}
			for (int j = 0; j < pr.pass_count; j++)
				if (pr.passes[j].alias[0] && n == std::string(pr.passes[j].alias) + "Feedback")
					c.passes[size_t(j)].feedback = true;
		}
	}
	for (int i = 0; i < pr.pass_count; i++)
	{
		PassRt& p = c.passes[size_t(i)];
		p.def = &pr.passes[i];
		const bool last = i == pr.pass_count - 1;
		p.format = last ? g.target_format : PassFormat(p.def->framebuffer);
		if (!last)
			p.render_pass = MakeRenderPass(p.format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		if (!BuildPass(p, last ? g.final_pass : p.render_pass, ui))
		{
			OrbisLog("[gpu] preset %s: pass %d could not be built", pr.name, i);
			return false;
		}
	}
	for (int i = 0; i < pr.lut_count; i++)
	{
		int w = 0, h = 0, n = 0;
		stbi_uc* px = stbi_load_from_memory(pr.luts[i].png, int(pr.luts[i].png_bytes), &w, &h, &n, 4);
		Image im;
		if (!px || !MakeImage(im, w, h, VK_FORMAT_R8G8B8A8_UNORM,
						 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
			!UploadImage(im, px, w, h))
		{
			OrbisLog("[gpu] preset %s: texture %s could not be loaded", pr.name, pr.luts[i].name);
			if (px)
				stbi_image_free(px);
			return false;
		}
		stbi_image_free(px);
		c.luts.push_back(im);
	}
	ApplyOverrides(c);
	c.built = true;
	return true;
}

// a one-off upload (LUTs): through a temporary staging buffer, waited for
bool UploadImage(Image& im, const uint8_t* rgba, int w, int h)
{
	Buffer st;
	if (!MakeBuffer(st, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		return false;
	memcpy(st.map, rgba, size_t(w) * h * 4);
	std::lock_guard<std::mutex> upload_lock(g.upload_mx);
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = g.upload_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer c;
	if (!Check(vkAllocateCommandBuffers(g.device, &ai, &c), "vkAllocateCommandBuffers"))
	{
		FreeBuffer(st);
		return false;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(c, &bi);
	Barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {};
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {uint32_t(w), uint32_t(h), 1};
	vkCmdCopyBufferToImage(c, st.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	Barrier(c, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(c);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c;
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence done = VK_NULL_HANDLE;
	vkCreateFence(g.device, &fi, nullptr, &done);
	bool ok;
	{
		std::lock_guard<std::mutex> q(g.queue_mx);
		ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (upload)");
	}
	if (ok)
		vkWaitForFences(g.device, 1, &done, VK_TRUE, UINT64_MAX);
	vkDestroyFence(g.device, done, nullptr);
	vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
	FreeBuffer(st);
	return ok;
}

// ---- per frame: sizes, textures, uniforms ------------------------------------------------------------------
struct Tex
{
	Image* im;
	bool linear;
	int wrap;
};

void SizeVec(float* v, int w, int h)
{
	v[0] = float(w);
	v[1] = float(h);
	v[2] = w ? 1.0f / float(w) : 0.0f;
	v[3] = h ? 1.0f / float(h) : 0.0f;
}

// a texture by its slang name, for pass `i` (Source is the previous pass's output)
bool Resolve(Chain& c, int i, const std::string& n, Tex& t, int& w, int& h)
{
	const ps5slang::Pass& d = *c.passes[size_t(i)].def;
	t = {&g.dummy, d.linear, d.wrap};
	Image* src = i == 0 ? &g.original : &c.passes[size_t(i - 1)].out[c.passes[size_t(i - 1)].cur];
	auto out_of = [&](int k, bool fb) -> Image* {
		PassRt& p = c.passes[size_t(k)];
		return fb ? (p.feedback ? &p.out[p.cur ^ 1] : &g.dummy) : &p.out[p.cur];
	};
	if (n == "Source")
		t.im = src;
	else if (n == "Original" || n == "OriginalHistory0")
		t.im = &g.original;
	else if (n.compare(0, 15, "OriginalHistory") == 0)
	{
		const int k = atoi(n.c_str() + 15);
		t.im = k >= 1 && k <= int(c.hist.size()) ? &c.hist[size_t(k - 1)] : &g.original;
	}
	else if (n.compare(0, 10, "PassOutput") == 0)
	{
		const int k = atoi(n.c_str() + 10);
		t.im = k >= 0 && k < i ? out_of(k, false) : &g.dummy;
	}
	else if (n.compare(0, 12, "PassFeedback") == 0)
	{
		const int k = atoi(n.c_str() + 12);
		t.im = k >= 0 && k < int(c.passes.size()) ? out_of(k, true) : &g.dummy;
	}
	else
	{
		bool found = false;
		for (int k = 0; k < int(c.passes.size()) && !found; k++)
		{
			const char* a = c.passes[size_t(k)].def->alias;
			if (!a[0])
				continue;
			if (n == a)
			{
				t.im = out_of(k, false);
				found = true;
			}
			else if (n == std::string(a) + "Feedback")
			{
				t.im = out_of(k, true);
				found = true;
			}
		}
		const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
		for (int k = 0; k < pr.lut_count && !found; k++)
			if (n == pr.luts[k].name)
			{
				t = {&c.luts[size_t(k)], pr.luts[k].linear, pr.luts[k].wrap};
				found = true;
			}
		if (!found)
			return false;
	}
	w = t.im->w;
	h = t.im->h;
	return true;
}

void FillUniforms(Chain& c, int i, uint8_t* block, const ps5slang::Member* m, int count, int out_w, int out_h,
	int vp_w, int vp_h)
{
	const ps5slang::Preset& pr = ps5slang::PresetAt(c.preset);
	const ps5slang::Pass& d = *c.passes[size_t(i)].def;
	for (int k = 0; k < count; k++)
	{
		const std::string n = m[k].name;
		uint8_t* at = block + m[k].offset;
		float v4[4];
		auto put4 = [&](int w, int h) {
			SizeVec(v4, w, h);
			memcpy(at, v4, 16);
		};
		if (n == "MVP")
			memcpy(at, kMvp, 64);
		else if (n == "OutputSize")
			put4(out_w, out_h);
		else if (n == "FinalViewportSize")
			put4(vp_w, vp_h);
		else if (n == "FrameCount")
		{
			const uint32_t f = d.frame_count_mod ? uint32_t(g.frame_count % uint64_t(d.frame_count_mod)) :
												   uint32_t(g.frame_count);
			memcpy(at, &f, 4);
		}
		else if (n == "FrameDirection")
		{
			const int32_t one = 1;
			memcpy(at, &one, 4);
		}
		else if (n == "Rotation")
		{
			const uint32_t zero = 0;
			memcpy(at, &zero, 4);
		}
		else if (n == "TotalSubFrames" || n == "CurrentSubFrame")
		{
			const uint32_t one = 1;
			memcpy(at, &one, 4);
		}
		else if (n.size() > 4 && n.compare(n.size() - 4, 4, "Size") == 0)
		{
			Tex t;
			int w = 0, h = 0;
			if (Resolve(c, i, n.substr(0, n.size() - 4), t, w, h))
				put4(w, h);
			else
				put4(0, 0);
		}
		else
		{
			float v = 0;
			for (int p = 0; p < pr.param_count; p++)
				if (n == pr.params[p].name)
				{
					v = c.params[size_t(p)];
					break;
				}
			if (m[k].kind == 0)
				memcpy(at, &v, 4);
		}
	}
}

// the passes' output sizes for this frame; (re)creates the images that changed size
bool SizeChain(Chain& c, int vp_w, int vp_h)
{
	int sw = g.original.w, sh = g.original.h;
	for (size_t i = 0; i < c.passes.size(); i++)
	{
		PassRt& p = c.passes[i];
		const ps5slang::Pass& d = *p.def;
		const bool last = i + 1 == c.passes.size();
		int w, h;
		if (last)
		{
			w = vp_w;
			h = vp_h;
		}
		else
		{
			auto dim = [&](int type, float scale, int src, int vp) {
				const float v = type == 0 ? src * scale : type == 1 ? vp * scale : scale;
				return std::max(1, std::min(8192, int(std::lround(v))));
			};
			w = dim(d.scale_type_x, d.scale_x, sw, vp_w);
			h = dim(d.scale_type_y, d.scale_y, sh, vp_h);
		}
		p.out_w = w;
		p.out_h = h;
		if (!last && (p.out[0].w != w || p.out[0].h != h))
		{
			for (int k = 0; k < (p.feedback ? 2 : 1); k++)
			{
				FreeImage(p.out[k]);
				if (!MakeImage(p.out[k], w, h, p.format,
						VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
							VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
					return false;
				p.out[k].fb = MakeFramebuffer(p.render_pass, p.out[k].view, w, h);
			}
			if (!p.feedback)
				p.out[1] = Image(); // never read
		}
		sw = w;
		sh = h;
	}
	return true;
}

// One pass: its textures and uniforms, then its draw. An intermediate pass opens and closes its own render pass;
// the last one draws into the final render pass (already begun) in the game's rectangle `vp`.
void RecordPass(VkCommandBuffer cmd, Chain& c, size_t i, Rect vp, bool plain_smooth)
{
	PassRt& p = c.passes[i];
	const ps5slang::Pass& d = *p.def;
	const bool last = i + 1 == c.passes.size();
	const int set_idx = int(g.frame_count & 1);
	if (p.feedback)
		p.cur ^= 1;
	std::vector<VkDescriptorImageInfo> infos(size_t(d.sampler_count));
	std::vector<VkWriteDescriptorSet> writes;
	VkDescriptorBufferInfo ubi = {p.ubo.buf, 0, VK_WHOLE_SIZE};
	if (d.ubo_binding >= 0)
	{
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = p.set[set_idx];
		w.dstBinding = uint32_t(d.ubo_binding);
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &ubi;
		writes.push_back(w);
	}
	for (int s = 0; s < d.sampler_count; s++)
	{
		Tex t;
		int tw, th;
		if (!Resolve(c, int(i), d.samplers[s].name, t, tw, th) || !t.im->img)
			t = {&g.dummy, false, 1};
		if (!last)
			Barrier(cmd, *t.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); // (inside a render pass: done before it)
		const bool linear = (c.preset == 0 && t.im == &g.original) ? plain_smooth : t.linear;
		infos[size_t(s)] = {g.samplers[linear ? 1 : 0][t.wrap & 3], t.im->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = p.set[set_idx];
		w.dstBinding = uint32_t(d.samplers[s].binding);
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &infos[size_t(s)];
		writes.push_back(w);
	}
	vkUpdateDescriptorSets(g.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
	const int ow = p.out_w, oh = p.out_h;
	if (d.ubo_binding >= 0)
		FillUniforms(c, int(i), static_cast<uint8_t*>(p.ubo.map), d.ubo, d.ubo_count, ow, oh, vp.w, vp.h);
	uint8_t push[256] = {};
	if (d.push_size > 0)
		FillUniforms(c, int(i), push, d.push, d.push_count, ow, oh, vp.w, vp.h);
	if (!last)
	{
		Image& out = p.out[p.cur];
		VkClearValue cv = {};
		VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rb.renderPass = p.render_pass;
		rb.framebuffer = out.fb;
		rb.renderArea = {{0, 0}, {uint32_t(ow), uint32_t(oh)}};
		rb.clearValueCount = 1;
		rb.pClearValues = &cv;
		vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
		out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // the render pass's final layout
	}
	const VkViewport view = last ? VkViewport{float(vp.x), float(vp.y), float(vp.w), float(vp.h), 0, 1} :
								   VkViewport{0, 0, float(ow), float(oh), 0, 1};
	const VkRect2D sc = last ? VkRect2D{{vp.x, vp.y}, {uint32_t(vp.w), uint32_t(vp.h)}} :
							   VkRect2D{{0, 0}, {uint32_t(ow), uint32_t(oh)}};
	vkCmdSetViewport(cmd, 0, 1, &view);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.set[set_idx], 0, nullptr);
	if (d.push_size > 0)
		vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			uint32_t(std::min(d.push_size, 256)), push);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buf, &zero);
	vkCmdDraw(cmd, 4, 1, 0, 0);
	if (!last)
		vkCmdEndRenderPass(cmd);
}

// ---- the target --------------------------------------------------------------------------------------------
bool MakeTarget(int want_w, int want_h)
{
	if (g.offscreen)
	{
		g.out_w = want_w;
		g.out_h = want_h;
		g.target_format = VK_FORMAT_B8G8R8A8_UNORM;
		if (!MakeImage(g.offscreen_target, want_w, want_h, g.target_format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
			return false;
		if (!MakeBuffer(g.readback, VkDeviceSize(want_w) * want_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
		g.final_pass = MakeRenderPass(g.target_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
		g.offscreen_target.fb = MakeFramebuffer(g.final_pass, g.offscreen_target.view, want_w, want_h);
		return g.final_pass && g.offscreen_target.fb;
	}
	// the display: its mode closest to 60 Hz (the largest of those), a plane surface, a FIFO swapchain
	VkDisplayPropertiesKHR disp;
	uint32_t n = 1;
	if (vkGetPhysicalDeviceDisplayPropertiesKHR(g.phys, &n, &disp) < 0 || n < 1)
	{
		OrbisLog("[gpu] no display");
		return false;
	}
	VkDisplayModePropertiesKHR modes[16];
	uint32_t mc = 16;
	if (vkGetDisplayModePropertiesKHR(g.phys, disp.display, &mc, modes) < 0 || mc == 0)
	{
		OrbisLog("[gpu] the display has no modes");
		return false;
	}
	int best = 0;
	for (uint32_t i = 0; i < mc; i++)
	{
		const VkDisplayModeParametersKHR& a = modes[i].parameters;
		const VkDisplayModeParametersKHR& b = modes[best].parameters;
		OrbisLog("[gpu] display mode %u: %ux%u at %.3f Hz", i, a.visibleRegion.width, a.visibleRegion.height,
			a.refreshRate / 1000.0);
		const int da = std::abs(int(a.refreshRate) - 60000), db = std::abs(int(b.refreshRate) - 60000);
		if (da < db - 500 ||
			(std::abs(da - db) <= 500 && a.visibleRegion.width * a.visibleRegion.height >
											 b.visibleRegion.width * b.visibleRegion.height))
			best = int(i);
	}
	const VkDisplayModeParametersKHR& mp = modes[best].parameters;
	VkDisplaySurfaceCreateInfoKHR si = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
	si.displayMode = modes[best].displayMode;
	si.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	si.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	si.imageExtent = mp.visibleRegion;
	if (!Check(vkCreateDisplayPlaneSurfaceKHR(g.instance, &si, nullptr, &g.surface), "vkCreateDisplayPlaneSurfaceKHR"))
		return false;
	VkBool32 sup = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.family, g.surface, &sup);
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps);
	VkSurfaceFormatKHR fmts[16];
	uint32_t fc = 16;
	vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &fc, fmts);
	g.target_format = fc ? fmts[0].format : VK_FORMAT_B8G8R8A8_UNORM;
	VkColorSpaceKHR cs = fc ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	for (uint32_t i = 0; i < fc; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM)
		{
			g.target_format = fmts[i].format;
			cs = fmts[i].colorSpace;
			break;
		}
	g.out_w = int(caps.currentExtent.width ? caps.currentExtent.width : mp.visibleRegion.width);
	g.out_h = int(caps.currentExtent.height ? caps.currentExtent.height : mp.visibleRegion.height);
	VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sc.surface = g.surface;
	sc.minImageCount = std::max(caps.minImageCount, 2u);
	sc.imageFormat = g.target_format;
	sc.imageColorSpace = cs;
	sc.imageExtent = {uint32_t(g.out_w), uint32_t(g.out_h)};
	sc.imageArrayLayers = 1;
	sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	sc.clipped = VK_TRUE;
	if (!sup || !Check(vkCreateSwapchainKHR(g.device, &sc, nullptr, &g.swapchain), "vkCreateSwapchainKHR"))
		return false;
	uint32_t ic = 0;
	vkGetSwapchainImagesKHR(g.device, g.swapchain, &ic, nullptr);
	g.swap_images.resize(ic);
	vkGetSwapchainImagesKHR(g.device, g.swapchain, &ic, g.swap_images.data());
	g.final_pass = MakeRenderPass(g.target_format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
	for (VkImage im : g.swap_images)
	{
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = im;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = g.target_format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView v = VK_NULL_HANDLE;
		if (!Check(vkCreateImageView(g.device, &vi, nullptr, &v), "vkCreateImageView (swapchain)"))
			return false;
		g.swap_views.push_back(v);
		g.swap_fbs.push_back(MakeFramebuffer(g.final_pass, v, g.out_w, g.out_h));
	}
	OrbisLog("[gpu] display %s: %dx%d at %.3f Hz, %u images, format %d", disp.displayName ? disp.displayName : "?",
		g.out_w, g.out_h, mp.refreshRate / 1000.0, ic, int(g.target_format));
	return true;
}

bool HasExtension(const std::vector<VkExtensionProperties>& v, const char* name)
{
	for (const auto& e : v)
		if (!strcmp(e.extensionName, name))
			return true;
	return false;
}
std::string PipelineCachePath()
{
	return OrbisDir("cache") + "/gpu-pipelines.bin";
}

// the pipeline cache to its file (written whole to a temporary file first)
void SavePipelineCache()
{
	if (!g.pcache)
		return;
	size_t n = 0;
	if (vkGetPipelineCacheData(g.device, g.pcache, &n, nullptr) != VK_SUCCESS || n == 0 || n == g.pcache_saved)
		return; // nothing new since the last save
	OrbisMkdirs(OrbisDir("cache"));
	std::vector<uint8_t> data(n);
	if (vkGetPipelineCacheData(g.device, g.pcache, &n, data.data()) != VK_SUCCESS)
		return;
	const std::string path = PipelineCachePath(), tmp = path + ".part";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f)
		return;
	const bool ok = fwrite(data.data(), 1, n, f) == n;
	if (fclose(f) == 0 && ok && rename(tmp.c_str(), path.c_str()) == 0)
	{
		g.pcache_saved = n;
		OrbisLog("[gpu] pipeline cache saved: %zu KiB", n >> 10);
	}
	else
		remove(tmp.c_str());
}

// Builds `preset` on a thread of its own; Present takes it when it is done.
void StartBuild(int preset)
{
	if (g.builder.joinable())
		g.builder.join();
	g.building_preset = preset;
	g.built_ready = false;
	g.building = true;
	g.builder = ps5::BigThread([preset] {
		Chain* c = new Chain;
		const auto t0 = std::chrono::steady_clock::now();
		if (!BuildChain(*c, preset, false))
		{
			FreeChain(*c);
			delete c;
			c = nullptr;
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		OrbisLog("[gpu] shader %s built in the background in %.2f s", ps5slang::PresetAt(preset).name, secs);
		g.built = c;
		g.built_ready = true;
		g.building = false;
	}, size_t(8) << 20);
}

// ---- the shelf's scene: textured quads in perspective, under the surface -------------------------------------
// The quads' shaders read everything from a vertex buffer, as the slang passes do. (2.0 read them from push
// constants with dynamic indexing, q.x[i]: the host's driver drew it, RADV on the PS5 drew nothing at all.)
const char* const kSceneVs = R"(#version 450
layout(location = 0) in vec4 Pos;    // clip space, w the corner's distance (perspective)
layout(location = 1) in vec2 Uv;
layout(location = 2) in float K;     // 0 at the top edge, 1 at the bottom (the reflection's fade)
layout(location = 3) in vec4 P;      // bright, alpha, mode
layout(location = 4) in vec4 Rect;   // glow: the rectangle, in target pixels
layout(location = 5) in vec4 Col;    // glow: colour, radius in target pixels
layout(location = 6) in vec4 UvRect; // the quad's texture rectangle (its edges are smoothed)
layout(location = 0) out vec2 vUv;
layout(location = 1) noperspective out float vK;
layout(location = 2) flat out vec4 vP;
layout(location = 3) flat out vec4 vRect;
layout(location = 4) flat out vec4 vCol;
layout(location = 5) flat out vec4 vUvRect;
void main()
{
	gl_Position = Pos;
	vUv = Uv;
	vK = K;
	vP = P;
	vRect = Rect;
	vCol = Col;
	vUvRect = UvRect;
}
)";
const char* const kSceneFs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec2 vUv;
layout(location = 1) noperspective in float vK;
layout(location = 2) flat in vec4 vP;
layout(location = 3) flat in vec4 vRect;
layout(location = 4) flat in vec4 vCol;
layout(location = 5) flat in vec4 vUvRect;
layout(location = 0) out vec4 FragColor;
void main()
{
	if (vP.z > 1.5)
	{
		// the glow: by the distance to the rectangle; added (alpha 0)
		vec2 f = gl_FragCoord.xy;
		float dx = max(max(vRect.x - f.x, 0.0), f.x - vRect.z);
		float dy = max(max(vRect.y - f.y, 0.0), f.y - vRect.w);
		float d = length(vec2(dx, dy));
		if (d >= vCol.w || d <= 0.0)
			discard;
		float k = 1.0 - d / vCol.w;
		float a = k * k * vP.y * 0.85;
		FragColor = vec4(vCol.rgb * a, 0.0);
		return;
	}
	vec4 c = texture(T, vUv);
	float a = vP.y;
	// the quad's own edges, smoothed over a pixel (no multisampling)
	vec2 lo = min(vUv - min(vUvRect.xy, vUvRect.zw), max(vUvRect.xy, vUvRect.zw) - vUv);
	vec2 fw = max(fwidth(vUv), vec2(1e-6));
	a *= clamp(min(lo.x / fw.x, lo.y / fw.y) + 0.5, 0.0, 1.0);
	if (vP.z > 0.5)
		a *= 0.30 * (1.0 - vK) * (1.0 - vK);
	FragColor = vec4(c.rgb * vP.x * a, a);
}
)";
constexpr int kSceneVertexFloats = 28; // pos 4, uv 2, k 1, pad 1, p 4, rect 4, col 4, uvrect 4, pad 4

bool AllocSceneSet(VkDescriptorSet& set, VkImageView view)
{
	VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = g.dpool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &g.scene_set_layout;
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		if (!Check(vkAllocateDescriptorSets(g.device, &ai, &set), "vkAllocateDescriptorSets (scene)"))
			return false;
	}
	VkDescriptorImageInfo info = {g.scene_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = set;
	w.dstBinding = 0;
	w.descriptorCount = 1;
	w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = &info;
	vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
	return true;
}

// The quad pipeline (its shaders compiled here by glslang). False: the shelf stays on the CPU.
bool InitScene()
{
	std::vector<uint32_t> vs, fs;
	std::string err;
	bool compiled = false;
	{
		// on a thread with a big stack (glslang's parser recurses; the caller's stack may be the SDK's small one)
		ps5::BigThread t([&] {
			compiled = ps5slang::CompileGlsl(kSceneVs, true, vs, err) && ps5slang::CompileGlsl(kSceneFs, false, fs, err);
		}, size_t(4) << 20);
		t.join();
	}
	if (!compiled)
	{
		OrbisLog("[gpu] the shelf's quad shader did not compile (%s): the CPU draws the shelf", err.c_str());
		return false;
	}
	VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = 1;
	li.pBindings = &b;
	if (!Check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.scene_set_layout), "vkCreateDescriptorSetLayout"))
		return false;
	VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &g.scene_set_layout;
	if (!Check(vkCreatePipelineLayout(g.device, &pli, nullptr, &g.scene_layout), "vkCreatePipelineLayout"))
		return false;
	VkShaderModule vm = Module(vs.data(), unsigned(vs.size() * 4)), fm = Module(fs.data(), unsigned(fs.size() * 4));
	if (!vm || !fm)
		return false;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
		{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	st[0].module = vm;
	st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	st[1].module = fm;
	st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkVertexInputBindingDescription vb = {0, kSceneVertexFloats * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	const VkVertexInputAttributeDescription va[7] = {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
		{1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)}, {2, 0, VK_FORMAT_R32_SFLOAT, 6 * sizeof(float)},
		{3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 * sizeof(float)}, {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 * sizeof(float)},
		{5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * sizeof(float)}, {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 * sizeof(float)}};
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vb;
	vi.vertexAttributeDescriptionCount = 7;
	vi.pVertexAttributeDescriptions = va;
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = 0xf;
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	ba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	ds.dynamicStateCount = 2;
	ds.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.stageCount = 2;
	pi.pStages = st;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &ds;
	pi.layout = g.scene_layout;
	pi.renderPass = g.final_pass;
	const bool ok = Check(vkCreateGraphicsPipelines(g.device, g.pcache, 1, &pi, nullptr, &g.scene_pipe),
		"vkCreateGraphicsPipelines (scene)");
	vkDestroyShaderModule(g.device, vm, nullptr);
	vkDestroyShaderModule(g.device, fm, nullptr);
	if (!ok)
		return false;
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = si.minFilter = VK_FILTER_LINEAR;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	si.maxLod = 16.0f;
	if (!Check(vkCreateSampler(g.device, &si, nullptr, &g.scene_sampler), "vkCreateSampler (scene)"))
		return false;
	return AllocSceneSet(g.scene_dummy_set, g.dummy.view);
}

void FreeSceneTex(State::SceneTex& t)
{
	if (t.set)
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		vkFreeDescriptorSets(g.device, g.dpool, 1, &t.set);
	}
	if (t.view)
		vkDestroyImageView(g.device, t.view, nullptr);
	if (t.img)
		vkDestroyImage(g.device, t.img, nullptr);
	if (t.mem)
		vkFreeMemory(g.device, t.mem, nullptr);
	t = State::SceneTex();
}

void FreeScene()
{
	for (auto& kv : g.scene_tex)
		FreeSceneTex(kv.second);
	g.scene_tex.clear();
	g.scene.clear();
	g.scene_on = false;
	FreeBuffer(g.scene_vb);
	if (g.scene_dummy_set)
	{
		std::lock_guard<std::mutex> lock(g.dpool_mx);
		vkFreeDescriptorSets(g.device, g.dpool, 1, &g.scene_dummy_set);
	}
	if (g.scene_sampler)
		vkDestroySampler(g.device, g.scene_sampler, nullptr);
	if (g.scene_pipe)
		vkDestroyPipeline(g.device, g.scene_pipe, nullptr);
	if (g.scene_layout)
		vkDestroyPipelineLayout(g.device, g.scene_layout, nullptr);
	if (g.scene_set_layout)
		vkDestroyDescriptorSetLayout(g.device, g.scene_set_layout, nullptr);
	g.scene_dummy_set = VK_NULL_HANDLE;
	g.scene_sampler = VK_NULL_HANDLE;
	g.scene_pipe = VK_NULL_HANDLE;
	g.scene_layout = VK_NULL_HANDLE;
	g.scene_set_layout = VK_NULL_HANDLE;
}

// a texture with its whole mip chain: level 0 copied in, the others blitted down from it
bool MakeSceneTex(State::SceneTex& t, const uint32_t* px, int w, int h, bool column_major)
{
	int levels = 1;
	while ((std::max(w, h) >> levels) > 0)
		levels++;
	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = VK_FORMAT_R8G8B8A8_UNORM; // the surface's (and the covers') byte order
	ii.extent = {uint32_t(w), uint32_t(h), 1};
	ii.mipLevels = uint32_t(levels);
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (!Check(vkCreateImage(g.device, &ii, nullptr, &t.img), "vkCreateImage (scene)"))
		return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(g.device, t.img, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (ai.memoryTypeIndex == UINT32_MAX)
		ai.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
	if (!Check(vkAllocateMemory(g.device, &ai, nullptr, &t.mem), "vkAllocateMemory (scene)"))
		return false;
	vkBindImageMemory(g.device, t.img, t.mem, 0);
	t.bytes = size_t(req.size);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = t.img;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = ii.format;
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(levels), 0, 1};
	if (!Check(vkCreateImageView(g.device, &vi, nullptr, &t.view), "vkCreateImageView (scene)"))
		return false;

	Buffer st;
	if (!MakeBuffer(st, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		return false;
	uint32_t* dst = static_cast<uint32_t*>(st.map);
	if (column_major)
	{
		for (int x = 0; x < w; x++)
			for (int y = 0; y < h; y++)
				dst[size_t(y) * w + x] = px[size_t(x) * h + y] | 0xff000000u;
	}
	else
		for (size_t i = 0; i < size_t(w) * h; i++)
			dst[i] = px[i] | 0xff000000u;

	std::lock_guard<std::mutex> upload_lock(g.upload_mx);
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.upload_pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkCommandBuffer c;
	if (!Check(vkAllocateCommandBuffers(g.device, &cai, &c), "vkAllocateCommandBuffers"))
	{
		FreeBuffer(st);
		return false;
	}
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(c, &bi);
	auto barrier = [&](int level, VkImageLayout from, VkImageLayout to) {
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = t.img;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 1, 0, 1};
		vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
			0, nullptr, 1, &b);
	};
	for (int l = 0; l < levels; l++)
		barrier(l, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {};
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {uint32_t(w), uint32_t(h), 1};
	vkCmdCopyBufferToImage(c, st.buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	for (int l = 1; l < levels; l++)
	{
		barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit bl = {};
		bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l - 1), 0, 1};
		bl.srcOffsets[1] = {std::max(1, w >> (l - 1)), std::max(1, h >> (l - 1)), 1};
		bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(l), 0, 1};
		bl.dstOffsets[1] = {std::max(1, w >> l), std::max(1, h >> l), 1};
		vkCmdBlitImage(c, t.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
			VK_FILTER_LINEAR);
		barrier(l - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	barrier(levels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkEndCommandBuffer(c);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &c;
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence done = VK_NULL_HANDLE;
	vkCreateFence(g.device, &fi, nullptr, &done);
	bool ok;
	{
		std::lock_guard<std::mutex> q(g.queue_mx);
		ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (scene texture)");
	}
	if (ok)
		vkWaitForFences(g.device, 1, &done, VK_TRUE, UINT64_MAX);
	vkDestroyFence(g.device, done, nullptr);
	vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
	FreeBuffer(st);
	return ok && AllocSceneSet(t.set, t.view);
}

// the quads' vertices into vb (grown when needed: the caller has waited for the frame that used it)
bool FillQuads(Buffer& vb, const std::vector<SceneQuad>& quads, int sw, int sh, int ow, int oh)
{
	const VkDeviceSize need = VkDeviceSize(std::max<size_t>(quads.size(), 1)) * 4 * kSceneVertexFloats * sizeof(float);
	if (vb.size < need)
	{
		FreeBuffer(vb);
		if (!MakeBuffer(vb, std::max<VkDeviceSize>(need * 2, 64 * 1024), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
			return false;
	}
	float* v = static_cast<float*>(vb.map);
	const float kx = float(ow) / float(sw), ky = float(oh) / float(sh);
	for (const SceneQuad& q : quads)
		for (int i = 0; i < 4; i++, v += kSceneVertexFloats)
		{
			const float w = q.w[i] > 1e-3f ? q.w[i] : 1e-3f;
			const float nx = q.x[i] / float(sw) * 2.0f - 1.0f, ny = q.y[i] / float(sh) * 2.0f - 1.0f;
			const float vert[kSceneVertexFloats] = {nx * w, ny * w, 0.0f, w, (i & 1) ? q.u1 : q.u0, (i & 2) ? q.v1 : q.v0,
				(i & 2) ? 1.0f : 0.0f, 0.0f, q.bright, q.alpha, float(q.mode), 0.0f, q.rect[0] * kx, q.rect[1] * ky,
				q.rect[2] * kx, q.rect[3] * ky, q.color[0], q.color[1], q.color[2], q.glow_r * kx, q.u0, q.v0, q.u1, q.v1,
				0.0f, 0.0f, 0.0f, 0.0f};
			memcpy(v, vert, sizeof(vert));
		}
	return true;
}

// the quads, in a render pass already begun: sw x sh the coordinates' space (the surface's), ow x oh the target's
void RecordQuads(VkCommandBuffer cmd, const std::vector<SceneQuad>& quads, int sw, int sh, int ow, int oh, Buffer& vb)
{
	if (quads.empty() || !FillQuads(vb, quads, sw, sh, ow, oh))
		return;
	const VkViewport view = {0, 0, float(ow), float(oh), 0, 1};
	const VkRect2D sc = {{0, 0}, {uint32_t(ow), uint32_t(oh)}};
	vkCmdSetViewport(cmd, 0, 1, &view);
	vkCmdSetScissor(cmd, 0, 1, &sc);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.scene_pipe);
	const VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buf, &zero);
	for (size_t n = 0; n < quads.size(); n++)
	{
		const SceneQuad& q = quads[n];
		VkDescriptorSet set = g.scene_dummy_set;
		if (q.mode != kQuadGlow)
		{
			auto it = g.scene_tex.find(q.tex);
			if (it == g.scene_tex.end())
				continue;
			it->second.used = g.presents;
			set = it->second.set;
		}
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.scene_layout, 0, 1, &set, 0, nullptr);
		vkCmdDraw(cmd, 4, 1, uint32_t(n * 4), 0);
	}
}

void RecordScene(VkCommandBuffer cmd)
{
	static int logs = 0;
	if (logs < 3 && !g.scene.empty())
	{
		logs++;
		size_t found = 0;
		for (const SceneQuad& q : g.scene)
			found += q.mode == kQuadGlow || g.scene_tex.count(q.tex);
		OrbisLog("[gpu] shelf scene: %zu quad(s) drawn (%zu with their texture), %zu texture(s) kept, %dx%d output",
			g.scene.size(), found, g.scene_tex.size(), g.out_w, g.out_h);
	}
	RecordQuads(cmd, g.scene, g.ui_w, g.ui_h, g.out_w, g.out_h, g.scene_vb);
}

// The scene's self-test, at start: two quads drawn into a small image of the target's format with the target's
// render pass -- one flat (w 1, a blue texture), one in perspective as a cover is (w about 1000, a red texture,
// column-major as the covers are) -- and the pixels read back. The 2.0 shelf drew nothing on a PS5 while the
// host's driver drew it right: this says what the console's driver makes of it, and keeps the shelf on the CPU
// when the picture is not what it should be.
bool SceneSelfTest()
{
	constexpr int S = 64, SW = 96; // three 32-pixel columns: flat, perspective, glow
	const uint64_t kBlue = 0xfff0000000000001ull, kRed = 0xfff0000000000002ull;
	uint32_t blue[16], red[16];
	for (int i = 0; i < 16; i++)
	{
		blue[i] = 0xffff0000u; // R8G8B8A8 in memory: R G B A -> as a uint32, A B G R
		red[i] = 0xff0000ffu;
	}
	if (!SceneTexture(kBlue, blue, 4, 4, false) || !SceneTexture(kRed, red, 4, 4, true))
	{
		OrbisLog("[gpu] shelf self-test: the test textures could not be made");
		return false;
	}
	Image im;
	bool ok = MakeImage(im, SW, S, g.target_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (ok)
		im.fb = MakeFramebuffer(g.final_pass, im.view, SW, S);
	Buffer rb, test_vb;
	ok = ok && im.fb && MakeBuffer(rb, VkDeviceSize(SW) * S * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	std::vector<SceneQuad> quads(3);
	if (ok)
	{
		// left half: flat, blue
		SceneQuad& a = quads[0];
		a.tex = kBlue;
		const float ax[4] = {2, 30, 2, 30}, ay[4] = {2, 2, 62, 62};
		for (int i = 0; i < 4; i++)
		{
			a.x[i] = ax[i];
			a.y[i] = ay[i];
		}
		// right half: in perspective (its two ends at different distances, as a side cover), red
		SceneQuad& b = quads[1];
		b.tex = kRed;
		const float bx[4] = {34, 62, 34, 62}, by[4] = {2, 10, 62, 54}, bw[4] = {1000, 1300, 1000, 1300};
		for (int i = 0; i < 4; i++)
		{
			b.x[i] = bx[i];
			b.y[i] = by[i];
			b.w[i] = bw[i];
		}
		// third column: a green glow (no texture) around a point at (80, 32), 40 pixels out
		SceneQuad& c = quads[2];
		c.mode = kQuadGlow;
		const float cx[4] = {64, 96, 64, 96}, cy[4] = {0, 0, 64, 64};
		for (int i = 0; i < 4; i++)
		{
			c.x[i] = cx[i];
			c.y[i] = cy[i];
		}
		c.rect[0] = c.rect[2] = 80;
		c.rect[1] = c.rect[3] = 32;
		c.glow_r = 40;
		c.color[1] = 1;
	}
	VkCommandBuffer c = VK_NULL_HANDLE;
	if (ok)
	{
		std::lock_guard<std::mutex> upload_lock(g.upload_mx);
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = g.upload_pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		ok = Check(vkAllocateCommandBuffers(g.device, &cai, &c), "vkAllocateCommandBuffers (self-test)");
		if (ok)
		{
			VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkBeginCommandBuffer(c, &bi);
			VkClearValue cv = {};
			cv.color.float32[3] = 1.0f; // black, as the frame's
			VkRenderPassBeginInfo rb2 = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rb2.renderPass = g.final_pass;
			rb2.framebuffer = im.fb;
			rb2.renderArea = {{0, 0}, {uint32_t(SW), uint32_t(S)}};
			rb2.clearValueCount = 1;
			rb2.pClearValues = &cv;
			vkCmdBeginRenderPass(c, &rb2, VK_SUBPASS_CONTENTS_INLINE);
			RecordQuads(c, quads, SW, S, SW, S, test_vb);
			vkCmdEndRenderPass(c);
			// the render pass leaves it in the target's final layout (present, or transfer source off screen)
			im.layout = g.offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			Barrier(c, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy r = {};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {uint32_t(SW), uint32_t(S), 1};
			vkCmdCopyImageToBuffer(c, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
			vkEndCommandBuffer(c);
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &c;
			VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			VkFence done = VK_NULL_HANDLE;
			vkCreateFence(g.device, &fi, nullptr, &done);
			{
				std::lock_guard<std::mutex> q(g.queue_mx);
				ok = Check(vkQueueSubmit(g.queue, 1, &si, done), "vkQueueSubmit (self-test)");
			}
			if (ok)
				ok = vkWaitForFences(g.device, 1, &done, VK_TRUE, 5000000000ull) == VK_SUCCESS;
			vkDestroyFence(g.device, done, nullptr);
			vkFreeCommandBuffers(g.device, g.upload_pool, 1, &c);
		}
	}
	bool good = false;
	if (ok)
	{
		// the target is B8G8R8A8 (or R8G8B8A8): find each pixel's red and blue whatever the order
		const bool bgra = g.target_format == VK_FORMAT_B8G8R8A8_UNORM || g.target_format == VK_FORMAT_B8G8R8A8_SRGB;
		auto px = [&](int x, int y, int& r, int& gg, int& b) {
			const uint8_t* p = static_cast<const uint8_t*>(rb.map) + (size_t(y) * SW + x) * 4;
			r = bgra ? p[2] : p[0];
			gg = p[1];
			b = bgra ? p[0] : p[2];
		};
		int r1, g1, b1, r2, g2, b2, r0, g0, b0, r3, g3, b3;
		px(16, 32, r1, g1, b1); // flat quad: blue
		px(48, 32, r2, g2, b2); // perspective quad: red
		px(32, 1, r0, g0, b0);  // between: the black clear
		px(84, 32, r3, g3, b3); // the glow, 4 pixels from its point: green
		OrbisLog("[gpu] shelf self-test (format %d): flat quad %d,%d,%d (want blue), perspective quad %d,%d,%d (want red), "
				 "glow %d,%d,%d (want green), background %d,%d,%d (want black)",
			int(g.target_format), r1, g1, b1, r2, g2, b2, r3, g3, b3, r0, g0, b0);
		good = b1 > 200 && r1 < 60 && r2 > 200 && b2 < 60 && g3 > 100 && r3 < 60 && r0 < 30 && b0 < 30;
	}
	else
		OrbisLog("[gpu] shelf self-test: could not run");
	FreeBuffer(rb);
	FreeBuffer(test_vb);
	FreeImage(im);
	// the test textures go
	for (uint64_t k : {kBlue, kRed})
	{
		auto it = g.scene_tex.find(k);
		if (it != g.scene_tex.end())
		{
			FreeSceneTex(it->second);
			g.scene_tex.erase(it);
		}
	}
	return good;
}
} // namespace

bool Building()
{
	return g.building.load();
}

int ShaderParamCount(int shader)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	return p > 1 && p < ps5slang::PresetCount() ? ps5slang::PresetAt(p).param_count : 0;
}

bool ShaderParam(int shader, int i, ParamInfo* out)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount() || i < 0 || i >= ps5slang::PresetAt(p).param_count)
		return false;
	const ps5slang::Preset& pr = ps5slang::PresetAt(p);
	const ps5slang::Param& d = pr.params[i];
	out->name = d.name;
	out->desc = d.desc;
	out->def = d.value;
	out->min = d.min;
	out->max = d.max;
	out->step = d.step;
	out->value = d.value;
	std::lock_guard<std::mutex> lk(g.param_mx);
	const auto ov = g.param_overrides.find(pr.name);
	if (ov != g.param_overrides.end())
	{
		const auto v = ov->second.find(d.name);
		if (v != ov->second.end())
			out->value = v->second;
	}
	return true;
}

void SetShaderParam(int shader, const char* name, float value)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount())
		return;
	const ps5slang::Preset& pr = ps5slang::PresetAt(p);
	for (int i = 0; i < pr.param_count; i++)
		if (!strcmp(pr.params[i].name, name))
		{
			std::lock_guard<std::mutex> lk(g.param_mx);
			if (value == pr.params[i].value)
				g.param_overrides[pr.name].erase(name);
			else
				g.param_overrides[pr.name][name] = value;
			if (g.game_chain.preset == p && size_t(i) < g.game_chain.params.size())
				g.game_chain.params[size_t(i)] = value;
		}
}

void ResetShaderParams(int shader)
{
	const int p = shader <= 0 ? -1 : shader + 1;
	if (p <= 1 || p >= ps5slang::PresetCount())
		return;
	{
		std::lock_guard<std::mutex> lk(g.param_mx);
		g.param_overrides.erase(ps5slang::PresetAt(p).name);
	}
	if (g.game_chain.preset == p)
		ApplyOverrides(g.game_chain);
}

namespace
{
std::string ShaderParamsPath()
{
	return OrbisRoot() + "/shader-params.ini";
}

// "<preset>\t<parameter>\t<value>" a line (preset names have spaces); a preset or parameter this build doesn't have is
// left out
void LoadShaderParams()
{
	FILE* f = fopen(ShaderParamsPath().c_str(), "r");
	if (!f)
		return;
	char line[512];
	int n = 0;
	std::lock_guard<std::mutex> lk(g.param_mx);
	while (fgets(line, sizeof(line), f))
	{
		char* t1 = strchr(line, '\t');
		char* t2 = t1 ? strchr(t1 + 1, '\t') : nullptr;
		if (!t2)
			continue;
		*t1 = *t2 = 0;
		const char* preset = line;
		const char* name = t1 + 1;
		char* end = nullptr;
		const float v = strtof(t2 + 1, &end);
		if (end == t2 + 1 || !std::isfinite(v))
			continue;
		for (int p = 2; p < ps5slang::PresetCount(); p++)
		{
			const ps5slang::Preset& pr = ps5slang::PresetAt(p);
			if (strcmp(pr.name, preset))
				continue;
			if (ps5slang::IsExternal(p))
			{
				// not compiled yet: its parameters are not known, the value is kept as it is (applied by name)
				g.param_overrides[pr.name][name] = v;
				n++;
				continue;
			}
			for (int i = 0; i < pr.param_count; i++)
			{
				const ps5slang::Param& d = pr.params[i];
				if (!strcmp(d.name, name))
				{
					g.param_overrides[pr.name][d.name] = std::max(d.min, std::min(d.max, v));
					n++;
				}
			}
		}
	}
	fclose(f);
	if (n)
		OrbisLog("[gpu] %d shader parameter(s) set in the menu, from %s", n, ShaderParamsPath().c_str());
}
} // namespace

bool SaveShaderParams()
{
	std::string text;
	{
		std::lock_guard<std::mutex> lk(g.param_mx);
		for (const auto& pr : g.param_overrides)
			for (const auto& kv : pr.second)
			{
				char buf[64];
				snprintf(buf, sizeof(buf), "\t%.6g\n", double(kv.second));
				text += pr.first + "\t" + kv.first + buf;
			}
	}
	const std::string path = ShaderParamsPath(), tmp = path + ".part";
	if (text.empty())
	{
		remove(path.c_str());
		return true;
	}
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
		return false;
	const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0)
	{
		remove(tmp.c_str());
		OrbisLog("[gpu] could not write %s", path.c_str());
		return false;
	}
	return true;
}

bool Init(int ui_w, int ui_h, bool offscreen, int out_w, int out_h)
{
	if (g.ok)
		return true;
	g.offscreen = offscreen;
	g.ui_w = ui_w;
	g.ui_h = ui_h;
#define GPU_LOAD_GLOBAL(name) vk##name = reinterpret_cast<PFN_vk##name>(GPU_GET_INSTANCE_PROC(nullptr, "vk" #name));
	GPU_GLOBAL_FNS(GPU_LOAD_GLOBAL)
#undef GPU_LOAD_GLOBAL
	if (!vkCreateInstance)
	{
		OrbisLog("[gpu] no Vulkan driver");
		return false;
	}
	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> iexts(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
	std::vector<const char*> want;
	if (!offscreen)
	{
		if (!HasExtension(iexts, VK_KHR_SURFACE_EXTENSION_NAME) || !HasExtension(iexts, VK_KHR_DISPLAY_EXTENSION_NAME))
		{
			OrbisLog("[gpu] the driver has no VK_KHR_display");
			return false;
		}
		want = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
	}
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "Genesis Plus GX PS5";
	app.pEngineName = "genplus-ps5";
	app.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ii.pApplicationInfo = &app;
	ii.enabledExtensionCount = uint32_t(want.size());
	ii.ppEnabledExtensionNames = want.data();
	if (!Check(vkCreateInstance(&ii, nullptr, &g.instance), "vkCreateInstance"))
		return false;
#define GPU_LOAD_INSTANCE(name) vk##name = reinterpret_cast<PFN_vk##name>(GPU_GET_INSTANCE_PROC(g.instance, "vk" #name));
	GPU_INSTANCE_FNS(GPU_LOAD_INSTANCE)
	if (!offscreen)
	{
		GPU_DISPLAY_FNS(GPU_LOAD_INSTANCE)
	}
#undef GPU_LOAD_INSTANCE
	uint32_t pc = 0;
	vkEnumeratePhysicalDevices(g.instance, &pc, nullptr);
	if (pc == 0)
	{
		OrbisLog("[gpu] no GPU");
		Shutdown();
		return false;
	}
	std::vector<VkPhysicalDevice> devs(pc);
	vkEnumeratePhysicalDevices(g.instance, &pc, devs.data());
	g.phys = devs[0];
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(g.phys, &props);
	snprintf(g.device_name, sizeof(g.device_name), "%s", props.deviceName);
	g.ts_period = props.limits.timestampPeriod;
	vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memprops);
	uint32_t qc = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, nullptr);
	std::vector<VkQueueFamilyProperties> qf(qc);
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, qf.data());
	g.family = UINT32_MAX;
	for (uint32_t i = 0; i < qc; i++)
		if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			g.family = i;
			g.timestamps = qf[i].timestampValidBits > 0 && g.ts_period > 0;
			break;
		}
	if (g.family == UINT32_MAX)
	{
		OrbisLog("[gpu] no graphics queue");
		Shutdown();
		return false;
	}
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> dexts(n);
	vkEnumerateDeviceExtensionProperties(g.phys, nullptr, &n, dexts.data());
	std::vector<const char*> dwant;
	if (!offscreen)
	{
		if (!HasExtension(dexts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
		{
			OrbisLog("[gpu] the driver has no VK_KHR_swapchain");
			Shutdown();
			return false;
		}
		dwant.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueFamilyIndex = g.family;
	qi.queueCount = 1;
	qi.pQueuePriorities = &prio;
	VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	di.queueCreateInfoCount = 1;
	di.pQueueCreateInfos = &qi;
	di.enabledExtensionCount = uint32_t(dwant.size());
	di.ppEnabledExtensionNames = dwant.data();
	if (!Check(vkCreateDevice(g.phys, &di, nullptr, &g.device), "vkCreateDevice"))
	{
		Shutdown();
		return false;
	}
#define GPU_LOAD_DEVICE(name) vk##name = reinterpret_cast<PFN_vk##name>(vkGetDeviceProcAddr(g.device, "vk" #name));
	GPU_DEVICE_FNS(GPU_LOAD_DEVICE)
	if (!offscreen)
	{
		GPU_SWAPCHAIN_FNS(GPU_LOAD_DEVICE)
	}
#undef GPU_LOAD_DEVICE
	vkGetDeviceQueue(g.device, g.family, 0, &g.queue);

	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = g.family;
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	bool ok = Check(vkCreateCommandPool(g.device, &cpi, nullptr, &g.pool), "vkCreateCommandPool");
	if (ok)
	{
		cai.commandPool = g.pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		ok = Check(vkAllocateCommandBuffers(g.device, &cai, &g.cmd), "vkAllocateCommandBuffers") &&
			 Check(vkCreateFence(g.device, &fi, nullptr, &g.fence), "vkCreateFence") &&
			 Check(vkCreateSemaphore(g.device, &smi, nullptr, &g.acquired), "vkCreateSemaphore") &&
			 Check(vkCreateSemaphore(g.device, &smi, nullptr, &g.rendered), "vkCreateSemaphore");
	}
	if (ok)
	{
		VkCommandPoolCreateInfo upi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		upi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		upi.queueFamilyIndex = g.family;
		ok = Check(vkCreateCommandPool(g.device, &upi, nullptr, &g.upload_pool), "vkCreateCommandPool (uploads)");
	}
	if (ok)
	{
		// the pipelines built before (the driver checks the header: another driver or GPU starts empty)
		std::vector<uint8_t> data;
		if (FILE* f = fopen(PipelineCachePath().c_str(), "rb"))
		{
			fseek(f, 0, SEEK_END);
			const long n = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (n > 0 && n < (64 << 20))
			{
				data.resize(size_t(n));
				if (fread(data.data(), 1, data.size(), f) != data.size())
					data.clear();
			}
			fclose(f);
		}
		VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		ci.initialDataSize = data.size();
		ci.pInitialData = data.empty() ? nullptr : data.data();
		if (vkCreatePipelineCache(g.device, &ci, nullptr, &g.pcache) == VK_SUCCESS)
			g.pcache_saved = data.size();
		else
		{
			ci.initialDataSize = 0;
			ci.pInitialData = nullptr;
			vkCreatePipelineCache(g.device, &ci, nullptr, &g.pcache);
		}
		OrbisLog("[gpu] pipeline cache: %zu KiB from %s", data.size() >> 10, PipelineCachePath().c_str());
	}
	if (ok && g.timestamps)
	{
		VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qpi.queryCount = 2;
		g.timestamps = vkCreateQueryPool(g.device, &qpi, nullptr, &g.queries) == VK_SUCCESS;
	}
	// samplers: [linear][wrap]
	for (int f = 0; ok && f < 2; f++)
		for (int w = 0; ok && w < 4; w++)
		{
			static const VkSamplerAddressMode kWrap[4] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
				VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
				VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
			VkSamplerCreateInfo s = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			s.magFilter = s.minFilter = f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			s.addressModeU = s.addressModeV = s.addressModeW = kWrap[w];
			s.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			s.maxLod = 0.0f;
			ok = Check(vkCreateSampler(g.device, &s, nullptr, &g.samplers[f][w]), "vkCreateSampler");
		}
	if (ok)
	{
		VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		dpi.maxSets = 1024;
		dpi.poolSizeCount = 2;
		dpi.pPoolSizes = sizes;
		ok = Check(vkCreateDescriptorPool(g.device, &dpi, nullptr, &g.dpool), "vkCreateDescriptorPool");
	}
	// the quad: Position (vec4), TexCoord (vec2), as RetroArch feeds slang passes
	static const float kQuad[4][6] = {{0, 0, 0, 1, 0, 0}, {1, 0, 0, 1, 1, 0}, {0, 1, 0, 1, 0, 1}, {1, 1, 0, 1, 1, 1}};
	ok = ok && MakeBuffer(g.vertices, sizeof(kQuad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	if (ok)
		memcpy(g.vertices.map, kQuad, sizeof(kQuad));
	ok = ok && MakeTarget(out_w, out_h);
	ok = ok && MakeImage(g.ui, ui_w, ui_h, VK_FORMAT_R8G8B8A8_UNORM,
				   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
		 MakeBuffer(g.ui_staging, VkDeviceSize(ui_w) * ui_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) &&
		 MakeBuffer(g.game_staging, 2048 * 1024 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	if (ok)
	{
		const uint8_t black[4] = {0, 0, 0, 255};
		ok = MakeImage(g.dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
				 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
			 UploadImage(g.dummy, black, 1, 1);
	}
	ok = ok && BuildChain(g.ui_chain, 1, true); // preset 1: the surface over the game
	if (!ok)
	{
		OrbisLog("[gpu] set-up failed: the CPU draws the picture");
		Shutdown();
		return false;
	}
	g.ok = true;
	if (ok && !InitScene())
		FreeScene(); // the shelf stays on the CPU
	else if (ok && !SceneSelfTest())
	{
		OrbisLog("[gpu] the shelf's quads did not come out right on this driver: the CPU draws the shelf");
		FreeScene();
	}
	ps5slang::ScanExternal();
#ifndef __PROSPERO__
	ps5slang::CheckAllExternal();
#endif
	LoadShaderParams();
	OrbisLog("[gpu] %s: Vulkan renderer, %dx%d output, %d shader presets", g.device_name, g.out_w, g.out_h,
		ShaderCount());
	return true;
}

bool Active()
{
	return g.ok;
}

void Shutdown()
{
	if (g.builder.joinable())
		g.builder.join();
	if (g.built)
	{
		FreeChain(*g.built);
		delete g.built;
		g.built = nullptr;
	}
	if (g.device)
	{
		vkDeviceWaitIdle(g.device);
		SavePipelineCache();
		if (g.pcache)
			vkDestroyPipelineCache(g.device, g.pcache, nullptr);
		if (g.upload_pool)
			vkDestroyCommandPool(g.device, g.upload_pool, nullptr);
		FreeScene();
		FreeChain(g.game_chain);
		FreeChain(g.ui_chain);
		FreeImage(g.ui);
		FreeImage(g.original);
		FreeImage(g.dummy);
		FreeImage(g.offscreen_target);
		FreeBuffer(g.ui_staging);
		FreeBuffer(g.game_staging);
		FreeBuffer(g.readback);
		FreeBuffer(g.vertices);
		for (VkFramebuffer f : g.swap_fbs)
			vkDestroyFramebuffer(g.device, f, nullptr);
		for (VkImageView v : g.swap_views)
			vkDestroyImageView(g.device, v, nullptr);
		if (g.swapchain)
			vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
		if (g.final_pass)
			vkDestroyRenderPass(g.device, g.final_pass, nullptr);
		for (auto& row : g.samplers)
			for (VkSampler s : row)
				if (s)
					vkDestroySampler(g.device, s, nullptr);
		if (g.dpool)
			vkDestroyDescriptorPool(g.device, g.dpool, nullptr);
		if (g.queries)
			vkDestroyQueryPool(g.device, g.queries, nullptr);
		if (g.acquired)
			vkDestroySemaphore(g.device, g.acquired, nullptr);
		if (g.rendered)
			vkDestroySemaphore(g.device, g.rendered, nullptr);
		if (g.fence)
			vkDestroyFence(g.device, g.fence, nullptr);
		if (g.pool)
			vkDestroyCommandPool(g.device, g.pool, nullptr);
		vkDestroyDevice(g.device, nullptr);
	}
	if (g.surface && vkDestroySurfaceKHR)
		vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
	if (g.instance && vkDestroyInstance)
		vkDestroyInstance(g.instance, nullptr);
	g.~State();
	new (&g) State();
}

int OutWidth()
{
	return g.out_w;
}

int OutHeight()
{
	return g.out_h;
}

const char* DeviceName()
{
	return g.device_name;
}

// preset 0 is the plain picture, 1 the surface; the shaders the user picks are 0 (plain) and presets 2... (the
// built-in ones, then the user's own)
int ShaderCount()
{
	return std::max(1, ps5slang::PresetCount() - 1);
}

const char* ShaderName(int i)
{
	if (i <= 0 || i + 1 >= ps5slang::PresetCount())
		return "Off";
	return ps5slang::PresetAt(i + 1).name;
}

bool ShaderFullscreen(int i)
{
	const int p = i <= 0 ? 0 : i + 1;
	return p < ps5slang::PresetCount() && ps5slang::PresetAt(p).fullscreen;
}

int PassCount(int i)
{
	const int p = i <= 0 ? 0 : i + 1;
	return p < ps5slang::PresetCount() ? ps5slang::PresetAt(p).pass_count : 0;
}

void SetGame(const uint32_t* argb, int w, int h, Rect dst, int shader, bool smooth, bool scanlines)
{
	if (!g.ok || !argb || w <= 0 || h <= 0 || size_t(w) * h * 4 > g.game_staging.size)
		return;
	if (g.in_flight)
	{
		vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
		// the previous frame is done: read its GPU time
		if (g.timestamps && g.queries_written)
		{
			uint64_t ts[2];
			if (vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
					VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			{
				g.gpu_ms += double(ts[1] - ts[0]) * g.ts_period / 1e6;
				g.gpu_frames++;
			}
			g.queries_written = false;
		}
		g.in_flight = false;
	}
	memcpy(g.game_staging.map, argb, size_t(w) * h * 4);
	g.game = true;
	g.presents_without_game = 0;
	g.game_new = true;
	g.game_w = w;
	g.game_h = h;
	g.game_dst = dst;
	g.shader = std::max(0, std::min(shader, ShaderCount() - 1));
	g.smooth = smooth;
	g.scanlines = scanlines;
}

void HideGame()
{
	g.game = false;
}

double TakeShaderMs()
{
	const double ms = g.gpu_frames ? g.gpu_ms / g.gpu_frames : 0.0;
	g.gpu_ms = 0;
	g.gpu_frames = 0;
	return ms;
}

bool SceneAvailable()
{
	return g.ok && g.scene_pipe != VK_NULL_HANDLE;
}

bool HasSceneTexture(uint64_t key)
{
	return g.scene_tex.count(key) != 0;
}

bool SceneTexture(uint64_t key, const uint32_t* px, int w, int h, bool column_major)
{
	if (!SceneAvailable() || !px || w <= 0 || h <= 0)
		return false;
	if (g.scene_tex.count(key))
		return true;
	// at most kMax textures: the ones not drawn for the longest go first (none drawn in the frame in flight)
	constexpr size_t kMax = 48;
	if (g.scene_tex.size() >= kMax)
	{
		if (g.in_flight)
		{
			vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
		}
		while (g.scene_tex.size() >= kMax)
		{
			auto old = g.scene_tex.begin();
			for (auto it = g.scene_tex.begin(); it != g.scene_tex.end(); ++it)
				if (it->second.used < old->second.used)
					old = it;
			bool in_scene = false;
			for (const SceneQuad& q : g.scene)
				in_scene |= q.tex == old->first;
			if (in_scene)
				break; // all in use: one more, then
			FreeSceneTex(old->second);
			g.scene_tex.erase(old);
		}
	}
	State::SceneTex t;
	if (!MakeSceneTex(t, px, w, h, column_major))
	{
		FreeSceneTex(t);
		static int failures = 0;
		if (failures++ < 5)
			OrbisLog("[gpu] a %dx%d texture for the shelf could not be made", w, h);
		return false;
	}
	t.used = g.presents;
	g.scene_tex[key] = t;
	return true;
}

void SetScene(const std::vector<SceneQuad>& quads)
{
	g.scene = quads;
	g.scene_on = true;
}

void ClearScene()
{
	g.scene.clear();
	g.scene_on = false;
}

bool Present(const uint32_t* ui, Rect dirty, bool wait_vsync)
{
	if (!g.ok)
		return false;
	if (g.in_flight)
	{
		vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
		g.in_flight = false;
	}
	vkResetFences(g.device, 1, &g.fence);
	// the surface's changed rows to the staging buffer
	if (g.ui_full)
		dirty = {0, 0, g.ui_w, g.ui_h};
	dirty.x = std::max(0, dirty.x);
	dirty.y = std::max(0, dirty.y);
	dirty.w = std::min(dirty.w, g.ui_w - dirty.x);
	dirty.h = std::min(dirty.h, g.ui_h - dirty.y);
	const bool ui_copy = dirty.w > 0 && dirty.h > 0;
	if (ui_copy)
		for (int y = dirty.y; y < dirty.y + dirty.h; y++)
			memcpy(static_cast<uint32_t*>(g.ui_staging.map) + size_t(y) * g.ui_w + dirty.x,
				ui + size_t(y) * g.ui_w + dirty.x, size_t(dirty.w) * 4);

	// the game's chain for the shader picked: built on a thread of its own, the picture going on meanwhile with the
	// shader it had (or the plain picture, which builds at once)
	int preset = g.shader <= 0 ? 0 : g.shader + 1;
	if (g.built_ready)
	{
		if (g.builder.joinable())
			g.builder.join();
		g.built_ready = false;
		Chain* c = g.built;
		g.built = nullptr;
		if (c)
		{
			vkDeviceWaitIdle(g.device);
			FreeChain(g.game_chain);
			g.game_chain = std::move(*c);
			delete c;
			ApplyOverrides(g.game_chain); // what the menu changed while it was being built
			OrbisLog("[gpu] shader %s: %d pass(es)", ps5slang::PresetAt(g.game_chain.preset).name,
				int(g.game_chain.passes.size()));
			SavePipelineCache();
		}
		else if (g.building_preset == preset)
		{
			OrbisLog("[gpu] shader %s could not be set up: the plain picture", ShaderName(g.shader));
			g.shader = 0;
			preset = 0;
		}
		g.building_preset = -1;
	}
	if (g.game && g.game_chain.preset != preset)
	{
		if (preset == 0 || !g.game_chain.built)
		{
			// the plain picture (or nothing yet on screen): built here, at once
			vkDeviceWaitIdle(g.device);
			BuildChain(g.game_chain, 0, false);
			if (preset == 0)
				OrbisLog("[gpu] shader Off: 1 pass(es)");
		}
		if (preset != 0 && !g.building && g.building_preset != preset)
		{
			StartBuild(preset);
#ifndef __PROSPERO__
			// host tests: GENPLUS_HOST_GPU_SYNC_BUILD=1 waits for it (a shader's picture checked at a set frame)
			if (getenv("GENPLUS_HOST_GPU_SYNC_BUILD") && g.builder.joinable())
				g.builder.join();
#endif
		}
	}
	// the plain picture's filter and scanlines
	if (g.game && preset == 0 && !g.game_chain.params.empty())
		g.game_chain.params[0] = g.scanlines ? 1.0f : 0.0f;

	uint32_t index = 0;
	if (!g.offscreen)
	{
		const VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.acquired, VK_NULL_HANDLE, &index);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			OrbisLog("[gpu] vkAcquireNextImageKHR -> %d", int(r));
			return false;
		}
	}
	VkCommandBuffer cmd = g.cmd;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	if (ui_copy)
	{
		Barrier(cmd, g.ui, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy r = {};
		r.bufferOffset = (VkDeviceSize(dirty.y) * g.ui_w + dirty.x) * 4;
		r.bufferRowLength = uint32_t(g.ui_w);
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageOffset = {dirty.x, dirty.y, 0};
		r.imageExtent = {uint32_t(dirty.w), uint32_t(dirty.h), 1};
		vkCmdCopyBufferToImage(cmd, g.ui_staging.buf, g.ui.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
		g.ui_full = false;
	}
	Barrier(cmd, g.ui, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// the game's frame, the history it pushes back
	Chain& gc = g.game_chain;
	Rect vp = {0, 0, 0, 0};
	const bool draw_game = g.game && gc.built && g.presents_without_game < 30;
	g.presents_without_game++;
	if (draw_game)
	{
		if (g.game_new)
		{
			if (g.original.w != g.game_w || g.original.h != g.game_h)
			{
				vkDeviceWaitIdle(g.device);
				FreeImage(g.original);
				for (Image& im : gc.hist)
					FreeImage(im);
				gc.hist.clear();
				MakeImage(g.original, g.game_w, g.game_h, VK_FORMAT_B8G8R8A8_UNORM,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			}
			if (gc.history > 0)
			{
				if (gc.hist.size() != size_t(gc.history))
				{
					gc.hist.resize(size_t(gc.history));
					for (Image& im : gc.hist)
						if (!im.img)
							MakeImage(im, g.game_w, g.game_h, VK_FORMAT_B8G8R8A8_UNORM,
								VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
									VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
				}
				// the oldest goes, the others move back one, the last original becomes OriginalHistory1
				std::rotate(gc.hist.rbegin(), gc.hist.rbegin() + 1, gc.hist.rend());
				Barrier(cmd, g.original, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				Barrier(cmd, gc.hist[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				VkImageCopy ic = {};
				ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				ic.extent = {uint32_t(g.game_w), uint32_t(g.game_h), 1};
				vkCmdCopyImage(cmd, g.original.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gc.hist[0].img,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
			}
			Barrier(cmd, g.original, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			VkBufferImageCopy r = {};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {uint32_t(g.game_w), uint32_t(g.game_h), 1};
			vkCmdCopyBufferToImage(cmd, g.game_staging.buf, g.original.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			g.game_new = false;
			g.frame_count++;
		}
		Barrier(cmd, g.original, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		// the surface rectangle on the target
		const double sx = double(g.out_w) / g.ui_w, sy = double(g.out_h) / g.ui_h;
		vp = {int(std::lround(g.game_dst.x * sx)), int(std::lround(g.game_dst.y * sy)),
			int(std::lround(g.game_dst.w * sx)), int(std::lround(g.game_dst.h * sy))};
		if (ps5slang::PresetAt(gc.preset).fullscreen)
			vp = {0, 0, g.out_w, g.out_h}; // the preset places the game itself, in its bezel
		vp.w = std::max(1, std::min(vp.w, g.out_w - vp.x));
		vp.h = std::max(1, std::min(vp.h, g.out_h - vp.y));
		if (!SizeChain(gc, vp.w, vp.h))
			return false;
	}
	if (g.timestamps)
	{
		vkCmdResetQueryPool(cmd, g.queries, 0, 2);
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 0);
	}
	const bool plain_smooth = g.smooth && !(vp.w % std::max(1, g.game_w) == 0 && vp.h % std::max(1, g.game_h) == 0);
	// the intermediate passes render before the final render pass begins; the textures the last pass reads go to
	// the shader's layout before it too (no barrier may sit inside a render pass)
	if (draw_game)
	{
		for (size_t i = 0; i + 1 < gc.passes.size(); i++)
			RecordPass(cmd, gc, i, vp, plain_smooth);
		const PassRt& lp = gc.passes.back();
		for (int s = 0; s < lp.def->sampler_count; s++)
		{
			Tex t;
			int tw, th;
			if (Resolve(gc, int(gc.passes.size() - 1), lp.def->samplers[s].name, t, tw, th) && t.im->img)
				Barrier(cmd, *t.im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
	}
	VkFramebuffer fb = g.offscreen ? g.offscreen_target.fb : g.swap_fbs[index];
	VkClearValue cv = {};
	cv.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rb.renderPass = g.final_pass;
	rb.framebuffer = fb;
	rb.renderArea = {{0, 0}, {uint32_t(g.out_w), uint32_t(g.out_h)}};
	rb.clearValueCount = 1;
	rb.pClearValues = &cv;
	vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
	if (draw_game)
	{
		// a feedback last pass would flip its images: the last pass never has any (it draws to the target)
		RecordPass(cmd, gc, gc.passes.size() - 1, vp, plain_smooth);
	}
	// the shelf's scene, under the surface (no game then)
	if (g.scene_on && !draw_game && g.scene_pipe)
		RecordScene(cmd);
	else if (g.scene_on)
	{
		static int skipped = 0;
		if (skipped++ == 0)
			OrbisLog("[gpu] shelf scene not drawn: game %d, pipeline %d", int(draw_game), int(g.scene_pipe != VK_NULL_HANDLE));
	}
	g.presents++;
	// the surface over everything
	{
		Chain& uc = g.ui_chain;
		PassRt& p = uc.passes[0];
		const ps5slang::Pass& d = *p.def;
		const int set_idx = int(g.frame_count & 1);
		VkDescriptorImageInfo info = {g.samplers[0][1], g.ui.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorBufferInfo ubi = {p.ubo.buf, 0, VK_WHOLE_SIZE};
		VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
		w[0].dstSet = p.set[set_idx];
		w[0].dstBinding = uint32_t(d.ubo_binding);
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w[0].pBufferInfo = &ubi;
		w[1].dstSet = p.set[set_idx];
		w[1].dstBinding = uint32_t(d.samplers[0].binding);
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[1].pImageInfo = &info;
		vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);
		memcpy(p.ubo.map, kMvp, 64);
		float push[8];
		SizeVec(push, g.ui_w, g.ui_h);
		SizeVec(push + 4, g.out_w, g.out_h);
		const VkViewport view = {0, 0, float(g.out_w), float(g.out_h), 0, 1};
		const VkRect2D sc = {{0, 0}, {uint32_t(g.out_w), uint32_t(g.out_h)}};
		vkCmdSetViewport(cmd, 0, 1, &view);
		vkCmdSetScissor(cmd, 0, 1, &sc);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.set[set_idx], 0, nullptr);
		vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			uint32_t(std::min(d.push_size, 32)), push);
		const VkDeviceSize zero = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.buf, &zero);
		vkCmdDraw(cmd, 4, 1, 0, 0);
	}
	vkCmdEndRenderPass(cmd);
	if (g.timestamps)
	{
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 1);
		g.queries_written = draw_game;
	}
	if (g.offscreen)
	{
		g.offscreen_target.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkBufferImageCopy r = {};
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		r.imageExtent = {uint32_t(g.out_w), uint32_t(g.out_h), 1};
		vkCmdCopyImageToBuffer(cmd, g.offscreen_target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback.buf, 1, &r);
	}
	vkEndCommandBuffer(cmd);
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	if (!g.offscreen)
	{
		si.waitSemaphoreCount = 1;
		si.pWaitSemaphores = &g.acquired;
		si.pWaitDstStageMask = &stage;
		si.signalSemaphoreCount = 1;
		si.pSignalSemaphores = &g.rendered;
	}
	std::unique_lock<std::mutex> queue_lock(g.queue_mx);
	if (!Check(vkQueueSubmit(g.queue, 1, &si, g.fence), "vkQueueSubmit"))
		return false;
	g.in_flight = true;
	if (!g.offscreen)
	{
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &g.rendered;
		pi.swapchainCount = 1;
		pi.pSwapchains = &g.swapchain;
		pi.pImageIndices = &index;
		const VkResult r = vkQueuePresentKHR(g.queue, &pi);
		queue_lock.unlock();
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
		{
			OrbisLog("[gpu] vkQueuePresentKHR -> %d", int(r));
			return false;
		}
	}
	if (queue_lock.owns_lock())
		queue_lock.unlock();
#ifndef __PROSPERO__
	if (g.offscreen)
	{
		vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
		g.in_flight = false;
		if (g.timestamps && g.queries_written)
		{
			uint64_t ts[2];
			if (vkGetQueryPoolResults(g.device, g.queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
					VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
			{
				g.gpu_ms += double(ts[1] - ts[0]) * g.ts_period / 1e6;
				g.gpu_frames++;
			}
			g.queries_written = false;
		}
		HostGpuFlip(static_cast<const uint8_t*>(g.readback.map), g.out_w, g.out_h, wait_vsync);
	}
#endif
	(void)wait_vsync;
	return true;
}
} // namespace ps5gpu
#endif // GENPLUS_GPU
