#include "EditorCore/FrameCompositor.h"

#include "EditorCore/GpuCompositor.h"
#include "EditorCore/TextFrame.h"

namespace editor::core {

std::unique_ptr<IFrameCompositor> makeFrameCompositor(int width, int height, AVPixelFormat pixelFormat, bool allowGpu,
                                                      std::shared_ptr<GpuDevice> device)
{
    if (allowGpu)
        if (std::unique_ptr<GpuCompositor> gpu = GpuCompositor::create(width, height, pixelFormat, false, nullptr, std::move(device)))
            return gpu;
    return std::make_unique<TextFrameBlender>(width, height, pixelFormat);
}

}  // namespace editor::core
