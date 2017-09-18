#include <core/pvs/particle_vector.h>
#include <core/pvs/object_vector.h>
#include <core/celllist.h>
#include <core/logger.h>
#include <core/cuda_common.h>

#include <core/mpi/object_halo_exchanger.h>
#include <core/mpi/valid_cell.h>

#include <vector>
#include <algorithm>
#include <limits>



__device__ void packExtraData(int objId, char** extraData, int nPtrsPerObj, const int* dataSizes, char* destanation)
{
	int baseId = 0;

	for (int ptrId = 0; ptrId < nPtrsPerObj; ptrId++)
		{
			// dataSizes are in bytes
			const int size = dataSizes[ptrId];
			for (int i = threadIdx.x; i < size; i += blockDim.x)
				destanation[baseId+i] = extraData[ptrId][objId*size + i];

			baseId += dataSizes[ptrId];
		}
}

__device__ void unpackExtraData(int objId, char** extraData, int nPtrsPerObj, const int* dataSizes, const char* source)
{
	int baseId = 0;

	for (int ptrId = 0; ptrId < nPtrsPerObj; ptrId++)
	{
		// dataSizes are in bytes
		const int size = dataSizes[ptrId];
		for (int i = threadIdx.x; i < size; i += blockDim.x)
			extraData[ptrId][objId*size + i] = source[baseId+i];

		baseId += dataSizes[ptrId];
	}
}


__global__ void getObjectHalos(const float4* __restrict__ coosvels, const LocalObjectVector::COMandExtent* props, const int nObj, const int objSize,
		const float3 localDomainSize, const float rc,
		const int64_t dests[27], int bufSizes[27], /*int* haloParticleIds,*/
		const int packedObjSize_byte, char** extraData, int nPtrsPerObj, const int* dataSizes)
{
	const int objId = blockIdx.x;
	const int tid = threadIdx.x;
	const int sh  = tid % 2;

	if (objId >= nObj) return;

	int nHalos = 0;
	short validHalos[7];

	// Find to which halos this object should go
	auto prop = props[objId];
	int cx = 1, cy = 1, cz = 1;

	if (prop.low.x  < -0.5*localDomainSize.x + rc) cx = 0;
	if (prop.low.y  < -0.5*localDomainSize.y + rc) cy = 0;
	if (prop.low.z  < -0.5*localDomainSize.z + rc) cz = 0;

	if (prop.high.x >  0.5*localDomainSize.x - rc) cx = 2;
	if (prop.high.y >  0.5*localDomainSize.y - rc) cy = 2;
	if (prop.high.z >  0.5*localDomainSize.z - rc) cz = 2;

//	if (tid == 0) printf("Obj %d : [%f %f %f] -- [%f %f %f]\n", objId,
//			prop.low.x, prop.low.y, prop.low.z, prop.high.x, prop.high.y, prop.high.z);

	for (int ix = min(cx, 1); ix <= max(cx, 1); ix++)
		for (int iy = min(cy, 1); iy <= max(cy, 1); iy++)
			for (int iz = min(cz, 1); iz <= max(cz, 1); iz++)
			{
				if (ix == 1 && iy == 1 && iz == 1) continue;
				const int bufId = (iz*3 + iy)*3 + ix;
				validHalos[nHalos] = bufId;
				nHalos++;
			}

	// Copy objects to each halo
	// TODO: maybe other loop order?
	__shared__ int shDstObjId;
	for (int i=0; i<nHalos; i++)
	{
		const int bufId = validHalos[i];

		const int ix = bufId % 3;
		const int iy = (bufId / 3) % 3;
		const int iz = bufId / 9;
		const float3 shift{ localDomainSize.x*(ix-1),
							localDomainSize.y*(iy-1),
							localDomainSize.z*(iz-1) };

		__syncthreads();
		if (tid == 0)
			shDstObjId = atomicAdd(bufSizes + bufId, 1);
		__syncthreads();

//		if (tid == 0)
//			if (objId == 5)
//				printf("obj  %d  to halo  %d  [%f %f %f] - [%f %f %f]  %d %d %d\n", objId, bufId,
//						prop.low.x, prop.low.y, prop.low.z, prop.high.x, prop.high.y, prop.high.z, cx, cy, cz);

		float4* dstAddr = (float4*) (dests[bufId]) + packedObjSize_byte/sizeof(float4) * shDstObjId;

		for (int pid = tid/2; pid < objSize; pid += blockDim.x/2)
		{
			const int srcId = objId * objSize + pid;
			Float3_int data(coosvels[2*srcId + sh]);

			// Remember your origin, little particle!
			if (sh == 1)
				data.s2 = objId;

			if (sh == 0)
				data.v -= shift;

			dstAddr[2*pid + sh] = data.toFloat4();
		}

		// Add extra data at the end of the object
		dstAddr += objSize*2;
		packExtraData(objId, extraData, nPtrsPerObj, dataSizes, (int32_t*)dstAddr);
	}
}


