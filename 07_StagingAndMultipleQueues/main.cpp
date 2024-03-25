// Copyright (C) 2018-2023 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

// TODO: improve validation of writes (IDescriptor::E_TYPE IGPUDescriptorSet::validateWrite)

// I've moved out a tiny part of this example into a shared header for reuse, please open and read it.
#include "../common/BasicMultiQueueApplication.hpp"
#include "../common/MonoAssetManagerAndBuiltinResourceApplication.hpp"

//std::this_thread::sleep_for(std::chrono::seconds(1)); // TODO: remove

using namespace nbl;
using namespace core;
using namespace system;
using namespace asset;
using namespace video;

#include "app_resources/common.hlsl"

// This time we let the new base class score and pick queue families, as well as initialize `nbl::video::IUtilities` for us
class StagingAndMultipleQueuesApp final : public examples::BasicMultiQueueApplication, public examples::MonoAssetManagerAndBuiltinResourceApplication
{
	using device_base_t = examples::BasicMultiQueueApplication;
	using asset_base_t = examples::MonoAssetManagerAndBuiltinResourceApplication;

	static constexpr std::array imagesToLoad = {
		"../app_resources/test0.png",
		"../app_resources/test1.png",
		"../app_resources/test2.png",
		"../app_resources/test0.png",
		"../app_resources/test1.png",
		"../app_resources/test2.png"
	};
	static constexpr size_t IMAGE_CNT = imagesToLoad.size();

public:
	// Yay thanks to multiple inheritance we cannot forward ctors anymore
	StagingAndMultipleQueuesApp(const path& _localInputCWD, const path& _localOutputCWD, const path& _sharedInputCWD, const path& _sharedOutputCWD) :
		system::IApplicationFramework(_localInputCWD, _localOutputCWD, _sharedInputCWD, _sharedOutputCWD) {}

	// This time we will load images and compute their histograms and output them as CSV
	bool onAppInitialized(smart_refctd_ptr<ISystem>&& system) override
	{
		// Remember to call the base class initialization!
		if (!device_base_t::onAppInitialized(std::move(system)))
			return false;
		if (!asset_base_t::onAppInitialized(std::move(system)))
			return false;

		// TODO: create all semaphores before going into threads

		constexpr size_t TIMELINE_SEMAPHORE_STARTING_VALUE = 0;
		m_imagesLoadedSemaphore = m_device->createSemaphore(TIMELINE_SEMAPHORE_STARTING_VALUE);
		m_imagesProcessedSemaphore = m_device->createSemaphore(TIMELINE_SEMAPHORE_STARTING_VALUE);
		m_histogramSavedSeamphore = m_device->createSemaphore(TIMELINE_SEMAPHORE_STARTING_VALUE);

		// TODO: create/initialize array of atomic pointers to IGPUImage* and IGPUBuffer* to hold results

		std::thread loadImagesThread(&StagingAndMultipleQueuesApp::loadImages, this);
		std::thread saveHistogramsThread(&StagingAndMultipleQueuesApp::saveHistograms, this);

		calculateHistograms();

		loadImagesThread.join();
		saveHistogramsThread.join();

		return true;
	}

	//
	void workLoopBody() override {}

	//
	bool keepRunning() override { return false; }

	//
	bool onAppTerminated() override
	{
		return device_base_t::onAppTerminated();
	}

protected:
	// Override will become irrelevant in the vulkan_1_3 branch
	SPhysicalDeviceFeatures getRequiredDeviceFeatures() const override
	{
		auto retval = device_base_t::getRequiredDeviceFeatures();
		//retval.shaderStorageImageWriteWithoutFormat = true;
		//retval.vulkanMemoryModelDeviceScope = true; // Needed for atomic operations.
		return retval;
	}

	// Ideally don't want to have to 
	SPhysicalDeviceFeatures getPreferredDeviceFeatures() const override
	{
		auto retval = device_base_t::getPreferredDeviceFeatures();
		//retval.shaderStorageImageReadWithoutFormat = true;
		return retval;
	}

	template<typename... Args>
	void logFailAndTerminate(const char* msg, Args&&... args)
	{
		m_logger->log(msg, ILogger::ELL_ERROR, std::forward<Args>(args)...);
		std::exit(-1);
	}

private:
	smart_refctd_ptr<ISemaphore> m_imagesLoadedSemaphore, m_imagesProcessedSemaphore, m_histogramSavedSeamphore;
	std::atomic<uint32_t> m_imagesLoadedCnt, m_imagesProcessedCnt, m_imagesDownloadedCnt, m_imagesSavedCnt;
	std::array<core::smart_refctd_ptr<IGPUImage>, IMAGE_CNT> images;

