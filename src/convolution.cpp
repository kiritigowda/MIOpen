#include <miopen/convolution.hpp>
#include <miopen/errors.hpp>

namespace miopen {

ConvolutionDescriptor::ConvolutionDescriptor(int p_pad_h, int p_pad_w, int p_u, int p_v, int p_dilation_h, int p_dilation_w) 
: mode(miopenConvolution), pad_h(p_pad_h), pad_w(p_pad_w), u(p_u), v(p_v), dilation_h(p_dilation_h), dilation_w(p_dilation_w) 
{
	if(pad_h < 0 || pad_w < 0 || u < 0 || v < 0 || dilation_h < 0 || dilation_w < 0) {
		MIOPEN_THROW(miopenStatusBadParm, "Parameters to filter cannot be negative");
	}
}

ConvolutionDescriptor::ConvolutionDescriptor(miopenConvolutionMode_t p_mode, int p_pad_h, int p_pad_w, int p_u, int p_v, int p_dilation_h, int p_dilation_w)
: mode(p_mode), pad_h(p_pad_h), pad_w(p_pad_w), u(p_u), v(p_v), dilation_h(p_dilation_h), dilation_w(p_dilation_w)
{
	if(pad_h < 0 || pad_w < 0 || u < 0 || v < 0 || dilation_h < 0 || dilation_w < 0) {
		MIOPEN_THROW(miopenStatusBadParm, "Parameters to filter cannot be negative");
	}
}

std::tuple<int, int, int, int> ConvolutionDescriptor::GetForwardOutputDim(
	const TensorDescriptor& inputTensorDesc, 
	const TensorDescriptor& filterDesc) 
const
{
	assert(inputTensorDesc.GetLengths().size() == 4);
	assert(filterDesc.GetLengths().size() == 4);

	if (inputTensorDesc.GetType() != filterDesc.GetType()) {
		MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
	}

	int input_n;
	int input_c;
	int input_h;
	int input_w;

	std::tie(input_n, input_c, input_h, input_w) = miopen::tie4(inputTensorDesc.GetLengths());

	int filter_k;
	int filter_c;
	int filter_h;
	int filter_w;
	
	std::tie(filter_k, filter_c, filter_h, filter_w) = miopen::tie4(filterDesc.GetLengths());

if (mode == miopenConvolution) {
	if(input_c != filter_c) {
		MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
	}
}
else if (mode == miopenTranspose) {
	if (input_c != filter_k) {
		MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
	}
}

	int output_c;
	int output_h;
	int output_w;
	if (mode == miopenTranspose) {
		output_c = filter_c;
		output_h = std::max(1, u * (input_h - 1) + filter_h - 2 * pad_h);
		output_w = std::max(1, v * (input_w - 1) + filter_w - 2 * pad_w);
	}
	else {
		output_c = filter_k;
		output_h = std::max(1, (input_h - filter_h + 2 * pad_h) / u + 1);
		output_w = std::max(1, (input_w - filter_w + 2 * pad_w) / v + 1);
	}

	return std::make_tuple(
		input_n,
		output_c,
		output_h,
		output_w
	);

//	return std::make_tuple(
//		input_n, 
//		filter_k, 
//		std::max(1, (input_h - filter_h + 2*pad_h) / u + 1), 
//		std::max(1, (input_w - filter_w + 2*pad_w) / v + 1)
//	);
}

size_t ConvolutionDescriptor::ForwardGetWorkSpaceSizeGEMM(
        Handle&                 handle,
		const TensorDescriptor& wDesc,
		const TensorDescriptor& yDesc) const
{
	int out_h, out_w;
	std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tie4(yDesc.GetLengths());

	int wei_c, wei_h, wei_w;
	std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tie4(wDesc.GetLengths());
	
	size_t workspace_size = wei_c*wei_h*wei_w * out_h*out_w * sizeof(yDesc.GetType());

    // gfx803 devices have 4gb-6gb memory 
    if(workspace_size > (1 << 30) && handle.GetDeviceName() == "gfx803")
        workspace_size = 0;

	return (wei_h == 1 && wei_w == 1 && v == 1 && u == 1) ? 0 : workspace_size;
}


size_t ConvolutionDescriptor::ForwardGetWorkSpaceSize(
        Handle&                 handle,
		const TensorDescriptor& wDesc,
		const TensorDescriptor& xDesc,
		const TensorDescriptor& yDesc) const
{
	size_t workspace_size_gemm = ForwardGetWorkSpaceSizeGEMM(handle, wDesc, yDesc);
	size_t workspace_size_fft = ForwardGetWorkSpaceSizeFFT(wDesc, xDesc, yDesc);
	if (mode == miopenTranspose)
	    return BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, xDesc);

