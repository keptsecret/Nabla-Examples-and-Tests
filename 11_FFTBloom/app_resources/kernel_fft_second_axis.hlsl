#include "fft_mirror_common.hlsl"

[[vk::binding(2, 0)]] RWTexture2DArray<float32_t2> kernelChannels;

// ------------------------------------------ SECOND AXIS FFT -------------------------------------------------------------
// This time each Workgroup will compute the FFT along a horizontal line (fixed y for the whole Workgroup). We get the y coordinate for the row a workgroup is working on via `gl_WorkGroupID().x`.
// After first axis FFT, for every two columns we have saved one whole column with the packed FFT of both columns. Since they're real signals, the FFT of each is conjugate-mirrored
// (meaning we only need to retrieve the lower half of the DFT after unpacking, since the result of the FFT for any row after Nyquist can be obtained as a conjugate-inverse: see wikipedia
// for more info on how `DFT(conj(Z))` relates to `DFT(Z)`). 
// We have thus launched FFTParameters::TotalSize / 2 workgroups, as there are exactly that amount of rows to be computed. 
// Since Z and N are real signals (where `Z` is the actual 0th row and `N` is the Nyquist row, as in the one with index FFTParameters::TotalSize / 2) we can pack those together again for an FFT.  
// We unpack values on load 

struct PreloadedSecondAxisAccessor : MultiChannelPreloadedAccessorMirrorTradeBase
{
	NBL_CONSTEXPR_STATIC_INLINE uint32_t PreviousWorkgroupSize = uint32_t(ConstevalParameters::PreviousWorkgroupSize);

	// ---------------------------------------------------- Utils ---------------------------------------------------------
	uint32_t colMajorOffset(uint32_t x, uint32_t y)
	{
		return x * TotalSize | y;
	}

	uint32_t rowMajorOffset(uint32_t x, uint32_t y)
	{
		return y * TotalSize | x;
	}

	uint64_t getChannelStartOffsetBytes(uint16_t channel)
	{
		return uint64_t(channel) * TotalSize * TotalSize / 2 * sizeof(complex_t<scalar_t>);
	}

	// ---------------------------------------------------- End Utils ---------------------------------------------------------

