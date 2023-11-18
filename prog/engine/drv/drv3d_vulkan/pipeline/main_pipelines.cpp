#include "device.h"
#include "perfMon/dag_statDrv.h"
#include "render_pass_resource.h"

using namespace drv3d_vulkan;

uint32_t ComputePipeline::spirvWorkGroupSizeDimConstantIds[ComputePipeline::workGroupDims] = {
  spirv::WORK_GROUP_SIZE_X_CONSTANT_ID,
  spirv::WORK_GROUP_SIZE_Y_CONSTANT_ID,
  spirv::WORK_GROUP_SIZE_Z_CONSTANT_ID,
};

uint32_t PipelineBindlessConfig::bindlessSetCount = 0;
VulkanDescriptorSetLayoutHandle PipelineBindlessConfig::bindlessTextureSetLayout;
VulkanDescriptorSetLayoutHandle PipelineBindlessConfig::bindlessSamplerSetLayout;

const VkShaderStageFlagBits ComputePipelineShaderConfig::stages[ComputePipelineShaderConfig::count] = {VK_SHADER_STAGE_COMPUTE_BIT};

const VkShaderStageFlagBits GraphicsPipelineShaderConfig::stages[GraphicsPipelineShaderConfig::count] = {VK_SHADER_STAGE_VERTEX_BIT,
  VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};

const uint32_t ComputePipelineShaderConfig::registerIndexes[ComputePipelineShaderConfig::count] = {
  spirv::compute::REGISTERS_SET_INDEX};

const uint32_t GraphicsPipelineShaderConfig::registerIndexes[GraphicsPipelineShaderConfig::count] = {
  spirv::graphics::vertex::REGISTERS_SET_INDEX, spirv::graphics::fragment::REGISTERS_SET_INDEX,
  spirv::graphics::geometry::REGISTERS_SET_INDEX, spirv::graphics::control::REGISTERS_SET_INDEX,
  spirv::graphics::evaluation::REGISTERS_SET_INDEX};

#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
const ShaderDebugInfo GraphicsPipeline::emptyDebugInfo = {};
#endif

namespace drv3d_vulkan
{

template <>
void ComputePipeline::onDelayedCleanupFinish<ComputePipeline::CLEANUP_DESTROY>()
{
  if (!checkCompiled())
    get_device().getContext().getBackend().pipelineCompiler.waitFor(this);
  shutdown(get_device().getVkDevice());
  delete this;
}

} // namespace drv3d_vulkan

ComputePipeline::ComputePipeline(VulkanDevice &, ProgramID prog, VulkanPipelineCacheHandle cache, LayoutType *l,
  const CreationInfo &info) :
  DebugAttachedPipeline(l)
{
  ComputePipelineCompileScratchData localScratch;
  compileScratch = info.allowAsyncCompile ? new ComputePipelineCompileScratchData() : &localScratch;
  compileScratch->allocated = info.allowAsyncCompile;

  compileScratch->vkModule = get_device().makeVkModule(info.sci);
  compileScratch->vkLayout = layout->handle;
  compileScratch->vkCache = cache;
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
  compileScratch->name = info.sci->name;
#endif
  compileScratch->progIdx = prog.get();

  if (info.allowAsyncCompile)
    get_device().getContext().getBackend().pipelineCompiler.queue(this);
  else
    compile();
}

void ComputePipeline::bind(VulkanDevice &vk_dev, VulkanCommandBufferHandle cmd_buffer)
{
  if (!checkCompiled())
    get_device().getContext().getBackend().pipelineCompiler.waitFor(this);
#if VULKAN_LOG_PIPELINE_ACTIVITY > 1
  debug("vulkan: bind compute cs %s", debugInfo.cs().name);
#endif
  VULKAN_LOG_CALL(vk_dev.vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, getHandle()));
}

