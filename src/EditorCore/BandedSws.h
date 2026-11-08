#pragma once

#include "EditorCore/FfmpegUtil.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace editor::core {

// Quante bande di righe usare per un'immagine alta `height`: una per core (al massimo 8), ognuna di almeno 64 righe.
inline int bandCount(int height)
{
    const int cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    return std::clamp(std::min(cores, 8), 1, std::max(1, height / 64));
}

// I bordi delle bande (count + 1 valori da 0 a `height`): tutti pari, così anche il colore sottocampionato resta allineato.
inline std::vector<int> bandEdges(int height, int count)
{
    std::vector<int> edges(static_cast<std::size_t>(count) + 1);
    for (int i = 0; i <= count; ++i)
        edges[static_cast<std::size_t>(i)] = i == count ? height : ((height * i / count) & ~1);
    return edges;
}

// Chiama fn(indice, riga iniziale, riga finale) per ogni banda, su thread diversi (la prima sul thread chiamante).
// fn non deve lanciare eccezioni.
template <typename F>
void runBands(const std::vector<int>& edges, F&& fn)
{
    const int count = static_cast<int>(edges.size()) - 1;
    if (count <= 1) {
        fn(0, edges.front(), edges.back());
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(count) - 1);
    for (int i = 1; i < count; ++i)
        workers.emplace_back([&fn, &edges, i] { fn(i, edges[static_cast<std::size_t>(i)], edges[static_cast<std::size_t>(i) + 1]); });
    fn(0, edges[0], edges[1]);
    for (std::thread& w : workers)
        w.join();
}

// Conversione di formato (swscale, stessa misura) a bande di righe su più thread: ogni banda è come un'immagine a sé
// con il suo contesto. Il risultato è quello della conversione intera, tranne al più qualche livello di colore sulle
// righe di bordo fra due bande nei formati con colore sottocampionato in verticale.
class BandedSws
{
public:
    // Prepara i contesti (solo se qualcosa è cambiato). `srcFull`/`dstFull`: gamma piena (0..255) invece che limitata.
    // Restituisce false se la conversione non è possibile.
    bool prepare(int width, int height, AVPixelFormat src, AVPixelFormat dst, bool srcFull, bool dstFull)
    {
        const Key key{width, height, src, dst, srcFull, dstFull};
        if (ready_ && key == key_)
            return true;
        ready_ = false;
        contexts_.clear();
        const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(src);
        const AVPixFmtDescriptor* dstDesc = av_pix_fmt_desc_get(dst);
        if (srcDesc == nullptr || dstDesc == nullptr || width <= 0 || height <= 0)
            return false;
        srcShift_ = srcDesc->log2_chroma_h;
        dstShift_ = dstDesc->log2_chroma_h;
        edges_ = bandEdges(height, bandCount(height));
        const int* coefficients = sws_getCoefficients(SWS_CS_DEFAULT);
        for (std::size_t i = 0; i + 1 < edges_.size(); ++i) {
            const int rows = edges_[i + 1] - edges_[i];
            std::unique_ptr<SwsContext, detail::SwsDeleter> context(
                sws_getContext(width, rows, src, width, rows, dst, SWS_BICUBIC, nullptr, nullptr, nullptr));
            if (!context)
                return false;
            sws_setColorspaceDetails(context.get(), coefficients, srcFull ? 1 : 0, coefficients, dstFull ? 1 : 0, 0, 1 << 16, 1 << 16);
            contexts_.push_back(std::move(context));
        }
        key_ = key;
        ready_ = true;
        return true;
    }

    // Converte l'immagine intera (dopo prepare()): piani e passi come per sws_scale.
    void convert(const std::uint8_t* const src[4], const int srcStride[4], std::uint8_t* const dst[4], const int dstStride[4])
    {
        runBands(edges_, [&](int band, int y0, int y1) {
            const std::uint8_t* s[4] = {};
            std::uint8_t* d[4] = {};
            for (int p = 0; p < 4; ++p) {
                const bool chroma = p == 1 || p == 2;
                if (src[p] != nullptr)
                    s[p] = src[p] + static_cast<std::ptrdiff_t>(chroma ? y0 >> srcShift_ : y0) * srcStride[p];
                if (dst[p] != nullptr)
                    d[p] = dst[p] + static_cast<std::ptrdiff_t>(chroma ? y0 >> dstShift_ : y0) * dstStride[p];
            }
            sws_scale(contexts_[static_cast<std::size_t>(band)].get(), s, srcStride, 0, y1 - y0, d, dstStride);
        });
    }

private:
    struct Key
    {
        int width = 0;
        int height = 0;
        AVPixelFormat src = AV_PIX_FMT_NONE;
        AVPixelFormat dst = AV_PIX_FMT_NONE;
        bool srcFull = false;
        bool dstFull = false;
        bool operator==(const Key&) const = default;
    };
    Key key_;
    bool ready_ = false;
    int srcShift_ = 0;
    int dstShift_ = 0;
    std::vector<int> edges_;
    std::vector<std::unique_ptr<SwsContext, detail::SwsDeleter>> contexts_;
};

}  // namespace editor::core
