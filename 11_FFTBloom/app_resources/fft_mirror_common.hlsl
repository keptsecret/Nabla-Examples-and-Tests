#include "fft_common.hlsl"

struct PreloadedAccessorMirrorTradeBase : PreloadedAccessorBase
{
	// TODO: Explain this a bit better in the readme
	// Some operations require a thread to have both elements `DFT[T]` and `DFT[-T]` (where the latter is the mirror around Nyquist, or the "negative frequency" of T). For example,
	// this is needed when unpacking two different FFTs of real sequences `x, y` from the FFT of a single packed sequence `z = x + iy`. 
	// Suppose we are on a particular thread, we have an index `globalElementIdx` for a particular element (this index is an index into the Nabla-ordered array, not the proper DFT-ordered one)
	// If `NablaFFT[globalElementIdx] = DFT[T]` for some T, first we must find the `otherElementIdx` such that `NablaFFT[otherElementIdx] = DFT[-T]`. This way we know which is the other
	// element to get to do something such as unpacking. This is achieved via `FFTIndexingUtils::getNablaMirrorIndex`. 
	// Once we have the otherElementIdx, we must know which thread holds that other element. Thankfully, this is just the lower bits of the index. 
	// Now, either for our thread or for the other thread, we need to send an element to the other thread and receive one from them (there is a proof somewhere, might add it to
	// readme, that at each step a pair of threads always trades two values between them, there's no long cycles of trades or anything like that. Known exception is the 0th
	// element of the first two threads: threads indexed 0 and 1 actually trade with themselves).
	// The question is which element. This part is still unproven, but it turns out that at each step the local element index `elementToTradeLocalIdx` of the element
	// we need to send is a function of just `localElementIndex`, and it turns out to be the higher bits of `otherElementIdx`.
	// Leaving this as comment for myself: The proof should be easy. It stands to reason that the chain of functions in `getNablaMirrorIndex` yield the same higher bits
	// for X | Y for any Y as long as you fix X, probably because of how the `mirror` function works.
	template<typename sharedmem_adaptor_t>
	complex_t<scalar_t> getDFTMirror(uint32_t localElementIndex, sharedmem_adaptor_t adaptorForSharedMemory)
	{
		FFTIndexingUtils::NablaMirrorTradeInfo info = FFTIndexingUtils::getNablaMirrorTradeInfo(localElementIndex);
		uint32_t elementToTradeLocalIdx = info.mirrorLocalIndex;
		complex_t<scalar_t> toTrade = preloaded[elementToTradeLocalIdx];
		vector<scalar_t, 2> toTradeVector = { toTrade.real(), toTrade.imag() };
		uint32_t otherThreadID = info.otherThreadID;
		workgroup::Shuffle<sharedmem_adaptor_t, vector<scalar_t, 2> >::__call(toTradeVector, otherThreadID, adaptorForSharedMemory);
		toTrade.real(toTradeVector.x);
		toTrade.imag(toTradeVector.y);
		return toTrade;
	}
};

struct MultiChannelPreloadedAccessorMirrorTradeBase : MultiChannelPreloadedAccessorBase
{
	template<typename sharedmem_adaptor_t>
	complex_t<scalar_t> getDFTMirror(uint32_t localElementIndex, uint16_t channel, sharedmem_adaptor_t adaptorForSharedMemory)
	{
		FFTIndexingUtils::NablaMirrorTradeInfo info = FFTIndexingUtils::getNablaMirrorTradeInfo(localElementIndex);
		uint32_t elementToTradeLocalIdx = info.mirrorLocalIndex;		
		complex_t<scalar_t> toTrade = preloaded[channel][elementToTradeLocalIdx];
		vector<scalar_t, 2> toTradeVector = { toTrade.real(), toTrade.imag() };
		uint32_t otherThreadID = info.otherThreadID;
		workgroup::Shuffle<sharedmem_adaptor_t, vector<scalar_t, 2> >::__call(toTradeVector, otherThreadID, adaptorForSharedMemory);
		toTrade.real(toTradeVector.x);
		toTrade.imag(toTradeVector.y);
		return toTrade;
	}
};