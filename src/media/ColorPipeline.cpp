#include "ColorPipeline.h"
#include <QColorSpace>
#include <QtConcurrent/QtConcurrentMap>
#include <qfloat16.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
namespace openframe {
namespace {
struct Pixel {
    float r, g, b, a;
};
QString name(const char *p, const char *fallback = "unknown") {
    return QString::fromLatin1(p ? p : fallback);
}
double rational(AVRational v) {
    return v.den ? double(v.num) / v.den : 0;
}
void descriptors(VideoColorInfo &c, int format, int primaries, int transfer, int matrix, int range,
                 int chroma) {
    if (format >= 0) {
        c.pixelFormat = name(av_get_pix_fmt_name(AVPixelFormat(format)), "");
        if (auto d = av_pix_fmt_desc_get(AVPixelFormat(format)))
            for (int n = 0; n < d->nb_components; ++n)
                c.componentBits = std::max(c.componentBits, d->comp[n].depth);
    }
    if (primaries != AVCOL_PRI_UNSPECIFIED)
        c.primaries = name(av_color_primaries_name(AVColorPrimaries(primaries)));
    if (transfer != AVCOL_TRC_UNSPECIFIED)
        c.transfer = name(av_color_transfer_name(AVColorTransferCharacteristic(transfer)));
    if (matrix != AVCOL_SPC_UNSPECIFIED)
        c.matrix = name(av_color_space_name(AVColorSpace(matrix)));
    if (range != AVCOL_RANGE_UNSPECIFIED)
        c.range = name(av_color_range_name(AVColorRange(range)));
    if (chroma != AVCHROMA_LOC_UNSPECIFIED)
        c.chromaLocation = name(av_chroma_location_name(AVChromaLocation(chroma)), "unspecified");
}
QColorSpace colorSpace(const VideoColorInfo &c) {
    const auto primaries = c.primaries == "bt2020"     ? QColorSpace::Primaries::Bt2020
                           : c.primaries == "adobergb" ? QColorSpace::Primaries::AdobeRgb
                           : c.primaries == "smpte432" ? QColorSpace::Primaries::DciP3D65
                                                       : QColorSpace::Primaries::SRgb;
    if (c.transfer == "smpte2084")
        return {primaries, QColorSpace::TransferFunction::St2084};
    if (c.transfer == "arib-std-b67")
        return {primaries, QColorSpace::TransferFunction::Hlg};
    if (c.transfer == "gamma22" || c.transfer == "gamma28")
        return {primaries, QColorSpace::TransferFunction::Gamma,
                c.transfer == "gamma22" ? 2.2f : 2.8f};
    if (c.transfer == "linear")
        return {primaries, QColorSpace::TransferFunction::Linear};
    if (c.transfer == "bt709" || c.transfer.startsWith("bt2020"))
        return {primaries, QColorSpace::TransferFunction::Bt2020};
    return {primaries, QColorSpace::TransferFunction::SRgb};
}
double pqDecode(double x) {
    constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128,
                     c3 = 2392.0 / 128;
    auto p = std::pow(std::max(0.0, x), 1 / m2);
    return 10000 * std::pow(std::max(0.0, p - c1) / std::max(1e-12, c2 - c3 * p), 1 / m1);
}
double pqEncode(double nits) {
    constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128,
                     c3 = 2392.0 / 128;
    auto p = std::pow(std::max(0.0, nits) / 10000, m1);
    return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
}
double decode(double x, const QString &transfer) {
    x = std::max(0.0, x);
    if (transfer == "linear")
        return x;
    if (transfer == "arib-std-b67") {
        constexpr double a = .17883277, b = .28466892, c = .55991073;
        return x <= .5 ? x * x / 3 : (std::exp((x - c) / a) + b) / 12;
    }
    if (transfer == "bt709" || transfer.startsWith("bt2020"))
        return x < .081 ? x / 4.5 : std::pow((x + .099) / 1.099, 1 / .45);
    if (transfer == "gamma22")
        return std::pow(x, 2.2);
    if (transfer == "gamma28")
        return std::pow(x, 2.8);
    return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
}
double encode(double x, const QString &transfer) {
    x = std::max(0.0, x);
    if (transfer == "linear")
        return x;
    if (transfer == "arib-std-b67") {
        constexpr double a = .17883277, b = .28466892, c = .55991073;
        return x <= 1.0 / 12 ? std::sqrt(3 * x) : a * std::log(12 * x - b) + c;
    }
    if (transfer == "bt709" || transfer.startsWith("bt2020"))
        return x < .018 ? 4.5 * x : 1.099 * std::pow(x, .45) - .099;
    if (transfer == "gamma22")
        return std::pow(x, 1 / 2.2);
    if (transfer == "gamma28")
        return std::pow(x, 1 / 2.8);
    return x <= .0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - .055;
}
struct TransferTables {
    std::vector<float> srgb, bt709, gamma22, gamma28, hlg, pq, srgbOut;
    TransferTables()
        : srgb(65537), bt709(65537), gamma22(65537), gamma28(65537), hlg(65537), pq(65537),
          srgbOut(65537) {
        for (int i = 0; i <= 65536; ++i) {
            const double x = i / 65536.0;
            srgb[i] = float(decode(x, "iec61966-2-1"));
            bt709[i] = float(decode(x, "bt709"));
            gamma22[i] = float(std::pow(x, 2.2));
            gamma28[i] = float(std::pow(x, 2.8));
            hlg[i] = float(decode(x, "arib-std-b67"));
            pq[i] = float(pqDecode(x));
            srgbOut[i] = float(encode(x, "iec61966-2-1"));
        }
    }
};
const TransferTables &transferTables() {
    static const TransferTables tables;
    return tables;
}
double lookup(const std::vector<float> &table, double value) {
    auto x = std::clamp(value, 0.0, 1.0) * 65536;
    auto n = std::min(65535, int(x));
    auto fraction = x - n;
    return table[n] + fraction * (table[n + 1] - table[n]);
}
using Vec = std::array<double, 3>;
Vec matrix(Vec a, const std::array<double, 9> &m) {
    return {m[0] * a[0] + m[1] * a[1] + m[2] * a[2], m[3] * a[0] + m[4] * a[1] + m[5] * a[2],
            m[6] * a[0] + m[7] * a[1] + m[8] * a[2]};
}
Vec to709(Vec v, const QString &p) {
    if (p == "bt2020")
        return matrix(v, {1.660491, -.587641, -.072850, -.124550, 1.132900, -.008350, -.018151,
                          -.100579, 1.118730});
    if (p == "adobergb")
        return matrix(v, {1.398283, -.398283, 0, 0, 1, 0, 0, -.042938, 1.042938});
    if (p == "smpte432")
        return matrix(v,
                      {1.224940, -.224940, 0, -.042057, 1.042057, 0, -.019638, -.078636, 1.098274});
    return v;
}
Vec from709(Vec v, const QString &p) {
    if (p == "bt2020")
        return matrix(v, {.627403896, .329283038, .043313066, .069097289, .919540395, .011362316,
                          .016391439, .088013308, .895595253});
    if (p == "adobergb")
        return matrix(v, {.71516269, .28483731, 0, 0, 1, 0, 0, .04117065, .95882935});
    if (p == "smpte432")
        return matrix(v, {.822461969, .177538031, 0, .033194199, .966805801, 0, .017082631,
                          .072397441, .910519928});
    return v;
}
double peak(const VideoColorInfo &c, const ColorSettings &v) {
    return v.sourcePeakNits > 0     ? v.sourcePeakNits
           : c.maxCLL > 0           ? c.maxCLL
           : c.masteringMaxNits > 0 ? c.masteringMaxNits
                                    : 1000;
}
double luminance(Vec v, const QString &p) {
    return p == "bt2020" ? .2627 * v[0] + .6780 * v[1] + .0593 * v[2]
                         : .2126 * v[0] + .7152 * v[1] + .0722 * v[2];
}
double mobius(double x, double p, double j) {
    if (p <= 1 || x <= j)
        return x;
    // Unique rational shoulder with value j and slope 1 at j, and value 1 at peak.
    // Above the declared peak the SDR target clips explicitly; no hidden peak estimation.
    const double c = (p * (1 - 2 * j) + j * j) / (p - 1);
    return std::min(1.0, ((2 * j + c) * x - j * j) / (x + c));
}
} // namespace
QSize VideoFrame::size() const {
    return native ? QSize(native->width, native->height) : raster.size();
}
bool VideoFrame::isNull() const {
    return !native && raster.isNull();
}
qint64 VideoFrame::byteSize() const {
    if (!native)
        return raster.sizeInBytes();
    qint64 n = 0;
    for (auto buffer : native->buf)
        if (buffer)
            n += buffer->size;
    for (int i = 0; i < native->nb_extended_buf; ++i)
        if (native->extended_buf[i])
            n += native->extended_buf[i]->size;
    return n;
}
VideoColorInfo videoColorInfo(const AVCodecParameters *p) {
    VideoColorInfo c;
    if (p)
        descriptors(c, p->format, p->color_primaries, p->color_trc, p->color_space, p->color_range,
                    p->chroma_location);
    return c;
}
VideoColorInfo videoColorInfo(const AVFrame *f, const VideoColorInfo &fallback) {
    auto c = fallback;
    c.componentBits = 0;
    descriptors(c, f->format, f->color_primaries, f->color_trc, f->colorspace, f->color_range,
                f->chroma_location);
    if (auto side = av_frame_get_side_data(f, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        auto m = reinterpret_cast<const AVMasteringDisplayMetadata *>(side->data);
        if (m->has_luminance) {
            c.masteringMinNits = rational(m->min_luminance);
            c.masteringMaxNits = rational(m->max_luminance);
        }
        if (m->has_primaries) {
            c.masteringChromaticities.clear();
            for (auto &p : m->display_primaries)
                for (auto v : p)
                    c.masteringChromaticities.append(rational(v));
            for (auto v : m->white_point)
                c.masteringChromaticities.append(rational(v));
        }
    }
    if (auto side = av_frame_get_side_data(f, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        auto m = reinterpret_cast<const AVContentLightMetadata *>(side->data);
        c.maxCLL = m->MaxCLL;
        c.maxFALL = m->MaxFALL;
    }
    return c;
}
VideoColorInfo rasterColorInfo(const QImage &image) {
    int bits = 8;
    switch (image.format()) {
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        bits = 32;
        break;
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale16:
        bits = 16;
        break;
    case QImage::Format_BGR30:
    case QImage::Format_RGB30:
    case QImage::Format_A2BGR30_Premultiplied:
    case QImage::Format_A2RGB30_Premultiplied:
        bits = 10;
        break;
    default:
        break;
    }
    VideoColorInfo c = sdrColorInfo(bits);
    auto cs = image.colorSpace();
    if (cs.isValid()) {
        if (cs.primaries() != QColorSpace::Primaries::SRgb &&
            cs.primaries() != QColorSpace::Primaries::Bt2020 &&
            cs.primaries() != QColorSpace::Primaries::DciP3D65 &&
            cs.primaries() != QColorSpace::Primaries::AdobeRgb)
            throw std::runtime_error("Unsupported image color primaries/profile; convert the image "
                                     "to sRGB, Adobe RGB, P3 or BT.2020");
        c.primaries = cs.primaries() == QColorSpace::Primaries::AdobeRgb   ? "adobergb"
                      : cs.primaries() == QColorSpace::Primaries::Bt2020   ? "bt2020"
                      : cs.primaries() == QColorSpace::Primaries::DciP3D65 ? "smpte432"
                                                                           : "bt709";
        switch (cs.transferFunction()) {
        case QColorSpace::TransferFunction::St2084:
            c.transfer = "smpte2084";
            break;
        case QColorSpace::TransferFunction::Hlg:
            c.transfer = "arib-std-b67";
            break;
        case QColorSpace::TransferFunction::Linear:
            c.transfer = "linear";
            break;
        case QColorSpace::TransferFunction::Bt2020:
            c.transfer = "bt2020-10";
            break;
        case QColorSpace::TransferFunction::SRgb:
            break;
        case QColorSpace::TransferFunction::Gamma:
            if (std::abs(cs.gamma() - 2.2) < .001)
                c.transfer = "gamma22";
            else if (std::abs(cs.gamma() - 2.8) < .001)
                c.transfer = "gamma28";
            else
                throw std::runtime_error("Unsupported image gamma; convert the image to sRGB");
            break;
        default:
            throw std::runtime_error(
                "Unsupported image transfer/profile; convert the image to sRGB");
        }
    }
    const bool alpha = image.hasAlphaChannel();
    c.pixelFormat = bits == 32   ? (alpha ? "gbrapf32le" : "gbrpf32le")
                    : bits == 16 ? (alpha ? "rgba64le" : "rgb48le")
                    : bits == 10 ? (alpha ? "gbrap10le" : "gbrp10le")
                                 : (alpha ? "rgba" : "rgb24");
    return c;
}
VideoFrame retainVideoFrame(const AVFrame *f, qint64 ordinal, const VideoColorInfo &fallback) {
    auto clone = av_frame_clone(f);
    if (!clone)
        throw std::bad_alloc();
    return {std::shared_ptr<AVFrame>(clone, [](AVFrame *p) { av_frame_free(&p); }),
            {},
            videoColorInfo(f, fallback),
            ordinal};
}
VideoColorInfo sdrColorInfo(int bits) {
    VideoColorInfo c;
    c.componentBits = bits;
    c.primaries = "bt709";
    c.transfer = "iec61966-2-1";
    c.matrix = "gbr";
    c.range = "pc";
    return c;
}
bool isHdrColor(const VideoColorInfo &c) {
    return c.transfer == "smpte2084" || c.transfer == "arib-std-b67";
}
bool equivalentColorEncoding(const VideoColorInfo &a, const VideoColorInfo &b) {
    return a.primaries == b.primaries && a.transfer == b.transfer && a.matrix == b.matrix &&
           a.range == b.range;
}
void ColorPipeline::tagImage(QImage &image, const VideoColorInfo &c) {
    image.setColorSpace(colorSpace(c));
}
QImage ColorPipeline::encodedRgba(const VideoFrame &video) {
    if (video.isNull())
        return {};
    if (!video.native) {
        auto out = video.raster.convertToFormat(QImage::Format_RGBA32FPx4);
        tagImage(out, video.color);
        return out;
    }
    const auto f = video.native.get();
    const auto desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (desc && video.color.componentBits <= 8 &&
        (!(desc->flags & AV_PIX_FMT_FLAG_RGB) || video.color.range != "tv")) {
        QImage bytes(f->width, f->height, QImage::Format_RGBA8888);
        if (bytes.isNull())
            throw std::bad_alloc();
        auto sws =
            sws_getContext(f->width, f->height, AVPixelFormat(f->format), f->width, f->height,
                           AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
            throw std::runtime_error("Cannot convert 8-bit source");
        const auto coeff = sws_getCoefficients(video.color.matrix == "bt2020nc" ? SWS_CS_BT2020
                                               : video.color.matrix == "bt709"  ? SWS_CS_ITU709
                                                                                : SWS_CS_ITU601);
        sws_setColorspaceDetails(sws, coeff, video.color.range == "pc", coeff, 1, 0, 1 << 16,
                                 1 << 16);
        uint8_t *dst[] = {bytes.bits(), nullptr, nullptr, nullptr};
        int stride[] = {int(bytes.bytesPerLine()), 0, 0, 0};
        auto converted = sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
        sws_freeContext(sws);
        if (converted != f->height)
            throw std::runtime_error("Incomplete 8-bit conversion");
        auto out = bytes.convertToFormat(QImage::Format_RGBA32FPx4);
        tagImage(out, video.color);
        return out;
    }
    QImage out(f->width, f->height, QImage::Format_RGBA32FPx4);
    if (out.isNull())
        throw std::bad_alloc();
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_RGB) &&
        !(desc->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM))) {
        for (int y = 0; y < f->height; ++y) {
            auto dst = reinterpret_cast<Pixel *>(out.scanLine(y));
            for (int x = 0; x < f->width; ++x) {
                float components[4] = {0, 0, 0, 1};
                for (int n = 0; n < desc->nb_components; ++n) {
                    const auto &d = desc->comp[n];
                    const auto p = f->data[d.plane] + qint64(y) * f->linesize[d.plane] +
                                   qint64(x) * d.step + d.offset;
                    quint32 value = 0;
                    const int bytes = (d.depth + d.shift + 7) / 8;
                    if (bytes > 4)
                        throw std::runtime_error("Unsupported RGB component packing");
                    for (int b = 0; b < bytes; ++b)
                        value |= quint32(p[b])
                                 << (8 * ((desc->flags & AV_PIX_FMT_FLAG_BE) ? bytes - b - 1 : b));
                    if (desc->flags & AV_PIX_FMT_FLAG_FLOAT) {
                        if (d.depth == 32)
                            std::memcpy(&components[n], &value, 4);
                        else if (d.depth == 16) {
                            quint16 half = quint16(value);
                            qfloat16 v;
                            std::memcpy(&v, &half, 2);
                            components[n] = float(v);
                        } else
                            throw std::runtime_error("Unsupported float RGB component");
                    } else
                        components[n] =
                            float(double((value >> d.shift) & ((quint64(1) << d.depth) - 1)) /
                                  double((quint64(1) << d.depth) - 1));
                }
                if (video.color.range == "tv")
                    for (int n = 0; n < 3; ++n) {
                        const auto depth = desc->comp[n].depth;
                        const auto maximum = double((quint64(1) << depth) - 1);
                        const auto scale = std::ldexp(1.0, depth - 8);
                        components[n] =
                            float((components[n] * maximum - 16 * scale) / (219 * scale));
                    }
                dst[x] = {components[0], components[1], components[2], components[3]};
            }
        }
    } else if (desc && desc->nb_components >= 3 &&
               !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL |
                                AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_FLOAT))) {
        auto sample = [&](int component, int x, int y) {
            const auto &d = desc->comp[component];
            const auto cw = (f->width + (1 << desc->log2_chroma_w) - 1) >> desc->log2_chroma_w;
            const auto ch = (f->height + (1 << desc->log2_chroma_h) - 1) >> desc->log2_chroma_h;
            x = std::clamp(x, 0, component == 1 || component == 2 ? cw - 1 : f->width - 1);
            y = std::clamp(y, 0, component == 1 || component == 2 ? ch - 1 : f->height - 1);
            auto p =
                f->data[d.plane] + qint64(y) * f->linesize[d.plane] + qint64(x) * d.step + d.offset;
            quint32 code = 0;
            int bytes = (d.depth + d.shift + 7) / 8;
            for (int b = 0; b < bytes; ++b)
                code |= quint32(p[b])
                        << (8 * ((desc->flags & AV_PIX_FMT_FLAG_BE) ? bytes - b - 1 : b));
            return double((code >> d.shift) & ((quint64(1) << d.depth) - 1));
        };
        auto chroma = [&](int component, int x, int y) {
            const double sx = double(1 << desc->log2_chroma_w),
                         sy = double(1 << desc->log2_chroma_h);
            const bool left = video.color.chromaLocation == "left" ||
                              video.color.chromaLocation == "topleft" ||
                              video.color.chromaLocation == "bottomleft";
            const bool top =
                video.color.chromaLocation == "topleft" || video.color.chromaLocation == "top";
            const auto px = (x - (left ? 0 : (sx - 1) / 2)) / sx,
                       py = (y - (top ? 0 : (sy - 1) / 2)) / sy;
            int ix = int(std::floor(px)), iy = int(std::floor(py));
            double dx = px - ix, dy = py - iy;
            return (1 - dy) *
                       ((1 - dx) * sample(component, ix, iy) + dx * sample(component, ix + 1, iy)) +
                   dy * ((1 - dx) * sample(component, ix, iy + 1) +
                         dx * sample(component, ix + 1, iy + 1));
        };
        const bool limited = video.color.range != "pc";
        const double kr = video.color.matrix == "bt2020nc" ? .2627
                          : video.color.matrix == "bt709"  ? .2126
                                                           : .299;
        const double kb = video.color.matrix == "bt2020nc" ? .0593
                          : video.color.matrix == "bt709"  ? .0722
                                                           : .114;
        const double yscale = std::ldexp(1.0, desc->comp[0].depth - 8),
                     cscale = std::ldexp(1.0, desc->comp[1].depth - 8);
        const double ymax = double((quint64(1) << desc->comp[0].depth) - 1),
                     cmax = double((quint64(1) << desc->comp[1].depth) - 1);
        for (int y = 0; y < f->height; ++y) {
            auto dst = reinterpret_cast<Pixel *>(out.scanLine(y));
            for (int x = 0; x < f->width; ++x) {
                const double luma = limited ? (sample(0, x, y) - 16 * yscale) / (219 * yscale)
                                            : sample(0, x, y) / ymax;
                const double cb =
                    (chroma(1, x, y) - 128 * cscale) / (limited ? 224 * cscale : cmax);
                const double cr =
                    (chroma(2, x, y) - 128 * cscale) / (limited ? 224 * cscale : cmax);
                const double r = luma + 2 * (1 - kr) * cr, b = luma + 2 * (1 - kb) * cb,
                             g = (luma - kr * r - kb * b) / (1 - kr - kb);
                const auto alpha =
                    desc->nb_components > 3
                        ? sample(3, x, y) / double((quint64(1) << desc->comp[3].depth) - 1)
                        : 1;
                dst[x] = {float(r), float(g), float(b), float(alpha)};
            }
        }
    } else {
        QImage high(f->width, f->height, QImage::Format_RGBA64);
        if (high.isNull())
            throw std::bad_alloc();
        auto sws = sws_getContext(f->width, f->height, AVPixelFormat(f->format), f->width,
                                  f->height, AV_PIX_FMT_RGBA64LE, SWS_BILINEAR | SWS_ACCURATE_RND,
                                  nullptr, nullptr, nullptr);
        if (!sws)
            throw std::runtime_error("Unsupported high precision color conversion");
        auto cs = video.color.matrix == "bt2020nc"                      ? SWS_CS_BT2020
                  : video.color.matrix == "bt709"                       ? SWS_CS_ITU709
                  : video.color.matrix == "unknown" && f->height >= 720 ? SWS_CS_ITU709
                                                                        : SWS_CS_ITU601;
        auto coeff = sws_getCoefficients(cs);
        sws_setColorspaceDetails(sws, coeff, video.color.range == "pc", coeff, 1, 0, 1 << 16,
                                 1 << 16);
        uint8_t *data[] = {high.bits(), nullptr, nullptr, nullptr};
        int lines[] = {int(high.bytesPerLine()), 0, 0, 0};
        auto r = sws_scale(sws, f->data, f->linesize, 0, f->height, data, lines);
        sws_freeContext(sws);
        if (r != f->height)
            throw std::runtime_error("Incomplete high precision conversion");
        out = high.convertToFormat(QImage::Format_RGBA32FPx4);
    }
    tagImage(out, video.color);
    return out;
}
QImage ColorPipeline::convertEncoded(const QImage &image, const VideoColorInfo &source,
                                     const VideoColorInfo &target, const ColorSettings &settings,
                                     bool toneMap) {
    auto out = image.convertToFormat(QImage::Format_RGBA32FPx4);
    if (out.isNull())
        return out;
    const auto sourcePrimaries =
        source.primaries == "unknown" ? QString("bt709") : source.primaries;
    const auto targetPrimaries =
        target.primaries == "unknown" ? QString("bt709") : target.primaries;
    const auto sourceTransfer =
        source.transfer == "unknown" ? QString("iec61966-2-1") : source.transfer;
    const auto targetTransfer =
        target.transfer == "unknown" ? QString("iec61966-2-1") : target.transfer;
    if (sourcePrimaries == targetPrimaries && sourceTransfer == targetTransfer && !toneMap) {
        tagImage(out, target);
        return out;
    }
    const auto white = settings.referenceWhiteNits, sourcePeak = peak(source, settings);
    const bool hdr = isHdrColor(source);
    const auto gamma = 1.2 + .42 * std::log10(sourcePeak / 1000);
    const bool sourcePq = sourceTransfer == "smpte2084",
               sourceHlg = sourceTransfer == "arib-std-b67";
    const bool targetPq = targetTransfer == "smpte2084",
               targetHlg = targetTransfer == "arib-std-b67",
               targetSrgb = targetTransfer == "iec61966-2-1";
    const auto &tables = transferTables();
    const auto *inputLut = sourcePq                     ? &tables.pq
                           : sourceHlg                  ? &tables.hlg
                           : sourceTransfer == "linear" ? nullptr
                           : sourceTransfer == "bt709" || sourceTransfer.startsWith("bt2020")
                               ? &tables.bt709
                           : sourceTransfer == "gamma22" ? &tables.gamma22
                           : sourceTransfer == "gamma28" ? &tables.gamma28
                                                         : &tables.srgb;
    thread_local std::vector<float> hlgGain(65537);
    thread_local double previousGamma = -1000, previousScale = -1;
    if (sourceHlg && (previousGamma != gamma || previousScale != sourcePeak / white)) {
        for (int i = 0; i <= 65536; ++i)
            hlgGain[i] =
                float(sourcePeak / white * std::pow(std::max(1e-12, i / 65536.0), gamma - 1));
        previousGamma = gamma;
        previousScale = sourcePeak / white;
    }
    const bool transformSource = sourcePrimaries != "bt709",
               transformTarget = targetPrimaries != "bt709";
    const auto basisTo =
        std::array<Vec, 3>{to709({1, 0, 0}, sourcePrimaries), to709({0, 1, 0}, sourcePrimaries),
                           to709({0, 0, 1}, sourcePrimaries)};
    const auto basisFrom =
        std::array<Vec, 3>{from709({1, 0, 0}, targetPrimaries), from709({0, 1, 0}, targetPrimaries),
                           from709({0, 0, 1}, targetPrimaries)};
    const std::array<double, 9> sourceMatrix{basisTo[0][0], basisTo[1][0], basisTo[2][0],
                                             basisTo[0][1], basisTo[1][1], basisTo[2][1],
                                             basisTo[0][2], basisTo[1][2], basisTo[2][2]};
    const std::array<double, 9> targetMatrix{basisFrom[0][0], basisFrom[1][0], basisFrom[2][0],
                                             basisFrom[0][1], basisFrom[1][1], basisFrom[2][1],
                                             basisFrom[0][2], basisFrom[1][2], basisFrom[2][2]};
    const bool source2020 = sourcePrimaries == "bt2020", target2020 = targetPrimaries == "bt2020";
    const double targetPeak = peak(target, settings),
                 targetGamma = 1.2 + .42 * std::log10(targetPeak / 1000);
    for (int y = 0; y < out.height(); ++y) {
        auto pixels = reinterpret_cast<Pixel *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            auto &p = pixels[x];
            Vec v{p.r, p.g, p.b};
            for (auto &c : v) {
                auto decoded = inputLut && c >= 0 && c <= 1 ? lookup(*inputLut, c)
                               : sourcePq                   ? pqDecode(c)
                                                            : decode(c, sourceTransfer);
                c = sourcePq ? decoded / white : decoded;
            }
            if (sourceHlg) {
                const auto light = source2020 ? .2627 * v[0] + .6780 * v[1] + .0593 * v[2]
                                              : .2126 * v[0] + .7152 * v[1] + .0722 * v[2];
                const auto gain =
                    light >= 0 && light <= 1
                        ? lookup(hlgGain, light)
                        : sourcePeak / white * std::pow(std::max(1e-12, light), gamma - 1);
                for (auto &c : v)
                    c *= gain;
            }
            if (transformSource)
                v = matrix(v, sourceMatrix);
            if (toneMap && hdr && settings.hdrToneMapping) {
                const auto signal = std::max({v[0], v[1], v[2], 1e-12});
                auto gain = mobius(signal, sourcePeak / white, settings.mobiusTransition) / signal;
                gain *= settings.displayPeakNits / white;
                for (auto &c : v)
                    c *= gain;
            }
            if (transformTarget)
                v = matrix(v, targetMatrix);
            if (targetHlg) {
                const auto light = target2020 ? .2627 * v[0] + .6780 * v[1] + .0593 * v[2]
                                              : .2126 * v[0] + .7152 * v[1] + .0722 * v[2];
                const auto yd = std::max(1e-12, light) * white / targetPeak;
                const auto gain =
                    white / targetPeak * std::pow(yd, (1 - targetGamma) / targetGamma);
                for (auto &c : v)
                    c *= gain;
            }
            for (auto &c : v)
                c = targetPq                         ? pqEncode(c * white)
                    : targetSrgb && c >= 0 && c <= 1 ? lookup(tables.srgbOut, c)
                                                     : encode(c, targetTransfer);
            p.r = float(v[0]);
            p.g = float(v[1]);
            p.b = float(v[2]);
        }
    }
    tagImage(out, target);
    return out;
}
QImage ColorPipeline::displayRgba(const VideoFrame &frame, const ColorSettings &settings) {
    return convertEncoded(encodedRgba(frame), frame.color, sdrColorInfo(32), settings,
                          isHdrColor(frame.color));
}
QImage ColorPipeline::quantize(const QImage &image, int bits) {
    if (image.isNull())
        return {};
    if (bits != 8 && bits != 16)
        throw std::runtime_error("Raster output supports 8 or 16 bits per component");
    auto straight = image.convertToFormat(QImage::Format_RGBA32FPx4);
    QImage out(image.size(), bits == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (out.isNull())
        throw std::bad_alloc();
    const double maximum = bits == 16 ? 65535 : 255;
    const int channelsPerLine = image.width() * 4;
    auto *output = out.bits();
    const qsizetype stride = out.bytesPerLine();
    auto quantizeRow = [&](int y) {
        const auto src = reinterpret_cast<const float *>(straight.constScanLine(y));
        auto *row = output + y * stride;
        if (bits == 16) {
            auto dst = reinterpret_cast<quint16 *>(row);
            for (int x = 0; x < channelsPerLine; ++x)
                dst[x] = quint16(std::clamp(double(src[x]) * maximum, 0.0, maximum) + .5);
        } else {
            for (int x = 0; x < channelsPerLine; ++x)
                row[x] = quint8(std::clamp(double(src[x]) * maximum, 0.0, maximum) + .5);
        }
    };
    if (qint64(image.width()) * image.height() >= 1024 * 1024) {
        QList<int> rows(image.height());
        std::iota(rows.begin(), rows.end(), 0);
        QtConcurrent::blockingMap(rows, quantizeRow);
    } else {
        for (int y = 0; y < image.height(); ++y)
            quantizeRow(y);
    }
    out.setColorSpace(image.colorSpace());
    return out;
}

} // namespace openframe
