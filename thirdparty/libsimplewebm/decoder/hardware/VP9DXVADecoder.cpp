/*
 *    MIT License
 *
 *    Copyright (c) 2016 Błażej Szczygieł
 *       Copyright (c) 2026-present The Simplicity Group & contributors
 *       See AUTHORS.md for more information
 *
 *    Permission is hereby granted, free of charge, to any person obtaining a copy
 *    of this software and associated documentation files (the "Software"), to deal
 *    in the Software without restriction, including without limitation the rights
 *    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *    copies of the Software, and to permit persons to whom the Software is
 *    furnished to do so, subject to the following conditions:
 *
 *    The above copyright notice and this permission notice shall be included in all
 *    copies or substantial portions of the Software.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *    SOFTWARE.
 */

#include "include/decoder/hardware/VP9DXVADecoder.h"

#include <decoder/hardware/VP9FrameParser.h>

#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct VP9DXVADecoder::VP9DXVAHelper::VP9DXVAHelperImpl
{
    struct VP9RefFrame
    {
        static constexpr UINT INVALID_SURFACE_INDEX = 0xFF;

        UINT surfaceIndex = INVALID_SURFACE_INDEX;
        UINT width = 0;
        UINT height = 0;
        VP9FrameParser::COLOR_SPACE colorSpace = VP9FrameParser::COLOR_SPACE_UNKNOWN;
    };

    ComPtr<ID3D11Device> device = nullptr;
    ComPtr<ID3D11DeviceContext> context = nullptr;
    ComPtr<ID3D11VideoDevice> videoDevice = nullptr;
    ComPtr<ID3D11VideoContext> videoContext = nullptr;
    ComPtr<ID3D11VideoDecoder> videoDecoder = nullptr;
    ComPtr<ID3D11Texture2D> decodeTexture = nullptr;
    ComPtr<ID3D11Texture2D> stagingTexture = nullptr;
    std::vector<ComPtr<ID3D11VideoDecoderOutputView>> outputViews = {};

    VP9FrameParser::VP9Parser parser = VP9FrameParser::VP9Parser();
    VP9RefFrame refFrames[8] = {
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
        VP9RefFrame(),
    };

    UINT statusReportFeedbackNumber = 0;

    UINT lastFrameWidth = 0;
    UINT lastFrameHeight = 0;
    VP9FrameParser::COLOR_SPACE lastFrameColorSpace = VP9FrameParser::COLOR_SPACE_UNKNOWN;
    bool lastFrameIsVisible = false;

    UINT lastVisibleFrameOutputViewIndex = VP9RefFrame::INVALID_SURFACE_INDEX;
    UINT lastVisibleFrameWidth = 0;
    UINT lastVisibleFrameHeight = 0;
    VP9FrameParser::COLOR_SPACE lastVisibleFrameColorSpace = VP9FrameParser::COLOR_SPACE_UNKNOWN;

    bool createVideoDevice()
    {
        HRESULT hr;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context
        );
        if (FAILED(hr))
        {
            return false;
        }

        this->device = device;
        this->context = context;

        ComPtr<ID3D11VideoDevice> videoDevice;
        ComPtr<ID3D11VideoContext> videoContext;

        hr = device.As(&videoDevice);
        if (FAILED(hr))
        {
            return false;
        }
        hr = context.As(&videoContext);
        if (FAILED(hr))
        {
            return false;
        }

        this->videoDevice = videoDevice;
        this->videoContext = videoContext;

        return true;
    }

    bool isVideoDecoderProfileSupported(GUID profile, DXGI_FORMAT format)
    {
        HRESULT hr;

        bool isProfileSupported = false;
        UINT profileCount = this->videoDevice->GetVideoDecoderProfileCount();
        for (UINT profileIndex = 0; profileIndex < profileCount; ++profileIndex)
        {
            GUID currentProfile;
            hr = this->videoDevice->GetVideoDecoderProfile(
                profileIndex,
                &currentProfile
            );
            if (FAILED(hr))
            {
                return false;
            }
            if (IsEqualGUID(currentProfile, profile))
            {
                isProfileSupported = true;
                break;
            }
        }
        if (!isProfileSupported)
        {
            return false;
        }

        BOOL isFormatSupported = FALSE;
        hr = this->videoDevice->CheckVideoDecoderFormat(
            &profile,
            format,
            &isFormatSupported
        );
        if (FAILED(hr))
        {
            return false;
        }
        if (isFormatSupported != TRUE)
        {
            return false;
        }

        return true;
    }

    bool createVideoDecoder(GUID profile, DXGI_FORMAT format, UINT width, UINT height)
    {
        HRESULT hr;

        D3D11_VIDEO_DECODER_DESC desc = {};
        desc.Guid = profile;
        desc.SampleWidth = width;
        desc.SampleHeight = height;
        desc.OutputFormat = format;

        UINT configCount;
        hr = this->videoDevice->GetVideoDecoderConfigCount(
            &desc,
            &configCount
        );
        if (FAILED(hr))
        {
            return false;
        }

        bool wasConfigFound = false;
        D3D11_VIDEO_DECODER_CONFIG config = {};
        for (UINT configIndex = 0; configIndex < configCount; ++configIndex)
        {
            hr = this->videoDevice->GetVideoDecoderConfig(
                &desc,
                configIndex,
                &config
            );
            if (SUCCEEDED(hr))
            {
                wasConfigFound = true;
                break;
            }
        }
        if (!wasConfigFound)
        {
            return false;
        }

        ComPtr<ID3D11VideoDecoder> videoDecoder;
        hr = this->videoDevice->CreateVideoDecoder(
            &desc,
            &config,
            &videoDecoder
        );
        if (FAILED(hr))
        {
            return false;
        }

        this->videoDecoder = videoDecoder;

        return true;
    }

    bool createOutputViews(GUID profile, DXGI_FORMAT format, UINT width, UINT height, UINT outputViewsCount)
    {
        HRESULT hr;

        D3D11_TEXTURE2D_DESC decodeTextureDesc = {};
        decodeTextureDesc.Width = width;
        decodeTextureDesc.Height = height;
        decodeTextureDesc.MipLevels = 1; // Disable mipmaps.
        decodeTextureDesc.ArraySize = outputViewsCount;
        decodeTextureDesc.Format = format;
        decodeTextureDesc.SampleDesc.Count = 1; // Disable MSAA (multisampling).
        decodeTextureDesc.SampleDesc.Quality = 0; // Ignore MSAA quality level.
        decodeTextureDesc.Usage = D3D11_USAGE_DEFAULT;
        decodeTextureDesc.BindFlags = D3D11_BIND_DECODER;

        hr = this->device->CreateTexture2D(
            &decodeTextureDesc,
            nullptr,
            &this->decodeTexture
        );
        if (FAILED(hr))
        {
            return false;
        }

        this->outputViews.resize(outputViewsCount);
        for (UINT outputViewIndex = 0; outputViewIndex < outputViewsCount; ++outputViewIndex)
        {
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC outputViewDesc = {};
            outputViewDesc.DecodeProfile = profile;
            outputViewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
            outputViewDesc.Texture2D.ArraySlice = outputViewIndex;

            ComPtr<ID3D11VideoDecoderOutputView> outputView;

            hr = this->videoDevice->CreateVideoDecoderOutputView(
                this->decodeTexture.Get(),
                &outputViewDesc,
                outputView.GetAddressOf()
            );
            if (FAILED(hr))
            {
                return false;
            }

            this->outputViews[outputViewIndex] = outputView;
        }

        // Create a texture to read from the GPU back to the CPU.
        D3D11_TEXTURE2D_DESC stagingTextureDesc = {};
        decodeTexture->GetDesc(&stagingTextureDesc); // Copy values from decodeTexture.
        stagingTextureDesc.ArraySize = 1; // We only need to read back one of the outputViews.
        stagingTextureDesc.Usage = D3D11_USAGE_STAGING;
        stagingTextureDesc.BindFlags = 0;
        stagingTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingTextureDesc.MiscFlags = 0;
        hr = this->device->CreateTexture2D(
            &stagingTextureDesc,
            nullptr,
            &this->stagingTexture
        );
        if (FAILED(hr))
        {
            return false;
        }

        return true;
    }

    bool findUnusedOutputViewIndex(UINT &outputViewIndex)
    {
        for (UINT i = 0; i < outputViews.size(); ++i)
        {
            if (i == lastVisibleFrameOutputViewIndex)
            {
                continue;
            }

            bool isUsed = false;
            for (const VP9RefFrame &refFrame : refFrames)
            {
                if (refFrame.surfaceIndex == i)
                {
                    isUsed = true;
                    break;
                }
            }
            if (!isUsed)
            {
                outputViewIndex = i;
                return true;
            }
        }

        return false;
    }

    bool decodeFrame(const void *pictureParameters, UINT pictureParametersSize, const uint8_t *bitstream, UINT bitstreamSize, UINT outputViewIndex)
    {
        HRESULT hr;

        // DXVA2 requires bitstreams to be zero-padded to a multiple of 128 bytes.
        const UINT bitstreamPaddingSize = roundUpToNearest(bitstreamSize, 128) - bitstreamSize;

        hr = this->videoContext->DecoderBeginFrame(
            this->videoDecoder.Get(),
            this->outputViews[outputViewIndex].Get(),
            0,
            nullptr
        );
        if (FAILED(hr))
        {
            return false;
        }

        UINT pictureParametersBufferSize = 0;
        void *pictureParametersBuffer = nullptr;

        hr = this->videoContext->GetDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &pictureParametersBufferSize,
            &pictureParametersBuffer
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }
        if (pictureParametersBufferSize < pictureParametersSize)
        {
            this->videoContext->ReleaseDecoderBuffer(
                this->videoDecoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS
            );
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        memcpy(pictureParametersBuffer, pictureParameters, pictureParametersSize);

        hr = this->videoContext->ReleaseDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        DXVA_Slice_VPx_Short slice = {};
        slice.BSNALunitDataLocation = 0; // The frame is at the beginning of the bitstream buffer.
        slice.SliceBytesInBuffer = bitstreamSize + bitstreamPaddingSize;
        slice.wBadSliceChopping = 0; // Don't split the frame across multiple buffers.

        UINT bitstreamBufferSize = 0;
        void *bitstreamBuffer = nullptr;

        hr = this->videoContext->GetDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bitstreamBufferSize,
            &bitstreamBuffer
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }
        if (bitstreamBufferSize < bitstreamSize + bitstreamPaddingSize)
        {
            this->videoContext->ReleaseDecoderBuffer(
                this->videoDecoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM
            );
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        memcpy(bitstreamBuffer, bitstream, bitstreamSize);

        // Zero-out the padding bytes.
        memset(static_cast<uint8_t *>(bitstreamBuffer) + bitstreamSize, 0, bitstreamPaddingSize);

        hr = this->videoContext->ReleaseDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        UINT sliceBufferSize = 0;
        void *sliceBuffer = nullptr;

        hr = this->videoContext->GetDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &sliceBufferSize,
            &sliceBuffer
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }
        if (sliceBufferSize < sizeof(slice))
        {
            this->videoContext->ReleaseDecoderBuffer(
                this->videoDecoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL
            );
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        memcpy(sliceBuffer, &slice, sizeof(slice));

        hr = this->videoContext->ReleaseDecoderBuffer(
            this->videoDecoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        D3D11_VIDEO_DECODER_BUFFER_DESC buffers[3] = {};

        buffers[0] = {};
        buffers[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
        buffers[0].DataSize = pictureParametersSize;

        buffers[1] = {};
        buffers[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        buffers[1].DataSize = bitstreamSize + bitstreamPaddingSize;

        buffers[2] = {};
        buffers[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        buffers[2].DataSize = sizeof(slice);

        hr = this->videoContext->SubmitDecoderBuffers(
            this->videoDecoder.Get(),
            3,
            buffers
        );
        if (FAILED(hr))
        {
            this->videoContext->DecoderEndFrame(
                this->videoDecoder.Get()
            );
            return false;
        }

        hr = this->videoContext->DecoderEndFrame(
            this->videoDecoder.Get()
        );
        if (FAILED(hr))
        {
            return false;
        }

        return true;
    }

    bool stageFrame(UINT outputViewIndex, DXGI_FORMAT format, UINT width, UINT height, std::vector<uint8_t> &yPlane, std::vector<uint8_t> &uPlane, std::vector<uint8_t> &vPlane)
    {
        HRESULT hr;

        // Copy the texture from the GPU to the CPU.
        this->context->CopySubresourceRegion(
            stagingTexture.Get(),
            D3D11CalcSubresource(
                0, // Use the first mipmap level (since mipmaps are disabled).
                0,
                1 // There is only one mipmap level (since mipmaps are disabled).
            ),
            0,
            0,
            0,
            decodeTexture.Get(),
            D3D11CalcSubresource(
                0, // Use the first mipmap level (since mipmaps are disabled).
                outputViewIndex,
                1 // There is only one mipmap level (since mipmaps are disabled).
            ),
            nullptr
        );

        D3D11_MAPPED_SUBRESOURCE mappedSubresource = {};

        // Make the texture on the CPU available for use.
        hr = this->context->Map(
            stagingTexture.Get(),
            0,
            D3D11_MAP_READ,
            0,
            &mappedSubresource
        );
        if (FAILED(hr))
        {
            return false;
        }

        // Copy the data from each plane.
        if (format == DXGI_FORMAT_NV12) // YUV 4:2:0 format.
        {
            const UINT chromaWidth = (width + 1) / 2;
            const UINT chromaHeight = (height + 1) / 2;

            const uint8_t *sourceY = static_cast<const uint8_t *>(mappedSubresource.pData);
            const uint8_t *sourceUV = static_cast<const uint8_t *>(sourceY + (mappedSubresource.RowPitch * height));

            yPlane.resize(width * height);
            uPlane.resize(chromaWidth * chromaHeight);
            vPlane.resize(chromaWidth * chromaHeight);

            for (UINT rowIndex = 0; rowIndex < height; ++rowIndex)
            {
                const uint8_t *sourceYRow = sourceY + (rowIndex * mappedSubresource.RowPitch);
                uint8_t *destinationYRow = yPlane.data() + (rowIndex * width);

                memcpy(destinationYRow, sourceYRow, width);
            }

            for (UINT rowIndex = 0; rowIndex < chromaHeight; ++rowIndex)
            {
                const uint8_t *sourceUVRow = sourceUV + (rowIndex * mappedSubresource.RowPitch);
                uint8_t *destinationURow = uPlane.data() + (rowIndex * chromaWidth);
                uint8_t *destinationVRow = vPlane.data() + (rowIndex * chromaWidth);

                for (UINT column = 0; column < chromaWidth; ++column)
                {
                    destinationURow[column] = sourceUVRow[column * 2];
                    destinationVRow[column] = sourceUVRow[(column * 2) + 1];
                }
            }
        }
        else // Unsupported format.
        {
            this->context->Unmap(
                stagingTexture.Get(),
                0
            );
            return false;
        }

        this->context->Unmap(
            stagingTexture.Get(),
            0
        );

        return true;
    }

    static constexpr UINT roundUpToNearest(UINT value, UINT step)
    {
        if (step == 0)
        {
            return value;
        }
        UINT remainder = value % step;
        if (remainder == 0)
        {
            return value;
        }
        return value + step - remainder;
    }
};

VP9DXVADecoder::VP9DXVAHelper::VP9DXVAHelper() :
    impl(std::make_unique<VP9DXVAHelperImpl>())
{
}

bool VP9DXVADecoder::VP9DXVAHelper::setupVP9(unsigned int width, unsigned int height)
{
    const GUID profile = D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0; // VP9 8-bit.
    const DXGI_FORMAT format = DXGI_FORMAT_NV12; // YUV 4:2:0.
    const UINT outputViewsCount = 8 + 1; // VP9 uses 8 reference slots, and decoding the next frame requires one extra slot.

    if (!impl->createVideoDevice())
    {
        return false;
    }

    if (!impl->isVideoDecoderProfileSupported(profile, format))
    {
        return false;
    }

    if (!impl->createVideoDecoder(profile, format, width, height))
    {
        return false;
    }

    if (!impl->createOutputViews(profile, format, width, height, outputViewsCount))
    {
        return false;
    }

    return true;
}

bool VP9DXVADecoder::VP9DXVAHelper::decodeVP9(const uint8_t *bitstream, size_t bitstreamSize)
{
    VP9FrameParser::Result<std::vector<VP9FrameParser::VP9Frame>> parsePacketResult = impl->parser.parsePacket({ bitstream, bitstreamSize });
    if (parsePacketResult.error)
    {
        return false;
    }

    for (const VP9FrameParser::VP9Frame &frame : parsePacketResult.value)
    {
        if (frame.showExistingFrame)
        {
            const VP9DXVAHelperImpl::VP9RefFrame &refFrame = impl->refFrames[frame.frameToShowMapIdx];

            if (refFrame.surfaceIndex == VP9DXVAHelperImpl::VP9RefFrame::INVALID_SURFACE_INDEX)
            {
                return false;
            }

            impl->lastFrameWidth = refFrame.width;
            impl->lastFrameHeight = refFrame.height;
            impl->lastFrameColorSpace = refFrame.colorSpace;
            impl->lastFrameIsVisible = true;

            impl->lastVisibleFrameOutputViewIndex = refFrame.surfaceIndex;
            impl->lastVisibleFrameWidth = refFrame.width;
            impl->lastVisibleFrameHeight = refFrame.height;
            impl->lastVisibleFrameColorSpace = refFrame.colorSpace;

            continue;
        }

        UINT outputViewIndex;
        if (!impl->findUnusedOutputViewIndex(outputViewIndex))
        {
            return false;
        }

        // See how Chromium initializes `DXVA_PicParams_VP9`:
        // https://chromium.googlesource.com/chromium/src/+/refs/heads/main/media/gpu/windows/d3d_vp9_accelerator.cc
        DXVA_PicParams_VP9 pictureParameters = {};
        pictureParameters.CurrPic.Index7Bits = outputViewIndex;
        pictureParameters.CurrPic.AssociatedFlag = 0; // Not relevant for VP9.
        pictureParameters.profile = frame.profile;
        pictureParameters.frame_type = frame.frameType;
        pictureParameters.show_frame = frame.showFrame;
        pictureParameters.error_resilient_mode = frame.errorResilientMode;
        pictureParameters.subsampling_x = frame.subsamplingX;
        pictureParameters.subsampling_y = frame.subsamplingY;
        pictureParameters.extra_plane = 0; // VP9 with alpha does not support hardware acceleration.
        pictureParameters.refresh_frame_context = frame.refreshFrameContext;
        pictureParameters.frame_parallel_decoding_mode = frame.frameParallelDecodingMode;
        pictureParameters.intra_only = frame.intraOnly;
        pictureParameters.frame_context_idx = frame.frameContextIdx;
        pictureParameters.reset_frame_context = frame.resetFrameContext;
        pictureParameters.allow_high_precision_mv = frame.allowHighPrecisionMv;
        pictureParameters.ReservedFormatInfo2Bits = 0;
        pictureParameters.width = frame.width;
        pictureParameters.height = frame.height;
        switch (frame.colorDepth)
        {
            case VP9FrameParser::COLOR_DEPTH_8:
                pictureParameters.BitDepthMinus8Luma = 8 - 8;
                pictureParameters.BitDepthMinus8Chroma = 8 - 8;
                break;
            case VP9FrameParser::COLOR_DEPTH_10:
                pictureParameters.BitDepthMinus8Luma = 10 - 8;
                pictureParameters.BitDepthMinus8Chroma = 10 - 8;
                break;
            case VP9FrameParser::COLOR_DEPTH_12:
                pictureParameters.BitDepthMinus8Luma = 12 - 8;
                pictureParameters.BitDepthMinus8Chroma = 12 - 8;
                break;
            default:
                return false;
        }
        pictureParameters.interp_filter = frame.interpolationFilter;
        pictureParameters.Reserved8Bits = 0;
        for (UINT i = 0; i < std::size(impl->refFrames); ++i)
        {
            VP9DXVAHelperImpl::VP9RefFrame refFrame = impl->refFrames[i];

            if (refFrame.surfaceIndex == VP9DXVAHelperImpl::VP9RefFrame::INVALID_SURFACE_INDEX)
            {
                pictureParameters.ref_frame_map[i].Index7Bits = VP9DXVAHelperImpl::VP9RefFrame::INVALID_SURFACE_INDEX;
                pictureParameters.ref_frame_map[i].AssociatedFlag = 0; // Not relevant for VP9.
                pictureParameters.ref_frame_coded_width[i] = 0;
                pictureParameters.ref_frame_coded_height[i] = 0;
            }
            else
            {
                pictureParameters.ref_frame_map[i].Index7Bits = refFrame.surfaceIndex;
                pictureParameters.ref_frame_map[i].AssociatedFlag = 0; // Not relevant for VP9.
                pictureParameters.ref_frame_coded_width[i] = refFrame.width;
                pictureParameters.ref_frame_coded_height[i] = refFrame.height;
            }
        }
        for (UINT i = 0; i < std::size(pictureParameters.frame_refs); ++i)
        {
            pictureParameters.frame_refs[i] = pictureParameters.ref_frame_map[frame.refFrameIndices[i]];
        }
        memcpy(pictureParameters.ref_frame_sign_bias, frame.refFrameSignBias, sizeof(frame.refFrameSignBias));
        pictureParameters.filter_level = frame.loopFilterLevel;
        pictureParameters.sharpness_level = frame.loopFilterSharpness;
        pictureParameters.mode_ref_delta_enabled = frame.loopFilterDeltaEnabled;
        pictureParameters.mode_ref_delta_update = frame.loopFilterUpdateRefDelta;
        pictureParameters.use_prev_in_find_mv_refs = (impl->lastFrameWidth == frame.width
            && impl->lastFrameHeight == frame.height
            && !frame.errorResilientMode
            && !frame.intraOnly
            && impl->lastFrameIsVisible);
        pictureParameters.ReservedControlInfo5Bits = 0;
        memcpy(pictureParameters.ref_deltas, frame.loopFilterRefDeltas, sizeof(frame.loopFilterRefDeltas));
        memcpy(pictureParameters.mode_deltas, frame.loopFilterModeDeltas, sizeof(frame.loopFilterModeDeltas));
        pictureParameters.base_qindex = frame.baseQIdx;
        pictureParameters.y_dc_delta_q = frame.deltaQYDc;
        pictureParameters.uv_dc_delta_q = frame.deltaQUvDc;
        pictureParameters.uv_ac_delta_q = frame.deltaQUvAc;
        pictureParameters.stVP9Segments.enabled = frame.segmentationEnabled;
        pictureParameters.stVP9Segments.update_map = frame.segmentationUpdateMap;
        pictureParameters.stVP9Segments.temporal_update = frame.segmentationTemporalUpdate;
        pictureParameters.stVP9Segments.abs_delta = frame.segmentationAbsOrDeltaUpdate;
        pictureParameters.stVP9Segments.ReservedSegmentFlags4Bits = 0;
        memcpy(pictureParameters.stVP9Segments.tree_probs, frame.segmentTreeProbs, sizeof(frame.segmentTreeProbs));
        memcpy(pictureParameters.stVP9Segments.pred_probs, frame.segmentPredProbs, sizeof(frame.segmentPredProbs));
        memcpy(pictureParameters.stVP9Segments.feature_data, frame.segmentFeatureData, sizeof(frame.segmentFeatureData));
        memset(pictureParameters.stVP9Segments.feature_mask, 0, sizeof(pictureParameters.stVP9Segments.feature_mask));
        for (UINT segment = 0; segment < std::size(frame.segmentFeatureEnabled); ++segment)
        {
            for (UINT feature = 0; feature < std::size(frame.segmentFeatureEnabled[0]); ++feature)
            {
                if (frame.segmentFeatureEnabled[segment][feature])
                {
                    pictureParameters.stVP9Segments.feature_mask[segment] |= (1 << feature);
                }
            }
        }
        pictureParameters.log2_tile_cols = frame.tileColsLog2;
        pictureParameters.log2_tile_rows = frame.tileRowsLog2;
        pictureParameters.uncompressed_header_size_byte_aligned = static_cast<USHORT>(frame.uncompressedHeaderSize);
        pictureParameters.first_partition_size = static_cast<USHORT>(frame.compressedHeaderSize);
        pictureParameters.Reserved16Bits = 0;
        pictureParameters.Reserved32Bits = 0;
        pictureParameters.StatusReportFeedbackNumber = ++impl->statusReportFeedbackNumber;

        if (!impl->decodeFrame(&pictureParameters, sizeof(pictureParameters), frame.bitstream.ptr, static_cast<UINT>(frame.bitstream.len), outputViewIndex))
        {
            return false;
        }

        for (UINT i = 0; i < std::size(impl->refFrames); ++i)
        {
            if (frame.refreshFrameFlags & (1 << i))
            {
                impl->refFrames[i].surfaceIndex = outputViewIndex;
                impl->refFrames[i].width = frame.width;
                impl->refFrames[i].height = frame.height;
                impl->refFrames[i].colorSpace = frame.colorSpace;
            }
        }

        impl->lastFrameWidth = frame.width;
        impl->lastFrameHeight = frame.height;
        impl->lastFrameColorSpace = frame.colorSpace;
        impl->lastFrameIsVisible = frame.showFrame;
        if (frame.showFrame)
        {
            impl->lastVisibleFrameOutputViewIndex = outputViewIndex;
            impl->lastVisibleFrameWidth = frame.width;
            impl->lastVisibleFrameHeight = frame.height;
            impl->lastVisibleFrameColorSpace = frame.colorSpace;
        }
    }

    return true;
}

bool VP9DXVADecoder::VP9DXVAHelper::stageVP9(VP9DXVADecoder::Image &image, std::vector<uint8_t> &yPlane, std::vector<uint8_t> &uPlane, std::vector<uint8_t> &vPlane)
{
    const DXGI_FORMAT format = DXGI_FORMAT_NV12; // YUV 4:2:0.

    if (!impl->stageFrame(impl->lastVisibleFrameOutputViewIndex, format, impl->lastFrameWidth, impl->lastFrameHeight, yPlane, uPlane, vPlane))
    {
        return false;
    }

    image.w = impl->lastVisibleFrameWidth;
    image.h = impl->lastVisibleFrameHeight;
    image.cs = impl->lastVisibleFrameColorSpace;

    image.chromaShiftW = 1; // NV12 format.
    image.chromaShiftH = 1; // NV12 format.
    image.bitDepth = 8; // NV12 format.

    image.planes[0] = yPlane.data();
    image.planes[1] = uPlane.data();
    image.planes[2] = vPlane.data();

    image.linesize[0] = image.w; // NV12 format.
    image.linesize[1] = (image.w + 1) / 2; // NV12 format.
    image.linesize[2] = (image.w + 1) / 2; // NV12 format.

    return true;
}

VP9DXVADecoder::VP9DXVADecoder(const WebMDemuxer &demuxer)
{
    switch (demuxer.getVideoCodec())
    {
#ifndef LIBSIMPLEWEBM_DISABLE_VP9
        case WebMDemuxer::VIDEO_VP9:
            vp9DXVAHelper = VP9DXVAHelper();
            break;
#endif

        default:
            return;
    }

    if (!vp9DXVAHelper.setupVP9(demuxer.getWidth(), demuxer.getHeight()))
    {
        vp9DXVAHelper = VP9DXVAHelper();
        return;
    }
}

VP9DXVADecoder::~VP9DXVADecoder()
{
    vp9DXVAHelper = VP9DXVAHelper();
}

VP9DXVADecoder::IMAGE_ERROR VP9DXVADecoder::decode(const WebMFrame &frame)
{
    if (!frame.isValid())
    {
        return IMAGE_ERROR_NO_FRAME;
    }

    if (!vp9DXVAHelper.decodeVP9(frame.buffer, frame.bufferSize))
    {
        return IMAGE_ERROR_UNSUPPORTED_FRAME;
    }

    return IMAGE_ERROR_OK;
}

VP9DXVADecoder::IMAGE_ERROR VP9DXVADecoder::getImage(Image &image)
{
    if (!vp9DXVAHelper.stageVP9(image, yPlane, uPlane, vPlane))
    {
        return IMAGE_ERROR_UNSUPPORTED_FRAME;
    }

    return IMAGE_ERROR_OK;
}

static inline int ceilRshift(int val, int shift)
{
    return (val + (1 << shift) - 1) >> shift;
}

int VP9DXVADecoder::Image::getWidth(int plane) const
{
    if (plane == 0 || plane == 3)
        return w;

    return ceilRshift(w, chromaShiftW);
}

int VP9DXVADecoder::Image::getHeight(int plane) const
{
    if (plane == 0 || plane == 3)
        return h;

    return ceilRshift(h, chromaShiftH);
}

void VP9DXVADecoder::reset()
{
    yPlane.clear();
    uPlane.clear();
    vPlane.clear();

    vp9DXVAHelper = VP9DXVAHelper();
}
