#define _IRR_STATIC_LIB_
#include <irrlicht.h>

#include "../common/QToQuitEventReceiver.h"
#include "../source/Irrlicht/COpenGLExtensionHandler.h"

#include <random>

using namespace irr;
using namespace core;
using namespace asset;
using namespace video;


bool doCulling = false;
bool useDrawIndirect = false;

class MyEventReceiver : public QToQuitEventReceiver
{
public:

	MyEventReceiver()
	{
	}

	bool OnEvent(const SEvent& event)
	{
        if (event.EventType == irr::EET_KEY_INPUT_EVENT && !event.KeyInput.PressedDown)
        {
            switch (event.KeyInput.Key)
            {
            case irr::KEY_KEY_Q: // so we can quit
				return QToQuitEventReceiver::OnEvent(event);
            case irr::KEY_KEY_C: // so we can quit
                doCulling = !doCulling; // Not enabled/necessary yet
                return true;
            case irr::KEY_SPACE: // toggle between gpu and cpu cull
                useDrawIndirect = !useDrawIndirect;
                return true;
            default:
                break;
            }
        }

		return false;
	}

private:
};


#include "common.glsl"
#include "commonIndirect.glsl"

int main()
{
	// create device with full flexibility over creation parameters
	// you can add more parameters if desired, check irr::SIrrlichtCreationParameters
	irr::SIrrlichtCreationParameters params;
	params.Bits = 24; //may have to set to 32bit for some platforms
	params.ZBufferBits = 24; //we'd like 32bit here
	params.DriverType = video::EDT_OPENGL; //! Only Well functioning driver, software renderer left for sake of 2D image drawing
	params.WindowSize = dimension2d<uint32_t>(1280, 720);
	params.Fullscreen = false;
	params.Vsync = false;
	params.Doublebuffer = true;
	params.Stencilbuffer = false; //! This will not even be a choice soon
	auto device = createDeviceEx(params);

	if (!device)
		return 1; // could not create selected driver.

	MyEventReceiver receiver;
	device->setEventReceiver(&receiver);


    auto* am = device->getAssetManager();
    video::IVideoDriver* driver = device->getVideoDriver();

    IAssetLoader::SAssetLoadParams lp;
    auto cullShaderBundle = am->getAsset("../boxFrustCull.comp", lp);
    auto vertexShaderBundleMDI = am->getAsset("../meshGPU.vert", lp);
    auto vertexShaderBundle = am->getAsset("../meshCPU.vert", lp);
    auto fragShaderBundle = am->getAsset("../mesh.frag", lp);

    CShaderIntrospector introspector(am->getGLSLCompiler());
    const auto extensions = driver->getSupportedGLSLExtensions();
    auto cpuCullPipeline = introspector.createApproximateComputePipelineFromIntrospection(IAsset::castDown<ICPUSpecializedShader>(cullShaderBundle.getContents().first->get()), extensions->begin(), extensions->end());
    auto gpuCullPipeline = driver->getGPUObjectsFromAssets(&cpuCullPipeline.get(),&cpuCullPipeline.get()+1)->operator[](0);

    ICPUSpecializedShader* shaders[2] = { IAsset::castDown<ICPUSpecializedShader>(vertexShaderBundle.getContents().first->get()),IAsset::castDown<ICPUSpecializedShader>(fragShaderBundle.getContents().first->get()) };
    auto cpuDrawDirectPipeline = introspector.createApproximateRenderpassIndependentPipelineFromIntrospection(shaders, shaders+2, extensions->begin(), extensions->end());
    shaders[0] = IAsset::castDown<ICPUSpecializedShader>(vertexShaderBundleMDI.getContents().first->get());
    auto cpuDrawIndirectPipeline = introspector.createApproximateRenderpassIndependentPipelineFromIntrospection(shaders, shaders+2, extensions->begin(), extensions->end());


#ifndef _IRR_DEBUG
    auto* fs = am->getFileSystem();
    
    auto* qnc = am->getMeshManipulator()->getQuantNormalCache();
    //loading cache from file
    //qnc->loadNormalQuantCacheFromFile<asset::E_QUANT_NORM_CACHE_TYPE::Q_2_10_10_10>(fs, "../../tmp/normalCache101010.sse", true);
#endif

    constexpr auto kInstanceCount = 4096;
    constexpr auto  kTotalTriangleLimit = 64*1024*1024;
    
    core::vector<ModelData_t> instanceData(kInstanceCount);
    auto mbuff = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<core::smart_refctd_ptr<video::IGPUMeshBuffer> > >(kInstanceCount);
    
    //
    SBufferBinding<video::IGPUBuffer> globalVertexBindings[SVertexInputParams::MAX_ATTR_BUF_BINDING_COUNT];
    core::smart_refctd_ptr<video::IGPUBuffer> globalIndexBuffer,perDrawDataSSBO,indirectDrawSSBO,perInstanceDataSSBO;

    
    core::smart_refctd_ptr<video::IGPURenderpassIndependentPipeline> gpuDrawDirectPipeline,gpuDrawIndirectPipeline;
	{
        core::vector<IGeometryCreator::return_type> cpumesh(kInstanceCount);

        size_t vertexSize = 0;
        std::vector<uint8_t> vertexData;
        std::vector<uint32_t> indexData;

        std::random_device rd;
        std::mt19937 mt(rd());
        std::uniform_int_distribution<uint32_t> dist(16, 4*1024);
        for (size_t i=0; i<kInstanceCount; i++)
        {
            float poly = sqrtf(dist(mt))+0.5f;
            const auto& sphereData = cpumesh[i] = am->getGeometryCreator()->createSphereMesh(2.f,poly,poly);

            //some assumptions about generated mesh
            assert(sphereData.assemblyParams.primitiveType==asset::EPT_TRIANGLE_LIST);
            assert(sphereData.indexType==asset::EIT_32BIT);
            assert(sphereData.indexBuffer.offset==0);

            assert(sphereData.inputParams.enabledBindingFlags&0x1u); //helpful assumption

            const SBufferBinding<ICPUBuffer>* databuf = nullptr;
            for (size_t j=0; j<SVertexInputParams::MAX_ATTR_BUF_BINDING_COUNT; j++)
            if ((sphereData.inputParams.enabledBindingFlags>>j)&0x1u)
            {
                if (databuf) // if first found
                {
                    assert(databuf->operator==(sphereData.bindings[j])); // all sphere vertex data will be packed in the same buffer
                }
                else
                    databuf = sphereData.bindings+j;
                

                if (vertexSize) //if set
                {
                    assert(sphereData.inputParams.bindings[j].stride==vertexSize); //all data in the same buffer == same vertex stride for all attributes
                }
                else
                    vertexSize = sphereData.inputParams.bindings[j].stride;
            }

            auto vdatasize = core::roundUp(databuf->buffer->getSize(),vertexSize);
            auto vdata = reinterpret_cast<const uint8_t*>(databuf->buffer->getPointer());
            vertexData.insert(vertexData.end(),vdata,vdata+vdatasize);

            auto idata = reinterpret_cast<const uint32_t*>(sphereData.indexBuffer.buffer->getPointer());
            indexData.insert(indexData.end(),idata,idata+sphereData.indexCount);
        }
        
#ifndef _IRR_DEBUG
        //! cache results -- speeds up mesh generation on second run
        //qnc->saveCacheToFile(asset::E_QUANT_NORM_CACHE_TYPE::Q_2_10_10_10, fs, "../../tmp/normalCache101010.sse");
#endif

        //
        {
            globalIndexBuffer = driver->createFilledDeviceLocalGPUBufferOnDedMem(indexData.size()*sizeof(uint32_t),indexData.data());
            indexData.clear();
        }

        globalVertexBindings[0] = { 0u,driver->createFilledDeviceLocalGPUBufferOnDedMem(vertexData.size(),vertexData.data()) };
        vertexData.clear();

        {
            DrawElementsIndirectCommand_t indirectDrawData[kInstanceCount];

            uint32_t baseVertex = 0;
            uint32_t indexOffset = 0;
            std::uniform_real_distribution<float> dist3D(0.f,400.f);
            for (size_t i=0; i<kInstanceCount; i++)
            {
                const auto& sphereData = cpumesh[i];
                if (i==0)
                {
                    auto enabledAttribBackupDirect = cpuDrawDirectPipeline->getVertexInputParams().enabledAttribFlags;
                    auto enabledAttribBackupIndirect = cpuDrawIndirectPipeline->getVertexInputParams().enabledAttribFlags;
                    cpuDrawDirectPipeline->getVertexInputParams() = sphereData.inputParams;
                    cpuDrawIndirectPipeline->getVertexInputParams() = sphereData.inputParams;
                    cpuDrawDirectPipeline->getVertexInputParams().enabledAttribFlags = enabledAttribBackupDirect;
                    cpuDrawIndirectPipeline->getVertexInputParams().enabledAttribFlags = enabledAttribBackupIndirect;

                    cpuDrawDirectPipeline->getPrimitiveAssemblyParams() = sphereData.assemblyParams;
                    cpuDrawIndirectPipeline->getPrimitiveAssemblyParams() = sphereData.assemblyParams;

                    gpuDrawDirectPipeline = driver->getGPUObjectsFromAssets(&cpuDrawDirectPipeline.get(),&cpuDrawDirectPipeline.get()+1)->operator[](0);
                    gpuDrawIndirectPipeline = driver->getGPUObjectsFromAssets(&cpuDrawIndirectPipeline.get(),&cpuDrawIndirectPipeline.get()+1)->operator[](0);
                }

                indirectDrawData[i].count = sphereData.indexCount;
                indirectDrawData[i].instanceCount = 1;
                indirectDrawData[i].firstIndex = indexOffset/sizeof(uint32_t);
                indirectDrawData[i].baseVertex = baseVertex;
                indirectDrawData[i].baseInstance = 0;

                auto& meshbuffer = mbuff->operator[](i);
                meshbuffer = core::make_smart_refctd_ptr<video::IGPUMeshBuffer>(core::smart_refctd_ptr(gpuDrawDirectPipeline),nullptr,globalVertexBindings,SBufferBinding<video::IGPUBuffer>{indexOffset,core::smart_refctd_ptr(globalIndexBuffer)});
                indexOffset += sphereData.indexCount*sizeof(uint32_t);

                meshbuffer->setBaseVertex(baseVertex);
                // TODO: checks on this!
                baseVertex += (sphereData.bindings[0u].offset+sphereData.inputParams.attributes[0u].relativeOffset)/vertexSize;

                meshbuffer->setIndexCount(sphereData.indexCount);

                meshbuffer->setIndexType(asset::EIT_32BIT);
            
                meshbuffer->setBoundingBox(sphereData.bbox);

                auto& instance = instanceData[i];
                {
                    float scale = dist3D(mt)*0.0025f+1.f;
                    instance.worldMatrix.setScale(core::vectorSIMDf(scale,scale,scale));
                }
                instance.worldMatrix.setTranslation(core::vectorSIMDf(dist3D(mt),dist3D(mt),dist3D(mt)));
                instance.worldMatrix.getSub3x3InverseTranspose(instance.normalMatrix);
                instance.bbox[0].set(sphereData.bbox.MinEdge);
                instance.bbox[1].set(sphereData.bbox.MaxEdge);
            }

            perInstanceDataSSBO = driver->createFilledDeviceLocalGPUBufferOnDedMem(instanceData.size()*sizeof(ModelData_t),instanceData.data());
            indirectDrawSSBO = driver->createFilledDeviceLocalGPUBufferOnDedMem(sizeof(indirectDrawData),indirectDrawData);
        }
	}
    
	core::vector<DrawData_t> perDrawData(kInstanceCount);
	perDrawDataSSBO = driver->createDeviceLocalGPUBufferOnDedMem(perDrawData.size()*sizeof(DrawData_t));
    
    // TODO: get rid of the `const_cast`s
    auto drawDirectLayout = const_cast<video::IGPUPipelineLayout*>(gpuDrawDirectPipeline->getLayout());
    auto drawIndirectLayout = const_cast<video::IGPUPipelineLayout*>(gpuDrawIndirectPipeline->getLayout());
    auto cullLayout = const_cast<video::IGPUPipelineLayout*>(gpuCullPipeline->getLayout());
    auto drawDirectDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(drawDirectLayout->getDescriptorSetLayout(1));
    auto drawIndirectDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(drawIndirectLayout->getDescriptorSetLayout(1));
    auto cullDescriptorLayout = const_cast<video::IGPUDescriptorSetLayout*>(cullLayout->getDescriptorSetLayout(1));
    auto drawDirectDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(drawDirectDescriptorLayout));
    auto drawIndirectDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(drawIndirectDescriptorLayout));
    auto cullDescriptorSet = driver->createGPUDescriptorSet(core::smart_refctd_ptr<video::IGPUDescriptorSetLayout>(cullDescriptorLayout));
    {
        constexpr auto BindingCount = 3u;
        video::IGPUDescriptorSet::SWriteDescriptorSet writes[BindingCount];
        video::IGPUDescriptorSet::SDescriptorInfo infos[BindingCount];
        for (auto i=0; i<BindingCount; i++)
        {
            writes[i].binding = i;
            writes[i].arrayElement = 0u;
            writes[i].count = 1u;
            writes[i].descriptorType = asset::EDT_STORAGE_BUFFER;
            writes[i].info = infos+i;
        }
        infos[0].desc = perDrawDataSSBO;
        infos[0].buffer = { 0u,perDrawDataSSBO->getSize() };
        infos[1].desc = indirectDrawSSBO;
        infos[1].buffer = { 0u,indirectDrawSSBO->getSize() };
        infos[2].desc = perInstanceDataSSBO;
        infos[2].buffer = { 0u,perInstanceDataSSBO->getSize() };

        writes[0].dstSet = drawDirectDescriptorSet.get();
        driver->updateDescriptorSets(1u,writes,0u,nullptr);

        writes[0].dstSet = drawIndirectDescriptorSet.get();
        driver->updateDescriptorSets(1u,writes,0u,nullptr);

        writes[0].dstSet = cullDescriptorSet.get();
        writes[1].dstSet = cullDescriptorSet.get();
        writes[2].dstSet = cullDescriptorSet.get();
        driver->updateDescriptorSets(BindingCount,writes,0u,nullptr);
    }
    

    auto smgr = device->getSceneManager();

	scene::ICameraSceneNode* camera =
		smgr->addCameraSceneNodeFPS(0,100.0f,0.01f);
	camera->setPosition(core::vector3df(-4,0,0));
	camera->setTarget(core::vector3df(0,0,0));
	camera->setNearValue(0.01f);
	camera->setFarValue(250.0f);
    smgr->setActiveCamera(camera);

    device->getCursorControl()->setVisible(false);


	uint64_t lastFPSTime = 0;
	float lastFastestMeshFrameNr = -1.f;

	while(device->run() && receiver.keepOpen())
	//if (device->isWindowActive())
	{
		driver->beginScene(true, true, video::SColor(255,255,255,255) );

        //! This animates (moves) the camera and sets the transforms
        camera->OnAnimate(std::chrono::duration_cast<std::chrono::milliseconds>(device->getTimer()->getTime()).count());
        camera->render();
        
        core::matrix3x4SIMD normalMatrix;
        camera->getViewMatrix().getSub3x3InverseTranspose(normalMatrix);
        if (useDrawIndirect)
        {
            CullShaderData_t pc;
            pc.viewProjMatrix = camera->getConcatenatedMatrix();
            pc.viewInverseTransposeMatrix = normalMatrix;
            pc.maxDrawCount = kInstanceCount;
            pc.cull = doCulling ? 1u:0u;

            driver->bindComputePipeline(gpuCullPipeline.get());
            driver->bindDescriptorSets(video::EPBP_COMPUTE, gpuCullPipeline->getLayout(), 1u, 1u, &cullDescriptorSet.get(), nullptr);
            driver->dispatch(kInstanceCount/kCullWorkgroupSize,1u,1u);
            video::COpenGLExtensionHandler::extGlMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_COMMAND_BARRIER_BIT);

            driver->bindGraphicsPipeline(gpuDrawIndirectPipeline.get());
            driver->bindDescriptorSets(video::EPBP_GRAPHICS, gpuDrawIndirectPipeline->getLayout(), 1u, 1u, &drawIndirectDescriptorSet.get(), nullptr);
            driver->pushConstants(gpuDrawIndirectPipeline->getLayout(), asset::ICPUSpecializedShader::ESS_COMPUTE, 0u, sizeof(CullShaderData_t), &pc);
            driver->drawIndexedIndirect(globalVertexBindings,asset::EPT_TRIANGLE_LIST,asset::EIT_32BIT, globalIndexBuffer.get(),indirectDrawSSBO.get(),0,kInstanceCount,sizeof(DrawElementsIndirectCommand_t));
        }
        else
        {
            uint32_t unculledNum = 0u;
            uint32_t mb2draw[kInstanceCount];
            for (uint32_t i=0; i<kInstanceCount; i++)
            {
                if (doCulling)
                {
                    continue;
                }

                const auto& instance = instanceData[i];
                auto& draw = perDrawData[i];
                draw.modelViewProjMatrix = core::concatenateBFollowedByA(camera->getConcatenatedMatrix(),instance.worldMatrix);
                draw.normalMatrix = core::concatenateBFollowedByA(normalMatrix,instance.normalMatrix);
                mb2draw[unculledNum++] = i;
            }
            {
                auto defaultUploadBuffer = driver->getDefaultUpStreamingBuffer();

                const void* dataPtr = reinterpret_cast<const uint8_t*>(perDrawData.data());
                uint32_t offset = video::StreamingTransientDataBufferMT<>::invalid_address;
                uint32_t dataSize = perDrawData.size()*sizeof(DrawData_t);
                while (offset == video::StreamingTransientDataBufferMT<>::invalid_address)
                {
                    uint32_t alignment = 8u;
                    defaultUploadBuffer->multi_place(std::chrono::microseconds(500u), 1u, (const void* const*)&dataPtr, &offset, &dataSize, &alignment);
                }
                // some platforms expose non-coherent host-visible GPU memory, so writes need to be flushed explicitly
                if (defaultUploadBuffer->needsManualFlushOrInvalidate())
                    driver->flushMappedMemoryRanges({ {defaultUploadBuffer->getBuffer()->getBoundMemory(),offset,dataSize} });
                // after we make sure writes are in GPU memory (visible to GPU) and not still in a cache, we can copy using the GPU to device-only memory
                driver->copyBuffer(defaultUploadBuffer->getBuffer(), perDrawDataSSBO.get(), offset, 0u, dataSize);
                // this doesn't actually free the memory, the memory is queued up to be freed only after the GPU fence/event is signalled
                defaultUploadBuffer->multi_free(1u, &offset, &dataSize, std::move(driver->placeFence()));
            }

            driver->bindGraphicsPipeline(gpuDrawDirectPipeline.get());
            driver->bindDescriptorSets(video::EPBP_GRAPHICS, gpuDrawDirectPipeline->getLayout(), 1u, 1u, &drawDirectDescriptorSet.get(), nullptr);
            for (uint32_t i=0; i<unculledNum; i++)
            {
                driver->pushConstants(gpuDrawDirectPipeline->getLayout(),asset::ICPUSpecializedShader::ESS_VERTEX,0u,sizeof(uint32_t),mb2draw+i);
                driver->drawMeshBuffer(mbuff->operator[](mb2draw[i]).get());
            }
        }

		driver->endScene();

		// display frames per second in window title
		uint64_t time = device->getTimer()->getRealTime();
		if (time-lastFPSTime > 1000)
		{
			std::wostringstream str;
			str << L"MultiDrawIndirect Benchmark - Irrlicht Engine [" << driver->getName() << "] FPS:" << driver->getFPS() << " PrimitvesDrawn:" << driver->getPrimitiveCountDrawn();

			device->setWindowCaption(str.str());
			lastFPSTime = time;
		}
	}

	return 0;
}
