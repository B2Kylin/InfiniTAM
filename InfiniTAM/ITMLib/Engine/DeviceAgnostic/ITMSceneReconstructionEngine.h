// Copyright 2014 Isis Innovation Limited and the authors of InfiniTAM

#pragma once

#include "../../Utils/ITMLibDefines.h"
#include "../../Utils/ITMPixelUtils.h"
#include "ITMRepresentationAccess.h"

template<class TVoxel>
_CPU_AND_GPU_CODE_ inline float computeUpdatedVoxelDepthInfo(TVoxel &voxel, const Vector4f & pt_model, const Matrix4f & M_d, const Vector4f & projParams_d,
	float mu, int maxW, const float *depth, const Vector2i & imgSize)
{
	Vector4f pt_camera; Vector2f pt_image;
	float depth_measure, eta, oldF, newF;
	int oldW, newW;

	// project point into image
	pt_camera = M_d * pt_model;
	if (pt_camera.z <= 0) return -1;

	pt_image.x = projParams_d.x * pt_camera.x / pt_camera.z + projParams_d.z;
	pt_image.y = projParams_d.y * pt_camera.y / pt_camera.z + projParams_d.w;
	if ((pt_image.x < 1) || (pt_image.x > imgSize.x - 2) || (pt_image.y < 1) || (pt_image.y > imgSize.y - 2)) return - 1;

	// get measured depth from image
	depth_measure = depth[(int)(pt_image.x + 0.5f) + (int)(pt_image.y + 0.5f) * imgSize.x];
	if (depth_measure <= 0.0) return -1;

	// check whether voxel needs updating
	eta = depth_measure - pt_camera.z;
	if (eta < -mu) return eta;

	// compute updated SDF value and reliability
	oldF = TVoxel::SDF_valueToFloat(voxel.sdf); oldW = voxel.w_depth;
	newF = MIN(1.0f, eta / mu);
	newW = 1;

	newF = oldW * oldF + newW * newF;
	newW = oldW + newW;
	newF /= newW;
	newW = MIN(newW, maxW);

	// write back
	voxel.sdf = TVoxel::SDF_floatToValue(newF);
	voxel.w_depth = newW;

	return eta;
}


template<class TVoxel>
_CPU_AND_GPU_CODE_ inline void computeUpdatedVoxelColorInfo(TVoxel &voxel, const Vector4f & pt_model, const Matrix4f & M_rgb, const Vector4f & projParams_rgb,
	float mu, uchar maxW, float eta, const Vector4u *rgb, const Vector2i & imgSize)
{
	Vector4f pt_camera; Vector2f pt_image;
	Vector3f rgb_measure, oldC, newC; Vector3u buffV3u;
	float newW, oldW;

	buffV3u = voxel.clr;
	oldW = (float)voxel.w_color;

	oldC = buffV3u.toFloat() / 255.0f;
	newC = oldC;

	pt_camera = M_rgb * pt_model;

	pt_image.x = projParams_rgb.x * pt_camera.x / pt_camera.z + projParams_rgb.z;
	pt_image.y = projParams_rgb.y * pt_camera.y / pt_camera.z + projParams_rgb.w;

	if ((pt_image.x < 1) || (pt_image.x > imgSize.x - 2) || (pt_image.y < 1) || (pt_image.y > imgSize.y - 2)) return;

	rgb_measure = interpolateBilinear(rgb, pt_image, imgSize).toVector3() / 255.0f;
	//rgb_measure = rgb[(int)(pt_image.x + 0.5f) + (int)(pt_image.y + 0.5f) * imgSize.x].toVector3().toFloat() / 255.0f;
	newW = 1;

	newC = oldC * oldW + rgb_measure * newW;
	newW = oldW + newW;
	newC /= newW;
	newW = MIN(newW, maxW);

	buffV3u = (newC * 255.0f).toUChar();
	
	voxel.clr = buffV3u;
	voxel.w_color = (uchar)newW;
}

template<bool hasColor,class TVoxel> struct ComputeUpdatedVoxelInfo;