	// The lower half of the DFT is retrieved (in bit-reversed order as an N/2 bit number, N being the length of the whole DFT) as the even elements of the Nabla-ordered FFT.
	// That is, for the threads in the previous pass you take all `preloaded[0]` elements in thread-ascending order (so preloaded[0] for the 0th thread, then 1st thread etc).
	// Then you do the same for the next even index of `preloaded` (`prealoded[2]`, `preloaded[4]`, etc).
	// Every two consecutive threads here have to get the value for the row given by `gl_WorkGroupID().x`, for two consecutive columns which were stored as one column holding the packed FFT.
	// Except for the special case `gl_WorkGroupID().x = 0`, to unpack the values for the current two columns at `y = NablaFFT[gl_WorkGroupID().x] = DFT[T]` we need the value at 
	// `NablaFFT[getDFTMirrorIndex(gl_WorkGroupID().x)] = DFT[-T]`. To achieve this, we make the thread with an even ID load the former element and the one with an odd ID load the latter.
	// Then, we do a subgroup shuffle (`xor`ing with 1) so each even thread shares its value with their corresponding odd thread. Then they perform a computation to retrieve the value
	// corresponding to the column they correspond to. 
	// The `gl_WorkGroupID().x = 0` case is special because instead of getting the mirror we need to get both zero and nyquist frequencies for the columns, which doesn't happen just by mirror
	// indexing. 
	void preload()
	{
		// This one shows up a lot so we give it a name
		const bool oddThread = glsl::gl_SubgroupInvocationID() & 1u;

		if (glsl::gl_WorkGroupID().x)
		{
			for (uint16_t channel = 0; channel < Channels; channel++)
			{
				const uint64_t channelStartOffsetBytes = getChannelStartOffsetBytes(channel);
				// Set LegacyBdaAccessor for reading
				const LegacyBdaAccessor<complex_t<scalar_t> > colMajorAccessor = LegacyBdaAccessor<complex_t<scalar_t> >::create(pushConstants.colMajorBufferAddress + channelStartOffsetBytes);

				for (uint32_t elementIndex = 0; elementIndex < ElementsPerInvocation; elementIndex++)
				{
					// Since every two consecutive columns are stored as one packed column, we divide the index by 2 to get the index of that packed column
					const uint32_t index = (WorkgroupSize * elementIndex | workgroup::SubgroupContiguousIndex()) / 2;
					// Even thread must index a y corresponding to an even element of the previous FFT pass, and the odd thread must index its DFT Mirror
					// The math here essentially ensues we enumerate all even elements in order: we alternate `PreviousWorkgroupSize` even elements (all `preloaded[0]` elements of
					// the previous pass' threads), then `PreviousWorkgroupSize` odd elements (`preloaded[1]`) and so on
					const uint32_t evenRow = glsl::gl_WorkGroupID().x + ((glsl::gl_WorkGroupID().x / PreviousWorkgroupSize) * PreviousWorkgroupSize);
					uint32_t y = oddThread ? FFTIndexingUtils::getNablaMirrorIndex(evenRow) : evenRow;
					const complex_t<scalar_t> loOrHi = colMajorAccessor.get(colMajorOffset(index, y));
					// Make it a vector so it can be subgroup-shuffled
					const vector <scalar_t, 2> loOrHiVector = vector <scalar_t, 2>(loOrHi.real(), loOrHi.imag());
					const vector <scalar_t, 2> otherThreadloOrHiVector = glsl::subgroupShuffleXor< vector <scalar_t, 2> >(loOrHiVector, 1u);
					const complex_t<scalar_t> otherThreadLoOrHi = { otherThreadloOrHiVector.x, otherThreadloOrHiVector.y };
					complex_t<scalar_t> lo = ternaryOperator(oddThread, otherThreadLoOrHi, loOrHi);
					complex_t<scalar_t> hi = ternaryOperator(oddThread, loOrHi, otherThreadLoOrHi);
					fft::unpack<scalar_t>(lo, hi);
					preloaded[channel][elementIndex] = ternaryOperator(oddThread, hi, lo);
				}
			}
		}
		// Special case where we retrieve 0 and Nyquist
		else
		{
			for (uint16_t channel = 0; channel < Channels; channel++)
			{
				const uint64_t channelStartOffsetBytes = getChannelStartOffsetBytes(channel);
				// Set LegacyBdaAccessor for reading
				const LegacyBdaAccessor<complex_t<scalar_t> > colMajorAccessor = LegacyBdaAccessor<complex_t<scalar_t> >::create(pushConstants.colMajorBufferAddress + channelStartOffsetBytes);

				for (uint32_t elementIndex = 0; elementIndex < ElementsPerInvocation; elementIndex++)
				{
					// Since every two consecutive columns are stored as one packed column, we divide the index by 2 to get the index of that packed column
					const uint32_t index = (WorkgroupSize * elementIndex | workgroup::SubgroupContiguousIndex()) / 2;

					// Even thread retrieves Zero, odd thread retrieves Nyquist. Zero is always `preloaded[0]` of the previous FFT's 0th thread, while Nyquist is always `preloaded[1]` of that same thread.
					// Therefore we know Nyquist ends up exactly at y = PreviousWorkgroupSize
					uint32_t y = oddThread ? PreviousWorkgroupSize : 0;
					const complex_t<scalar_t> loOrHi = colMajorAccessor.get(colMajorOffset(index, y));
					// Make it a vector so it can be subgroup-shuffled
					const vector <scalar_t, 2> loOrHiVector = vector <scalar_t, 2>(loOrHi.real(), loOrHi.imag());
					const vector <scalar_t, 2> otherThreadloOrHiVector = glsl::subgroupShuffleXor< vector <scalar_t, 2> >(loOrHiVector, 1u);
					const complex_t<scalar_t> otherThreadLoOrHi = { otherThreadloOrHiVector.x, otherThreadloOrHiVector.y };

					// `lo` holds `Z0 + iZ1` and `hi` holds `N0 + iN1`. We want at the end for `lo` to hold the packed `Z0 + iN0` and `hi` to hold `Z1 + iN1`
					// For the even thread (`oddThread == false`) `lo = loOrHi` and `hi = otherThreadLoOrHi`. For the odd thread the opposite is true

					// Even thread writes `lo = Z0 + iN0`
					const complex_t<scalar_t> evenThreadLo = { loOrHi.real(), otherThreadLoOrHi.real() };
					// Odd thread writes `hi = Z1 + iN1`
					const complex_t<scalar_t> oddThreadHi = { otherThreadLoOrHi.imag(), loOrHi.imag() };
					preloaded[channel][elementIndex] = ternaryOperator(oddThread, oddThreadHi, evenThreadLo);
				}
			}
		}
	}