	return (workspace_size_fft > workspace_size_gemm ? workspace_size_fft : workspace_size_gemm);
}


size_t ConvolutionDescriptor::BackwardDataGetWorkSpaceSize(
        Handle&                 handle,
		const TensorDescriptor& wDesc,
		const TensorDescriptor& dyDesc,
		const TensorDescriptor& dxDesc) const
{
	(void)handle; // suppress warning
	size_t workspace_size_gemm = BackwardDataGetWorkSpaceSizeGEMM(handle, wDesc, dyDesc);
	size_t workspace_size_fft  = BackwardGetWorkSpaceSizeFFT (wDesc, dyDesc, dxDesc);
	if (mode == miopenTranspose)
		return ForwardGetWorkSpaceSizeGEMM(handle, wDesc, dxDesc);

	return (workspace_size_fft > workspace_size_gemm ? workspace_size_fft : workspace_size_gemm);
}

// weights_n = output_c
// weights_c = input_c
// weights_h = 2*pad_h + input_h - u*(output_h - 1)
// weights_w = 2*pad_w + input_w - v*(output_w - 1)
std::tuple<int, int, int, int> ConvolutionDescriptor::GetBackwardsWeightsDim(
	const TensorDescriptor& inputTensorDesc, 
	const TensorDescriptor& outputTensorDesc) 
const
{
	assert(inputTensorDesc.GetLengths().size() == 4);
	assert(outputTensorDesc.GetLengths().size() == 4);

	if (inputTensorDesc.GetType() != outputTensorDesc.GetType()) {
		MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
	}

	int input_n;
	int input_c;
	int input_h;
	int input_w;

	std::tie(input_n, input_c, input_h, input_w) = miopen::tie4(inputTensorDesc.GetLengths());

	int output_n;
	int output_c;
	int output_h;
	int output_w;

	std::tie(output_n, output_c, output_h, output_w) = miopen::tie4(outputTensorDesc.GetLengths());

	// if(input_c != filter_c) {
	// 	MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
	// }

	return std::make_tuple(
		output_c, 
		input_c, 
		2*pad_h + input_h - u*(output_h - 1), 
		2*pad_w + input_w - v*(output_w - 1)
	);
}

std::tuple<int, int, int, int> ConvolutionDescriptor::GetBackwardOutputDim(
	const TensorDescriptor& outputTensorDesc, 
	const TensorDescriptor& filterDesc) 
const
{
	assert(outputTensorDesc.GetLengths().size() == 4);
	assert(filterDesc.GetLengths().size() == 4);

	if (outputTensorDesc.GetType() != filterDesc.GetType()) {
		MIOPEN_THROW(miopenStatusBadParm, "Types do not match for the filter");
	}

	int output_n;
	int output_c;
	int output_h;
	int output_w;

	std::tie(output_n, output_c, output_h, output_w) = miopen::tie4(outputTensorDesc.GetLengths());

	int filter_k;
	int filter_c;
	int filter_h;
	int filter_w;
	
	std::tie(filter_k, filter_c, filter_h, filter_w) = miopen::tie4(filterDesc.GetLengths());

	if(output_c != filter_k) {
		MIOPEN_THROW(miopenStatusBadParm, "Channels do not match for the filter");
	}

	return std::make_tuple(
		output_n, 
		filter_c, 
		u * (output_h - 1) - 2*pad_h + filter_h, 
		v * (output_w - 1) - 2*pad_w + filter_w
	);
}

TensorDescriptor ConvolutionDescriptor::GetForwardOutputTensor(
	const TensorDescriptor& inputTensorDesc, 
	const TensorDescriptor& filterDesc) const
{
	auto dims = this->GetForwardOutputDim(inputTensorDesc, filterDesc);
	return TensorDescriptor(inputTensorDesc.GetType(), {
		std::get<0>(dims),
		std::get<1>(dims),
		std::get<2>(dims),
		std::get<3>(dims)});
}

TensorDescriptor ConvolutionDescriptor::GetBackwardOutputTensor(
	const TensorDescriptor& outputTensorDesc, 
	const TensorDescriptor& filterDesc) const
{
	auto dims = this->GetBackwardOutputDim(outputTensorDesc, filterDesc);
	return TensorDescriptor(outputTensorDesc.GetType(), {
		std::get<0>(dims),
		std::get<1>(dims),
		std::get<2>(dims),
		std::get<3>(dims)});
}

