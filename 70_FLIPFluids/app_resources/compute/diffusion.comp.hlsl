#include "../common.hlsl"
#include "../gridUtils.hlsl"
#include "../cellUtils.hlsl"
#include "../descriptor_bindings.hlsl"

struct SPushConstants
{
    float4 diffusionParameters;
};

[[vk::push_constant]] SPushConstants pc;

[[vk::binding(b_dGridData, s_d)]]
cbuffer GridData
{
    SGridData gridData;
};

[[vk::binding(b_dCMBuffer, s_d)]] RWStructuredBuffer<uint> cellMaterialBuffer;
[[vk::binding(b_dVelBuffer, s_d)]] RWStructuredBuffer<float4> velocityFieldBuffer;
[[vk::binding(b_dAxisInBuffer, s_d)]] RWStructuredBuffer<uint4> axisCellMaterialInBuffer;
[[vk::binding(b_dAxisOutBuffer, s_d)]] RWStructuredBuffer<uint4> axisCellMaterialOutBuffer;
[[vk::binding(b_dDiffInBuffer, s_d)]] RWStructuredBuffer<float4> gridDiffusionInBuffer;
[[vk::binding(b_dDiffOutBuffer, s_d)]] RWStructuredBuffer<float4> gridDiffusionOutBuffer;

[numthreads(WorkgroupSize, 1, 1)]
void setAxisCellMaterial(uint32_t3 ID : SV_DispatchThreadID)
{
    uint cid = ID.x;
    int3 cellIdx = flatIdxToCellIdx(cid, gridData.gridSize);

    uint cellMaterial = cellMaterialBuffer[cid];

    uint this_cm = getCellMaterial(cellMaterial);
    uint xp_cm = getXPrevMaterial(cellMaterial);
    uint yp_cm = getYPrevMaterial(cellMaterial);
    uint zp_cm = getZPrevMaterial(cellMaterial);

    uint3 cellAxisType;
    cellAxisType.x = 
        isSolidCell(this_cm) || isSolidCell(xp_cm) ? CM_SOLID :
        isFluidCell(this_cm) || isFluidCell(xp_cm) ? CM_FLUID :
        CM_AIR;
    cellAxisType.y = 
        isSolidCell(this_cm) || isSolidCell(yp_cm) ? CM_SOLID :
        isFluidCell(this_cm) || isFluidCell(yp_cm) ? CM_FLUID :
        CM_AIR;
    cellAxisType.z = 
        isSolidCell(this_cm) || isSolidCell(zp_cm) ? CM_SOLID :
        isFluidCell(this_cm) || isFluidCell(zp_cm) ? CM_FLUID :
        CM_AIR;

    uint3 cmAxisTypes = 0;
    setCellMaterial(cmAxisTypes, cellAxisType);

    axisCellMaterialOutBuffer[cid].xyz = cmAxisTypes;
}

[numthreads(WorkgroupSize, 1, 1)]
void setNeighborAxisCellMaterial(uint32_t3 ID : SV_DispatchThreadID)
{
    uint cid = ID.x;
    int3 cellIdx = flatIdxToCellIdx(cid, gridData.gridSize);

    uint3 axisCm = (uint3)0;
    uint3 this_axiscm = getCellMaterial(axisCellMaterialInBuffer[cid].xyz);
    setCellMaterial(axisCm, this_axiscm);

    uint3 xp_axiscm = cellIdx.x == 0 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(-1, 0, 0), gridData.gridSize)].xyz);
    setXPrevMaterial(axisCm, xp_axiscm);

    uint3 xn_axiscm = cellIdx.x == gridData.gridSize.x - 1 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(1, 0, 0), gridData.gridSize)].xyz);
    setXNextMaterial(axisCm, xn_axiscm);

    uint3 yp_axiscm = cellIdx.y == 0 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, -1, 0), gridData.gridSize)].xyz);
    setYPrevMaterial(axisCm, yp_axiscm);

    uint3 yn_axiscm = cellIdx.y == gridData.gridSize.y - 1 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 1, 0), gridData.gridSize)].xyz);
    setYNextMaterial(axisCm, yn_axiscm);

    uint3 zp_axiscm = cellIdx.z == 0 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 0, -1), gridData.gridSize)].xyz);
    setZPrevMaterial(axisCm, zp_axiscm);

    uint3 zn_axiscm = cellIdx.z == gridData.gridSize.z - 1 ? (uint3)CM_SOLID : getCellMaterial(axisCellMaterialInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 0, 1), gridData.gridSize)].xyz);
    setZNextMaterial(axisCm, zn_axiscm);

    axisCellMaterialOutBuffer[cid].xyz = axisCm;
}

[numthreads(WorkgroupSize, 1, 1)]
void applyDiffusion(uint32_t3 ID : SV_DispatchThreadID)
{
    uint cid = ID.x;
    int3 cellIdx = flatIdxToCellIdx(cid, gridData.gridSize);

    uint3 axisCm = axisCellMaterialInBuffer[cid].xyz;
    float3 velocity = (float3)0;

    float3 diff = gridDiffusionInBuffer[cid].xyz;
    float3 xp_diff = select(isFluidCell(getXPrevMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(-1, 0, 0), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.x * xp_diff;

    float3 xn_diff = select(isFluidCell(getXNextMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(1, 0, 0), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.x * xn_diff;

    float3 yp_diff = select(isFluidCell(getYPrevMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, -1, 0), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.y * yp_diff;

    float3 yn_diff = select(isFluidCell(getYNextMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 1, 0), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.y * yn_diff;

    float3 zp_diff = select(isFluidCell(getZPrevMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 0, -1), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.z * zp_diff;

    float3 zn_diff = select(isFluidCell(getZNextMaterial(axisCm)),
        gridDiffusionInBuffer[cellIdxToFlatIdx(cellIdx + int3(0, 0, 1), gridData.gridSize)].xyz, diff);
    velocity += pc.diffusionParameters.z * zn_diff;

    velocity += pc.diffusionParameters.w * velocityFieldBuffer[cid].xyz;

    enforceBoundaryCondition(velocity, cellMaterialBuffer[cid]);

    gridDiffusionOutBuffer[cid].xyz = velocity;
}

[numthreads(WorkgroupSize, 1, 1)]
void updateVelocity(uint32_t3 ID : SV_DispatchThreadID)
{
    uint cid = ID.x;
    int3 cellIdx = flatIdxToCellIdx(cid, gridData.gridSize);

    float3 velocity = gridDiffusionInBuffer[cid].xyz;

    enforceBoundaryCondition(velocity, cellMaterialBuffer[cid]);

    velocityFieldBuffer[cid].xyz = velocity;
}