	static constexpr uint32_t FRAMES_IN_FLIGHT = 3u;
	smart_refctd_ptr<video::IGPUCommandPool> commandPools[FRAMES_IN_FLIGHT];

	static constexpr size_t CHANEL_CNT = 3;
	static constexpr size_t VAL_PER_CHANEL_CNT = 256;
	static constexpr size_t HISTOGRAM_SIZE = CHANEL_CNT * VAL_PER_CHANEL_CNT;
	static constexpr size_t HISTOGRAM_BYTE_SIZE = HISTOGRAM_SIZE * sizeof(uint32_t);
	static constexpr size_t COMBINED_HISTOGRAM_BUFFER_BYTE_SIZE = HISTOGRAM_BYTE_SIZE * FRAMES_IN_FLIGHT;
	smart_refctd_ptr<IGPUBuffer> histogramBuffer = nullptr;
	nbl::video::IDeviceMemoryAllocator::SAllocation m_histogramBufferAllocation = {};
	// TODO: make sure ranges are ok
	std::array<ILogicalDevice::MappedMemoryRange, FRAMES_IN_FLIGHT> m_histogramBufferMemoryRanges;
	uint32_t* m_histogramBufferMemPtrs[3];

	std::mutex assetManagerMutex; // TODO: make function for loading assets