void ComputePipeline::compile()
{
  Device &vkDev = get_device();
  VulkanDevice &device = vkDev.getVkDevice();

  VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL};
  cpci.flags = 0;
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.pNext = NULL;
  cpci.stage.flags = 0;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = compileScratch->vkModule;
  cpci.stage.pName = "main";
  cpci.stage.pSpecializationInfo = NULL;
  cpci.layout = layout->handle;
  cpci.basePipelineHandle = VK_NULL_HANDLE;
  cpci.basePipelineIndex = -1;

  CreationFeedback crFeedback;
  crFeedback.chainWith(cpci, device);

  int64_t compilationTime = 0;
  VkResult compileResult = VK_ERROR_UNKNOWN;
  VulkanPipelineHandle retHandle;
  {
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
    TIME_PROFILE_NAME(vulkan_cs_pipeline_compile, compileScratch->name)
#else
    TIME_PROFILE(vulkan_cs_pipeline_compile)
#endif
    ScopedTimer compilationTimer(compilationTime);
    compileResult = device.vkCreateComputePipelines(device.get(), compileScratch->vkCache, 1, &cpci, NULL, ptr(retHandle));
  }

  if (is_null(retHandle) && VULKAN_OK(compileResult))
  {
    debug("vulkan: pipeline [compute:%u] not compiled but result was ok (%u)", compileScratch->progIdx, compileResult);
    compileResult = VK_ERROR_UNKNOWN;
  }

#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
  if (VULKAN_FAIL(compileResult))
    debug("vulkan: pipeline [compute:%u] cs: %s failed to compile", compileScratch->progIdx, compileScratch->name);
  get_device().setPipelineName(retHandle, compileScratch->name);
  get_device().setPipelineLayoutName(getLayout()->handle, compileScratch->name);
  totalCompilationTime = compilationTime;
  variantCount = 1;
#endif
  VULKAN_EXIT_ON_FAIL(compileResult);

#if VULKAN_LOG_PIPELINE_ACTIVITY < 1
  if (compilationTime > PIPELINE_COMPILATION_LONG_THRESHOLD && !compileScratch->allocated)
#endif
  {
    debug("vulkan: pipeline [compute:%u] compiled in %u us", compileScratch->progIdx, compilationTime);
    crFeedback.logFeedback();
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
    debug("vulkan: with cs %s , handle %p", compileScratch->name, generalize(retHandle));
#endif
  }

  // no need to keep the shader module, delete it to save memory
  VULKAN_LOG_CALL(device.vkDestroyShaderModule(device.get(), compileScratch->vkModule, NULL));

  if (compileScratch->allocated)
  {
    delete compileScratch;
    setCompiledHandle(retHandle);
  }
  else
    setHandle(retHandle);
  compileScratch = nullptr;
}

bool ComputePipeline::pendingCompilation() { return !checkCompiled(); }

static VkSampleCountFlagBits checkSampleCount(unsigned int count, uint8_t colorMask, uint8_t hasDepth)
{
  G_UNUSED(colorMask);
  G_UNUSED(hasDepth);
  if (count <= 1)
    return VK_SAMPLE_COUNT_1_BIT;
  VkSampleCountFlagBits ret = VK_SAMPLE_COUNT_1_BIT;
  switch (count)
  {
    case 2: ret = VK_SAMPLE_COUNT_2_BIT; break;
    case 4: ret = VK_SAMPLE_COUNT_4_BIT; break;
    case 8: ret = VK_SAMPLE_COUNT_8_BIT; break;
    case 16: ret = VK_SAMPLE_COUNT_16_BIT; break;
    case 32: ret = VK_SAMPLE_COUNT_32_BIT; break;
    case 64:
      ret = VK_SAMPLE_COUNT_64_BIT;
      break;
      // this allows the compiler to optimize out this switch, while maintaining correctness
    default: ret = static_cast<VkSampleCountFlagBits>(count); break;
  }
  const VkPhysicalDeviceProperties &properties = get_device().getDeviceProperties().properties;
  G_UNUSED(properties);
  G_ASSERTF(properties.limits.framebufferNoAttachmentsSampleCounts & ret,
    "Selected sample count is not supported on the current platform");
  G_ASSERTF(!hasDepth && !colorMask, "Forced multisampling is only supported when there is no color and depth attachment");
  return ret;
}

static VkDynamicState grPipeDynamicStateList[] = //
  {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS};

