/**************************************************************************/
/*  webm_dxva.cpp                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "webm_dxva.h"

#include "core/object/ref_counted.h"

#include <d3d11.h>
#include <dxva.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct WebmDXVA::WebmDXVAImpl {
	ComPtr<ID3D11Device> m_device = nullptr;
	ComPtr<ID3D11DeviceContext> m_context = nullptr;
	ComPtr<ID3D11VideoDevice> m_video_device = nullptr;
	ComPtr<ID3D11VideoContext> m_video_context = nullptr;
	ComPtr<ID3D11VideoDecoder> m_video_decoder = nullptr;

	ComPtr<ID3D11Texture2D> m_decode_texture = nullptr;
	Vector<ComPtr<ID3D11VideoDecoderOutputView>> m_output_views;

	bool create_video_device() {
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
		if (FAILED(hr)) {
			return false;
		}

		m_device = device;
		m_context = context;

		ComPtr<ID3D11VideoDevice> video_device;
		ComPtr<ID3D11VideoContext> video_context;

		hr = device.As(&video_device);
		if (FAILED(hr)) {
			return false;
		}
		hr = context.As(&video_context);
		if (FAILED(hr)) {
			return false;
		}

		m_video_device = video_device;
		m_video_context = video_context;

		return true;
	}

	bool is_video_decoder_profile_supported(GUID p_profile, DXGI_FORMAT p_format) {
        HRESULT hr;

        bool is_profile_supported = false;
		UINT profile_count = m_video_device->GetVideoDecoderProfileCount();
		for (UINT profile_index = 0; profile_index < profile_count; profile_index++) {
            GUID profile;
			hr = m_video_device->GetVideoDecoderProfile(
                profile_index,
                &profile
            );
			if (FAILED(hr)) {
                return false;
            }
            if (IsEqualGUID(profile, p_profile)) {
                is_profile_supported = true;
                break;
            }
        }
        if (!is_profile_supported) {
            return false;
        }

        BOOL is_format_supported = false;
		hr = m_video_device->CheckVideoDecoderFormat(
            &p_profile,
            p_format,
            &is_format_supported
        );
		if (FAILED(hr)) {
            return false;
        }
        if (is_format_supported != TRUE) {
            return false;
        }

        return true;
    }

    bool create_video_decoder(GUID p_profile, DXGI_FORMAT p_format, UINT p_width, UINT p_height) {
        HRESULT hr;

        D3D11_VIDEO_DECODER_DESC desc = {};
        desc.Guid = p_profile;
        desc.SampleWidth = p_width;
        desc.SampleHeight = p_height;
        desc.OutputFormat = p_format;

        UINT config_count;
		hr = m_video_device->GetVideoDecoderConfigCount(
            &desc,
            &config_count
        );
		if (FAILED(hr)) {
            return false;
        }

        bool was_config_found = false;
        D3D11_VIDEO_DECODER_CONFIG config = {};
        for (UINT config_index = 0; config_index < config_count; config_index++) {
			hr = m_video_device->GetVideoDecoderConfig(
                &desc,
                config_index,
                &config
            );
			if (SUCCEEDED(hr)) {
                was_config_found = true;
                break;
            }
        }
        if (!was_config_found) {
            return false;
        }

        ComPtr<ID3D11VideoDecoder> video_decoder;
		hr = m_video_device->CreateVideoDecoder(
            &desc,
            &config,
            &video_decoder
        );
		if (FAILED(hr)) {
            return false;
        }

		m_video_decoder = video_decoder;

		return true;
    }

	bool create_output_views(GUID p_profile, DXGI_FORMAT p_format, UINT p_width, UINT p_height, UINT output_views_count) {
		HRESULT hr;

		D3D11_TEXTURE2D_DESC texture_desc = {};
		texture_desc.Width = p_width;
		texture_desc.Height = p_height;
		texture_desc.MipLevels = 1; // Disable mipmaps.
		texture_desc.ArraySize = output_views_count;
		texture_desc.Format = p_format;
		texture_desc.SampleDesc.Count = 1; // Disable MSAA (multisampling).
		texture_desc.SampleDesc.Quality = 0; // Ignore MSAA quality level.
		texture_desc.Usage = D3D11_USAGE_DEFAULT;
		texture_desc.BindFlags = D3D11_BIND_DECODER;

		hr = m_device->CreateTexture2D(
            &texture_desc,
            nullptr,
            &m_decode_texture
        );
        if (FAILED(hr)) {
			return false;
		}

		m_output_views.resize(output_views_count);
		for (UINT output_view_index = 0; output_view_index < output_views_count; output_view_index++) {
			D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC output_view_desc = {};
			output_view_desc.DecodeProfile = p_profile;
			output_view_desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
			output_view_desc.Texture2D.ArraySlice = output_view_index;

			ComPtr<ID3D11VideoDecoderOutputView> output_view;

			hr = m_video_device->CreateVideoDecoderOutputView(
                m_decode_texture.Get(),
                &output_view_desc,
                output_view.GetAddressOf()
            );
            if (FAILED(hr)) {
				return false;
			}

			m_output_views.set(output_view_index, output_view);
		}

		return true;
	}

	bool decode_frame(const void *p_picture_parameters, UINT p_picture_parameters_size, const uint8_t *p_bitstream, UINT p_bitstream_size, UINT p_output_view_index) {
		HRESULT hr;

        // DXVA2 requires bitstreams to be zero-padded to a multiple of 128 bytes.
		const UINT bitstream_padding_size = round_up_to_nearest(p_bitstream_size, 128) - p_bitstream_size;

		hr = m_video_context->DecoderBeginFrame(
            m_video_decoder.Get(),
            m_output_views[p_output_view_index].Get(),
            0,
            nullptr
        );
        if (FAILED(hr)) {
			return false;
		}

		UINT picture_parameters_buffer_size = 0;
		void *picture_parameters_buffer = nullptr;

		hr = m_video_context->GetDecoderBuffer(
            m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &picture_parameters_buffer_size,
            &picture_parameters_buffer
        );
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}
		if (picture_parameters_buffer_size < p_picture_parameters_size) {
			m_video_context->ReleaseDecoderBuffer(
                m_video_decoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS
			);
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		memcpy(picture_parameters_buffer, p_picture_parameters, p_picture_parameters_size);

        hr = m_video_context->ReleaseDecoderBuffer(
            m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS
        );
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		DXVA_Slice_VPx_Short slice = {};
		slice.BSNALunitDataLocation = 0; // The frame is at the beginning of the bitstream buffer.
		slice.SliceBytesInBuffer = p_bitstream_size + bitstream_padding_size;
		slice.wBadSliceChopping = 0; // Don't split the frame across multiple buffers.

		UINT bitstream_buffer_size = 0;
		void *bitstream_buffer = nullptr;

		hr = m_video_context->GetDecoderBuffer(
            m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bitstream_buffer_size,
            &bitstream_buffer
        );
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}
		if (bitstream_buffer_size < p_bitstream_size + bitstream_padding_size) {
			m_video_context->ReleaseDecoderBuffer(
                m_video_decoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_BITSTREAM
			);
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		memcpy(bitstream_buffer, p_bitstream, p_bitstream_size);

        // Zero-out the padding bytes.
		memset(static_cast<uint8_t *>(bitstream_buffer) + p_bitstream_size, 0, bitstream_padding_size);

		hr = m_video_context->ReleaseDecoderBuffer(
            m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM
        );
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		UINT slice_buffer_size = 0;
		void *slice_buffer = nullptr;

		hr = m_video_context->GetDecoderBuffer(
			m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &slice_buffer_size,
            &slice_buffer
		);
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}
		if (slice_buffer_size < sizeof(slice)) {
			m_video_context->ReleaseDecoderBuffer(
                m_video_decoder.Get(),
                D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL
			);
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		memcpy(slice_buffer, &slice, sizeof(slice));

		hr = m_video_context->ReleaseDecoderBuffer(
            m_video_decoder.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL
        );
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		D3D11_VIDEO_DECODER_BUFFER_DESC buffers[3] = {};

		buffers[0] = {};
		buffers[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
		buffers[0].DataSize = p_picture_parameters_size;

		buffers[1] = {};
		buffers[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
		buffers[1].DataSize = p_bitstream_size + bitstream_padding_size;

		buffers[2] = {};
		buffers[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
		buffers[2].DataSize = sizeof(slice);

		hr = m_video_context->SubmitDecoderBuffers(
			m_video_decoder.Get(),
            3,
            buffers
		);
        if (FAILED(hr)) {
            m_video_context->DecoderEndFrame(
                m_video_decoder.Get()
            );
			return false;
		}

		hr = m_video_context->DecoderEndFrame(
            m_video_decoder.Get()
        );
        if (FAILED(hr)) {
			return false;
		}

		return true;
	}

    static constexpr UINT round_up_to_nearest(UINT value, UINT step) {
		if (step == 0) {
			return value;
		}
		UINT remainder = value % step;
		if (remainder == 0) {
			return value;
		}
		return value + step - remainder;
	}
};

bool WebmDXVA::setup_vp9(unsigned int p_width, unsigned int p_height) {
	const GUID profile = D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0; // VP9 8-bit.
	const DXGI_FORMAT format = DXGI_FORMAT_NV12; // YUV 4:2:0.
	const UINT output_views_count = 8 + 1; // VP9 uses 8 reference slots.

	if (!impl->create_video_device()) {
		return false;
	}

    if (!impl->is_video_decoder_profile_supported(profile, format)) {
		return false;
	}

	if (!impl->create_video_decoder(profile, format, p_width, p_height)) {
		return false;
	}

    if (!impl->create_output_views(profile, format, p_width, p_height, output_views_count)) {
		return false;
	}

	return true;
}

bool WebmDXVA::decode_frame_vp9(const uint8_t *p_bitstream, unsigned int p_bitstream_size) {
	const UINT output_view_index = 0;

	DXVA_PicParams_VP9 picture_parameters = {}; // TODO: What do I put in here?

	if (!impl->decode_frame(&picture_parameters, sizeof(picture_parameters), p_bitstream, p_bitstream_size, output_view_index)) {
		return false;
	}

	return true;
}