__global__ void unpackObject(const float4* from, float4* to, const int objSize, const int packedObjSize_byte, const int nObj,
		char** extraData, int nPtrsPerObj, const int* dataSizes)
{
	const int objId = blockIdx.x;
	const int tid = threadIdx.x;
	const int sh  = tid % 2;

	for (int pid = tid/2; pid < objSize; pid += blockDim.x/2)
	{
		const int srcId = objId * packedObjSize_byte/sizeof(float4) + pid*2;
		float4 data = from[srcId + sh];

		to[2*(objId*objSize + pid) + sh] = data;
	}

	unpackExtraData(objId, extraData, nPtrsPerObj, dataSizes, (int32_t*)( ((char*)from) + objId * packedObjSize_byte + objSize*sizeof(Particle) ));
}





void ObjectHaloExchanger::attach(ObjectVector* ov, float rc)
{
	objects.push_back(ov);
	rcs.push_back(rc);

	const float objPerCell = 0.1f;

	const int maxdim = std::max({ov->localDomainSize.x, ov->localDomainSize.y, ov->localDomainSize.z});

	const int sizes[3] = { (int)(4*objPerCell * maxdim*maxdim + 10),
						   (int)(4*objPerCell * maxdim + 10),
						   (int)(4*objPerCell + 10) };


	ExchangeHelper* helper = new ExchangeHelper(ov->name, ov->local()->packedObjSize_bytes, sizes);
	ov->halo()->pushStream(helper->stream);
	helpers.push_back(helper);
}


void ObjectHaloExchanger::prepareData(int id, cudaStream_t defStream)
{
	auto ov = objects[id];
	auto rc = rcs[id];
	auto helper = helpers[id];

	debug2("Preparing %s halo on the device", ov->name.c_str());

	helper->bufSizes.pushStream(defStream);
	helper->bufSizes.clearDevice();
	helper->bufSizes.popStream();

	const int nthreads = 128;
	if (ov->local()->nObjects > 0)
	{
		int       nPtrs  = ov->local()->extraDataPtrs.size();
		int totSize_byte = ov->local()->packedObjSize_bytes;

		getObjectHalos <<< ov->local()->nObjects, nthreads, 0, defStream >>> (
				(float4*)ov->local()->coosvels.devPtr(), ov->local()->comAndExtents.devPtr(),
				ov->local()->nObjects, ov->local()->objSize, ov->localDomainSize, rc,
				(int64_t*)helper->sendAddrs.devPtr(), helper->bufSizes.devPtr(),
				totSize_byte, ov->local()->extraDataPtrs.devPtr(), nPtrs, ov->local()->extraDataSizes.devPtr());
	}
}

void ObjectHaloExchanger::combineAndUploadData(int id)
{
	auto ov = objects[id];
	auto helper = helpers[id];

	ov->halo()->resize(helper->recvOffsets[27] * ov->halo()->objSize, resizeAnew);
	ov->halo()->resize(helper->recvOffsets[27] * ov->halo()->objSize, resizeAnew);

	const int nthreads = 128;
	for (int i=0; i < 27; i++)
	{
		const int nObjs = helper->recvOffsets[i+1] - helper->recvOffsets[i];
		if (nObjs > 0)
		{
			int        nPtrs = ov->local()->extraDataPtrs.size();
			int totSize_byte = ov->local()->packedObjSize_bytes;

			unpackObject<<< nObjs, nthreads, 0, defStream >>>
					((float4*)helper->recvBufs[i].devPtr(), (float4*)(ov->halo()->coosvels.devPtr() + helper->recvOffsets[i]*nObjs), ov->local()->objSize, totSize_byte, nObjs,
					 ov->halo()->extraDataPtrs.devPtr(), nPtrs, ov->halo()->extraDataSizes.devPtr());
		}
	}
}