template<class TVoxel>
struct ComputeUpdatedVoxelInfo<false,TVoxel> {
	_CPU_AND_GPU_CODE_ static void compute(TVoxel & voxel, const Vector4f & pt_model,
		const Matrix4f & M_d, const Vector4f & projParams_d,
		const Matrix4f & M_rgb, const Vector4f & projParams_rgb,
		float mu, int maxW,
		const float *depth, const Vector2i & imgSize_d,
		const Vector4u *rgb, const Vector2i & imgSize_rgb)
	{
		computeUpdatedVoxelDepthInfo(voxel, pt_model, M_d, projParams_d, mu, maxW, depth, imgSize_d);
	}
};

template<class TVoxel>
struct ComputeUpdatedVoxelInfo<true,TVoxel> {
	_CPU_AND_GPU_CODE_ static void compute(TVoxel & voxel, const Vector4f & pt_model,
		const Matrix4f & M_d, const Vector4f & projParams_d,
		const Matrix4f & M_rgb, const Vector4f & projParams_rgb,
		float mu, int maxW,
		const float *depth, const Vector2i & imgSize_d,
		const Vector4u *rgb, const Vector2i & imgSize_rgb)
	{
		float eta = computeUpdatedVoxelDepthInfo(voxel, pt_model, M_d, projParams_d, mu, maxW, depth, imgSize_d);
		if ((eta > mu) || (fabsf(eta / mu) > 0.25f)) return;
		computeUpdatedVoxelColorInfo(voxel, pt_model, M_rgb, projParams_rgb, mu, maxW, eta, rgb, imgSize_rgb);
	}
};

_CPU_AND_GPU_CODE_ inline void buildHashAllocAndVisibleTypePP(uchar *entriesAllocType, uchar *entriesVisibleType, int x, int y, Vector3s *blockCoords,
	const float *depth, Matrix4f invM_d, Vector4f projParams_d, float mu, Vector2i imgSize, float oneOverVoxelSize, ITMHashEntry *hashTable,
	float viewFrustum_min, float viewFrustum_max)
{
	float depth_measure; unsigned int hashIdx; int noSteps, lastFreeInBucketIdx;
	Vector3f pt_camera_f, pt_block_e, pt_block, direction; Vector3s pt_block_a;

	depth_measure = depth[x + y * imgSize.x];
	if (depth_measure <= 0 || (depth_measure - mu) < 0 || (depth_measure - mu) < viewFrustum_min || (depth_measure + mu) > viewFrustum_max) return;

	pt_camera_f.z = depth_measure;
	pt_camera_f.x = pt_camera_f.z * ((float(x) - projParams_d.z) * projParams_d.x);
	pt_camera_f.y = pt_camera_f.z * ((float(y) - projParams_d.w) * projParams_d.y);

	float norm = sqrtf(pt_camera_f.x * pt_camera_f.x + pt_camera_f.y * pt_camera_f.y + pt_camera_f.z * pt_camera_f.z);

	pt_block   = invM_d * (pt_camera_f * (1.0f - mu/norm)) * oneOverVoxelSize;
	pt_block_e = invM_d * (pt_camera_f * (1.0f + mu/norm)) * oneOverVoxelSize;

	direction = pt_block_e - pt_block;
	norm = sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	noSteps = (int)ceilf(2.0f*norm);

	direction /= (noSteps-1);

	//add neighbouring blocks
	for (int i = 0; i < noSteps; i++)
	{
		pt_block_a = pt_block.toShortFloor();

		//compute index in hash table
		hashIdx = hashIndex(pt_block_a, SDF_HASH_MASK) * SDF_ENTRY_NUM_PER_BUCKET;

		//check if hash table contains entry
		lastFreeInBucketIdx = -1; bool foundValue = false; int offsetExcess = 0;
		for (int inBucketIdx = 0; inBucketIdx < SDF_ENTRY_NUM_PER_BUCKET; inBucketIdx++)
		{
			const ITMHashEntry &hashEntry = hashTable[hashIdx + inBucketIdx];
			offsetExcess = hashEntry.offset - 1;

			if (hashEntry.pos == pt_block_a && hashEntry.ptr >= -1)
			{
				if (hashEntry.ptr == -1) entriesVisibleType[hashIdx + inBucketIdx] = 2;
				else entriesVisibleType[hashIdx + inBucketIdx] = 1;

				foundValue = true;
				break;
			}

			if (lastFreeInBucketIdx == -1 && hashEntry.ptr < -2) lastFreeInBucketIdx = inBucketIdx;
		}

		if (!foundValue)
		{
			int hashIdx_toModify; //will contain parent index for excess list or normal hash+bucket index for ordered list

			if (lastFreeInBucketIdx >= 0) //not found and have room in the ordered part of the list (-> no excess list to search)
			{
				hashIdx_toModify = hashIdx + lastFreeInBucketIdx;

				entriesAllocType[hashIdx_toModify] = 1; //needs allocation and has room in ordered list
				entriesVisibleType[hashIdx_toModify] = 1; //new entry is visible

				blockCoords[hashIdx_toModify] = pt_block_a; //per-image hash collisions are ignored (will be picked up next frame)
			}
			else //might be in the excess list
			{
				hashIdx_toModify = hashIdx + SDF_ENTRY_NUM_PER_BUCKET - 1;

				int noOrderedEntries = SDF_BUCKET_NUM * SDF_ENTRY_NUM_PER_BUCKET;

				while (offsetExcess >= 0)
				{
					const ITMHashEntry &hashEntry = hashTable[noOrderedEntries + offsetExcess];

					if (hashEntry.pos == pt_block_a && hashEntry.ptr >= -1)
					{
						if (hashEntry.ptr == -1) entriesVisibleType[noOrderedEntries + offsetExcess] = 2;
						else entriesVisibleType[noOrderedEntries + offsetExcess] = 1;

						foundValue = true;
						break;
					}

					hashIdx_toModify = noOrderedEntries + offsetExcess;
					offsetExcess = hashEntry.offset - 1;
				}

				if (!foundValue) //still not found -> must add into excess list
				{
					entriesAllocType[hashIdx_toModify] = 2; //needs allocation in the excess list
					blockCoords[hashIdx_toModify] = pt_block_a; //per-image hash collisions are ignored 
				}
			}
		}

		pt_block += direction;
	}
}

