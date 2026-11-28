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

#ifndef VP9_DXVA_DECODER_H
#define VP9_DXVA_DECODER_H

#include "include/demuxer/WebMDemuxer.h"

class VP9DXVADecoder
{
    VP9DXVADecoder(const VP9DXVADecoder &);
    void operator =(const VP9DXVADecoder &);

    struct VP9DXVAHelper
    {
    private:
        struct VP9DXVAHelperImpl;
        std::unique_ptr<VP9DXVAHelperImpl> impl;

    public:
        VP9DXVAHelper();

        bool setupVP9(unsigned int width, unsigned int height);
        bool decodeVP9(const uint8_t *bitstream, size_t bitstreamSize);
        bool stageVP9(WebMImage &image, std::vector<uint8_t> &yPlane, std::vector<uint8_t> &uPlane, std::vector<uint8_t> &vPlane);
    };

public:
    VP9DXVADecoder(const WebMDemuxer &demuxer);
    ~VP9DXVADecoder();

    inline bool isOpen() const
    {
        return (bool)m_vp9DXVAHelper;
    }

    WebMImage::IMAGE_ERROR decode(const WebMFrame &frame);

    WebMImage::IMAGE_ERROR getImage(WebMImage &image);

    bool reset();

private:
    VP9DXVAHelper *m_vp9DXVAHelper = nullptr;

    std::vector<uint8_t> yPlane = {};
    std::vector<uint8_t> uPlane = {};
    std::vector<uint8_t> vPlane = {};
};

#endif // VP9_DXVA_DECODER_H