	void loadImages()
	{
		IAssetLoader::SAssetLoadParams lp;
		lp.logger = m_logger.get();

		// LOAD CPU IMAGES
		core::smart_refctd_ptr<ICPUImage> cpuImages[IMAGE_CNT];
		{
			std::lock_guard<std::mutex> assetManagerLock(assetManagerMutex);

			for(uint32_t i = 0; i < IMAGE_CNT; ++i)
			{
				SAssetBundle bundle = m_assetMgr->getAsset(imagesToLoad[i], lp);

				if (bundle.getContents().empty())
					logFailAndTerminate("Couldn't load an image.", ILogger::ELL_ERROR);

				cpuImages[i] = IAsset::castDown<ICPUImage>(bundle.getContents()[0]);
				if(!cpuImages[i])
					logFailAndTerminate("Asset loaded is not an image.", ILogger::ELL_ERROR);
			}
		}

		auto transferUpQueue = getTransferUpQueue();
		// TODO: i want to use IGPUCommandPool::CREATE_FLAGS::NONE but `updateImageViaStagingBuffer` requires command buffers to be resetable
		const core::bitflag<IGPUCommandPool::CREATE_FLAGS> commandPoolFlags = static_cast<IGPUCommandPool::CREATE_FLAGS>(IGPUCommandPool::CREATE_FLAGS::RESET_COMMAND_BUFFER_BIT);
		std::array<core::smart_refctd_ptr<nbl::video::IGPUCommandPool>, FRAMES_IN_FLIGHT> commandPools;
		std::array<core::smart_refctd_ptr<nbl::video::IGPUCommandBuffer>, FRAMES_IN_FLIGHT> commandBuffers;
		std::fill(commandPools.begin(), commandPools.end(), nullptr);
		for (uint32_t i = 0u; i < FRAMES_IN_FLIGHT; ++i)
		{
			commandPools[i] = m_device->createCommandPool(transferUpQueue->getFamilyIndex(), commandPoolFlags);
			commandPools[i]->createCommandBuffers(IGPUCommandPool::BUFFER_LEVEL::PRIMARY, std::span(commandBuffers.data() + i, 1), core::smart_refctd_ptr(m_logger));
		}

		// TODO: loop over images to load
			// DONE: If imageIndex>=FRAMES_IN_FLIGHT need to block for Semaphore at value imageIndex+1-FRAMES_IN_FLIGHT
			// DONE: Create IGPUImage with matching parameters, EXCEPT format, format we promote using IPHysicalDevice
			// DONE: just use Transfer UP queue with IUtilities to fill image
			// DONE: layout transition from TRANSFER_DST to SHADER_READ_ONLY/SAMPLED
			// TODO: make sure to use the non-blocking variant of `IUtilities` to control the `IQueue::SSubmitInfo::signalSemaphores`

		for (uint32_t imageIdx = 0; imageIdx < IMAGE_CNT; ++imageIdx)
		{
			const size_t resourceIdx = imageIdx % FRAMES_IN_FLIGHT;
			const auto& imageToLoad = imagesToLoad[imageIdx];
			auto& cmdBuff = commandBuffers[resourceIdx]; 

			auto isResourceReused = waitForResourceAvailability(m_imagesLoadedSemaphore.get(), imageIdx);
			if(isResourceReused)
				cmdBuff->reset(IGPUCommandBuffer::RESET_FLAGS::NONE);

			IGPUImage::SCreationParams imgParams;
			imgParams.type = IImage::E_TYPE::ET_2D;
			imgParams.extent.height = cpuImages[0]->getCreationParameters().extent.height;
			imgParams.extent.width = cpuImages[0]->getCreationParameters().extent.height;
			imgParams.extent.depth = cpuImages[0]->getCreationParameters().extent.depth;
			IPhysicalDevice::SImageFormatPromotionRequest formatPromotionRequest;
			IPhysicalDevice::SFormatImageUsages::SUsage usage;
			usage.sampledImage = 1;
			usage.transferDst = 1;
			formatPromotionRequest.usages = usage;
			// why my images are VK_FORMAT_R8_SRGB?
			//imgParams.format = m_physicalDevice->promoteImageFormat(formatPromotionRequest, IGPUImage::TILING::OPTIMAL);
			imgParams.format = E_FORMAT::EF_R8G8B8A8_SRGB;
			imgParams.mipLevels = 1u;
			imgParams.flags = IImage::ECF_NONE;
			imgParams.arrayLayers = 1u;
			imgParams.samples = IImage::E_SAMPLE_COUNT_FLAGS::ESCF_1_BIT;
			imgParams.usage = asset::IImage::EUF_TRANSFER_DST_BIT | asset::IImage::EUF_SAMPLED_BIT; 
			// constexpr uint32_t FAMILY_INDICES_CNT = 3; // TODO: test on intel integrated GPU (which allows only one queue family)
			std::array familyIndices = { getTransferUpQueue()->getFamilyIndex(), getComputeQueue()->getFamilyIndex() };
			imgParams.queueFamilyIndexCount = familyIndices.size();
			imgParams.queueFamilyIndices = familyIndices.data();
			imgParams.preinitialized = false;

			images[imageIdx] = m_device->createImage(std::move(imgParams));
			auto imageAllocation = m_device->allocate(images[imageIdx]->getMemoryReqs(), images[imageIdx].get(), IDeviceMemoryAllocation::EMAF_NONE);

			if (!imageAllocation.isValid())
				logFailAndTerminate("Failed to allocate Device Memory compatible with our GPU Buffer!\n");

			IGPUCommandBuffer::SImageMemoryBarrier<IGPUCommandBuffer::SOwnershipTransferBarrier> imageLayoutTransitionBarrier0; // TODO: better names maybe?
			{
				IImage::SSubresourceRange imgSubresourceRange{};
				imgSubresourceRange.aspectMask = IImage::E_ASPECT_FLAGS::EAF_COLOR_BIT;
				imgSubresourceRange.baseMipLevel = 0u;
				imgSubresourceRange.baseArrayLayer = 0u;
				imgSubresourceRange.levelCount = 1;
				imgSubresourceRange.layerCount = 1u;

				imageLayoutTransitionBarrier0.barrier.dep.srcAccessMask = ACCESS_FLAGS::NONE;
				imageLayoutTransitionBarrier0.barrier.dep.dstAccessMask = ACCESS_FLAGS::MEMORY_WRITE_BITS;
				imageLayoutTransitionBarrier0.oldLayout = asset::IImage::LAYOUT::UNDEFINED;
				imageLayoutTransitionBarrier0.newLayout = asset::IImage::LAYOUT::TRANSFER_DST_OPTIMAL; // TODO: use more suitable layout
				imageLayoutTransitionBarrier0.image = images[imageIdx].get();
				imageLayoutTransitionBarrier0.subresourceRange = imgSubresourceRange;
			}

			IGPUCommandBuffer::SImageMemoryBarrier<IGPUCommandBuffer::SOwnershipTransferBarrier> imageLayoutTransitionBarrier1;
			{
				IImage::SSubresourceRange imgSubresourceRange{};
				imgSubresourceRange.aspectMask = IImage::E_ASPECT_FLAGS::EAF_COLOR_BIT;
				imgSubresourceRange.baseMipLevel = 0u;
				imgSubresourceRange.baseArrayLayer = 0u;
				imgSubresourceRange.levelCount = 1u;
				imgSubresourceRange.layerCount = 1u;

				imageLayoutTransitionBarrier1.barrier.dep.srcAccessMask = ACCESS_FLAGS::MEMORY_WRITE_BITS; 
				imageLayoutTransitionBarrier1.barrier.dep.dstAccessMask = ACCESS_FLAGS::NONE;
				imageLayoutTransitionBarrier1.oldLayout = asset::IImage::LAYOUT::TRANSFER_DST_OPTIMAL;
				imageLayoutTransitionBarrier1.newLayout = asset::IImage::LAYOUT::GENERAL; // TODO: use more suitable layout
				imageLayoutTransitionBarrier1.image = images[imageIdx].get();
				imageLayoutTransitionBarrier1.subresourceRange = imgSubresourceRange;
			}

			core::smart_refctd_ptr<ISemaphore> imgFillSemaphore = m_device->createSemaphore(0); // TODO: don't create semaphore every iteration
			IQueue::SSubmitInfo::SCommandBufferInfo imgFillCmdBuffInfo = { cmdBuff.get() };

			IQueue::SSubmitInfo::SSemaphoreInfo imgFillSemaphoreWaitInfo = {
				.semaphore = imgFillSemaphore.get(),
				.value = 1,
				.stageMask = PIPELINE_STAGE_FLAGS::ALL_TRANSFER_BITS
			};

			IQueue::SSubmitInfo imgFillSubmitInfo = {
				.waitSemaphores = {&imgFillSemaphoreWaitInfo, 1},
				.commandBuffers = {&imgFillCmdBuffInfo, 1}
			};

			transferUpQueue->submit({ &imgFillSubmitInfo, 1 });

			IQueue::SSubmitInfo::SSemaphoreInfo imgFillSemaphoreInfo =
			{
				.semaphore = imgFillSemaphore.get(),
				.value = 0,
				.stageMask = PIPELINE_STAGE_FLAGS::ALL_TRANSFER_BITS
			};
			SIntendedSubmitInfo intendedSubmit = {
				.frontHalf = {.queue = transferUpQueue, .waitSemaphores = {}, .commandBuffers = {&imgFillCmdBuffInfo, 1}}, .signalSemaphores = {&imgFillSemaphoreInfo, 1}
			};
			
			cmdBuff->begin(IGPUCommandBuffer::USAGE::ONE_TIME_SUBMIT_BIT);


			IGPUCommandBuffer::SPipelineBarrierDependencyInfo pplnBarrierDepInfo0;
			pplnBarrierDepInfo0.imgBarriers = std::span(&imageLayoutTransitionBarrier0, &imageLayoutTransitionBarrier0 + 1);
			if (!cmdBuff->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE, pplnBarrierDepInfo0))
				logFailAndTerminate("Failed to issue barrier!\n");

			//transferUpQueue->startCapture();
			bool uploaded = m_utils->updateImageViaStagingBuffer(
				intendedSubmit, cpuImages[imageIdx]->getBuffer(), cpuImages[imageIdx]->getCreationParameters().format,
				images[imageIdx].get(), IImage::LAYOUT::TRANSFER_DST_OPTIMAL, cpuImages[imageIdx]->getRegions()
			);
			if (!uploaded)
				logFailAndTerminate("Couldn't update image data.\n");

			IGPUCommandBuffer::SPipelineBarrierDependencyInfo pplnBarrierDepInfo1;
			pplnBarrierDepInfo1.imgBarriers = std::span(&imageLayoutTransitionBarrier1, &imageLayoutTransitionBarrier1 + 1);

			if(!cmdBuff->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE, pplnBarrierDepInfo1))
				logFailAndTerminate("Failed to issue barrier!\n");