	// Write spectra in their right positions
	template<typename sharedmem_adaptor_t>
	void unload(sharedmem_adaptor_t adaptorForSharedMemory)
	{
		NBL_CONSTEXPR_STATIC_INLINE uint16_t NumWorkgroupsLog2 = ConstevalParameters::NumWorkgroupsLog2;

		// Most rows have just have to reflect their values along the Nyquist row.
		// If you'll remember, however, the first axis FFT stored the lower half of the DFT, bit-reversed not as a `log2(FFTSize)` bit number but in the range of the lower half 
		// (so as a `log2(FFTSize / 2) bit number)`. Which means that whenever we get an element at `x' = index`, `y' = gl_WorkGroupID().x` we must get the actual coordinates of 
		// the element in the DFT with `x = F(x')` and `y = bitreverse(y')`
		if (glsl::gl_WorkGroupID().x)
		{
			for (uint16_t channel = 0; channel < Channels; channel++)
			{
				for (uint32_t localElementIndex = 0; localElementIndex < ElementsPerInvocation; localElementIndex++)
				{
					const uint32_t index = WorkgroupSize * localElementIndex | workgroup::SubgroupContiguousIndex();

					// Get actual x,y coordinates for the element we wish to write
					uint32_t x = FFTIndexingUtils::getDFTIndex(index);
					uint32_t y = fft::bitReverse<uint32_t, NumWorkgroupsLog2>(glsl::gl_WorkGroupID().x);

					vector<scalar_t, 2> toStoreVector = { preloaded[channel][localElementIndex].real(), preloaded[channel][localElementIndex].imag() };
					kernelChannels[uint32_t3(x, y, channel)] = toStoreVector;

					// Store the element at the column mirrored about the Nyquist column (so x'' = mirror(x))
					// https://en.wikipedia.org/wiki/Discrete_Fourier_transform#Conjugation_in_time
					// Guess what? The above says the row index is also the mirror about Nyquist! Neat
					x = FFTIndexingUtils::getDFTMirrorIndex(x);
					y = FFTIndexingUtils::getDFTMirrorIndex(y);
					const complex_t<scalar_t> conjugated = conj(preloaded[channel][localElementIndex]);
					toStoreVector.x = conjugated.real();
					toStoreVector.y = conjugated.imag();
					kernelChannels[uint32_t3(x, y, channel)] = toStoreVector;
				}
			}
		}
		// Remember that the first row has packed `Z + iN` so it has to unpack those
		else
		{
			for (uint16_t channel = 0; channel < Channels; channel++)
			{
				// FFT[Z + iN] was stored in the Nabla order
				for (uint32_t localElementIndex = 0; localElementIndex < ElementsPerInvocation; localElementIndex++)
				{
					complex_t<scalar_t> zero = preloaded[channel][localElementIndex];
					complex_t<scalar_t> nyquist = getDFTMirror<sharedmem_adaptor_t>(localElementIndex, channel, adaptorForSharedMemory);
					fft::unpack<scalar_t>(zero, nyquist);

					const uint32_t index = WorkgroupSize * localElementIndex | workgroup::SubgroupContiguousIndex();

					// We now have zero and Nyquist frequencies at NFFT[index], so we must use `getDFTIndex(index)` to get the actual index into the DFT
					const uint32_t indexDFT = FFTIndexingUtils::getDFTIndex(index);

					// Store zeroth element
					const uint32_t2 zeroCoord = uint32_t2(indexDFT, 0);
					vector<scalar_t, 2> zeroVector = { zero.real(), zero.imag() };
					kernelChannels[uint32_t3(zeroCoord, channel)] = zeroVector;

					// Store nyquist element
					const uint32_t2 nyquistCoord = uint32_t2(indexDFT, TotalSize / 2);
					vector<scalar_t, 2> nyquistVector = { nyquist.real(), nyquist.imag() };
					kernelChannels[uint32_t3(nyquistCoord, channel)] = nyquistVector;
				}
			}
		}
	}
};

[numthreads(FFTParameters::WorkgroupSize, 1, 1)]
void main(uint32_t3 ID : SV_DispatchThreadID)
{
	SharedMemoryAccessor sharedmemAccessor;
	PreloadedSecondAxisAccessor preloadedAccessor;

	preloadedAccessor.preload();
	for (uint16_t channel = 0; channel < Channels; channel++)
	{
		preloadedAccessor.currentChannel = channel;
		workgroup::FFT<false, FFTParameters>::template __call(preloadedAccessor, sharedmemAccessor);
	}
	// Set up the memory adaptor
	using sharedmem_adaptor_t = accessor_adaptors::StructureOfArrays<SharedMemoryAccessor, uint32_t, uint32_t, 1, FFTParameters::WorkgroupSize>;
	sharedmem_adaptor_t adaptorForSharedMemory;
	adaptorForSharedMemory.accessor = sharedmemAccessor;
	preloadedAccessor.unload(adaptorForSharedMemory);
}