static const VkRect2D grPipeStaticRect = {{0, 0}, {1, 1}};
static const VkViewport grPipeStaticViewport = {0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
// no need for unique states per variant, they are all the same
static const VkPipelineViewportStateCreateInfo grPipeViewportStates = //
  {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &grPipeStaticViewport, 1, &grPipeStaticRect};

GraphicsPipeline::GraphicsPipeline(VulkanDevice &device, VulkanPipelineCacheHandle cache, LayoutType *l, const CreationInfo &info) :
  BasePipeline(l), dynStateMask(info.dynStateMask)
{
  GraphicsPipelineCompileScratchData &csd = *info.scratch;
  compileScratch = info.scratch;
  csd.vkCache = cache;

  // deal with render pass dependencies
  VulkanRenderPassHandle renderPassHandle;
  bool hasDepth = false;
  bool forceNoZWrite = false;
  uint32_t rpColorTargetMask = 0;
  VkSampleCountFlagBits sampleCount;
  csd.nativeRP = info.nativeRP;
  if (info.nativeRP)
  {
    sampleCount = info.nativeRP->getMSAASamples(info.varDsc.subpass); // TODO: add MSAA test and make it work
    renderPassHandle = info.nativeRP->getHandle();
    hasDepth = info.nativeRP->hasDepthAtSubpass(info.varDsc.subpass);
    rpColorTargetMask = info.nativeRP->getColorWriteMaskAtSubpass(info.varDsc.subpass);
    csd.nativeRP->addPipelineCompileRef();
  }
  else
  {
    sampleCount = VkSampleCountFlagBits(eastl::max(info.varDsc.rpClass.colorSamples[0], uint8_t(1)));
    RenderPassClass *passClassRef = info.pass_man.getPassClass(info.varDsc.rpClass);
    renderPassHandle = passClassRef->getPass(device, 0);
    hasDepth = info.varDsc.rpClass.depthState != RenderPassClass::Identifier::NO_DEPTH;
    forceNoZWrite = info.varDsc.rpClass.depthState == RenderPassClass::Identifier::RO_DEPTH;
    rpColorTargetMask = info.varDsc.rpClass.colorTargetMask;
  }
  ///

  uint32_t attribs = 0;
  uint32_t lss = 0;

  auto &staticState = info.rsBackend.getStatic(info.varDsc.state.renderState.staticIdx);

  InputLayout inputLayout = get_shader_program_database().getInputLayoutFromId(info.varDsc.state.inputLayout);

  for (int32_t i = 0; i < inputLayout.attribs.size(); ++i)
    if (inputLayout.attribs[i].used)
      csd.inputAttribs[attribs++] = inputLayout.attribs[i].toVulkan();

  for (uint32_t i = 0; i < MAX_VERTEX_INPUT_STREAMS; ++i)
    if (inputLayout.streams.used[i])
      csd.inputStreams[lss++] = inputLayout.streams.toVulkan(i, info.varDsc.state.strides[i]);

  csd.vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  csd.vertexInput.pNext = NULL;
  csd.vertexInput.flags = 0;
  csd.vertexInput.vertexBindingDescriptionCount = lss;
  csd.vertexInput.pVertexBindingDescriptions = csd.inputStreams.data();
  csd.vertexInput.vertexAttributeDescriptionCount = attribs;
  csd.vertexInput.pVertexAttributeDescriptions = csd.inputAttribs.data();

  csd.tesselation.sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
  csd.tesselation.pNext = NULL;
  csd.tesselation.flags = 0;
  csd.tesselation.patchControlPoints = 4;

  csd.raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  csd.raster.pNext = NULL;
  csd.raster.flags = 0;
#if !_TARGET_ANDROID
  csd.raster.depthClampEnable = staticState.depthClipEnable ? VK_FALSE : VK_TRUE;
#else
  csd.raster.depthClampEnable = VK_FALSE;
#endif
  csd.raster.rasterizerDiscardEnable = VK_FALSE;
  csd.raster.polygonMode = static_cast<VkPolygonMode>((uint32_t)info.varDsc.state.polygonLine);
  uint32_t cull_mode = staticState.cullMode;

  if (!cull_mode)
  {
    csd.raster.cullMode = 0;
  }
  else if (cull_mode == (CULL_CW - CULL_NONE))
  {
    csd.raster.cullMode = VK_CULL_MODE_FRONT_BIT;
  }
  else if (cull_mode == (CULL_CCW - CULL_NONE))
  {
    csd.raster.cullMode = VK_CULL_MODE_BACK_BIT;
  }
  csd.raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
  csd.raster.depthBiasEnable = VK_TRUE;
  csd.raster.depthBiasConstantFactor = 0.f;
  csd.raster.depthBiasClamp = 0.f;
  csd.raster.depthBiasSlopeFactor = 0.f;
  csd.raster.lineWidth = 1.f;
#if VK_EXT_conservative_rasterization
  if (staticState.conservativeRasterEnable && device.hasExtension<ConservativeRasterizationEXT>())
  {
    csd.conservativeRasterStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT;
    csd.conservativeRasterStateCI.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    csd.conservativeRasterStateCI.extraPrimitiveOverestimationSize = 0;
    chain_structs(csd.raster, csd.conservativeRasterStateCI);
  }
#endif

  auto forcedSamplerCount = staticState.getForcedSamplerCount();
  csd.multisample.rasterizationSamples =
    forcedSamplerCount == 0 ? sampleCount : checkSampleCount(staticState.getForcedSamplerCount(), rpColorTargetMask, hasDepth);
  csd.multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  csd.multisample.pNext = NULL;
  csd.multisample.flags = 0;
  csd.multisample.sampleShadingEnable = VK_FALSE;
  csd.multisample.minSampleShading = 1.f;
  csd.multisample.pSampleMask = NULL;
  csd.multisample.alphaToCoverageEnable = staticState.alphaToCoverage ? VK_TRUE : VK_FALSE;
  csd.multisample.alphaToOneEnable = VK_FALSE;

  G_ASSERTF((csd.multisample.alphaToCoverageEnable == VK_FALSE) ||
              ((csd.multisample.alphaToCoverageEnable == VK_TRUE) && !(csd.multisample.rasterizationSamples & VK_SAMPLE_COUNT_1_BIT)),
    "vulkan: alpha to coverage must be used with MSAA");

  csd.depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  csd.depthStencil.pNext = NULL;
  csd.depthStencil.flags = 0;
  csd.depthStencil.depthTestEnable = staticState.depthTestEnable;
  csd.depthStencil.depthWriteEnable = staticState.depthWriteEnable && !forceNoZWrite;
  csd.depthStencil.depthCompareOp = (VkCompareOp)(uint32_t)staticState.depthTestFunc;
  csd.depthStencil.depthBoundsTestEnable = staticState.depthBoundsEnable;
  csd.depthStencil.stencilTestEnable = staticState.stencilTestEnable;
  csd.depthStencil.front.failOp = (VkStencilOp)(uint32_t)staticState.stencilTestOpStencilFail;
  csd.depthStencil.front.passOp = (VkStencilOp)(uint32_t)staticState.stencilTestOpPass;
  csd.depthStencil.front.depthFailOp = (VkStencilOp)(uint32_t)staticState.stencilTestOpDepthFail;
  csd.depthStencil.front.compareOp = (VkCompareOp)(uint32_t)staticState.stencilTestFunc;
  csd.depthStencil.front.compareMask = 0xFF;
  csd.depthStencil.front.writeMask = 0xFF;
  csd.depthStencil.front.reference = 0xFF;
  csd.depthStencil.back = csd.depthStencil.front;
  csd.depthStencil.minDepthBounds = 0.f;
  csd.depthStencil.maxDepthBounds = 1.f;

  uint32_t attachmentCount = 0;

  // if color targets are needed, fill attachments to max slot used by colorTargetMask
  // to be both compatible with render pass and conserve resources
  //
  // compatibility with fragment shader on output attachments amount is not needed as
  // when fragment writes to non defined attachment, write is simply ignored
  //
  // The driver then has to sort out stuff by removing not written or not
  // used outputs (which it has to do anyways to optimize shaders).
  if (rpColorTargetMask)
  {
    uint8_t swMask = layout->registers.fs().header.outputMask;
    uint8_t rpMask = rpColorTargetMask;
    for (uint32_t i = 0; (i < Driver3dRenderTarget::MAX_SIMRT) && rpMask; ++i, rpMask >>= 1)
    {
      bool isResolveAttachment = (i > 0) && info.varDsc.rpClass.colorSamples[i - 1] > 1 && !info.nativeRP;
      if (isResolveAttachment)
      {
        continue;
      }

      auto &state = csd.attachmentStates[attachmentCount];

      uint32_t blendStateId =
        staticState.indenpendentBlendEnabled && (i < shaders::RenderState::NumIndependentBlendParameters) ? i : 0;
      const auto &blendState = staticState.blendStates[blendStateId];

      state.blendEnable = blendState.blendEnable;
      state.srcColorBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendSrcFactor;
      state.dstColorBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendDstFactor;
      state.colorBlendOp = (VkBlendOp)(uint32_t)blendState.blendOp;

      if (blendState.blendSeparateAlphaEnable)
      {
        state.srcAlphaBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendSrcAlphaFactor;
        state.dstAlphaBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendDstAlphaFactor;
        state.alphaBlendOp = (VkBlendOp)(uint32_t)blendState.blendOpAlpha;
      }
      else
      {
        state.srcAlphaBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendSrcFactor;
        state.dstAlphaBlendFactor = (VkBlendFactor)(uint32_t)blendState.blendDstFactor;
        state.alphaBlendOp = (VkBlendOp)(uint32_t)blendState.blendOp;
      }

      if (0 != (swMask & rpMask & 1))
      {
        state.colorWriteMask = staticState.colorMask >> (attachmentCount * 4) & VK_COLOR_COMPONENT_RGBA_BIT;
      }
      else
      {
        // if the shader does not provide any value,
        // or if the shader does provide value but RP do not have target
        // we need to set this to 0
        // to avoid writing random values to that slot in the framebuffer.
        state.colorWriteMask = 0;
      }

      ++attachmentCount;
      swMask >>= 1;
    }
  }

  csd.colorBlendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  csd.colorBlendState.pNext = NULL;
  csd.colorBlendState.flags = 0;
  csd.colorBlendState.logicOpEnable = VK_FALSE;
  csd.colorBlendState.logicOp = VK_LOGIC_OP_COPY;
  csd.colorBlendState.attachmentCount = attachmentCount;
  csd.colorBlendState.pAttachments = csd.attachmentStates.data();
  memset(csd.colorBlendState.blendConstants, 0, sizeof(csd.colorBlendState.blendConstants));

  if (!hasDepth)
  {
    csd.depthStencil.depthTestEnable = VK_FALSE;
    csd.depthStencil.depthWriteEnable = VK_FALSE;
    csd.depthStencil.depthBoundsTestEnable = VK_FALSE;
    csd.depthStencil.stencilTestEnable = VK_FALSE;

    csd.raster.depthBiasEnable = VK_FALSE;
    csd.raster.depthClampEnable = VK_FALSE;
  }

  csd.dynamicStates.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  csd.dynamicStates.pNext = NULL;
  csd.dynamicStates.flags = 0;
  csd.dynamicStates.dynamicStateCount = array_size(grPipeDynamicStateList);
  csd.dynamicStates.pDynamicStates = grPipeDynamicStateList;

  csd.piasci = //
    {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, info.varDsc.topology, VK_FALSE};

  unsigned stagesCount = 0;
  for (size_t i = 0; i < spirv::graphics::MAX_SETS; ++i)
  {
    const ShaderModule *shModule = info.modules.list[i];
    if (shModule)
    {
      csd.stages[stagesCount].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      csd.stages[stagesCount].stage = LayoutType::ShaderConfiguration::stages[i];
      csd.stages[stagesCount].module = shModule->module;
      csd.stages[stagesCount].pName = "main";
      stagesCount++;
    }
  }
  csd.gpci.stageCount = stagesCount;

  csd.gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  csd.gpci.pNext = NULL;
  csd.gpci.pVertexInputState = &csd.vertexInput;
  csd.gpci.pTessellationState = layout->hasTC() ? &csd.tesselation : NULL;
  csd.gpci.pRasterizationState = &csd.raster;
  csd.gpci.pMultisampleState = &csd.multisample;
  csd.gpci.pDepthStencilState = &csd.depthStencil;
  csd.gpci.pColorBlendState = &csd.colorBlendState;
  csd.gpci.pDynamicState = &csd.dynamicStates;
  csd.gpci.pInputAssemblyState = &csd.piasci;
  csd.gpci.pViewportState = &grPipeViewportStates;
  csd.gpci.pStages = csd.stages;
  csd.gpci.layout = layout->handle;
  csd.gpci.renderPass = renderPassHandle;
  csd.gpci.subpass = info.varDsc.subpass;
  csd.gpci.basePipelineIndex = 0;
  csd.gpci.basePipelineHandle = VK_NULL_HANDLE;
  csd.gpci.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  csd.parentPipe = info.parentPipeline;
}

void GraphicsPipeline::bind(VulkanDevice &vk_dev, VulkanCommandBufferHandle cmd_buffer) const
{
  VULKAN_LOG_CALL(vk_dev.vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, getHandle()));
}