			cmdBuff->end();

			IQueue::SSubmitInfo submitInfo[1];
			IQueue::SSubmitInfo::SCommandBufferInfo cmdBuffSubmitInfo[] = { {cmdBuff.get()} };
			IQueue::SSubmitInfo::SSemaphoreInfo signalSemaphoreSubmitInfo[] = { { .semaphore = m_imagesLoadedSemaphore.get(), .value = imageIdx+1, .stageMask = PIPELINE_STAGE_FLAGS::ALL_COMMANDS_BITS } };
			submitInfo[0].commandBuffers = cmdBuffSubmitInfo;
			submitInfo[0].signalSemaphores = signalSemaphoreSubmitInfo;
			getTransferUpQueue()->submit(submitInfo);
			//transferUpQueue->endCapture();

			// this is for basic testing purposes, will be deleted ofc
			std::string msg = std::string("Image nr ") + std::to_string(imageIdx) + " loaded. Resource idx: " + std::to_string(resourceIdx);
			m_logger->log(msg);
		}
	}

	void calculateHistograms()
	{
		// INITIALIZE COMMON DATA
		auto computeQueue = getComputeQueue();
		const core::bitflag<IGPUCommandPool::CREATE_FLAGS> commandPoolFlags = static_cast<IGPUCommandPool::CREATE_FLAGS>(IGPUCommandPool::CREATE_FLAGS::NONE);
		std::array<core::smart_refctd_ptr<nbl::video::IGPUCommandPool>, FRAMES_IN_FLIGHT> commandPools;
		std::array<core::smart_refctd_ptr<nbl::video::IGPUCommandBuffer>, FRAMES_IN_FLIGHT> commandBuffers;
		core::smart_refctd_ptr<IGPUDescriptorSet> descSets[FRAMES_IN_FLIGHT];
		std::fill(commandPools.begin(), commandPools.end(), nullptr);
		nbl::video::IGPUDescriptorSetLayout::SBinding bindings[2] = {
			{
				.binding = 0,
				.type = nbl::asset::IDescriptor::E_TYPE::ET_COMBINED_IMAGE_SAMPLER,
				.createFlags = IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
				.stageFlags = IGPUShader::E_SHADER_STAGE::ESS_COMPUTE,
				.count = 1,
				.samplers = nullptr
			},
			{
				.binding = 1,
				.type = nbl::asset::IDescriptor::E_TYPE::ET_STORAGE_BUFFER,
				.createFlags = IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
				.stageFlags = IGPUShader::E_SHADER_STAGE::ESS_COMPUTE,
				.count = 1,
				.samplers = nullptr
			}
		};
		smart_refctd_ptr<IGPUDescriptorSetLayout> dsLayout[1] = { m_device->createDescriptorSetLayout(bindings) };
		if (!dsLayout[0])
			logFailAndTerminate("Failed to create a Descriptor Layout!\n");
		smart_refctd_ptr<nbl::video::IDescriptorPool> descPools[FRAMES_IN_FLIGHT] = { // TODO: only one desc pool?
			m_device->createDescriptorPoolForDSLayouts(IDescriptorPool::ECF_NONE, std::span(&dsLayout[0].get(), 1)),
			m_device->createDescriptorPoolForDSLayouts(IDescriptorPool::ECF_NONE, std::span(&dsLayout[0].get(), 1)),
			m_device->createDescriptorPoolForDSLayouts(IDescriptorPool::ECF_NONE, std::span(&dsLayout[0].get(), 1))
		};

		for (uint32_t i = 0u; i < FRAMES_IN_FLIGHT; ++i)
		{
			commandPools[i] = m_device->createCommandPool(getComputeQueue()->getFamilyIndex(), commandPoolFlags);
			commandPools[i]->createCommandBuffers(IGPUCommandPool::BUFFER_LEVEL::PRIMARY, std::span(commandBuffers.data() + i, 1), core::smart_refctd_ptr(m_logger));
			
			descSets[i] = descPools[i]->createDescriptorSet(core::smart_refctd_ptr(dsLayout[0]));
		}

		// LOAD SHADER FROM FILE
		smart_refctd_ptr<ICPUShader> source;
		{
			std::lock_guard<std::mutex> assetManagerLock(assetManagerMutex);

			IAssetLoader::SAssetLoadParams lp;
			lp.logger = m_logger.get();
			auto assetBundle = m_assetMgr->getAsset("../app_resources/comp_shader.hlsl", lp);
			assert(assetBundle.getAssetType() == IAsset::E_TYPE::ET_SHADER);
			const auto assets = assetBundle.getContents();
			if (assets.empty())
				logFailAndTerminate("Could not load shader!");

			// It would be super weird if loading a shader from a file produced more than 1 asset
			assert(assets.size() == 1);
			source = IAsset::castDown<ICPUShader>(assets[0]);
			source->setShaderStage(IShader::ESS_COMPUTE);
		}

		if (!source)
			logFailAndTerminate("Could not create a CPU shader!");

		core::smart_refctd_ptr<IGPUShader> shader = m_device->createShader(source.get());
		if(!shader)
			logFailAndTerminate("Could not create a GPU shader!");

		// CREATE COMPUTE PIPELINE
		SPushConstantRange pc[1];
		pc[0].stageFlags = IShader::E_SHADER_STAGE::ESS_COMPUTE;
		pc[0].offset = 0;
		pc[0].size = sizeof(PushConstants);

		smart_refctd_ptr<nbl::video::IGPUComputePipeline> pipeline;
		smart_refctd_ptr<IGPUPipelineLayout> pplnLayout = m_device->createPipelineLayout(pc, smart_refctd_ptr(dsLayout[0]));
		{
			// Nabla actually has facilities for SPIR-V Reflection and "guessing" pipeline layouts for a given SPIR-V which we'll cover in a different example
			if (!pplnLayout)
				logFailAndTerminate("Failed to create a Pipeline Layout!\n");

			IGPUComputePipeline::SCreationParams params = {};
			params.layout = pplnLayout.get();
			// Theoretically a blob of SPIR-V can contain multiple named entry points and one has to be chosen, in practice most compilers only support outputting one (and glslang used to require it be called "main")
			params.shader.entryPoint = "main";
			params.shader.shader = shader.get();
			// we'll cover the specialization constant API in another example
			if (!m_device->createComputePipelines(nullptr, { &params,1 }, &pipeline))
				logFailAndTerminate("Failed to create pipelines (compile & link shaders)!\n");
		}

		// CREATE AND MAP HISTOGRAM BUFFER
		{
			IGPUBuffer::SCreationParams gpuBufCreationParams;
			gpuBufCreationParams.size = COMBINED_HISTOGRAM_BUFFER_BYTE_SIZE;
			gpuBufCreationParams.usage = IGPUBuffer::E_USAGE_FLAGS::EUF_TRANSFER_DST_BIT | IGPUBuffer::E_USAGE_FLAGS::EUF_TRANSFER_SRC_BIT | IGPUBuffer::E_USAGE_FLAGS::EUF_STORAGE_BUFFER_BIT;
			histogramBuffer = m_device->createBuffer(std::move(gpuBufCreationParams));
			if (!histogramBuffer)
				logFailAndTerminate("Failed to create a GPU Buffer of size %d!\n", COMBINED_HISTOGRAM_BUFFER_BYTE_SIZE);

			histogramBuffer->setObjectDebugName("Histogram Buffer");

			nbl::video::IDeviceMemoryBacked::SDeviceMemoryRequirements reqs = histogramBuffer->getMemoryReqs();
			// you can simply constrain the memory requirements by AND-ing the type bits of the host visible memory types
			reqs.memoryTypeBits &= (m_physicalDevice->getHostVisibleMemoryTypeBits() & m_physicalDevice->getDeviceLocalMemoryTypeBits());
			auto a = m_physicalDevice->getHostVisibleMemoryTypeBits();
			auto b = m_physicalDevice->getDeviceLocalMemoryTypeBits();

			m_histogramBufferAllocation = m_device->allocate(reqs, histogramBuffer.get(), nbl::video::IDeviceMemoryAllocation::E_MEMORY_ALLOCATE_FLAGS::EMAF_DEVICE_ADDRESS_BIT);
			if (!m_histogramBufferAllocation.isValid())
				logFailAndTerminate("Failed to allocate Device Memory compatible with our GPU Buffer!\n");
			assert(histogramBuffer->getBoundMemory().memory == m_histogramBufferAllocation.memory.get());

			auto memoryRange = IDeviceMemoryAllocation::MemoryRange(0, m_histogramBufferAllocation.memory->getAllocationSize());

			m_histogramBufferMemoryRanges[0] = ILogicalDevice::MappedMemoryRange(histogramBuffer->getBoundMemory().memory, 0, HISTOGRAM_BYTE_SIZE);
			m_histogramBufferMemoryRanges[1] = ILogicalDevice::MappedMemoryRange(histogramBuffer->getBoundMemory().memory, HISTOGRAM_BYTE_SIZE, HISTOGRAM_BYTE_SIZE);
			m_histogramBufferMemoryRanges[2] = ILogicalDevice::MappedMemoryRange(histogramBuffer->getBoundMemory().memory, HISTOGRAM_BYTE_SIZE * 2, HISTOGRAM_BYTE_SIZE);

			m_histogramBufferMemPtrs[0] = static_cast<uint32_t*>(m_histogramBufferAllocation.memory->map(memoryRange, IDeviceMemoryAllocation::EMCAF_READ_AND_WRITE));
			if (!m_histogramBufferMemPtrs[0])
				logFailAndTerminate("Failed to map the Device Memory!\n");
			m_histogramBufferMemPtrs[1] = m_histogramBufferMemPtrs[0] + HISTOGRAM_SIZE;
			m_histogramBufferMemPtrs[2] = m_histogramBufferMemPtrs[1] + HISTOGRAM_SIZE;
		}

		// PROCESS IMAGES
		for (uint32_t imageToProcessId = 0; imageToProcessId < IMAGE_CNT; imageToProcessId++)
		{
			// TODO: wait for atomic instead? will it be even valid to do? we need to wait for gpu to finish anyway..
			waitForPreviousStep(m_imagesLoadedSemaphore.get(), imageToProcessId + 1);

			const auto resourceIdx = imageToProcessId % FRAMES_IN_FLIGHT;
			auto& cmdBuff = commandBuffers[resourceIdx];
			auto& commandPool = commandPools[resourceIdx];
			
			auto isResourceReused = waitForResourceAvailability(m_imagesProcessedSemaphore.get(), imageToProcessId);
			if (isResourceReused)
				commandPool->reset();

			// UPDATE DESCRIPTOR SET WRITES
			IGPUDescriptorSet::SDescriptorInfo imgInfo;
			IGPUImageView::SCreationParams params{};
			params.viewType = IImageView<IGPUImage>::ET_2D;
			params.image = images[imageToProcessId];
			params.format = images[imageToProcessId]->getCreationParameters().format;
			params.subresourceRange.aspectMask = IImage::E_ASPECT_FLAGS::EAF_COLOR_BIT;
			params.subresourceRange.layerCount = images[resourceIdx]->getCreationParameters().arrayLayers;

			IGPUSampler::SParams samplerParams;
			samplerParams.AnisotropicFilter = false;
			core::smart_refctd_ptr<IGPUSampler> sampler = m_device->createSampler(samplerParams);

			imgInfo.desc = m_device->createImageView(std::move(params));
			if (!imgInfo.desc)
				logFailAndTerminate("Couldn't create descriptor.");
			imgInfo.info.image = { .sampler = sampler, .imageLayout = IImage::LAYOUT::GENERAL };

			IGPUDescriptorSet::SDescriptorInfo bufInfo;
			bufInfo.desc = smart_refctd_ptr(histogramBuffer);
			bufInfo.info.buffer = { .offset = 0u, .size = histogramBuffer->getSize() };

			IGPUDescriptorSet::SWriteDescriptorSet writes[2] = {
				{.dstSet = descSets[resourceIdx].get(), .binding = 0, .arrayElement = 0, .count = 1, .info = &imgInfo },
				{.dstSet = descSets[resourceIdx].get(), .binding = 1, .arrayElement = 0, .count = 1, .info = &bufInfo }
			};
			m_device->updateDescriptorSets(2, writes, 0u, nullptr);

			computeQueue->startCapture();
			cmdBuff->begin(IGPUCommandBuffer::USAGE::NONE);
			cmdBuff->beginDebugMarker("My Compute Dispatch", core::vectorSIMDf(0, 1, 0, 1));
			cmdBuff->bindComputePipeline(pipeline.get());

			cmdBuff->bindDescriptorSets(nbl::asset::EPBP_COMPUTE, pplnLayout.get(), 0, 1, &descSets[resourceIdx].get());

			const auto imageExtent = images[imageToProcessId]->getCreationParameters().extent;
			const uint32_t wgCntX = imageExtent.width / WorkgroupSizeX;
			const uint32_t wgCntY = imageExtent.height / WorkgroupSizeY;

			PushConstants constants;
			constants.histogramBufferOffset = HISTOGRAM_SIZE * resourceIdx;

			cmdBuff->pushConstants(pplnLayout.get(), IShader::E_SHADER_STAGE::ESS_COMPUTE, 0u, sizeof(PushConstants), &constants);
			cmdBuff->dispatch(wgCntX, wgCntY, 1);

			cmdBuff->endDebugMarker();
			cmdBuff->end();

			IQueue::SSubmitInfo submitInfo[1];
			IQueue::SSubmitInfo::SCommandBufferInfo cmdBuffSubmitInfo[] = { {cmdBuff.get()} };
			IQueue::SSubmitInfo::SSemaphoreInfo signalSemaphoreSubmitInfo[] = { {.semaphore = m_imagesProcessedSemaphore.get(), .value = imageToProcessId+1, .stageMask = PIPELINE_STAGE_FLAGS::ALL_COMMANDS_BITS } };
			submitInfo[0].commandBuffers = cmdBuffSubmitInfo;
			submitInfo[0].signalSemaphores = signalSemaphoreSubmitInfo;

			waitForResourceAvailability(m_histogramSavedSeamphore.get(), imageToProcessId);
			computeQueue->submit(submitInfo);
			computeQueue->endCapture();
			std::string msg = std::string("Image nr ") + std::to_string(imageToProcessId) + " processed. Resource idx: " + std::to_string(resourceIdx);
			m_logger->log(msg);
		}
	}

	void saveHistograms()
	{
		std::array<std::ofstream, IMAGE_CNT> files;
		for (auto& file : files)
		{
			static uint32_t i = 0u;
			file.open("histogram_" + std::to_string(i++) + ".csv", std::ios::out | std::ios::trunc);
		}

		for (uint32_t imageHistogramIdx = 0; imageHistogramIdx < IMAGE_CNT; ++imageHistogramIdx)
		{
			waitForPreviousStep(m_imagesProcessedSemaphore.get(), imageHistogramIdx + 1);

			const uint32_t resourceIdx = imageHistogramIdx % FRAMES_IN_FLIGHT;
			uint32_t* histogramBuff = m_histogramBufferMemPtrs[resourceIdx];

			auto isResourceReused = waitForResourceAvailability(m_histogramSavedSeamphore.get(), imageHistogramIdx);
			if(!m_device->invalidateMappedMemoryRanges(1, &m_histogramBufferMemoryRanges[resourceIdx]))
				logFailAndTerminate("Failed to invalidate the Device Memory!\n");

			size_t offset = 0;
			auto& file = files[imageHistogramIdx];
			for (uint32_t i = 0u; i < CHANEL_CNT; ++i)
			{
				for (uint32_t j = 0u; j < VAL_PER_CHANEL_CNT; ++j)
				{
					file << histogramBuff[offset] << ',';
					histogramBuff[offset] = 0;
					offset++;
				}

				file << '\n';
			}

			if(!m_device->flushMappedMemoryRanges(1, &m_histogramBufferMemoryRanges[resourceIdx]))
				logFailAndTerminate("Failed to flush the Device Memory!\n");

			m_histogramSavedSeamphore->signal(imageHistogramIdx + 1);
			std::string msg = std::string("Image nr ") + std::to_string(imageHistogramIdx) + " saved. Resource idx: " + std::to_string(resourceIdx);
			m_logger->log(msg);
		}

		m_histogramBufferAllocation.memory->unmap();
		for (auto& file : files)
			file.close();
	}

	inline void waitForPreviousStep(ISemaphore* semaphore, uint32_t waitVal)
	{
		const ISemaphore::SWaitInfo imagesReadyToBeDownloaded[] = {
			{
				.semaphore = semaphore,
				.value = waitVal
			}
		};

		if (m_device->blockForSemaphores(imagesReadyToBeDownloaded) != ISemaphore::WAIT_RESULT::SUCCESS)
			logFailAndTerminate("Couldn't block for the `m_imagesProcessedSemaphore`.");
	}
	
	//! return value: inditaces if resource will be reused
	bool waitForResourceAvailability(ISemaphore* semaphore, uint32_t imageIdx)
	{
		if (imageIdx >= FRAMES_IN_FLIGHT)
		{
			const ISemaphore::SWaitInfo cmdBufDonePending[] = {
				{
					.semaphore = m_imagesProcessedSemaphore.get(),
					.value = imageIdx + 1 - FRAMES_IN_FLIGHT
				}
			};

			if (m_device->blockForSemaphores(cmdBufDonePending) != ISemaphore::WAIT_RESULT::SUCCESS)
				logFailAndTerminate("Couldn't block for the `m_imagesProcessedSemaphore`.");

			return true;
		}

		return false;
	}
};

NBL_MAIN_FUNC(StagingAndMultipleQueuesApp)
