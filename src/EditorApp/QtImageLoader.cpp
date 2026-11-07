#include "EditorApp/QtImageLoader.h"

#include "EditorCore/StillImage.h"

#include <QtGui/QImage>
#include <QtGui/QImageReader>

#include <cstring>

namespace editor::app {

void installQtImageLoader()
{
    core::setImageLoader([](const std::string& utf8Path) -> std::shared_ptr<core::VideoFrame> {
        QImageReader reader(QString::fromUtf8(utf8Path.c_str()));
        reader.setAutoTransform(true);  // orientamento EXIF
        QImage image = reader.read();
        if (image.isNull())
            return nullptr;
        // ARGB32 non premoltiplicato: in memoria (little endian) è B G R A, come i fotogrammi dei video.
        image = image.convertToFormat(QImage::Format_ARGB32);
        auto frame = std::make_shared<core::VideoFrame>();
        frame->time = 0.0;
        frame->width = image.width();
        frame->height = image.height();
        frame->stride = image.width() * 4;
        frame->pixels.resize(static_cast<std::size_t>(frame->stride) * frame->height);
        for (int y = 0; y < image.height(); ++y)
            std::memcpy(frame->pixels.data() + static_cast<std::size_t>(y) * frame->stride, image.constScanLine(y),
                        static_cast<std::size_t>(frame->stride));
        return frame;
    });
}

}  // namespace editor::app