void GraphicsPipeline::compile()
{
  int64_t compilationTime = 0;
  CreationFeedback crFeedback;
  VulkanPipelineHandle retHandle;
  {
    ScopedTimer compileTimer(compilationTime);
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
    TIME_PROFILE_NAME(vulkan_gr_pipeline_compile, compileScratch->shortDebugName)
#else
    TIME_PROFILE(vulkan_gr_pipeline_compile)
#endif
    retHandle = createPipelineObject(crFeedback);
  }

  if (is_null(retHandle))
  {
    logerr("vulkan: pipeline [gfx:%u:%u(%u)] not compiled but result was ok", compileScratch->progIdx, compileScratch->varIdx,
      compileScratch->varTotal);
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
    logerr("vulkan: with\n %s", compileScratch->fullDebugName);
#endif
  }
  else
  {
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
    get_device().setPipelineName(retHandle, compileScratch->fullDebugName.c_str());
    if (compileScratch->varIdx == 0)
      get_device().setPipelineLayoutName(getLayout()->handle, compileScratch->fullDebugName.c_str());
#endif

#if VULKAN_LOG_PIPELINE_ACTIVITY < 1
    if (compilationTime > PIPELINE_COMPILATION_LONG_THRESHOLD && !compileScratch->allocated)
#endif
    {
      debug("vulkan: pipeline [gfx:%u:%u(%u)] compiled in %u us", compileScratch->progIdx, compileScratch->varIdx,
        compileScratch->varTotal, compilationTime);
      crFeedback.logFeedback();
#if VULKAN_LOAD_SHADER_EXTENDED_DEBUG_DATA
      debug("vulkan: with\n %s handle: %p", compileScratch->fullDebugName, generalize(retHandle));
#endif
    }
  }

  if (compileScratch->nativeRP)
    compileScratch->nativeRP->releasePipelineCompileRef();

  if (compileScratch->allocated)
  {
    delete compileScratch;
    setCompiledHandle(retHandle);
  }
  else
    setHandle(retHandle);
  compileScratch = nullptr;
}

