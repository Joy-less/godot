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
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct WebmDXVA::WebmDXVAImpl {
	ComPtr<ID3D11VideoDevice> m_video_device = nullptr;
	ComPtr<ID3D11VideoContext> m_video_context = nullptr;
	ComPtr<ID3D11VideoDecoder> m_video_decoder = nullptr;

	bool is_video_decoder_profile_supported(GUID p_profile, DXGI_FORMAT p_format) {
        HRESULT hr;

        bool is_profile_supported = false;
		UINT profile_count = m_video_device->GetVideoDecoderProfileCount();
		for (UINT profile_index = 0; profile_index < profile_count; profile_index++) {
            GUID profile;
			hr = m_video_device->GetVideoDecoderProfile(profile_index, &profile);
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
		hr = m_video_device->CheckVideoDecoderFormat(&p_profile, DXGI_FORMAT_NV12, &is_format_supported);
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
		hr = m_video_device->GetVideoDecoderConfigCount(&desc, &config_count);
		if (FAILED(hr)) {
            return false;
        }

        bool was_config_found = false;
        D3D11_VIDEO_DECODER_CONFIG config = {};
        for (UINT config_index = 0; config_index < config_count; config_index++) {
			hr = m_video_device->GetVideoDecoderConfig(&desc, config_index, &config);
			if (SUCCEEDED(hr)) {
                was_config_found = true;
                break;
            }
        }
        if (!was_config_found) {
            return false;
        }

        ComPtr<ID3D11VideoDecoder> video_decoder;
		hr = m_video_device->CreateVideoDecoder(&desc, &config, &video_decoder);
		if (FAILED(hr)) {
            return false;
        }

		m_video_decoder = video_decoder;

		return true;
    }
};

bool WebmDXVA::create_video_device() {
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

	impl->m_video_device = video_device;
	impl->m_video_context = video_context;

	return true;
}

bool WebmDXVA::is_vp8_supported() {
	return impl->is_video_decoder_profile_supported(D3D11_DECODER_PROFILE_VP8_VLD, DXGI_FORMAT_NV12); // Profile: VP8 8-bit, Format: YUV 4:2:0
}

bool WebmDXVA::is_vp9_supported() {
	return impl->is_video_decoder_profile_supported(D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, DXGI_FORMAT_NV12); // Profile: VP9 8-bit, Format: YUV 4:2:0
}

bool WebmDXVA::create_vp8_decoder(unsigned int p_width, unsigned int p_height) {
	return impl->create_video_decoder(D3D11_DECODER_PROFILE_VP8_VLD, DXGI_FORMAT_NV12, p_width, p_height); // Profile: VP8 8-bit, Format: YUV 4:2:0
}

bool WebmDXVA::create_vp9_decoder(unsigned int p_width, unsigned int p_height) {
	return impl->create_video_decoder(D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, DXGI_FORMAT_NV12, p_width, p_height); // Profile: VP9 8-bit, Format: YUV 4:2:0
}
