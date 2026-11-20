#pragma once
#include "core/Project.h"
#include <memory>
extern "C" {
struct AVFrame;
struct AVCodecParameters;
}
namespace openframe {
struct VideoFrame {
    std::shared_ptr<AVFrame> native;
    QImage raster;
    VideoColorInfo color;
    qint64 ordinal = 0;
    QSize size() const;
    qint64 byteSize() const;
    bool isNull() const;
};
VideoColorInfo videoColorInfo(const AVFrame *, const VideoColorInfo &fallback = {});
VideoColorInfo videoColorInfo(const AVCodecParameters *);
VideoColorInfo rasterColorInfo(const QImage &);
VideoFrame retainVideoFrame(const AVFrame *, qint64 ordinal, const VideoColorInfo &fallback = {});
VideoColorInfo sdrColorInfo(int bits = 8);
bool isHdrColor(const VideoColorInfo &);
bool equivalentColorEncoding(const VideoColorInfo &, const VideoColorInfo &);
class ColorPipeline {
  public:
    // Encoded, unassociated float RGBA. No 8-bit intermediate; integer RGB codes normalize
    // by (2^componentBits-1), including exact endpoints in high-bit PNG output.
    static QImage encodedRgba(const VideoFrame &);
    static QImage displayRgba(const VideoFrame &, const ColorSettings & = {});
    static QImage convertEncoded(const QImage &, const VideoColorInfo &source,
                                 const VideoColorInfo &target, const ColorSettings & = {},
                                 bool toneMap = false);
    static QImage quantize(const QImage &, int bits);
    static void tagImage(QImage &, const VideoColorInfo &);
};
} // namespace openframe
