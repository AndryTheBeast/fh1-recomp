#include "nfsmw_video_session.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace nfsmw::native {
namespace {
void Check(VkResult r, const char* m) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(m)+": "+std::to_string(r));
}
VkDeviceSize Align(VkDeviceSize n) { return (n+4095)&~VkDeviceSize(4095); }
}
struct SessionVideo::Resources {
  VkDevice device;
  PFN_vkGetDeviceProcAddr proc;
  const VkPhysicalDeviceMemoryProperties& memory_block;
  uint32_t width, height;
  const Shader* shaderVS; const Shader* shaderPS[2];
  std::unique_ptr<VideoVulkan> video;
  VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memoryBuffer = VK_NULL_HANDLE;
  void* mapped = nullptr; bool coherent = false;
  VkDeviceAddress address = 0;
  std::array<VkDeviceSize,3> offsets{};
  VkDeviceSize offsetConstants = 0, offsetVertices = 0;
  std::array<VkImage,3> images{};
  std::array<VkImageView,3> views{};
  std::array<VkDeviceMemory,3> memories{};
  VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE; bool sent = false, texturesUsed = false;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  uint64_t version = 0; uint32_t widthTarget = 0, heightTarget = 0;
  template<class T> T D(const char* n) const {
    auto f = reinterpret_cast<T>(proc(device,n));
    if (!f) throw std::runtime_error(std::string("Missing ")+n);
    return f;
  }
#define N(n) D<PFN_vk##n>("vk" #n)
  uint32_t Type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i=0;i<memory_block.memoryTypeCount;++i)
      if ((bits&(1u<<i)) && (memory_block.memoryTypes[i].propertyFlags&flags)==flags) return i;
    return UINT32_MAX;
  }
  Resources(VkDevice d, PFN_vkGetDeviceProcAddr p, const VkPhysicalDeviceMemoryProperties& m,
            uint32_t family, ModulesShaders& modules, const FrameVideo& f)
      : device(d),proc(p),memory_block(m),width(f.width),height(f.height),shaderVS(f.vs),shaderPS{f.ps[0],f.ps[1]} {
    try {
      video = std::make_unique<VideoVulkan>(device,proc,modules,*f.vs,*f.ps[0],*f.ps[1],VK_FORMAT_A2B10G10R10_UNORM_PACK32);
      VkDeviceSize bytes = 0;
      for (unsigned i=0;i<3;++i) { offsets[i]=bytes; bytes=Align(bytes+f.planes[i].size()); }
      offsetConstants=bytes; offsetVertices=bytes+sizeof(ConstantsVideo);
      bytes=Align(offsetVertices+sizeof(f.vertices));
      VkBufferCreateInfo bc{}; bc.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bc.size=bytes;
      bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      Check(N(CreateBuffer)(device,&bc,nullptr,&buffer),"Buffer de video");
      VkMemoryRequirements mr{}; N(GetBufferMemoryRequirements)(device,buffer,&mr);
      uint32_t type=Type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      coherent=type!=UINT32_MAX;
      if (!coherent) type=Type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
      if (type==UINT32_MAX) throw std::runtime_error("Memory visible de video no available");
      VkMemoryAllocateFlagsInfo flags{}; flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
      flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
      VkMemoryAllocateInfo ma{}; ma.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ma.pNext=&flags; ma.allocationSize=mr.size; ma.memoryTypeIndex=type;
      Check(N(AllocateMemory)(device,&ma,nullptr,&memoryBuffer),"Memory de video");
      Check(N(BindBufferMemory)(device,buffer,memoryBuffer,0),"Binding de video");
      Check(N(MapMemory)(device,memoryBuffer,0,VK_WHOLE_SIZE,0,&mapped),"Mapping de video");
      VkBufferDeviceAddressInfo bd{}; bd.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO; bd.buffer=buffer;
      address=N(GetBufferDeviceAddress)(device,&bd);
      for (unsigned i=0;i<3;++i) {
        VkImageCreateInfo ic{}; ic.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R8_UNORM;
        ic.extent={i?width/2:width,i?height/2:height,1}; ic.mipLevels=ic.arrayLayers=1;
        ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        Check(N(CreateImage)(device,&ic,nullptr,&images[i]),"Flat de video");
        N(GetImageMemoryRequirements)(device,images[i],&mr);
        type=Type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type==UINT32_MAX) throw std::runtime_error("Memory local de video no available");
        ma.pNext=nullptr; ma.memoryTypeIndex=type; ma.allocationSize=mr.size;
        Check(N(AllocateMemory)(device,&ma,nullptr,&memories[i]),"Memory de flat");
        Check(N(BindImageMemory)(device,images[i],memories[i],0),"Binding de flat");
        VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image=images[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R8_UNORM;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        Check(N(CreateImageView)(device,&vi,nullptr,&views[i]),"Vista de flat");
      }
      video->ConfigureTextures(views);
      VkCommandPoolCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex=family;
      Check(N(CreateCommandPool)(device,&cp,nullptr,&pool),"Pool de video");
      VkCommandBufferAllocateInfo ca{}; ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
      Check(N(AllocateCommandBuffers)(device,&ca,&cmd),"Commands de video");
      VkFenceCreateInfo fc{}; fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
      Check(N(CreateFence)(device,&fc,nullptr,&fence),"Fence de video");
    } catch (...) { Free(); throw; }
  }
  ~Resources() { Free(); }
  void Free() {
    if (sent) N(WaitForFences)(device,1,&fence,VK_TRUE,UINT64_MAX);
    if (framebuffer) N(DestroyFramebuffer)(device,framebuffer,nullptr);
    if (fence) N(DestroyFence)(device,fence,nullptr);
    if (pool) N(DestroyCommandPool)(device,pool,nullptr);
    video.reset();
    for (unsigned i=0;i<3;++i) {
      if (views[i]) N(DestroyImageView)(device,views[i],nullptr);
      if (images[i]) N(DestroyImage)(device,images[i],nullptr);
      if (memories[i]) N(FreeMemory)(device,memories[i],nullptr);
    }
    if (mapped) N(UnmapMemory)(device,memoryBuffer);
    if (buffer) N(DestroyBuffer)(device,buffer,nullptr);
    if (memoryBuffer) N(FreeMemory)(device,memoryBuffer,nullptr);
  }
  bool Free() {
    const VkResult r=N(GetFenceStatus)(device,fence);
    if (r==VK_NOT_READY) return false;
    Check(r,"State de fence de video"); sent=false; return true;
  }
  void Barrier(VkImage i,VkImageLayout before,VkImageLayout after,VkPipelineStageFlags source,VkPipelineStageFlags target,VkAccessFlags a,VkAccessFlags b) {
    VkImageMemoryBarrier m{}; m.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    m.srcAccessMask=a; m.dstAccessMask=b; m.oldLayout=before; m.newLayout=after;
    m.srcQueueFamilyIndex=m.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    m.image=i; m.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    N(CmdPipelineBarrier)(cmd,source,target,0,0,nullptr,0,nullptr,1,&m);
  }
  void Record(const FrameVideo& f,VkImage image,VkImageView vista,uint64_t newVersion,bool written,uint32_t w,uint32_t h) {
    if (!framebuffer || version!=newVersion || widthTarget!=w || heightTarget!=h) {
      if (framebuffer) N(DestroyFramebuffer)(device,framebuffer,nullptr);
      framebuffer=VK_NULL_HANDLE;
      VkFramebufferCreateInfo fb{}; fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb.renderPass=video->render_pass(); fb.attachmentCount=1; fb.pAttachments=&vista; fb.width=w; fb.height=h; fb.layers=1;
      Check(N(CreateFramebuffer)(device,&fb,nullptr,&framebuffer),"Framebuffer de presentacion native");
      version=newVersion; widthTarget=w; heightTarget=h;
    }
    for (unsigned i=0;i<3;++i) std::memcpy(static_cast<uint8_t*>(mapped)+offsets[i],f.planes[i].data(),f.planes[i].size());
    ConstantsVideo c;
    // XenosRecomp convention also used by Marathon: D3D half-pixel offset.
    c.middlePixel[0]=1.f/w; c.middlePixel[1]=-1.f/h;
    std::memcpy(static_cast<uint8_t*>(mapped)+offsetConstants,&c,sizeof(c));
    std::memcpy(static_cast<uint8_t*>(mapped)+offsetVertices,f.vertices.data(),sizeof(f.vertices));
    if (!coherent) {
      VkMappedMemoryRange range{}; range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory=memoryBuffer; range.size=VK_WHOLE_SIZE;
      Check(N(FlushMappedMemoryRanges)(device,1,&range),"Publicacion de planes en memory_block no coherent");
    }
    Check(N(ResetCommandBuffer)(cmd,0),"Reset de commands native");
    VkCommandBufferBeginInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Check(N(BeginCommandBuffer)(cmd,&ci),"Start de video native");
    for (unsigned i=0;i<3;++i) {
      Barrier(images[i],texturesUsed?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          texturesUsed?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
          texturesUsed?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy copy{}; copy.bufferOffset=offsets[i]; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
      copy.imageExtent={i?width/2:width,i?height/2:height,1};
      N(CmdCopyBufferToImage)(cmd,buffer,images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
      Barrier(images[i],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    Barrier(image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        written?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        written?VK_ACCESS_SHADER_READ_BIT:0,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    video->Draw(cmd,framebuffer,w,h,buffer,offsetVertices,address+offsetConstants,f.variant,true);
    Barrier(image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    Check(N(EndCommandBuffer)(cmd),"Fin de video native");
  }
  void Send(VkQueue queue) {
    Check(N(ResetFences)(device,1,&fence),"Reset de fence native");
    VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    Check(N(QueueSubmit)(queue,1,&si,fence),"Submission de video native");
    sent=texturesUsed=true;
  }
#undef N
};
SessionVideo::SessionVideo(VkDevice d,PFN_vkGetDeviceProcAddr p,const VkPhysicalDeviceMemoryProperties& m,uint32_t family)
    :device_(d),proc_(p),memory_(m),family_(family) {
  modules_=std::make_unique<ModulesShaders>(d,reinterpret_cast<PFN_vkCreateShaderModule>(p(d,"vkCreateShaderModule")),
      reinterpret_cast<PFN_vkDestroyShaderModule>(p(d,"vkDestroyShaderModule")));
}
SessionVideo::~SessionVideo() = default;
bool SessionVideo::Prepare(const FrameVideo& f,VkImage i,VkImageView v,uint64_t version,bool written,uint32_t w,uint32_t h) {
  prepared_=nullptr;
  if (!f.vs || !f.ps[0] || !f.ps[1] || !f.width || !f.height || ((f.width|f.height)&1) || f.variant>1 || !w || !h) return false;
  for (unsigned n=0;n<3;++n) if (f.planes[n].size()!=size_t(n?f.width/2:f.width)*(n?f.height/2:f.height)) return false;
  for (unsigned n=0;n<3;++n) {
    auto& r=resources_[(next_+n)%3];
    if (r && !r->Free()) continue;
    if (r && (r->width!=f.width || r->height!=f.height || r->shaderVS!=f.vs || r->shaderPS[0]!=f.ps[0] || r->shaderPS[1]!=f.ps[1])) r.reset();
    if (!r) r=std::make_unique<Resources>(device_,proc_,memory_,family_,*modules_,f);
    r->Record(f,i,v,version,written,w,h);
    prepared_=r.get(); next_=(next_+n+1)%3; return true;
  }
  return false;
}
void SessionVideo::Send(VkQueue queue) {
  if (!prepared_) throw std::runtime_error("Video no prepared");
  prepared_->Send(queue); prepared_=nullptr;
}
}
