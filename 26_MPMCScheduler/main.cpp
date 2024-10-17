// Copyright (C) 2024-2025 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h
#include "nabla.h"
#include "nbl/application_templates/MonoAssetManagerAndBuiltinResourceApplication.hpp"
#include "SimpleWindowedApplication.hpp"

using namespace nbl;
using namespace nbl::core;
using namespace nbl::system;
using namespace nbl::asset;
using namespace nbl::ui;
using namespace nbl::video;


class MPMCSchedulerApp final : public examples::SimpleWindowedApplication, public application_templates::MonoAssetManagerAndBuiltinResourceApplication
{
		using device_base_t = examples::SimpleWindowedApplication;
		using asset_base_t = application_templates::MonoAssetManagerAndBuiltinResourceApplication;
		using clock_t = std::chrono::steady_clock;

		constexpr static inline uint32_t WIN_W = 1280, WIN_H = 720, SC_IMG_COUNT = 3u, FRAMES_IN_FLIGHT = 5u;
		static_assert(FRAMES_IN_FLIGHT > SC_IMG_COUNT);

	public:
		inline MPMCSchedulerApp(const path& _localInputCWD, const path& _localOutputCWD, const path& _sharedInputCWD, const path& _sharedOutputCWD)
			: IApplicationFramework(_localInputCWD, _localOutputCWD, _sharedInputCWD, _sharedOutputCWD) {}

		inline bool isComputeOnly() const override { return false; }

		inline core::vector<video::SPhysicalDeviceFilter::SurfaceCompatibility> getSurfaces() const override
		{
			if (!m_surface)
			{
				{
					IWindow::SCreationParams params = {};
					params.callback = core::make_smart_refctd_ptr<nbl::video::ISimpleManagedSurface::ICallback>();
					params.width = WIN_W;
					params.height = WIN_H;
					params.x = 32;
					params.y = 32;
					params.flags = IWindow::ECF_BORDERLESS | IWindow::ECF_HIDDEN;
					params.windowCaption = "MPMCSchedulerApp";
					const_cast<std::remove_const_t<decltype(m_window)>&>(m_window) = m_winMgr->createWindow(std::move(params));
				}
				auto surface = CSurfaceVulkanWin32::create(smart_refctd_ptr(m_api), smart_refctd_ptr_static_cast<IWindowWin32>(m_window));
				const_cast<std::remove_const_t<decltype(m_surface)>&>(m_surface) = nbl::video::CSimpleResizeSurface<CDefaultSwapchainFramebuffers>::create(std::move(surface));
			}

			if (m_surface)
				return { {m_surface->getSurface()/*,EQF_NONE*/} };

			return {};
		}