TensorDescriptor ConvolutionDescriptor::GetBackwardWeightsTensor(
	const TensorDescriptor& inputTensorDesc, 
	const TensorDescriptor& outputTensorDesc) const
{
	auto dims = this->GetBackwardsWeightsDim(inputTensorDesc, outputTensorDesc);
	return TensorDescriptor(outputTensorDesc.GetType(), {
		std::get<0>(dims),
		std::get<1>(dims),
		std::get<2>(dims),
		std::get<3>(dims)});
}

size_t ConvolutionDescriptor::BackwardDataGetWorkSpaceSizeGEMM(
	Handle&                     handle,
	const TensorDescriptor&     wDesc,
	const TensorDescriptor&		dyDesc) const
{
	int out_h, out_w;
	std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tie4(dyDesc.GetLengths());
	int wei_c, wei_h, wei_w;
	std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tie4(wDesc.GetLengths());
	size_t gemm_size = wei_c*wei_h*wei_w * out_h*out_w * sizeof(dyDesc.GetType());

	// gfx803 devices have limited memory
	// TODO: be graceful, need to ensure we can execute a config on the GPU
	// what if both the algos require > (1 << 30) memory
	if (handle.GetDeviceName() == "gfx803" && gemm_size > (1 << 30))
		gemm_size = 0;

	return (wei_h == 1 && wei_w == 1 && v == 1 && u == 1) ? 0 : gemm_size;

}

size_t ConvolutionDescriptor::BackwardWeightsGetWorkSpaceSizeGEMM(
    Handle&                     handle,
    const TensorDescriptor&     dyDesc,
	const TensorDescriptor&		dwDesc) const
{
    int out_h, out_w;
    std::tie(std::ignore, std::ignore, out_h, out_w) = miopen::tie4(dyDesc.GetLengths());
    int wei_c, wei_h, wei_w;
    std::tie(std::ignore, wei_c, wei_h, wei_w) = miopen::tie4(dwDesc.GetLengths());
    size_t gemm_size = wei_c*wei_h*wei_w * out_h*out_w * sizeof(dyDesc.GetType()); 

    // gfx803 devices have limited memory
    // TODO: be graceful, need to ensure we can execute a config on the GPU
    // what if both the algos require > (1 << 30) memory
    if(handle.GetDeviceName() == "gfx803" && gemm_size > (1 << 30))
        gemm_size = 0;

    return gemm_size;
}

size_t ConvolutionDescriptor::BackwardWeightsGetWorkSpaceSizeDirect(
    Handle&                     handle,
    const TensorDescriptor&     dyDesc,
    const TensorDescriptor&     xDesc,
    const TensorDescriptor&     dwDesc) const
{
    mlo_construct_BwdWrW2D construct_params(0); // backward with regards to weights
    construct_params.doSearch(false);
    construct_params.setStream(&handle);
    construct_params.setOutputDescFromMLDesc(dyDesc);
    construct_params.setInputDescFromMLDesc(xDesc);
    construct_params.setWeightDescFromMLDesc(dwDesc);
    construct_params.setConvDescr(pad_h, pad_w, u, v, dilation_h, dilation_w);
    construct_params.mloConstruct();

    return construct_params.getWorkSpaceSzBytes();
}

size_t ConvolutionDescriptor::ConvolutionBackwardWeightsGetWorkSpaceSize(
    Handle&                     handle,
    const TensorDescriptor&      dyDesc,
	const TensorDescriptor&		 xDesc,
	const TensorDescriptor&		 dwDesc) const
{
	if (mode == miopenTranspose)
		return BackwardWeightsGetWorkSpaceSizeGEMM(handle, xDesc, dwDesc);

    return std::max(
            BackwardWeightsGetWorkSpaceSizeDirect(handle, dyDesc, xDesc, dwDesc),
            BackwardWeightsGetWorkSpaceSizeGEMM(handle, dyDesc, dwDesc)
        );
}
std::ostream& operator<< (std::ostream& stream, const ConvolutionDescriptor& c)
{
	stream << c.pad_h << ", ";
	stream << c.pad_w << ", ";
	stream << c.u << ", ";
	stream << c.v << ", ";
	stream << c.dilation_h << ", ";
	stream << c.dilation_w << ", ";
	return stream;
}

} // namespace miopen