_CPU_AND_GPU_CODE_ inline void buildHHashAllocAndVisibleTypePP(uchar *globalEntriesAllocType, uchar *globalEntriesVisibleType, int x, int y, Vector3s *globalBlockCoords,
	const float *depth, Matrix4f invM_d, Vector4f projParams_d, float mu, Vector2i imgSize, float oneOverSmallestVoxelSize, ITMHashEntry *globalHashTable,
	float viewFrustum_min, float viewFrustum_max)
{
	float depth_measure; unsigned int hashIdx; int noSteps, lastFreeInBucketIdx;
	Vector3f pt_camera_f, pt_voxel, pt_voxel_e, direction; Vector3s pt_block;

	depth_measure = depth[x + y * imgSize.x];
	if (depth_measure <= 0 || (depth_measure - mu) < 0 || (depth_measure - mu) < viewFrustum_min || (depth_measure + mu) > viewFrustum_max) return;

	//find block coords for start ray
	pt_camera_f.z = depth_measure;
	pt_camera_f.x = pt_camera_f.z * ((float(x) - projParams_d.z) * projParams_d.x);
	pt_camera_f.y = pt_camera_f.z * ((float(y) - projParams_d.w) * projParams_d.y);

	float norm = sqrtf(pt_camera_f.x * pt_camera_f.x + pt_camera_f.y * pt_camera_f.y + pt_camera_f.z * pt_camera_f.z);

	pt_voxel   = invM_d * (pt_camera_f * (1.0f - mu/norm)) * oneOverSmallestVoxelSize;
	pt_voxel_e = invM_d * (pt_camera_f * (1.0f + mu/norm)) * oneOverSmallestVoxelSize;

	direction = pt_voxel_e - pt_voxel;
	norm = sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	noSteps = (int)ceilf(2.0f * norm / SDF_BLOCK_SIZE);
	direction /= (noSteps-1);

	//add neighbouring blocks
	for (int i = 0; i < noSteps; i++)
	{
		for (int level = SDF_HASH_NO_H_LEVELS-1; level >= 0; level--) {
			int hierBlockSize = (1 << level);
			ITMHashEntry *hashTable = globalHashTable + level * ITMVoxelBlockHHash::IndexData::noTotalEntriesPerLevel;
			uchar *entriesAllocType = globalEntriesAllocType + level * ITMVoxelBlockHHash::IndexData::noTotalEntriesPerLevel;
			uchar *entriesVisibleType = globalEntriesVisibleType + level * ITMVoxelBlockHHash::IndexData::noTotalEntriesPerLevel;
			Vector3s *blockCoords = globalBlockCoords + level * ITMVoxelBlockHHash::IndexData::noTotalEntriesPerLevel;

			Vector3i tmp_block = pointToSDFBlock(pt_voxel.toIntRound(), hierBlockSize);
			pt_block.x = tmp_block.x;
			pt_block.y = tmp_block.y;
			pt_block.z = tmp_block.z;

			//compute index in hash table
			hashIdx = hashIndex(pt_block, SDF_HASH_MASK) * SDF_ENTRY_NUM_PER_BUCKET;

			//check if hash table contains entry
			bool shouldContinueDown = false;
			lastFreeInBucketIdx = -1; bool foundValue = false; int offsetExcess;
			for (int inBucketIdx = 0; inBucketIdx < SDF_ENTRY_NUM_PER_BUCKET; inBucketIdx++)
			{
				const ITMHashEntry &hashEntry = hashTable[hashIdx + inBucketIdx];
				offsetExcess = hashEntry.offset - 1;

				if (hashEntry.pos == pt_block) {
					if (hashEntry.ptr == -2) {
						// block found, but it's split
						shouldContinueDown = true;
					} else if (hashEntry.ptr == -1) {
						// block found, but swapped out
						entriesVisibleType[hashIdx + inBucketIdx] = 2;
					} else if (hashEntry.ptr >= 0) {
						// block found
						entriesVisibleType[hashIdx + inBucketIdx] = 1;
					}
					foundValue = true;
					break;
				}

				if (lastFreeInBucketIdx == -1 && hashEntry.ptr < -2) lastFreeInBucketIdx = inBucketIdx;
			}

			if (!foundValue)
			{
				int hashIdx_toModify; //will contain parent index for excess list or normal hash+bucket index for ordered list

				if (lastFreeInBucketIdx >= 0) //not found and have room in the ordered part of the list (-> no excess list to search)
				{
					hashIdx_toModify = hashIdx + lastFreeInBucketIdx;

					entriesAllocType[hashIdx_toModify] = 1; //needs allocation and has room in ordered list
					entriesVisibleType[hashIdx_toModify] = 1; //new entry is visible

					blockCoords[hashIdx_toModify] = pt_block; //per-image hash collisions are ignored (will be picked up next frame)
				}
				else //might be in the excess list
				{
					hashIdx_toModify = hashIdx + SDF_ENTRY_NUM_PER_BUCKET - 1;

					int noOrderedEntries = SDF_BUCKET_NUM * SDF_ENTRY_NUM_PER_BUCKET;

					while (offsetExcess >= 0)
					{
						const ITMHashEntry &hashEntry = hashTable[noOrderedEntries + offsetExcess];

						if (hashEntry.pos == pt_block) {
							if (hashEntry.ptr == -2) {
								// block found, but it's split
								shouldContinueDown = true;
							} else if (hashEntry.ptr == -1) {
								// block found, but swapped out
								entriesVisibleType[noOrderedEntries + offsetExcess] = 2;
							} else if (hashEntry.ptr >= 0) {
								// block found
								entriesVisibleType[noOrderedEntries + offsetExcess] = 1;
							}
							foundValue = true;
							break;
						}

						hashIdx_toModify = noOrderedEntries + offsetExcess;
						offsetExcess = hashEntry.offset - 1;
					}

					if (!foundValue) //still not found -> must add into excess list
					{
						entriesAllocType[hashIdx_toModify] = 2; //needs allocation in the excess list
						blockCoords[hashIdx_toModify] = pt_block; //per-image hash collisions are ignored 
					}
				}
			}

			if (!shouldContinueDown) break;
		}

		pt_voxel += direction;
	}
}