		inline bool onAppInitialized(smart_refctd_ptr<ISystem>&& system) override
		{
			if (!device_base_t::onAppInitialized(smart_refctd_ptr(system)))
				return false;
			if (!asset_base_t::onAppInitialized(std::move(system)))
				return false;

			smart_refctd_ptr<IGPUShader> shader;
			{
				IAssetLoader::SAssetLoadParams lp = {};
				lp.logger = m_logger.get();
				lp.workingDirectory = ""; // virtual root
				auto assetBundle = m_assetMgr->getAsset("app_resources/shader.comp.hlsl", lp);
				const auto assets = assetBundle.getContents();
				if (assets.empty())
					return logFail("Failed to load shader from disk");

				// lets go straight from ICPUSpecializedShader to IGPUSpecializedShader
				auto source = IAsset::castDown<ICPUShader>(assets[0]);
				if (!source)
					return logFail("Failed to load shader from disk");

				shader = m_device->createShader(source.get());
				if (!shader)
					return false;
			}
			
			smart_refctd_ptr<IGPUDescriptorSetLayout> dsLayout;
			{
				const IGPUDescriptorSetLayout::SBinding bindings[1] = { {
					.binding = 0,
					.type = IDescriptor::E_TYPE::ET_STORAGE_IMAGE,
					.createFlags = IGPUDescriptorSetLayout::SBinding::E_CREATE_FLAGS::ECF_NONE,
					.stageFlags = IShader::E_SHADER_STAGE::ESS_COMPUTE,
					.count = 1
				}
				};
				dsLayout = m_device->createDescriptorSetLayout(bindings);
				if (!dsLayout)
					return logFail("Failed to Create Descriptor Layout");
			}

			{
				auto layout = m_device->createPipelineLayout({},smart_refctd_ptr(dsLayout));
				const IGPUComputePipeline::SCreationParams params[] = { {
					{
						.layout = layout.get()
					},
					{},
					IGPUComputePipeline::SCreationParams::FLAGS::NONE,
					{
						.entryPoint = "main",
						.shader = shader.get(),
						.entries = nullptr,
						.requiredSubgroupSize = IGPUShader::SSpecInfo::SUBGROUP_SIZE::UNKNOWN,
						.requireFullSubgroups = true
					}
				}};
				if (!m_device->createComputePipelines(nullptr,params,&m_ppln))
					return logFail("Failed to create Pipeline");
			}

			m_hdr = m_device->createImage({
				{
					.type = IGPUImage::E_TYPE::ET_2D,
					.samples = IGPUImage::E_SAMPLE_COUNT_FLAGS::ESCF_1_BIT,
					.format = E_FORMAT::EF_E5B9G9R9_UFLOAT_PACK32,
					.extent = {WIN_W,WIN_H,1},
					.mipLevels = 1,
					.arrayLayers = 1,
					.flags = core::bitflag(IGPUImage::E_CREATE_FLAGS::ECF_MUTABLE_FORMAT_BIT) | IGPUImage::E_CREATE_FLAGS::ECF_EXTENDED_USAGE_BIT,
					.usage = IGPUImage::E_USAGE_FLAGS::EUF_TRANSFER_SRC_BIT | IGPUImage::E_USAGE_FLAGS::EUF_STORAGE_BIT
				}
			});
			if (!m_hdr || m_device->allocate(m_hdr->getMemoryReqs(),m_hdr.get()).isValid())
				return logFail("Could not create HDR Image");

			{
				auto pool = m_device->createDescriptorPoolForDSLayouts(IDescriptorPool::E_CREATE_FLAGS::ECF_NONE,{&dsLayout.get(),1});
				if (!pool)
					return logFail("Could not create Descriptor Pool");
				auto ds = pool->createDescriptorSet(std::move(dsLayout));
				if (!ds)
					return logFail("Could not create Descriptor Set");
				IGPUDescriptorSet::SDescriptorInfo info = {};
				{
					info.desc = m_device->createImageView({
						.flags = IGPUImageView::ECF_NONE,
						.subUsages = IGPUImage::E_USAGE_FLAGS::EUF_STORAGE_BIT,
						.image = m_hdr,
						.viewType = IGPUImageView::E_TYPE::ET_2D,
						.format = E_FORMAT::EF_R32_UINT

					});
					if (!info.desc)
						return logFail("Failed to create image view");
					info.info.image.imageLayout = IGPUImage::LAYOUT::GENERAL;
				}
				const IGPUDescriptorSet::SWriteDescriptorSet writes[] = {{
					.dstSet = ds.get(),
					.binding = 0,
					.arrayElement =1,
					.count = 1,
					.info = &info
				}};
				if (!m_device->updateDescriptorSets(writes,{}))
					return logFail("Failed to write descriptor set");
			}

			m_semaphore = m_device->createSemaphore(m_realFrameIx);
			if (!m_semaphore)
				return logFail("Failed to Create a Semaphore!");

			ISwapchain::SCreationParams swapchainParams = { .surface = m_surface->getSurface() };
			if (!swapchainParams.deduceFormat(m_physicalDevice))
				return logFail("Could not choose a Surface Format for the Swapchain!");

			auto gQueue = getGraphicsQueue();
			if (!m_surface || !m_surface->init(gQueue, nullptr, swapchainParams.sharedParams))
				return logFail("Could not create Window & Surface or initialize the Surface!");

			m_maxFramesInFlight = m_surface->getMaxFramesInFlight();
			if (FRAMES_IN_FLIGHT < m_maxFramesInFlight)
			{
				m_logger->log("Lowering frames in flight!", ILogger::ELL_WARNING);
				m_maxFramesInFlight = FRAMES_IN_FLIGHT;
			}

			auto pool = m_device->createCommandPool(gQueue->getFamilyIndex(),IGPUCommandPool::CREATE_FLAGS::RESET_COMMAND_BUFFER_BIT);
			for (auto i=0u; i<m_maxFramesInFlight; i++)
			{
				if (!pool)
					return logFail("Couldn't create Command Pool!");
				if (!pool->createCommandBuffers(IGPUCommandPool::BUFFER_LEVEL::PRIMARY,{m_cmdBufs.data()+i,1}))
					return logFail("Couldn't create Command Buffer!");
			}

			m_winMgr->setWindowSize(m_window.get(), WIN_W, WIN_H);
			m_surface->recreateSwapchain();

			auto assetManager = make_smart_refctd_ptr<nbl::asset::IAssetManager>(smart_refctd_ptr(system));

			m_winMgr->show(m_window.get());

			return true;
		}

