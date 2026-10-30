#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"
// Image decoding is tested separately. This harness isolates text/layout behavior.
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageBlock::ImageBlock(const std::string& path, const std::string& src, int16_t w, int16_t h)
    : imagePath(path), srcPath(src), width(w), height(h) {}
bool PageLine::render(GfxRenderer&, int, int, int, const PageRenderCancellation*) { return true; }
bool PageLine::serialize(HalFile&) { return false; }
bool PageImage::render(GfxRenderer&, int, int, int, const PageRenderCancellation*) { return true; }
bool PageImage::serialize(HalFile&) { return false; }
bool PageHorizontalRule::render(GfxRenderer&, int, int, int, const PageRenderCancellation*) { return true; }
bool PageHorizontalRule::serialize(HalFile&) { return false; }