VulkanPipelineHandle GraphicsPipeline::createPipelineObject(CreationFeedback &cr_feedback)
{
  VulkanDevice &device = get_device().getVkDevice();

  if (compileScratch->parentPipe && !is_null(compileScratch->parentPipe->getCompiledHandle()))
  {
    compileScratch->gpci.flags |= VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    compileScratch->gpci.basePipelineIndex = -1;
    compileScratch->gpci.basePipelineHandle = compileScratch->parentPipe->getHandle();
  }

  cr_feedback.chainWith(compileScratch->gpci, device);
  VulkanPipelineHandle ret;
  VULKAN_EXIT_ON_FAIL(
    device.vkCreateGraphicsPipelines(device.get(), compileScratch->vkCache, 1, &compileScratch->gpci, NULL, ptr(ret)));
  return ret;
}

void GraphicsPipelineDynamicStateMask::from(RenderStateSystem::Backend &rs_backend, const GraphicsPipelineVariantDescription &desc,
  RenderPassResource *native_rp)
{
  auto rsSt = rs_backend.getStatic(desc.state.renderState.staticIdx);

  if ((desc.rpClass.depthState == RenderPassClass::Identifier::NO_DEPTH) ||
      (native_rp ? !native_rp->hasDepthAtSubpass(desc.subpass) : false))
  {
    hasDepthBias = 0;
    hasDepthBoundsTest = 0;
    hasStencilTest = 0;
  }
  else
  {
    // TODO: check this again.
    // if (rsSt.depthBiasEnable)
    hasDepthBias = 1;
    if (rsSt.depthBoundsEnable)
      hasDepthBoundsTest = 1;
    if (rsSt.stencilTestEnable)
      hasStencilTest = 1;
  }

  hasBlendConstants = 0;

  uint32_t blendStateToCheck = rsSt.indenpendentBlendEnabled ? shaders::RenderState::NumIndependentBlendParameters : 1;
  for (uint32_t i = 0; i < blendStateToCheck; i++)
  {
    if (rsSt.blendStates[i].blendEnable)
    {
      VkBlendFactor srcBF = (VkBlendFactor)(uint32_t)rsSt.blendStates[i].blendSrcFactor;
      VkBlendFactor dstBF = (VkBlendFactor)(uint32_t)rsSt.blendStates[i].blendDstFactor;

      if (((srcBF >= VK_BLEND_FACTOR_CONSTANT_COLOR) && (srcBF <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA)) ||
          ((dstBF >= VK_BLEND_FACTOR_CONSTANT_COLOR) && (dstBF <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA)))
      {
        hasBlendConstants = 1;
        break;
      }
    }
  }
}

void innerSetDebugName(VulkanPipelineHandle handle, const char *name);