		inline void workLoopBody() override
		{
			const auto resourceIx = m_realFrameIx % m_maxFramesInFlight;

			if (m_realFrameIx >= m_maxFramesInFlight)
			{
				const ISemaphore::SWaitInfo cbDonePending[] =
				{
					{
						.semaphore = m_semaphore.get(),
						.value = m_realFrameIx + 1 - m_maxFramesInFlight
					}
				};
				if (m_device->blockForSemaphores(cbDonePending) != ISemaphore::WAIT_RESULT::SUCCESS)
					return;
			}

			m_currentImageAcquire = m_surface->acquireNextImage();
			if (!m_currentImageAcquire)
				return;

			auto* const cb = m_cmdBufs.data()[resourceIx].get();
			cb->reset(IGPUCommandBuffer::RESET_FLAGS::RELEASE_RESOURCES_BIT);
			cb->begin(IGPUCommandBuffer::USAGE::ONE_TIME_SUBMIT_BIT);

			IGPUCommandBuffer::SImageMemoryBarrier<IGPUCommandBuffer::SOwnershipTransferBarrier> imgBarriers[] = {{
				.barrier = {
					.dep = {
						.srcStageMask = PIPELINE_STAGE_FLAGS::ALL_COMMANDS_BITS,
						.srcAccessMask = ACCESS_FLAGS::MEMORY_WRITE_BITS,
						.dstStageMask = PIPELINE_STAGE_FLAGS::CLEAR_BIT,
						.dstAccessMask = ACCESS_FLAGS::TRANSFER_WRITE_BIT
					}
					// no ownership transfer
				},
				.subresourceRange = {
				},
				.oldLayout = IImage::LAYOUT::UNDEFINED, // don't care about old contents
				.newLayout = IImage::LAYOUT::GENERAL
			}};

			// clear the image
			cb->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE,{.memBarriers={},.bufBarriers={},.imgBarriers=imgBarriers});
			{
				const IGPUCommandBuffer::SClearColorValue color = {
					.float32 = {0,0,0,1}
				};
				const IGPUImage::SSubresourceRange range = {
					.aspectMask = IGPUImage::E_ASPECT_FLAGS::EAF_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1
				};
				cb->clearColorImage(m_hdr.get(),IGPUImage::LAYOUT::GENERAL,&color,1,&range);
				// now we stay in same layout for remainder of the frame
				imgBarriers[0].oldLayout = IImage::LAYOUT::GENERAL;
			}

			const SMemoryBarrier computeToBlit = {
				.srcStageMask = PIPELINE_STAGE_FLAGS::COMPUTE_SHADER_BIT,
				.srcAccessMask = ACCESS_FLAGS::STORAGE_WRITE_BIT|ACCESS_FLAGS::STORAGE_READ_BIT,
				.dstStageMask = PIPELINE_STAGE_FLAGS::BLIT_BIT,
				.dstAccessMask = ACCESS_FLAGS::STORAGE_READ_BIT
			};

			auto& imgDep = imgBarriers[0].barrier.dep;
			imgDep = imgDep.nextBarrier(computeToBlit);
			cb->pipelineBarrier(E_DEPENDENCY_FLAGS::EDF_NONE,{.memBarriers={},.bufBarriers={},.imgBarriers=imgBarriers});

			// write the image
			{
				//
	//			cb->bindComputePipeline(rawPipeline);
	// push constants
			}

			cb->end();

			{
				const IQueue::SSubmitInfo::SSemaphoreInfo rendered[] =
				{
					{
						.semaphore = m_semaphore.get(),
						.value = ++m_realFrameIx,
						.stageMask = PIPELINE_STAGE_FLAGS::ALL_GRAPHICS_BITS
					}
				};
				{
					{
						const IQueue::SSubmitInfo::SCommandBufferInfo commandBuffers[] =
						{
							{.cmdbuf = cb }
						};

						const IQueue::SSubmitInfo::SSemaphoreInfo acquired[] =
						{
							{
								.semaphore = m_currentImageAcquire.semaphore,
								.value = m_currentImageAcquire.acquireCount,
								.stageMask = PIPELINE_STAGE_FLAGS::NONE
							}
						};
						const IQueue::SSubmitInfo infos[] =
						{
							{
								.waitSemaphores = acquired,
								.commandBuffers = commandBuffers,
								.signalSemaphores = rendered
							}
						};

						if (getGraphicsQueue()->submit(infos) == IQueue::RESULT::SUCCESS)
						{
							const nbl::video::ISemaphore::SWaitInfo waitInfos[] =
							{ {
								.semaphore = m_semaphore.get(),
								.value = m_realFrameIx
							} };

							m_device->blockForSemaphores(waitInfos); // this is not solution, quick wa to not throw validation errors
						}
						else
							--m_realFrameIx;
					}
				}

				m_surface->present(m_currentImageAcquire.imageIndex, rendered);
			}
		}

		inline bool keepRunning() override
		{
			if (m_surface->irrecoverable())
				return false;

			return true;
		}

		inline bool onAppTerminated() override
		{
			return device_base_t::onAppTerminated();
		}

	private:
		smart_refctd_ptr<IWindow> m_window;
		smart_refctd_ptr<CSimpleResizeSurface<CDefaultSwapchainFramebuffers>> m_surface;
		smart_refctd_ptr<IGPUImage> m_hdr;
		smart_refctd_ptr<IGPUComputePipeline> m_ppln;
		smart_refctd_ptr<ISemaphore> m_semaphore;
		uint64_t m_realFrameIx : 59 = 0;
		uint64_t m_maxFramesInFlight : 5;
		std::array<smart_refctd_ptr<IGPUCommandBuffer>,ISwapchain::MaxImages> m_cmdBufs;
		ISimpleManagedSurface::SAcquireResult m_currentImageAcquire = {};
};

NBL_MAIN_FUNC(MPMCSchedulerApp)