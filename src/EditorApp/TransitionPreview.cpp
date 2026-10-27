#include "EditorApp/TransitionPreview.h"

#include <QtGui/QColor>
#include <QtGui/QFont>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>

#include <algorithm>

namespace editor::app {

namespace {

QImage sample(QSize size, const QColor& light, const QColor& dark, const QString& letter)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, size.width(), size.height());
    gradient.setColorAt(0.0, light);
    gradient.setColorAt(1.0, dark);
    painter.fillRect(QRect(QPoint(0, 0), size), gradient);
    QFont font;
    font.setBold(true);
    font.setPixelSize(std::max(8, size.height() * 3 / 5));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignCenter, letter);
    return image;
}

}  // namespace

QImage transitionSampleFrom(QSize size)
{
    return sample(size, QColor(70, 130, 220), QColor(20, 50, 120), QStringLiteral("A"));
}

QImage transitionSampleTo(QSize size)
{
    return sample(size, QColor(250, 170, 60), QColor(170, 70, 20), QStringLiteral("B"));
}

QImage renderTransitionSample(const core::Transition& transition, const core::TransitionParams& params, double progress,
                              QSize size)
{
    const QImage from = transitionSampleFrom(size);
    const QImage to = transitionSampleTo(size);
    QImage out(size, QImage::Format_RGBA8888);
    transition.blend({from.constBits(), from.width(), from.height(), static_cast<int>(from.bytesPerLine())},
                     {to.constBits(), to.width(), to.height(), static_cast<int>(to.bytesPerLine())}, progress, params,
                     {out.bits(), static_cast<int>(out.bytesPerLine())});
    return out;
}

}  // namespace editor::app
