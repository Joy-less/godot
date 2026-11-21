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

#include "include/decoder/hardware/DXVADecoder.h"

#include <decoder/hardware/VP9FrameParser.h>

#include <cstring>

#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct DXVAHelper::DXVAHelperImpl
{
    ComPtr<ID3D11Device> device = nullptr;
    ComPtr<ID3D11DeviceContext> context = nullptr;
    ComPtr<ID3D11VideoDevice> videoDevice = nullptr;
    ComPtr<ID3D11VideoContext> videoContext = nullptr;
    ComPtr<ID3D11VideoDecoder> videoDecoder = nullptr;
    ComPtr<ID3D11Texture2D> decodeTexture = nullptr;
    std::vector<ComPtr<ID3D11VideoDecoderOutputView>> outputViews;

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
        for (UINT profileIndex = 0; profileIndex < profileCount; profileIndex++)
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
        for (UINT configIndex = 0; configIndex < configCount; configIndex++)
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

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.MipLevels = 1; // Disable mipmaps.
        textureDesc.ArraySize = outputViewsCount;
        textureDesc.Format = format;
        textureDesc.SampleDesc.Count = 1; // Disable MSAA (multisampling).
        textureDesc.SampleDesc.Quality = 0; // Ignore MSAA quality level.
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DECODER;

        hr = this->device->CreateTexture2D(
            &textureDesc,
            nullptr,
            &this->decodeTexture
        );
        if (FAILED(hr))
        {
            return false;
        }

        this->outputViews.resize(outputViewsCount);
        for (UINT outputViewIndex = 0; outputViewIndex < outputViewsCount; outputViewIndex++)
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

        return true;
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

DXVAHelper::DXVAHelper() :
    impl(std::make_unique<DXVAHelperImpl>())
{
}

bool DXVAHelper::setupVP9(unsigned int width, unsigned int height)
{
    const GUID profile = D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0; // VP9 8-bit.
    const DXGI_FORMAT format = DXGI_FORMAT_NV12; // YUV 4:2:0.
    const UINT outputViewsCount = 8 + 1; // VP9 uses 8 reference slots.

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

bool DXVAHelper::decodeFrameVP9(const uint8_t *bitstream, unsigned int bitstreamSize)
{
    const UINT outputViewIndex = 0;

    //VP9FrameParser::VP9Parser vp9Parser = {};
    //VP9FrameParser::VP9Parser::parsePacket();
    VP9FrameParser::VP9Frame frame = {};

    DXVA_PicParams_VP9 pictureParameters = {};
    pictureParameters.CurrPic.Index7Bits = outputViewIndex;
    pictureParameters.CurrPic.AssociatedFlag = 0; // TODO: ???
    pictureParameters.profile = frame.profile;
    pictureParameters.frame_type = frame.frameType;
    pictureParameters.show_frame = frame.showFrame;
    pictureParameters.error_resilient_mode = frame.errorResilientMode;
    pictureParameters.subsampling_x = frame.subsamplingX;
    pictureParameters.subsampling_y = frame.subsamplingY;
    pictureParameters.extra_plane = 0; // TODO: ???
    pictureParameters.refresh_frame_context = frame.refreshFrameContext;
    pictureParameters.frame_parallel_decoding_mode = frame.frameParallelDecodingMode;
    pictureParameters.intra_only = frame.intraOnly;
    pictureParameters.frame_context_idx = frame.frameContextIdx;
    pictureParameters.reset_frame_context = frame.resetFrameContext;
    pictureParameters.allow_high_precision_mv = frame.allowHighPrecisionMv;
    pictureParameters.ReservedFormatInfo2Bits = 0;
    pictureParameters.width = frame.width;
    pictureParameters.height = frame.height;
    pictureParameters.BitDepthMinus8Luma = 0; // 8-bit. TODO: ???
    pictureParameters.BitDepthMinus8Chroma = 0; // 8-bit. TODO: ???
    pictureParameters.interp_filter = frame.interpolationFilter;
	pictureParameters.Reserved8Bits = 0;
	memset(pictureParameters.ref_frame_map, 0, sizeof(pictureParameters.ref_frame_map)); // TODO: set to indices for DXVA surfaces
	memset(pictureParameters.ref_frame_coded_width, 0, sizeof(pictureParameters.ref_frame_coded_width)); // TODO: ???
	memset(pictureParameters.ref_frame_coded_height, 0, sizeof(pictureParameters.ref_frame_coded_height)); // TODO: ???
	memset(pictureParameters.frame_refs, 0, sizeof(pictureParameters.frame_refs)); // TODO: set to indices for DXVA surfaces
	memcpy(pictureParameters.ref_frame_sign_bias, frame.refFrameSignBias, sizeof(frame.refFrameSignBias));
    pictureParameters.filter_level = frame.loopFilterLevel;
    pictureParameters.sharpness_level = frame.loopFilterSharpness;
    pictureParameters.mode_ref_delta_enabled = frame.loopFilterDeltaEnabled;
    pictureParameters.mode_ref_delta_update = frame.loopFilterUpdateRefDelta;
    pictureParameters.use_prev_in_find_mv_refs = 0; // TODO: ???
    pictureParameters.ReservedControlInfo5Bits = 0;
	memcpy(pictureParameters.ref_deltas, frame.loopFilterRefDeltas, sizeof(frame.loopFilterRefDeltas));
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
	memset(pictureParameters.stVP9Segments.feature_mask, 0, sizeof(pictureParameters.stVP9Segments.feature_mask)); // TODO: ???
	pictureParameters.log2_tile_cols = frame.tileColsLog2;
    pictureParameters.log2_tile_rows = frame.tileRowsLog2;
    pictureParameters.uncompressed_header_size_byte_aligned = frame.uncompressedHeaderSize;
    pictureParameters.first_partition_size = 0; // TODO: ???
    pictureParameters.Reserved16Bits = 0;
    pictureParameters.Reserved32Bits = 0;
    pictureParameters.StatusReportFeedbackNumber = 0; // TODO: ???

    if (!impl->decodeFrame(&pictureParameters, sizeof(pictureParameters), bitstream, bitstreamSize, outputViewIndex)) {
        return false;
    }

    return true;
}

DXVADecoder::DXVADecoder(const WebMDemuxer &demuxer)
{
    switch (demuxer.getVideoCodec())
    {
#ifndef LIBSIMPLEWEBM_DISABLE_VP9
        case WebMDemuxer::VIDEO_VP9:
			dxvaHelper = DXVAHelper();
			break;
#endif

        default:
            return;
    }

	if (!dxvaHelper.setupVP9(demuxer.getWidth(), demuxer.getHeight()))
    {
		dxvaHelper = DXVAHelper();
		return;
	}
}

DXVADecoder::~DXVADecoder()
{
	dxvaHelper = DXVAHelper();
}

bool DXVADecoder::decode(const WebMFrame &frame)
{
    if (!m_ctx || !frame.isValid())
        return false;

    m_iter = NULL;
    m_alphaIter = NULL;

    if (vpx_codec_decode(m_ctx, frame.buffer, static_cast<unsigned int>(frame.bufferSize), NULL, 0))
    {
        return false;
    }

    // WebM alpha is stored as a second VP8/VP9 bitstream in
    // BlockAdditional with BlockAddID 1.
    // libvpx doesn't decode that data into VPX_PLANE_ALPHA.
    // It needs its own decoder instance.

    if (frame.hasAlpha())
    {
        if (!m_alphaCtx)
        {
            const vpx_codec_dec_cfg_t alphaCodecCfg = {
                m_threads,
                0,
                0
            };

            m_alphaCtx = new vpx_codec_ctx_t;

            if (vpx_codec_dec_init(m_alphaCtx, m_codecIface, &alphaCodecCfg, 0))
            {
                delete m_alphaCtx;
                m_alphaCtx = NULL;
                return false;
            }
        }

        if (vpx_codec_decode(m_alphaCtx, frame.alphaBuffer, static_cast<unsigned int>(frame.alphaBufferSize), NULL, 0))
        {
            return false;
        }
    }

    return true;
}

DXVADecoder::IMAGE_ERROR DXVADecoder::getImage(Image &image)
{
    IMAGE_ERROR err = NO_FRAME;

    vpx_image_t *img = NULL;
    vpx_image_t *alphaImg = NULL;

    if (m_ctx)
        img = vpx_codec_get_frame(m_ctx, &m_iter);

    if (m_alphaCtx)
        alphaImg = vpx_codec_get_frame(m_alphaCtx, &m_alphaIter);

    if (!img)
        return NO_FRAME;


    // The alpha decoder returns an ordinary I420 image.
    // Its Y plane is the actual alpha plane.

    if (alphaImg)
    {
        if (img->d_w != alphaImg->d_w || img->d_h != alphaImg->d_h)
        {
            return UNSUPPORTED_FRAME;
        }
    }

    if (img->cs != VPX_CS_UNKNOWN)
        m_last_space = img->cs;

    if (img->fmt & VPX_IMG_FMT_PLANAR)
    {
        if (img->stride[0] && img->stride[1] && img->stride[2])
        {
            const int uPlane = !!(img->fmt & VPX_IMG_FMT_UV_FLIP) + 1;

            const int vPlane = !(img->fmt & VPX_IMG_FMT_UV_FLIP) + 1;

            image.w = img->d_w;
            image.h = img->d_h;
            image.cs = m_last_space;
            image.chromaShiftW = img->x_chroma_shift;
            image.chromaShiftH = img->y_chroma_shift;
            image.bitDepth = img->bit_depth;

            image.planes[0] = img->planes[0];
            image.planes[1] = img->planes[uPlane];
            image.planes[2] = img->planes[vPlane];

            image.linesize[0] = img->stride[0];
            image.linesize[1] = img->stride[uPlane];
            image.linesize[2] = img->stride[vPlane];

            if (alphaImg && alphaImg->planes[VPX_PLANE_Y] && alphaImg->stride[VPX_PLANE_Y])
            {
                image.alpha = alphaImg->planes[VPX_PLANE_Y];
                image.alphaLinesize = alphaImg->stride[VPX_PLANE_Y];
            }
            else
            {
                image.alpha = NULL;
                image.alphaLinesize = 0;
            }

            err = NO_ERROR;
        }
    }
    else
    {
        err = UNSUPPORTED_FRAME;
    }

    return err;
}

static inline int ceilRshift(int val, int shift)
{
    return (val + (1 << shift) - 1) >> shift;
}

// 0 = Y, 1 = U, 2 = V, 3 = alpha.
int DXVADecoder::Image::getWidth(int plane) const
{
    if (plane == 0 || plane == 3)
        return w;

    return ceilRshift(w, chromaShiftW);
}

int DXVADecoder::Image::getHeight(int plane) const
{
    if (plane == 0 || plane == 3)
        return h;

    return ceilRshift(h, chromaShiftH);
}

bool DXVADecoder::reset()
{
    if (!m_ctx || !m_codecIface)
        return false;

    if (m_alphaCtx)
    {
        vpx_codec_destroy(m_alphaCtx);
        delete m_alphaCtx;
        m_alphaCtx = NULL;
    }

    vpx_codec_destroy(m_ctx);
    delete m_ctx;
    m_ctx = NULL;

    const vpx_codec_dec_cfg_t codecCfg = {
        m_threads,
        0,
        0
    };

    m_ctx = new vpx_codec_ctx_t;

    if (vpx_codec_dec_init(m_ctx, m_codecIface, &codecCfg, 0))
    {
        delete m_ctx;
        m_ctx = NULL;
        return false;
    }

    m_iter = NULL;
    m_alphaIter = NULL;
    m_last_space = VPX_CS_UNKNOWN;

    return true;
}
