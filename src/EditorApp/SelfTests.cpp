#include "EditorApp/SelfTests.h"

#include "EditorApp/MainWindow.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/H264Params.h"
#include "EditorCore/MediaProbe.h"
#include "EditorCore/PathUtil.h"
#include "EditorCore/Project.h"
#include "EditorCore/ProjectFile.h"
#include "EditorCore/Proxy.h"
#include "EditorCore/SegmentExport.h"
#include "EditorCore/Thumbnails.h"
#include "EditorCore/TimeUtil.h"
#include "EditorCore/VideoDecoder.h"

#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QToolButton>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>

#include <algorithm>

#include <windows.h>
#include "EditorCore/Encoder.h"
#include <atomic>
#include <thread>
#include <QtWidgets/QFileDialog>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>

namespace editor::app {

namespace {

constexpr int kSmokePlayMilliseconds = 2000;

namespace core = editor::core;

// Esegue una prova: un'eccezione (file che non si apre, esportazione rifiutata...) vuol dire codice 2.
int guarded(const std::function<int()>& test)
{
    try {
        return test();
    } catch (const std::exception& e) {
        std::printf("Errore: %s\n", e.what());
        return 2;
    }
}

// Come l'esportazione (formato del primo file): perché non si può esportare (vuoto = si può, copiando o
// ricodificando). Scrive anche quanti tratti si copiano e quanti si ricodificano.
std::string refusal(const core::Project& project)
{
    const core::ExportPreview preview = core::previewExport(project.exportPlan(core::GapExport::Black), core::AudioEdit{}, nullptr);
    if (preview.problem.empty())
        std::printf("Tratti copiati: %d (%.3f s), ricodificati: %d (%.3f s), tagli precisi: %d (%.3f s ricodificati ai bordi)\n",
                    preview.copiedSegments, preview.copiedSeconds, preview.reencodedSegments, preview.reencodedSeconds,
                    preview.preciseCuts, preview.preciseSeconds);
    return preview.problem.empty() ? std::string() : preview.problem + "\n";
}

// Decodifica tutto il video del file con il decoder software di FFmpeg, attento agli errori: quanti fotogrammi escono
// e quanti errori segnala (es. alle giunzioni, se i parametri di codifica non sono quelli giusti).
struct DecodeCheck
{
    int frames = 0;
    int errors = 0;
};

DecodeCheck decodeAllVideo(const std::string& path)
{
    DecodeCheck check;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        check.errors = 1;
        return check;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) {
        avformat_close_input(&fmt);
        return check;
    }
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[video]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[video]->codecpar);
    ctx->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM;
    ctx->thread_count = 0;  // tutti i processori: i file veri sono lunghi
    if (codec == nullptr || avcodec_open2(ctx, codec, nullptr) < 0) {
        check.errors = 1;
    } else {
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        const auto drain = [&] {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                ++check.frames;
                check.errors += (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) ? 1 : 0;
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == video) {
                if (avcodec_send_packet(ctx, pkt) < 0)
                    ++check.errors;
                drain();
            }
            av_packet_unref(pkt);
        }
        avcodec_send_packet(ctx, nullptr);
        drain();
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return check;
}

struct AnalyzedClip
{
    int id = 0;
    core::MediaInfo info;
};

// Analizza il file e lo aggiunge al progetto con durata e fotogrammi chiave (come fa l'interfaccia dopo l'analisi).
AnalyzedClip addAnalyzedClip(core::Project& project, const std::string& path)
{
    AnalyzedClip clip;
    clip.info = core::probeFile(path);
    clip.id = project.add(path);
    project.setAnalysis(clip.id, clip.info.keyframeTimes, clip.info.durationSeconds);
    return clip;
}

// --join-test <destinazione> <file1> <file2> ...: unisce i file interi come farebbe l'interfaccia (dopo il controllo
// di compatibilità) e controlla il file creato. Codice di uscita 0 se è tutto giusto, 4 se i file non si possono
// unire (il motivo viene scritto), 2 per altri errori, 3 se il controllo del risultato non riesce.
int runJoinTest(const QStringList& args, qsizetype at)
{
    if (at + 3 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --join-test <destinazione> <file1> <file2> [...]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        std::vector<core::MediaSignature> signatures;
        std::vector<core::MediaInfo> sources;
        for (qsizetype i = at + 2; i < args.size(); ++i) {
            const std::string path = args[i].toStdString();
            const AnalyzedClip clip = addAnalyzedClip(project, path);
            sources.push_back(clip.info);
            signatures.push_back(core::readMediaSignature(path));
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds);  // file intero
        }
        if (const std::string problems = refusal(project); !problems.empty()) {
            std::printf("Non si possono unire:\n%s", problems.c_str());
            return 4;
        }
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, core::AudioEdit{});
        if (result.parametersInStream)
            std::printf("Parametri di codifica diversi: ripetuti nel flusso davanti a ogni fotogramma chiave\n");
        const core::MediaInfo joined = core::probeFile(output);
        const double expected = project.sequence().totalLength();
        std::printf("Uniti %d file: durata attesa %.3f s, ottenuta %.3f s, %d flussi\n", result.clipCount, expected,
                    joined.durationSeconds, result.streamCount);

        const double tolerance = 0.1 + 0.05 * static_cast<double>(result.clipCount);
        bool ok = std::abs(joined.durationSeconds - expected) < tolerance
               && joined.streams.size() == static_cast<std::size_t>(result.streamCount);
        for (std::size_t s = 0; ok && s < joined.streams.size() && s < sources.front().streams.size(); ++s)
            ok = joined.streams[s].codec == sources.front().streams[s].codec;
        if (joined.keyframeStreamIndex >= 0)
            ok = ok && !joined.keyframeTimes.empty() && std::abs(joined.keyframeTimes.front()) < 0.001;
        if (joined.keyframeStreamIndex >= 0) {
            const DecodeCheck decoded = decodeAllVideo(output);
            std::printf("Decodifica completa: %d fotogrammi, %d errori\n", decoded.frames, decoded.errors);
            ok = ok && decoded.frames > 0 && decoded.errors == 0;
        }
        std::printf("%s\n", ok ? "Controllo del file unito: OK" : "Controllo del file unito: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// Luminosità media di un fotogramma (per riconoscere da quale punto di quale file arriva).
double meanBrightness(const std::string& file, double seconds)
{
    core::VideoDecoder video(file, {.allowHardware = false});
    const auto frame = video.seek(seconds);
    if (!frame)
        return -1.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < frame->pixels.size(); i += 4)
        sum += (frame->pixels[i] + frame->pixels[i + 1] + frame->pixels[i + 2]) / 3.0;
    return sum / (frame->pixels.size() / 4.0);
}

// --sequence-test <destinazione> <file> <IN s> <OUT s> [<file> <IN s> <OUT s> ...]: costruisce un montaggio con i pezzi
// in quell'ordine (lo stesso file può tornare più volte), lo esporta e controlla durata e ordine: all'inizio di ogni
// pezzo il file creato deve mostrare lo stesso fotogramma del file di origine. Codici come --join-test.
int runSequenceTest(const QStringList& args, qsizetype at)
{
    if (at + 4 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --sequence-test <destinazione> [spazio:<s>] <file> <IN> <OUT> [[spazio:<s>] <file> <IN> <OUT> ...]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        std::map<std::string, int> ids;
        std::vector<core::MediaSignature> signatures;
        std::vector<double> gaps;  // spazio vuoto prima di ogni pezzo ("spazio:<secondi>" prima del file)
        double gap = 0.0;
        for (qsizetype i = at + 2; i < args.size();) {
            if (args[i].startsWith(QStringLiteral("spazio:"))) {
                gap = args[i].mid(7).toDouble();
                ++i;
                continue;
            }
            if (i + 2 >= args.size())
                throw core::MediaError("Ogni pezzo vuole file, IN e OUT.");
            const std::string path = args[i].toStdString();
            if (!ids.contains(path)) {
                ids[path] = addAnalyzedClip(project, path).id;
                signatures.push_back(core::readMediaSignature(path));
            }
            if (project.sequence().insert(-1, ids[path], args[i + 1].toDouble(), args[i + 2].toDouble()) == 0)
                throw core::MediaError("Pezzo vuoto: OUT deve stare dopo IN.");
            gaps.push_back(gap);
            gap = 0.0;
            i += 3;
        }
        // Gli spazi: ogni pezzo spostato più avanti, dall'ultimo al primo (così non si sovrappone a nessuno).
        {
            core::Sequence& s = project.sequence();
            std::vector<double> targets;
            double position = 0.0;
            for (std::size_t k = 0; k < s.fragments().size(); ++k) {
                position += gaps[k];
                targets.push_back(position);
                position += s.fragments()[k].length();
            }
            for (std::size_t k = s.fragments().size(); k-- > 0;)
                s.moveTo(s.fragments()[k].id, targets[k]);
        }
        if (const std::string problems = refusal(project); !problems.empty()) {
            std::printf("Non si possono unire:\n%s", problems.c_str());
            return 4;
        }
        const core::Sequence& sequence = project.sequence();
        // I tratti come li esporterà davvero: con il taglio preciso quelli chiesti, altrimenti (formato che non si sa
        // ricodificare) con l'inizio al fotogramma chiave prima. Su questi si controllano durata e fotogrammi.
        const std::vector<core::ClipRanges> plan = project.exportPlan(core::GapExport::Black);
        const core::ExportPreview preview = core::previewExport(plan, core::AudioEdit{}, nullptr);
        if (!preview.impreciseCuts.empty())
            std::printf("%s\n", preview.impreciseCuts.c_str());
        if (!preview.closedGaps.empty())
            std::printf("%s\n", preview.closedGaps.c_str());
        if (preview.effectiveRanges.size() != sequence.fragments().size())
            throw core::MediaError("Il piano dell'esportazione non corrisponde ai pezzi del montaggio.");
        const core::ExportResult result = core::exportClips(plan, output, core::AudioEdit{});
        if (result.blackGaps > 0)
            std::printf("Spazi neri: %d (%.3f s)\n", result.blackGaps, preview.blackSeconds);
        if (result.parametersInStream)
            std::printf("Parametri di codifica diversi: ripetuti nel flusso davanti a ogni fotogramma chiave\n");
        const DecodeCheck decoded = decodeAllVideo(output);
        std::printf("Decodifica completa: %d fotogrammi, %d errori\n", decoded.frames, decoded.errors);
        const double length = core::probeFile(output).durationSeconds;
        std::printf("Montaggio di %d pezzi (%d tagli precisi): durata attesa %.3f s, ottenuta %.3f s\n", result.segmentCount,
                    result.preciseCuts, preview.outputSeconds, length);
        bool ok = std::abs(length - preview.outputSeconds) < 0.1 + 0.03 * static_cast<double>(result.segmentCount);
        ok = ok && decoded.frames > 0 && decoded.errors == 0;

        // Ordine: subito dopo l'inizio di ogni pezzo, lo stesso fotogramma dell'originale all'inizio esportato davvero.
        // Negli spazi neri, a metà, un fotogramma nero.
        const double fps = signatures.front().streams.empty() || signatures.front().streams.front().frameRate <= 0
                               ? 30.0
                               : signatures.front().streams.front().frameRate;
        double start = 0.0;  // inizio del pezzo nel montaggio
        for (std::size_t k = 0; k < sequence.fragments().size(); ++k) {
            const core::Fragment& f = sequence.fragments()[k];
            const core::TimeRange& exported = preview.effectiveRanges[k];
            if (f.gap > core::kTimeEpsilon && result.blackGaps > 0) {
                const double black = std::llround(f.gap * fps) / fps;  // arrotondato ai fotogrammi, come l'esportazione
                if (black > 0) {
                    const double inBlack = meanBrightness(output, start + black / 2);
                    const bool dark = inBlack >= 0 && inBlack < 8.0;
                    std::printf("  spazio prima del pezzo %d (%.3f s): luminosità %.1f%s\n", static_cast<int>(k) + 1, black,
                                inBlack, dark ? "" : "  <-- NON NERO");
                    ok = ok && dark;
                }
                start += black;
            }
            const double inOutput = meanBrightness(output, start + 0.01);
            start += exported.length();
            const double inSource = meanBrightness(project.find(f.clipId)->path, exported.start + 0.01);
            const bool same = inOutput >= 0 && std::abs(inOutput - inSource) < 3.0;
            std::printf("  pezzo %d (%.3f - %.3f s%s): luminosità %.1f, originale %.1f%s\n", static_cast<int>(k) + 1,
                        f.start, f.end,
                        std::abs(exported.start - f.start) > 1e-6 ? qPrintable(QStringLiteral(", esportato da %1 s").arg(exported.start, 0, 'f', 3)) : "",
                        inOutput, inSource, same ? "" : "  <-- DIVERSO");
            ok = ok && same;
        }
        std::printf("%s\n", ok ? "Controllo del montaggio: OK" : "Controllo del montaggio: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --codec-info <file> [<file> ...]: scrive i parametri dei flussi (per H.264 anche SPS e PPS letti) e, con più file,
// se si possono unire e perché. Serve a capire un rifiuto di unione.
int runCodecInfo(const QStringList& args, qsizetype at)
{
    std::vector<core::MediaSignature> signatures;
    return guarded([&]() -> int {
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const core::MediaSignature s = core::readMediaSignature(args[i].toStdString());
            std::printf("== %s\n", s.fileName.c_str());
            for (const core::StreamSignature& stream : s.streams) {
                if (stream.kind == core::StreamKind::Video) {
                    std::printf("video %s %s %dx%d %s %.3f fps, dati extra %d byte\n", stream.codec.c_str(),
                                stream.profile.c_str(), stream.width, stream.height, stream.pixelFormat.c_str(),
                                stream.frameRate, static_cast<int>(stream.extradata.size()));
                    if (stream.codec == "h264")
                        std::printf("%s", core::describeH264(core::parseH264Config(stream.extradata)).c_str());
                } else {
                    std::printf("audio %s %s %d Hz %s, dati extra %d byte\n", stream.codec.c_str(), stream.profile.c_str(),
                                stream.sampleRate, stream.channelLayout.c_str(), static_cast<int>(stream.extradata.size()));
                }
            }
            signatures.push_back(s);
        }
        // Ogni file rispetto al primo (il formato del progetto, se fosse il primo pezzo del montaggio).
        const core::ProjectFormat format = core::formatOf(signatures.front());
        for (std::size_t c = 1; c < signatures.size(); ++c) {
            const core::ClipFitResult fit = core::classifyClip(signatures[c], format);
            const char* verdict = fit.fit == core::ClipFit::Copy ? "si copia"
                                : fit.fit == core::ClipFit::CopyWithParameters ? "si copia con i suoi parametri nel flusso"
                                                                               : "DA RICODIFICARE";
            std::printf("%s: %s\n", signatures[c].fileName.c_str(), verdict);
            for (const std::string& reason : fit.reasons)
                std::printf("  - %s\n", reason.c_str());
        }
        return 0;
    });
}

// Legge tutto l'audio (`onChunk` vede ogni blocco) e restituisce il buco più lungo fra due blocchi decodificati di
// seguito (alcuni registratori ne lasciano già qualcuno).
double readLongestGap(core::AudioDecoder& audio,
                      const std::function<void(const core::AudioChunk&)>& onChunk = {})
{
    double previousEnd = -1.0;
    double longest = 0.0;
    while (auto chunk = audio.next()) {
        if (previousEnd >= 0)
            longest = std::max(longest, chunk->time - previousEnd);
        previousEnd = chunk->time + chunk->samples.size() / static_cast<double>(audio.channels()) / audio.sampleRate();
        if (onChunk)
            onChunk(*chunk);
    }
    return longest;
}

double longestAudioGap(const std::string& file)
{
    core::AudioDecoder audio(file);
    return readLongestGap(audio);
}

// --audio-test <destinazione> <file> [mute <da s> <a s>]... [replace <audio> <offset s>] [omit]: mette il file intero nel
// montaggio, toglie l'audio dai tratti indicati e/o lo sostituisce, esporta e controlla il file creato: durata,
// codec, audio continuo (nessun buco fra un blocco e l'altro) e silenzio nei tratti. "omit" toglie i pacchetti invece
// di metterci il silenzio (per confronto). Codici come --join-test (4 = audio sostitutivo non utilizzabile).
int runAudioTest(const QStringList& args, qsizetype at)
{
    if (at + 2 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --audio-test <destinazione> <file> [mute <da> <a>]... [replace <audio> <offset>] [omit]\n");
        return 1;
    }
    const std::string output = args[at + 1].toStdString();
    const std::string input = args[at + 2].toStdString();
    return guarded([&]() -> int {
        core::Project project;
        const AnalyzedClip clip = addAnalyzedClip(project, input);
        project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds);
        core::MuteMethod method = core::MuteMethod::Silence;
        for (qsizetype i = at + 3; i < args.size(); ++i) {
            if (args[i] == QStringLiteral("mute") && i + 2 < args.size()) {
                project.sequence().muteRange(args[i + 1].toDouble(), args[i + 2].toDouble());
                i += 2;
            } else if (args[i] == QStringLiteral("replace") && i + 2 < args.size()) {
                const std::string audio = args[i + 1].toStdString();
                if (const std::string problem = core::replacementAudioProblem(audio, output); !problem.empty()) {
                    std::printf("Audio sostitutivo non utilizzabile: %s\n", problem.c_str());
                    return 4;
                }
                project.setAudioReplacement(core::AudioReplacement{audio, args[i + 2].toDouble(), core::AudioDecoder(audio).durationSeconds()});
                i += 2;
            } else if (args[i] == QStringLiteral("omit")) {
                method = core::MuteMethod::Omit;
            }
        }
        const core::AudioEdit edit = project.audioEdit();
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, edit, {}, method);
        const core::MediaInfo exported = core::probeFile(output);
        const double expected = project.sequence().totalLength();
        std::printf("Esportato: durata attesa %.3f s, ottenuta %.3f s; audio: %d pacchetti col silenzio, %d tolti\n",
                    expected, exported.durationSeconds, result.silencedPackets, result.droppedPackets);
        bool ok = std::abs(exported.durationSeconds - expected) < 0.1;

        // Audio del file creato: codec, continuità e livello dentro e fuori dai tratti silenziati.
        std::string audioCodec;
        for (const core::StreamInfo& stream : exported.streams) {
            if (stream.kind == core::StreamKind::Audio && audioCodec.empty())
                audioCodec = stream.codec;
        }
        if (edit.replacement) {
            const core::MediaInfo source = core::probeFile(edit.replacement->path);
            for (const core::StreamInfo& stream : source.streams) {
                if (stream.kind == core::StreamKind::Audio) {
                    std::printf("Audio sostitutivo: %s, nel file creato: %s\n", stream.codec.c_str(), audioCodec.c_str());
                    ok = ok && stream.codec == audioCodec;
                    break;
                }
            }
        }
        if (!audioCodec.empty()) {
            core::AudioDecoder audio(output);
            const int channels = audio.channels();
            const double rate = audio.sampleRate();
            double mutedPeak = 0.0;
            double otherPeak = 0.0;
            const double longestGap = readLongestGap(audio, [&](const core::AudioChunk& chunk) {
                constexpr double kMargin = 0.05;  // ai bordi di un tratto un blocco AAC può essere a metà
                const std::size_t frames = chunk.samples.size() / channels;
                for (std::size_t n = 0; n < frames; ++n) {
                    const double t = chunk.time + n / rate;
                    const double level = std::abs(chunk.samples[n * channels]) / 32768.0;
                    bool inside = false;
                    bool nearMuted = false;
                    for (const core::TimeRange& m : edit.muted) {
                        inside = inside || (t >= m.start + kMargin && t < m.end - kMargin);
                        nearMuted = nearMuted || (t >= m.start - kMargin && t < m.end + kMargin);
                    }
                    if (inside)
                        mutedPeak = std::max(mutedPeak, level);
                    else if (!nearMuted)
                        otherPeak = std::max(otherPeak, level);
                }
            });
            // I buchi già presenti nell'audio di partenza restano (si copia senza ricodificare): non devono essercene
            // di nuovi.
            const double sourceGap = longestAudioGap(edit.replacement ? edit.replacement->path : input);
            std::printf("Audio %s: buco più lungo fra due blocchi %.1f ms (nell'audio di partenza %.1f ms); picco nei "
                        "tratti senza audio %.4f, altrove %.4f\n",
                        audioCodec.c_str(), longestGap * 1000, sourceGap * 1000, mutedPeak, otherPeak);
            ok = ok && (method == core::MuteMethod::Omit || longestGap < std::max(0.03, sourceGap + 0.005))
              && mutedPeak < 0.005;
        }
        std::printf("%s\n", ok ? "Controllo dell'audio: OK" : "Controllo dell'audio: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --project-test <file> [<file> ...]: prova il file di progetto con file veri, in una cartella temporanea. Copia i file
// in una cartella, costruisce un montaggio (con audio tolto e sostituito), lo salva e lo riapre (deve tornare
// identico), sposta la cartella (i file si devono ritrovare dal percorso relativo) ed esporta il progetto riaperto;
// poi toglie un file (deve risultare mancante) e prova un progetto di versione sconosciuta e uno rovinato (rifiutati
// con un messaggio). Codice 0 se è tutto giusto, 3 se un controllo non riesce, 2 per altri errori.
int runProjectTest(const QStringList& args, qsizetype at)
{
    namespace fs = std::filesystem;
    if (at + 1 >= args.size()) {
        std::printf("Uso: EditorVideo.exe --project-test <file> [<file> ...]\n");
        return 1;
    }
    const auto projectFileName = [](const std::string& name) { return name + core::kProjectExtension; };
    const auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        return content.str();
    };
    bool ok = true;
    const auto check = [&ok](bool condition, const char* what) {
        std::printf("  %s: %s\n", what, condition ? "OK" : "NON RIUSCITO");
        ok = ok && condition;
    };
    const fs::path base = fs::temp_directory_path() / "EditorVideoProvaProgetto";
    return guarded([&]() -> int {
        fs::remove_all(base);
        fs::create_directories(base / "originale" / "video");

        // Montaggio: per ogni file il file intero e poi la sua prima metà; audio tolto in un tratto; il primo file come
        // audio sostitutivo, spostato di mezzo secondo.
        core::Project project;
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const fs::path source = fs::path(args[i].toStdWString());
            const fs::path copy = base / "originale" / "video" / source.filename();
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing);
            const AnalyzedClip clip = addAnalyzedClip(project, core::utf8FromPath(copy));
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds);
            project.sequence().insert(-1, clip.id, 0.0, clip.info.durationSeconds / 2);
        }
        const double total = project.sequence().totalLength();
        project.sequence().muteRange(total * 0.2, total * 0.3);
        project.setAudioReplacement(core::AudioReplacement{project.clips().front().path, 0.5, project.clips().front().duration});
        const core::ProjectView view{project.clips().front().id, 0.0, 1.0, 0.5, total / 2};

        const std::string saved = core::utf8FromPath(base / "originale" / projectFileName("montaggio"));
        core::saveProjectFile(saved, project, view);
        std::printf("Progetto salvato: %s (%d file, %d pezzi)\n", saved.c_str(), static_cast<int>(project.clips().size()),
                    static_cast<int>(project.sequence().fragments().size()));

        // Riaperto e salvato di nuovo (dopo aver rifatto l'analisi): stesso testo.
        core::LoadedProject loaded = core::loadProjectFile(saved);
        check(loaded.missing.empty(), "riaperto, nessun file mancante");
        bool sameDurations = true;
        for (const core::Clip& clip : loaded.project.clips()) {
            const core::MediaInfo info = core::probeFile(clip.path);
            loaded.project.setAnalysis(clip.id, info.keyframeTimes, info.durationSeconds);
            sameDurations = sameDurations && std::abs(loaded.savedDurations[clip.id] - info.durationSeconds) < 1e-9;
        }
        check(sameDurations, "durate dei file uguali a quelle salvate");
        const std::string again = core::utf8FromPath(base / "originale" / projectFileName("di_nuovo"));
        core::saveProjectFile(again, loaded.project, loaded.view);
        check(read(core::pathFromUtf8(saved)) == read(core::pathFromUtf8(again)),
              "salvato di nuovo: identico");
        check(loaded.project.sequence().mutedRanges().size() == project.sequence().mutedRanges().size(), "tratti senza audio");
        check(loaded.view.in == view.in && loaded.view.out == view.out, "IN e OUT della Sorgente");

        // Cartella spostata: i file si ritrovano dal percorso relativo, e il progetto riaperto si esporta.
        fs::rename(base / "originale", base / "spostata");
        core::LoadedProject moved =
            core::loadProjectFile(core::utf8FromPath(base / "spostata" / projectFileName("montaggio")));
        bool inside = moved.missing.empty();
        for (const core::Clip& clip : moved.project.clips()) {
            inside = inside && clip.path.find("spostata") != std::string::npos
                  && fs::exists(core::pathFromUtf8(clip.path));
            const core::MediaInfo info = core::probeFile(clip.path);
            moved.project.setAnalysis(clip.id, info.keyframeTimes, info.durationSeconds);
        }
        check(inside, "cartella spostata: file ritrovati dal percorso relativo");
        const std::string extension = core::pathFromUtf8(moved.project.clips().front().path).extension().string();
        const std::string output = core::utf8FromPath(base / ("esportato" + extension));
        core::exportClips(moved.project.exportPlan(), output, moved.project.audioEdit());
        const double length = core::probeFile(output).durationSeconds;
        std::printf("  esportazione del progetto riaperto: %.3f s (attesi %.3f s)\n", length, total);
        check(std::abs(length - total) < 0.1 + 0.05 * static_cast<double>(moved.project.sequence().fragments().size()),
              "esportazione del progetto riaperto");

        // Un file tolto: risulta mancante; senza di lui i suoi pezzi vengono tolti.
        fs::remove(core::pathFromUtf8(moved.project.clips().front().path));
        core::LoadedProject missing =
            core::loadProjectFile(core::utf8FromPath(base / "spostata" / projectFileName("montaggio")));
        std::printf("  file mancanti: %d\n", static_cast<int>(missing.missing.size()));
        const int dropped = core::dropMissing(missing);
        check(!missing.project.audioReplacement() && dropped == 2, "file mancante segnalato e tolto con i suoi 2 pezzi");

        // Versione sconosciuta e file rovinato: rifiutati con un messaggio.
        std::string json = read(base / "spostata" / projectFileName("montaggio"));
        const auto refused = [&](const std::string& text, const char* what) {
            try {
                core::projectFromJson(core::utf8FromPath(base / projectFileName("x")), text);
                check(false, what);
            } catch (const core::ProjectFileError& e) {
                std::printf("  %s -> \"%s\"\n", what, e.what());
                check(true, what);
            }
        };
        std::string newer = json;
        newer.replace(newer.find("\"versione\": 1"), 13, "\"versione\": 9");
        refused(newer, "versione sconosciuta rifiutata");
        refused(json.substr(0, json.size() / 3), "progetto rovinato rifiutato");

        fs::remove_all(base);
        std::printf("%s\n", ok ? "Controllo del progetto: OK" : "Controllo del progetto: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --thumbnail-test <file> [...]: estrae una miniatura per ogni fotogramma chiave (al massimo 20 per file), come la
// timeline del montaggio, con la GPU se c'è, e scrive quanto ci vuole. Codice 0 se per ogni file con video tutte le
// miniature sono uscite, della misura giusta; 3 altrimenti; 2 per altri errori.
int runThumbnailTest(const QStringList& args, qsizetype at)
{
    bool ok = true;
    return guarded([&]() -> int {
        core::ThumbnailExtractor extractor(192, 108, true);
        for (qsizetype i = at + 1; i < args.size(); ++i) {
            const std::string path = args[i].toStdString();
            const core::MediaInfo info = core::probeFile(path);
            std::vector<double> times = info.keyframeTimes;
            if (times.size() > 20)
                times.resize(20);
            QElapsedTimer timer;
            timer.start();
            int made = 0;
            int width = 0;
            int height = 0;
            for (double t : times) {
                if (const auto frame = extractor.extract(path, t)) {
                    ++made;
                    width = frame->width;
                    height = frame->height;
                }
            }
            const double ms = timer.nsecsElapsed() / 1e6;
            std::printf("%s: %d miniature su %d (%dx%d), %.1f ms ciascuna, decodifica %s\n",
                        QFileInfo(args[i]).fileName().toUtf8().constData(), made, static_cast<int>(times.size()), width,
                        height, times.empty() ? 0.0 : ms / times.size(), extractor.decoderDescription().c_str());
            if (info.keyframeStreamIndex >= 0)
                ok = ok && made == static_cast<int>(times.size()) && width > 0 && width <= 192 && height <= 108;
        }
        std::printf("%s\n", ok ? "Controllo delle miniature: OK" : "Controllo delle miniature: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

// --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]: mette i tratti del file nel montaggio,
// nell'ordine dato, li esporta come farebbe l'interfaccia (taglio preciso se si può, altrimenti IN al fotogramma chiave
// prima) e controlla il file creato (durata, codec, fotogramma chiave all'inizio). Codici come --join-test.
int runExportTest(const QStringList& args, qsizetype at)
{
    const qsizetype pairs = (args.size() - at - 3) / 2;
    if (at + 4 >= args.size() || (args.size() - at - 3) % 2 != 0) {
        std::printf("Uso: EditorVideo.exe --export-test <origine> <destinazione> <IN s> <OUT s> [<IN s> <OUT s> ...]\n");
        return 1;
    }
    const std::string input = args[at + 1].toStdString();
    const std::string output = args[at + 2].toStdString();
    return guarded([&]() -> int {
        // Un montaggio con i tratti del file, nell'ordine dato.
        core::Project project;
        const AnalyzedClip clip = addAnalyzedClip(project, input);
        const core::MediaInfo& source = clip.info;
        const int id = clip.id;
        core::Sequence& sequence = project.sequence();
        for (qsizetype i = 0; i < pairs; ++i) {
            const double in = core::clampTo(args[at + 3 + 2 * i].toDouble(), source.durationSeconds);
            const double out = core::clampTo(args[at + 4 + 2 * i].toDouble(), source.durationSeconds);
            if (sequence.insert(-1, id, in, out) == 0)
                throw core::MediaError("OUT deve stare dopo IN.");
        }
        for (const core::Fragment& f : sequence.fragments())
            std::printf("Tratto: %.3f - %.3f s\n", f.start, f.end);

        // Durata attesa: quella dei tratti con il taglio preciso; se non si può, IN va al fotogramma chiave prima.
        const core::ExportPreview preview = core::previewExport(project.exportPlan(), core::AudioEdit{}, nullptr);
        if (!preview.impreciseCuts.empty())
            std::printf("%s\n", preview.impreciseCuts.c_str());
        const core::ExportResult result = core::exportClips(project.exportPlan(), output, core::AudioEdit{});
        const core::MediaInfo exported = core::probeFile(output);
        std::printf("Esportato: %d tratti (%d tagli precisi), durata attesa %.3f s, ottenuta %.3f s, %d flussi\n",
                    result.segmentCount, result.preciseCuts, preview.outputSeconds, exported.durationSeconds,
                    result.streamCount);

        // Tolleranza: un paio di fotogrammi per giunzione.
        const double tolerance = 0.1 + 0.05 * static_cast<double>(sequence.fragments().size() - 1);
        bool ok = exported.streams.size() == static_cast<std::size_t>(result.streamCount)
               && std::abs(exported.durationSeconds - preview.outputSeconds) < tolerance;
        // Stessi codec dell'originale, nello stesso ordine (video e audio).
        std::size_t next = 0;
        for (const core::StreamInfo& stream : source.streams) {
            if (stream.kind != core::StreamKind::Video && stream.kind != core::StreamKind::Audio)
                continue;
            if (stream.attachedPicture)
                continue;
            ok = ok && next < exported.streams.size() && exported.streams[next].codec == stream.codec;
            ++next;
        }
        // Il video deve partire con un fotogramma chiave a 0.
        if (exported.keyframeStreamIndex >= 0)
            ok = ok && !exported.keyframeTimes.empty() && std::abs(exported.keyframeTimes.front()) < 0.001;
        std::printf("%s\n", ok ? "Controllo del file esportato: OK" : "Controllo del file esportato: NON RIUSCITO");
        return ok ? 0 : 3;
    });
}

struct CommandLineTest
{
    const char* option;
    int (*run)(const QStringList& args, qsizetype at);
};

// Nell'ordine in cui si cercano fra gli argomenti: vale la prima trovata.
constexpr CommandLineTest kTests[] = {
    {"--export-test", runExportTest},   {"--thumbnail-test", runThumbnailTest}, {"--project-test", runProjectTest},
    {"--audio-test", runAudioTest},     {"--codec-info", runCodecInfo},         {"--sequence-test", runSequenceTest},
    {"--join-test", runJoinTest},
};

}  // namespace

std::optional<int> runCommandLineTest(const QStringList& arguments)
{
    for (const CommandLineTest& test : kTests) {
        if (const qsizetype at = arguments.indexOf(QLatin1String(test.option)); at >= 0) {
            const int code = test.run(arguments, at);
            std::fflush(stdout);
            return code;
        }
    }
    return std::nullopt;
}

namespace {

// La casella del nome del file nella finestra di Windows per salvare (Edit dentro ComboBox dentro ComboBoxEx32).
HWND fileNameEdit(HWND dialog)
{
    HWND found = nullptr;
    EnumChildWindows(
        dialog,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t name[64] = {};
            GetClassNameW(child, name, 64);
            wchar_t parent[64] = {};
            GetClassNameW(GetParent(child), parent, 64);
            if (std::wstring(name) == L"Edit" && std::wstring(parent) == L"ComboBox") {
                *reinterpret_cast<HWND*>(param) = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

}  // namespace

// --export-dialog-test <progetto> <destinazione>: come l'utente: apre il progetto, Esporta, Invio nella finestra del
// riepilogo, il nome del file nella finestra di Windows e Salva, chiude il messaggio finale. Un thread a parte guida le
// finestre (senza mouse) e dice a che punto si ferma. Codice 0 se arriva in fondo, 3 se si blocca.
int runExportDialogTest(QApplication& app, const QString& project, const QString& output)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MainWindow window;
    window.show();
    if (!window.openProject(project, false))
        return 2;
    QElapsedTimer waiting;
    waiting.start();
    while (!window.project().allAnalyzed() && waiting.elapsed() < 60000)
        app.processEvents(QEventLoop::AllEvents, 50);
    QFile::remove(output);
    std::atomic<int> stage{0};  // 1 riepilogo confermato, 2 nome del file dato, 3 messaggio finale chiuso
    std::atomic<bool> stop{false};
    const std::wstring target = QDir::toNativeSeparators(output).toStdWString();
    std::thread driver([&] {
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (stage == 0) {
                if (HWND h = FindWindowW(nullptr, L"Esporta il montaggio")) {
                    std::printf("  [%.1f s] riepilogo: Invio\n", waiting.elapsed() / 1000.0);
                    PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
                    PostMessageW(h, WM_KEYUP, VK_RETURN, 0xC0000001);
                    stage = 1;
                }
            } else if (stage == 1) {
                if (HWND h = FindWindowW(L"#32770", L"Esporta")) {
                    if (HWND edit = fileNameEdit(h)) {
                        std::printf("  [%.1f s] finestra di salvataggio: nome e Salva\n", waiting.elapsed() / 1000.0);
                        SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(target.c_str()));
                        PostMessageW(h, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                        stage = 2;
                    }
                }
            } else if (stage == 2) {
                for (const wchar_t* title : {L"Esportazione completata", L"Esportazione non riuscita"}) {
                    if (HWND h = FindWindowW(nullptr, title)) {
                        std::printf("  [%.1f s] messaggio finale: %ls\n", waiting.elapsed() / 1000.0, title);
                        PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
                        PostMessageW(h, WM_KEYUP, VK_RETURN, 0xC0000001);
                        stage = 3;
                    }
                }
            }
        }
    });
    waiting.restart();
    QTimer::singleShot(0, &window, [&window] { window.exportDialog(); });
    while (stage < 3 && waiting.elapsed() < 120000)
        app.processEvents(QEventLoop::AllEvents, 50);
    // Ancora un po' perché il messaggio si chiuda e l'esportazione sia registrata.
    QElapsedTimer tail;
    tail.start();
    while (tail.elapsed() < 1000)
        app.processEvents(QEventLoop::AllEvents, 50);
    stop = true;
    driver.join();
    const char* stages[] = {"riepilogo mai comparso", "finestra di salvataggio mai comparsa",
                            "messaggio finale mai comparso", "arrivata in fondo"};
    std::printf("Esportazione dal pulsante: %s, %s (file %s)\n", stages[stage.load()],
                window.lastExportSucceeded() ? "riuscita" : qPrintable(QStringLiteral("non riuscita: ") + window.lastExportProblem()),
                QFile::exists(output) ? "creato" : "assente");
    return stage == 3 && window.lastExportSucceeded() ? 0 : 3;
}

int runProjectExportTest(QApplication& app, const QString& project, const QString& output)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    MainWindow window;
    window.show();
    if (!window.openProject(project, false)) {
        std::printf("Il progetto non si apre\n");
        return 2;
    }
    QElapsedTimer waiting;
    waiting.start();
    while (!window.project().allAnalyzed() && waiting.elapsed() < 60000)
        app.processEvents(QEventLoop::AllEvents, 50);
    const core::ExportPreview preview = core::previewExport(window.project().exportPlan(), window.project().audioEdit(),
                                                            &window.project().format());
    std::printf("Pezzi copiati %d, ricodificati %d%s%s\n", preview.copiedSegments, preview.reencodedSegments,
                preview.problem.empty() ? "" : ": ", preview.problem.c_str());
    QFile::remove(output);
    QElapsedTimer exporting;
    exporting.start();
    if (!window.startExport(QDir::toNativeSeparators(output), false)) {
        std::printf("Esportazione non partita\n");
        return 2;
    }
    int lastPercent = -1;
    while (window.isExporting() && exporting.elapsed() < 300000) {
        app.processEvents(QEventLoop::AllEvents, 50);
        if (const int percent = static_cast<int>(exporting.elapsed() / 5000); percent != lastPercent) {
            lastPercent = percent;
            std::printf("  ... %.0f s\n", exporting.elapsed() / 1000.0);
        }
    }
    if (window.isExporting()) {
        std::printf("Esportazione BLOCCATA: dopo %.0f s non è ancora finita\n", exporting.elapsed() / 1000.0);
        return 3;
    }
    std::printf("Esportazione %s in %.1f s%s%s\n", window.lastExportSucceeded() ? "riuscita" : "NON riuscita",
                exporting.elapsed() / 1000.0, window.lastExportSucceeded() ? "" : ": ",
                qPrintable(window.lastExportProblem()));
    return window.lastExportSucceeded() ? 0 : 2;
}

int runDockTest(QApplication& app)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // ogni riga subito, anche se la prova si interrompe
    std::printf("Prova dei pannelli con la finestra vera\n");
    MainWindow window;
    window.setTimelinePollAutomatic(false);  // i passi li dà la prova
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    // EDITORVIDEO_DOCK_TEST_SAVED=1: con la disposizione salvata dall'utente e la finestra ingrandita (come la usa lui).
    if (qEnvironmentVariableIntValue("EDITORVIDEO_DOCK_TEST_SAVED") != 0) {
        window.restoreLayout();
        window.showMaximized();
    } else {
        window.resize(1200, 800);
        window.show();
    }
    QDockWidget* timeline = window.timelineDock();
    // Aspetta davvero (i riagganci partono da timer): processEvents da solo ritorna appena non c'è niente da fare.
    const auto settle = [&](int ms = 250) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            app.processEvents(QEventLoop::AllEvents, 20);
    };
    settle();
    const QRect frame = window.frameGeometry();
    const QPoint low = frame.center() + QPoint(0, frame.height() / 4);
    const QPoint high = frame.center() - QPoint(0, frame.height() / 4);
    // Lo spostamento che farebbe Windows trascinando il titolo: inizio, finestra spostata dov'è il cursore, fine.
    QPoint dropAt = low;
    int previews = 0;
    const auto windowsMove = [&](void* hwnd, QPoint) {
        const auto h = static_cast<HWND>(hwnd);
        window.setTestCursor(frame.center());
        // Come con il mouse vero: Qt riceve la pressione sul titolo di Windows (e avvierebbe il suo trascinamento dei
        // pannelli, che poi non vede mai finire perché il rilascio se lo prende lo spostamento di Windows).
        {
            const QPoint grab = timeline->mapToGlobal(QPoint(timeline->width() / 2, -8));
            QMouseEvent press(QEvent::NonClientAreaMouseButtonPress, QPointF(timeline->mapFromGlobal(grab)), QPointF(grab),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline, &press);
        }
        SendMessageW(h, WM_ENTERSIZEMOVE, 0, 0);
        settle(60);
        window.setTestCursor(dropAt);
        SetWindowPos(h, nullptr, dropAt.x() - 200, dropAt.y() - 12, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        window.timelinePollStep(true, dropAt);  // un passo del controllo periodico durante lo spostamento
        previews += window.isDockPreviewVisible() == (dropAt == low) ? 1 : 0;
        SendMessageW(h, WM_EXITSIZEMOVE, 0, 0);
    };
    window.setNativeDragForTest(windowsMove);
    const auto docked = [&](const char* how, int round) {
        settle();
        // I pannelli di sopra devono finire sopra la linea del tempo (non restare alti fino in fondo, coprendola).
        int overlap = 0;
        for (QDockWidget* dock : {window.filesDock(), window.sourceDock(), window.programDock()}) {
            if (dock->isVisible() && !dock->isFloating())
                overlap = std::max(overlap, dock->geometry().bottom() - timeline->geometry().top() + 1);
        }
        const bool ok = !timeline->isFloating() && window.dockWidgetArea(timeline) == Qt::BottomDockWidgetArea
                     && timeline->width() >= window.width() - 30 && timeline->height() >= 225 && overlap <= 0
                     && timeline->isVisible() && !timeline->testAttribute(Qt::WA_NativeWindow);
        std::printf("  %s, giro %d: %s (staccata %s, larga %d su %d, alta %d, sovrapposta %d px)\n", how, round,
                    ok ? "OK" : "NON riuscito", timeline->isFloating() ? "sì" : "no", timeline->width(), window.width(),
                    timeline->height(), overlap);
        return ok;
    };
    const auto undock = [&] {
        timeline->setAttribute(Qt::WA_ShowWithoutActivating);
        timeline->setFloating(true);
        settle(100);
        return timeline->isFloating();
    };
    // Agganciata, presa dal titolo e trascinata (eventi di Qt: pressione, movimento oltre la soglia).
    const auto dragFromDockedTitle = [&](QPoint to) {
        dropAt = to;
        const QPoint press(timeline->width() / 2, 6);
        const QPoint global = timeline->mapToGlobal(press);
        QMouseEvent down(QEvent::MouseButtonPress, QPointF(press), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(timeline, &down);
        const QPoint moved = press + QPoint(0, -30);
        QMouseEvent move(QEvent::MouseMove, QPointF(moved), QPointF(timeline->mapToGlobal(moved)), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(timeline, &move);  // qui parte lo spostamento (della prova, al posto di Windows)
    };
    int good = 0;
    int stays = 0;
    for (int round = 1; round <= 5; ++round) {
        // 1. Agganciata, trascinata dal titolo e lasciata nella metà bassa.
        dragFromDockedTitle(low);
        good += docked("trascinata dal titolo e lasciata in basso", round) ? 1 : 0;
        // 2. Agganciata, trascinata dal titolo e lasciata in alto: resta staccata; poi trascinata di nuovo (ora è già
        //    staccata: il titolo è di Windows) e lasciata in basso.
        dragFromDockedTitle(high);
        settle();
        stays += timeline->isFloating() ? 1 : 0;
        if (timeline->isFloating()) {
            dropAt = low;
            windowsMove(reinterpret_cast<void*>(timeline->windowHandle()->winId()), QPoint());
        }
        good += docked("staccata, poi trascinata di nuovo e lasciata in basso", round) ? 1 : 0;
        // 3. Doppio clic sul titolo di Windows della finestra staccata.
        if (undock()) {
            const QPoint title = timeline->mapToGlobal(QPoint(timeline->width() / 2, -8));
            PostMessageW(reinterpret_cast<HWND>(timeline->windowHandle()->winId()), WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM(title.x(), title.y()));
        }
        good += docked("doppio clic sul titolo", round) ? 1 : 0;
        // 4. Menu.
        if (undock())
            window.redockTimeline();
        good += docked("menu Riaggancia", round) ? 1 : 0;
    }
    std::printf("  lasciata in alto resta staccata: %d su 5; anteprima giusta durante lo spostamento: %d su 15\n", stays,
                previews);
    // Finestra > Ripristina disposizione dopo tutti questi sganci e riagganci: torna quella iniziale, senza sovrapposizioni.
    window.resetLayout();
    const bool resetOk = docked("Ripristina disposizione", 0);
    good += resetOk ? 1 : 0;
    // Con EDITORVIDEO_DOCK_TEST_CAPTURE=immagine.png un'immagine della finestra alla fine.
    if (const QString capture = qEnvironmentVariable("EDITORVIDEO_DOCK_TEST_CAPTURE"); !capture.isEmpty())
        window.grab().save(capture);

    // Il titolo di un pannello agganciato con il mouse sopra: disegnato davvero con il colore di evidenziazione.
    QDockWidget* source = window.sourceDock();
    const QPointF titlePoint(source->width() / 2.0, 5);
    QHoverEvent hover(QEvent::HoverMove, titlePoint, source->mapToGlobal(titlePoint), QPointF(-1, -1));
    QCoreApplication::sendEvent(source, &hover);
    settle();
    const QImage lit = source->grab().toImage();
    const QColor wanted = source->palette().color(QPalette::Highlight);
    const QColor got = lit.pixelColor(lit.width() / 3, static_cast<int>(5 * lit.devicePixelRatio()));
    const bool titleOk = window.isTitleHighlighted(source) && std::abs(got.red() - wanted.red()) < 30
                      && std::abs(got.green() - wanted.green()) < 30 && std::abs(got.blue() - wanted.blue()) < 30;
    std::printf("  titolo evidenziato: colore %s, atteso %s: %s\n", qPrintable(got.name()), qPrintable(wanted.name()),
                titleOk ? "OK" : "NO");
    const bool ok = good == 21 && stays == 5 && previews == 15 && titleOk;
    std::printf("%s (%d su 21)\n", ok ? "Controllo dei pannelli: OK" : "Controllo dei pannelli: NON RIUSCITO", good);
    return ok ? 0 : 3;
}

void startSmokeTest(QApplication& app, MainWindow& window, const QString& file)
{
    std::printf("Piattaforma Qt: %s\n%s\n", qPrintable(QGuiApplication::platformName()),
                qPrintable(versionSummary()));
    std::fflush(stdout);
    if (file.isEmpty()) {
        QTimer::singleShot(0, &app, &QApplication::quit);
    } else {
        PlayerWidget* player = window.player();
        // Il tempo vero dall'inizio della riproduzione: su un PC lento il controllo può arrivare più tardi dei 2 s.
        auto playStart = std::make_shared<QElapsedTimer>();
        QTimer::singleShot(0, &app, [player, playStart] {
            player->togglePlayPause();
            playStart->start();
        });
        QTimer::singleShot(kSmokePlayMilliseconds, &app, [&app, player, playStart, window = &window, file] {
            const double position = player->currentPosition();
            const double expected = playStart->isValid() ? playStart->elapsed() / 1000.0 : kSmokePlayMilliseconds / 1000.0;
            std::printf("Riproduzione: %s, posizione %.3f s dopo %.1f s (passati davvero %.3f s), %s\n",
                        player->isPlaying() ? "in corso" : "ferma", position, kSmokePlayMilliseconds / 1000.0, expected,
                        qPrintable(player->statusText()));
            std::fflush(stdout);
            // Codice di uscita 2 se la riproduzione non è partita o non avanza col tempo giusto (±25%).
            bool ok = player->isPlaying() && std::abs(position - expected) < expected * 0.25;
            if (!ok)
                std::printf("  NON riuscito: la riproduzione %s\n",
                            player->isPlaying() ? "non avanza col tempo giusto" : "non è partita o si è fermata");

            // Segmento: pausa, IN qui, 5 fotogrammi avanti, OUT. La posizione si rilegge dopo la pausa: fra la lettura
            // di sopra e questo punto la riproduzione va ancora avanti (con un file solo audio anche più di 0,1 s).
            if (player->isPlaying())
                player->togglePlayPause();
            const double pausedAt = player->currentPosition();
            player->markIn();
            for (int i = 0; i < 5; ++i)
                player->stepFrame(+1);
            player->markOut();
            const core::CutSegment& segment = player->segment();
            const auto& keyframes = segment.keyframes();
            const bool inOnKeyframe = keyframes.empty()
                                   || std::any_of(keyframes.begin(), keyframes.end(),
                                                  [&](double k) { return std::abs(k - segment.in()) < 1e-6; });
            std::printf("Segmento: IN %.3f s, OUT %.3f s, durata %.3f s%s\n", segment.in(), segment.out(),
                        segment.length(), inOnKeyframe ? "" : " (IN fra due fotogrammi chiave: taglio preciso)");
            std::fflush(stdout);
            // IN resta dove lo si mette (taglio preciso): non oltre la posizione della pausa.
            const bool segmentOk = segment.hasIn() && segment.hasOut() && segment.in() <= pausedAt + 0.1
                                && segment.length() > 0;
            if (!segmentOk)
                std::printf("  NON riuscito: segmento IN-OUT non valido\n");
            ok = ok && segmentOk;

            // Pulsanti dei player: velocità 2x, stop (torna al punto di partenza), all'indietro e pausa (la spola
            // finisce). Il tempo si misura davvero, con margini larghi: su un PC lento i tick arrivano in ritardo.
            {
                const auto runFor = [](int milliseconds) {
                    QElapsedTimer t;
                    t.start();
                    while (t.elapsed() < milliseconds)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    return t.elapsed() / 1000.0;
                };
                player->seekTo(0.0);
                player->setBaseSpeed(2.0);
                player->play();
                const double fastElapsed = runFor(600);
                const double fast = player->currentPosition();
                player->togglePlayPause();
                player->setBaseSpeed(1.0);
                const bool fastOk = std::abs(fast - 2.0 * fastElapsed) < 2.0 * fastElapsed * 0.35;
                player->stop();
                const bool stopOk = player->currentPosition() < 0.05 && !player->isPlaying();
                bool reverseOk = true;
                double reverseFrom = pausedAt;
                double reverseTo = pausedAt;
                if (pausedAt > 0.6) {
                    player->seekTo(pausedAt);
                    player->shuttle(-1);
                    runFor(300);
                    reverseTo = player->currentPosition();
                    reverseOk = player->speed() < 0 && reverseTo < reverseFrom - 0.1;
                    player->togglePlayPause();
                    reverseOk = reverseOk && player->speed() == 1.0 && !player->isPlaying();
                }
                std::printf("Comandi: 2x %.2f s in %.2f s %s, stop %s, all'indietro da %.2f a %.2f s %s\n", fast,
                            fastElapsed, fastOk ? "ok" : "NO", stopOk ? "ok" : "NO", reverseFrom, reverseTo,
                            reverseOk ? "ok" : "NO");
                std::fflush(stdout);
                ok = ok && fastOk && stopOk && reverseOk;
            }

            // Proxy: anteprima a 1/2, si aspetta il proxy del file (creato in sottofondo con l'encoder della scheda
            // video o di Windows) e la Sorgente passa a leggerlo restando allo stesso punto; poi la qualità torna com'era
            // e il proxy fatto per la prova si cancella.
            if (player->statusText().contains(QStringLiteral("Decodifica")) && core::preferredVideoEncoder(core::VideoCodec::H264)) {
                const int quality = window->previewQuality(window->currentFile());
                const double at = std::min(pausedAt, 0.5);
                player->seekTo(at);
                QElapsedTimer wait;
                wait.start();
                window->setPreviewQuality(window->currentFile(), 2);
                while (!player->statusText().contains(QStringLiteral("proxy")) && wait.elapsed() < 60000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                const bool usesProxy = player->statusText().contains(QStringLiteral("proxy"));
                const double position = player->currentPosition();
                const bool positionOk = std::abs(position - at) < 0.05;
                std::printf("Proxy 1/2: %s in %.1f s, posizione %.3f s (era %.3f) %s\n",
                            usesProxy ? "in uso" : "NON in uso", wait.elapsed() / 1000.0, position, at, positionOk ? "ok" : "NO");
                std::fflush(stdout);
                window->setPreviewQuality(window->currentFile(), quality);
                if (quality != 2) {
                    const std::string name = core::proxyFileName(QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()).toStdString(), 2);
                    QFile::remove(QDir(window->proxyFolder()).filePath(QString::fromStdString(name)));
                }
                ok = ok && usesProxy && positionOk;
            }

            const auto finish = [&app, window](bool success) {
                // Con EDITORVIDEO_SMOKE_SCREENSHOT=percorso.png salva anche un'immagine della finestra.
                const QString screenshot = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
                if (!screenshot.isEmpty()) {
                    QCoreApplication::processEvents();  // applica i cambi di layout ancora in sospeso
                    window->grab().save(screenshot);
                }
                app.exit(success ? 0 : 2);
            };

            // Montaggio: il pezzo appena segnato, poi uno all'inizio dello stesso file (A), poi gli altri file
            // interi.
            window->addToSequence();
            player->seekTo(0.0);
            player->markIn();
            for (int i = 0; i < 3; ++i)
                player->stepFrame(+1);
            player->markOut();
            window->addToSequence();

            // Con più file si aspetta che siano stati analizzati tutti (servono durata e fotogrammi chiave).
            QElapsedTimer analysis;
            analysis.start();
            while (!window->project().allAnalyzed() && analysis.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const int firstClip = window->currentClipId();
            for (const core::Clip& clip : window->project().clips()) {
                if (clip.id != firstClip) {
                    window->selectClip(clip.id);
                    window->addToSequence();  // senza IN e OUT: il file intero
                }
            }
            const core::Sequence& sequence = window->project().sequence();
            const std::size_t expectedPieces = 2 + window->project().clips().size() - 1;
            std::printf("Montaggio: %d pezzi, durata %.3f s\n", static_cast<int>(sequence.fragments().size()),
                        sequence.totalLength());
            std::fflush(stdout);
            ok = ok && sequence.fragments().size() == expectedPieces;

            // Audio: un tratto silenziato a cavallo della prima giunzione (M), poi le forme d'onda pronte.
            const double muteFrom = std::max(0.0, sequence.startOf(1) - 0.1);
            window->setAudioSelection(muteFrom, muteFrom + 0.2);
            window->muteSelection();
            const auto muted = sequence.mutedRanges();
            QElapsedTimer waves;
            waves.start();
            while (window->pendingWaveforms() > 0 && waves.elapsed() < 20000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            // Annulla e Ripeti: il silenzio appena messo sparisce e torna; l'asterisco segue.
            {
                const QString label = window->undoText();
                const bool undone = window->undo() && sequence.mutedRanges().empty();
                const bool redone = window->redo() && sequence.mutedRanges().size() == 1;
                std::printf("Annulla / Ripeti: \"%s\" %s, ripetuto %s\n", qPrintable(label.mid(1)),
                            undone ? "annullato" : "NON annullato", redone ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && undone && redone && label.contains(QStringLiteral("Togli audio"));
            }

            // Miniature della timeline: si disegna la finestra (che le chiede) e si aspetta che siano pronte.
            {
                window->grab();
                QElapsedTimer thumbs;
                thumbs.start();
                while (window->pendingThumbnails() > 0 && thumbs.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                const bool hasVideo = window->player()->statusText().contains(QStringLiteral("Decodifica"));
                std::printf("Miniature: %d pronte, %d in attesa\n", window->readyThumbnails(), window->pendingThumbnails());
                std::fflush(stdout);
                ok = ok && window->pendingThumbnails() == 0 && (!hasVideo || window->readyThumbnails() > 0);
            }

            // Zoom delle due linee del tempo: al massimo (12 pixel per fotogramma), disegnate, scorse fino in fondo e
            // di nuovo "Adatta". Con lo zoom al massimo si chiedono solo le miniature visibili.
            {
                bool zoomOk = true;
                for (ZoomableTimeline* timeline :
                     {static_cast<ZoomableTimeline*>(window->sequenceTimeline()), static_cast<ZoomableTimeline*>(window->player()->timeline())}) {
                    for (int i = 0; i < 80; ++i)
                        timeline->zoomIn();
                    const auto& zoom = timeline->zoom();
                    const bool atMax = zoom.duration() <= 0
                                    || std::abs(zoom.pixelsPerSecond() - zoom.maxPixelsPerSecond()) < 1e-6;
                    timeline->grab();
                    QWheelEvent wheel(QPointF(10, 10), timeline->mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, -120 * 50),
                                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                    QCoreApplication::sendEvent(timeline, &wheel);
                    timeline->grab();
                    // Oltre "Adatta": tutto in un decimo della larghezza, senza barra di scorrimento.
                    for (int i = 0; i < 80; ++i)
                        timeline->zoomOut();
                    const bool atMin = zoom.duration() <= 0
                                    || (std::abs(zoom.contentWidth() - zoom.width() * 0.1) < 1.0 && !zoom.canScroll());
                    timeline->grab();
                    timeline->fitAll();
                    timeline->grab();
                    zoomOk = zoomOk && atMax && atMin && timeline->zoom().isFit();
                }
                QCoreApplication::processEvents();
                std::printf("Zoom delle linee del tempo: massimo, minimo e Adatta %s, miniature in attesa %d\n", zoomOk ? "OK" : "NON riusciti",
                            window->pendingThumbnails());
                std::fflush(stdout);
                ok = ok && zoomOk && window->pendingThumbnails() < 200;
                QElapsedTimer thumbs;
                thumbs.start();
                while (window->pendingThumbnails() > 0 && thumbs.elapsed() < 20000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }

            // Pannelli: la Sorgente staccata come finestra durante la riproduzione (il video deve continuare ad andare
            // e a mostrarsi), riagganciata, a schermo intero e ritorno; disposizione salvata e ripristinata.
            {
                const auto advances = [&](const char* when) {
                    const double from = player->currentPosition();
                    QElapsedTimer t;
                    t.start();
                    while (t.elapsed() < 700)
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    const double moved = player->currentPosition() - from;
                    std::printf("  %s: la riproduzione avanza di %.2f s in 0,7 s (%s)\n", when, moved,
                                qPrintable(player->statusText()));
                    return moved > 0.3;
                };
                QDockWidget* source = window->sourceDock();
                player->seekTo(0.0);
                player->play();
                bool docksOk = advances("agganciata");
                source->setFloating(true);
                docksOk = advances("staccata") && source->isFloating() && docksOk;
                source->setFloating(false);
                docksOk = advances("riagganciata") && !source->isFloating() && docksOk;
                window->toggleFullScreen(source);
                const bool full = window->isPanelFullScreen() && source->isFloating();
                docksOk = advances("a schermo intero") && full && docksOk;
                window->toggleFullScreen(source);
                docksOk = docksOk && !window->isPanelFullScreen() && !source->isFloating();
                player->pause();

                // Disposizione: salvata, cambiata (pannello spostato e uno nascosto), ripristinata.
                const QByteArray state = window->saveState();
                const Qt::DockWidgetArea area = window->dockWidgetArea(window->filesDock());
                window->addDockWidget(Qt::RightDockWidgetArea, window->filesDock());
                window->timelineDock()->hide();
                const bool changed = window->dockWidgetArea(window->filesDock()) == Qt::RightDockWidgetArea;
                const bool restored = window->restoreState(state) && window->dockWidgetArea(window->filesDock()) == area
                                   && window->timelineDock()->isVisible();
                window->timelineDock()->hide();
                window->resetLayout();
                const bool reset = window->timelineDock()->isVisible() && !window->sourceDock()->isFloating();

                // La linea del tempo staccata e riagganciata torna sotto a tutto, larga quanto la finestra.
                QDockWidget* timelineDock = window->timelineDock();
                timelineDock->setFloating(true);
                QCoreApplication::processEvents();
                timelineDock->setFloating(false);
                window->addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
                QCoreApplication::processEvents();
                const bool fullWidth = window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea
                                    && timelineDock->width() >= window->width() - 30;

                // Staccata e lasciata dall'utente: nella metà alta della finestra resta staccata, nella metà bassa
                // (a pulsante rilasciato) si riaggancia sotto a tutto; il comando del menu la riaggancia sempre.
                timelineDock->setFloating(true);
                QCoreApplication::processEvents();
                const QRect frame = window->frameGeometry();
                const bool highStays = !window->redockTimelineIfDroppedLow(frame.center() - QPoint(0, frame.height() / 4), false)
                                    && timelineDock->isFloating();
                const bool heldStays = !window->redockTimelineIfDroppedLow(frame.center() + QPoint(0, frame.height() / 4), true)
                                    && timelineDock->isFloating();
                const bool lowDocks = window->redockTimelineIfDroppedLow(frame.center() + QPoint(0, frame.height() / 4), false)
                                   && !timelineDock->isFloating()
                                   && window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea;
                QCoreApplication::processEvents();
                QCoreApplication::processEvents();
                const bool lowFull = timelineDock->width() >= window->width() - 30 && timelineDock->height() >= 225;
                const int lowHeight = timelineDock->height();
                timelineDock->setFloating(true);
                window->redockTimeline();
                QCoreApplication::processEvents();
                QCoreApplication::processEvents();
                const bool menuDocks = !timelineDock->isFloating() && timelineDock->width() >= window->width() - 30
                                    && timelineDock->height() >= 225;
                const int menuHeight = timelineDock->height();
                // Più alta prima di staccarla: riagganciata riprende quell'altezza.
                window->resizeDocks({timelineDock}, {400}, Qt::Vertical);
                QCoreApplication::processEvents();
                const int before = timelineDock->height();
                timelineDock->setFloating(true);
                QCoreApplication::processEvents();
                timelineDock->setFloating(false);  // come il doppio clic sul titolo
                QCoreApplication::processEvents();
                QCoreApplication::processEvents();
                const bool keptHeight = !timelineDock->isFloating() && std::abs(timelineDock->height() - before) <= 4;
                std::printf("  altezza della linea del tempo riagganciata: %d px (in basso), %d px dal menu, %d px dopo il doppio "
                            "clic (prima %d)\n",
                            lowHeight, menuHeight, timelineDock->height(), before);
                std::printf("  linea del tempo lasciata in alto: %s, trascinata: %s, lasciata in basso: %s, dal menu: %s\n",
                            highStays ? "resta staccata" : "AGGANCIATA", heldStays ? "resta staccata" : "AGGANCIATA",
                            lowDocks && lowFull ? "riagganciata a tutta larghezza" : "NON riagganciata",
                            menuDocks ? "riagganciata" : "NON riagganciata");
                // Cicli ripetuti, come farebbe l'utente: staccata, trascinata (pulsante premuto, finestra spostata) e lasciata
                // nella metà bassa; poi staccata e riagganciata con il doppio clic; poi dal menu. Ogni volta in basso, a
                // tutta larghezza e alta almeno 230 px.
                int cycles = 0;
                for (int i = 0; i < 5; ++i) {
                    const auto docked = [&] {
                        QElapsedTimer wait;  // i riagganci partono da timer: si aspetta davvero
                        wait.start();
                        while (wait.elapsed() < 250)
                            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                        return !timelineDock->isFloating() && window->dockWidgetArea(timelineDock) == Qt::BottomDockWidgetArea
                            && timelineDock->width() >= window->width() - 30 && timelineDock->height() >= 225;
                    };
                    timelineDock->setFloating(true);
                    QCoreApplication::processEvents();
                    window->timelinePollStep(true, frame.center() - QPoint(0, frame.height() / 4));  // premuto sul titolo
                    timelineDock->move(timelineDock->pos() + QPoint(30, 120));                    // trascinata
                    window->timelinePollStep(true, frame.center() + QPoint(0, frame.height() / 4));
                    window->timelinePollStep(false, frame.center() + QPoint(0, frame.height() / 4));  // lasciata in basso
                    const bool dragged = docked();
                    timelineDock->setFloating(true);
                    QCoreApplication::processEvents();
                    timelineDock->setFloating(false);  // come il doppio clic sul titolo
                    const bool doubleClick = docked();
                    timelineDock->setFloating(true);
                    QCoreApplication::processEvents();
                    window->redockTimeline();
                    const bool menu = docked();
                    // Staccata, spostata e lasciata in alto: resta staccata.
                    timelineDock->setFloating(true);
                    QCoreApplication::processEvents();
                    window->timelinePollStep(true, frame.center());
                    timelineDock->move(timelineDock->pos() + QPoint(10, -60));
                    window->timelinePollStep(false, frame.center() - QPoint(0, frame.height() / 4));
                    docked();
                    const bool stays = timelineDock->isFloating();
                    window->redockTimeline();
                    docked();
                    cycles += dragged && doubleClick && menu && stays ? 1 : 0;
                }
                std::printf("  cicli stacca/riaggancia riusciti: %d su 5\n", cycles);

                // Titolo evidenziato con il mouse sopra (e non sul contenuto).
                QDockWidget* titled = window->sourceDock();
                const auto hover = [&](QPointF at) {
                    QHoverEvent e(QEvent::HoverMove, at, titled->mapToGlobal(at), QPointF(-1, -1));
                    QCoreApplication::sendEvent(titled, &e);
                };
                hover(QPointF(titled->width() / 2.0, 6));
                const bool titleLit = window->isTitleHighlighted(titled) && titled->cursor().shape() == Qt::OpenHandCursor;
                hover(QPointF(titled->width() / 2.0, titled->height() / 2.0));
                const bool contentNotLit = !window->isTitleHighlighted(titled);
                std::printf("  titolo dei pannelli: evidenziato %s, sul contenuto no %s\n", titleLit ? "OK" : "NO",
                            contentNotLit ? "OK" : "NO");

                const bool redockOk = highStays && heldStays && lowDocks && lowFull && menuDocks && keptHeight && cycles == 5
                                   && titleLit && contentNotLit;
                window->resetLayout();
                QCoreApplication::processEvents();
                window->resetLayout();
                QCoreApplication::processEvents();

                // Incrocio fra il separatore verticale (file | Sorgente) e quello orizzontale (sopra | linea del tempo):
                // trascinandolo cambiano insieme la larghezza dei file e l'altezza della linea del tempo.
                QDockWidget* files = window->filesDock();
                const QRect fg = files->geometry();
                const QRect sg = window->sourceDock()->geometry();
                const QRect tg = timelineDock->geometry();
                const QPoint corner((fg.right() + sg.left()) / 2 + 1, (fg.bottom() + tg.top()) / 2 + 1);
                const int filesWidth = files->width();
                const int timelineHeight = timelineDock->height();
                const auto mouse = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons) {
                    QMouseEvent e(type, QPointF(at), window->mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &e);
                };
                mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
                mouse(QEvent::MouseMove, corner + QPoint(40, -30), Qt::NoButton, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, corner + QPoint(40, -30), Qt::LeftButton, Qt::NoButton);
                QCoreApplication::processEvents();
                const int widthChange = files->width() - filesWidth;
                const int heightChange = timelineDock->height() - timelineHeight;
                const bool cornerOk = std::abs(widthChange - 40) <= 4 && std::abs(heightChange - 30) <= 4;
                std::printf("  incrocio dei separatori in (%d, %d): file %+d px, linea del tempo %+d px (attesi +40 e +30)\n",
                            corner.x(), corner.y(), widthChange, heightChange);
                window->resetLayout();

                // Pannelli piccoli: la misura minima di ognuno e, con EDITORVIDEO_SMOKE_SCREENSHOT, un'immagine di ognuno
                // staccato a 170x120.
                bool smallOk = true;
                const QString shots = qEnvironmentVariable("EDITORVIDEO_SMOKE_SCREENSHOT");
                for (QDockWidget* dock : {window->filesDock(), window->sourceDock(), window->programDock(), timelineDock}) {
                    const QSize minimum = dock->minimumSizeHint().expandedTo(dock->minimumSize());
                    std::printf("  pannello %s: misura minima %dx%d\n", qPrintable(dock->windowTitle()), minimum.width(),
                                minimum.height());
                    smallOk = smallOk && minimum.width() <= 170 && minimum.height() <= 140;
                    if (!shots.isEmpty()) {
                        dock->setFloating(true);
                        dock->resize(170, 120);
                        QCoreApplication::processEvents();
                        dock->grab().save(QString(shots).replace(QStringLiteral(".png"),
                                                                  QStringLiteral("_%1.png").arg(dock->objectName())));
                        dock->setFloating(false);
                    }
                }
                window->resetLayout();
                QCoreApplication::processEvents();
                std::printf("  linea del tempo riagganciata a tutta larghezza %s, incrocio %s, pannelli piccoli %s\n",
                            fullWidth ? "OK" : "NO", cornerOk ? "OK" : "NO", smallOk ? "OK" : "NO");
                docksOk = docksOk && fullWidth && redockOk && cornerOk && smallOk;
                std::printf("Pannelli: staccati e riagganciati %s, disposizione salvata e ripristinata %s, ripristino iniziale %s\n",
                            docksOk ? "OK" : "NON riuscito", changed && restored ? "OK" : "NON riuscita",
                            reset ? "OK" : "NON riuscito");
                std::fflush(stdout);
                ok = ok && (docksOk || !player->hasMedia()) && changed && restored && reset;
            }

            // File trascinati da Esplora file (simulati con QMimeData): sulla linea del tempo del montaggio, in fondo, il file
            // (già nel progetto) va intero nella sequenza senza un doppione nell'elenco; poi una cartella sulla Sorgente.
            // Ogni trascinamento si annulla in un colpo solo.
            {
                SequenceTimeline* timeline = window->sequenceTimeline();
                const auto& seq = window->project().sequence();
                const QString file = QString::fromStdString(window->project().clips().front().path);
                const std::size_t pieces = seq.fragments().size();
                const std::size_t clips = window->project().clips().size();
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(file)});
                // Linea del tempo rimpicciolita oltre "Adatta": lo spazio vuoto a destra vale come "in fondo".
                for (int i = 0; i < 20; ++i)
                    timeline->zoomOut();
                const QPointF end(timeline->width() - 12, 30);
                QDragEnterEvent enter(end.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &enter);
                QDropEvent drop(end, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(timeline, &drop);
                const bool inserted = enter.isAccepted() && drop.isAccepted() && seq.fragments().size() == pieces + 1
                                   && seq.fragments().back().clipId == seq.fragments().front().clipId
                                   && window->project().clips().size() == clips;
                timeline->fitAll();
                const QString label = window->undoText();
                const bool undone = window->undo() && seq.fragments().size() == pieces;

                // Una cartella sulla Sorgente (con una copia del file e un testo, che va ignorato): si aggiunge la
                // copia, che si apre nella Sorgente, e uno solo Annulla la toglie.
                QDir folder(QDir::temp().filePath(QStringLiteral("EditorVideoProvaTrascina")));
                folder.removeRecursively();
                QDir().mkpath(folder.path());
                QFile::copy(file, folder.filePath(QStringLiteral("copia.") + QFileInfo(file).suffix()));
                QFile note(folder.filePath(QStringLiteral("note.txt")));
                if (note.open(QIODevice::WriteOnly))
                    note.write("non è un video");
                note.close();
                window->dropFiles({QDir::toNativeSeparators(folder.path())}, MainWindow::DropTarget::Source);
                const std::size_t afterFolder = window->project().clips().size();
                const bool folderOk = afterFolder == clips + 1 && seq.fragments().size() == pieces
                                   && window->currentFile().contains(QStringLiteral("copia."));
                window->undo();
                const bool folderUndone = window->project().clips().size() == clips;
                QElapsedTimer settle;  // si lascia finire l'analisi della copia prima di cancellarla
                settle.start();
                while (settle.elapsed() < 3000 && !window->project().allAnalyzed())
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                std::printf("Trascinamento: nel montaggio %s (\"%s\" %s), cartella: %d file in più %s\n",
                            inserted ? "OK" : "NON riuscito", qPrintable(label.mid(1)), undone ? "annullato" : "NON annullato",
                            static_cast<int>(afterFolder - clips), folderOk && folderUndone ? "e annullati" : "NON riuscito");
                std::fflush(stdout);
                ok = ok && inserted && undone && label.contains(QStringLiteral("Aggiungi file")) && folderOk && folderUndone;
                folder.removeRecursively();
            }

            // Progetto: salvato e riaperto, con gli stessi pezzi e lo stesso audio tolto (e senza modifiche
            // in sospeso).
            {
                const QString projectFile =
                    QDir::temp().filePath(QStringLiteral("EditorVideoProvaProgetto") + core::kProjectExtension);
                const std::size_t pieces = sequence.fragments().size();
                const bool saved = window->saveProjectAs(projectFile, false) && !window->isModified();
                const bool reopened = saved && window->openProject(projectFile, false);
                const auto& again = window->project().sequence();
                const bool same = reopened && again.fragments().size() == pieces && again.mutedRanges().size() == 1
                               && !window->isModified();
                std::printf("Progetto: salvato %s, riaperto %s, %d pezzi\n", saved ? "sì" : "NO", same ? "uguale" : "DIVERSO",
                            static_cast<int>(again.fragments().size()));
                std::fflush(stdout);
                QFile::remove(projectFile);
                ok = ok && same;
                while (!window->project().allAnalyzed() && analysis.elapsed() < 40000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            }
            std::printf("Audio: %d tratti senza audio (%.3f - %.3f s), forme d'onda %s\n", static_cast<int>(muted.size()),
                        muted.empty() ? 0.0 : muted.front().start, muted.empty() ? 0.0 : muted.front().end,
                        window->pendingWaveforms() == 0 ? "pronte" : "NON pronte");
            std::fflush(stdout);
            ok = ok && muted.size() == 1 && window->pendingWaveforms() == 0;

            // Anteprima del montaggio: da poco prima della prima giunzione, 1 s di riproduzione deve passare al
            // pezzo successivo e avanzare col tempo giusto.
            if (sequence.fragments().size() >= 2) {
                auto* program = window->programMonitor();
                const double join = sequence.startOf(1);
                const double from = std::max(0.0, join - 0.3);
                program->seekTo(from);
                program->togglePlayPause();
                QElapsedTimer playing;
                playing.start();
                while (playing.elapsed() < 1000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const double reached = program->position();
                const int piece = program->currentIndex();
                if (program->isPlaying())
                    program->togglePlayPause();
                const bool crossed = piece >= 1 && reached > join && reached < from + 1.6;
                std::printf("Anteprima del montaggio: da %.3f s a %.3f s, pezzo %d (giunzione a %.3f s)%s\n", from,
                            reached, piece + 1, join, crossed ? "" : "  <-- NON RIUSCITA");
                std::fflush(stdout);
                ok = ok && crossed;
            }
            // Spazi vuoti: l'ultimo pezzo trascinato col mouse 1 s più avanti lascia uno spazio; Annulla e Ripeti;
            // l'anteprima passa lo spazio (nero) e arriva al pezzo; Canc sullo spazio selezionato lo chiude.
            if (sequence.fragments().size() >= 2) {
                auto* line = window->sequenceTimeline();
                auto* program = window->programMonitor();
                line->fitAll();
                QCoreApplication::processEvents();
                const int last = static_cast<int>(sequence.fragments().size()) - 1;
                const double lastStart = sequence.startOf(last);
                const double total = sequence.totalLength();
                const double y = line->videoLaneCenter();
                const double x0 = line->xForSequenceTime(lastStart + sequence.fragments().back().length() / 2);
                const double x1 = x0 + (line->xForSequenceTime(lastStart + 1.0) - line->xForSequenceTime(lastStart));
                const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
                    QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                    QCoreApplication::sendEvent(line, &e);
                };
                mouse(QEvent::MouseButtonPress, x0, Qt::LeftButton);
                mouse(QEvent::MouseMove, (x0 + x1) / 2, Qt::LeftButton);
                mouse(QEvent::MouseMove, x1, Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, x1, Qt::NoButton);
                QCoreApplication::processEvents();
                const double moved = sequence.startOf(last);
                const bool dragged = sequence.gapCount() == 1 && std::abs(moved - (lastStart + 1.0)) < 0.05
                                  && std::abs(sequence.startOf(last - 1) - (lastStart - sequence.fragments()[last - 1].length())) < 1e-6;
                const bool undone = window->undo() && sequence.gapCount() == 0 && std::abs(sequence.startOf(last) - lastStart) < 1e-6;
                const bool redone = window->redo() && sequence.gapCount() == 1;

                program->seekTo(lastStart + 0.2);  // nello spazio
                const bool inGap = program->inGap() && program->currentIndex() == -1;
                program->togglePlayPause();
                const bool started = program->isPlaying();
                // Fino all'arrivo nel pezzo dopo lo spazio (1 s di spazio da attraversare), al massimo 6 s: su un PC lento
                // (es. la build automatica, senza scheda video) un giro degli eventi può durare a lungo.
                QElapsedTimer playing;
                playing.start();
                while (playing.elapsed() < 6000 && (program->currentIndex() != last || program->position() < moved + 0.05))
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const double elapsed = playing.elapsed() / 1000.0;
                const double reached = program->position();
                const int piece = program->currentIndex();
                if (program->isPlaying())
                    program->togglePlayPause();
                // Il tempo scorre come quello vero (con un po' di margine per il passaggio al pezzo).
                const bool crossed = inGap && started && piece == last && reached > moved
                                  && reached < lastStart + 0.2 + elapsed + 0.6;
                std::printf("  spazio vuoto: partita %s, %.2f s per arrivare a %.3f s\n", started ? "sì" : "NO", elapsed, reached);

                // Come con un clic sullo spazio: il fuoco va alla linea del tempo (con il fuoco sull'elenco dei file Canc
                // toglierebbe il file).
                line->setFocus(Qt::MouseFocusReason);
                line->setSelectedGap(sequence.fragments()[last].id);
                if (const QString image = qEnvironmentVariable("EDITORVIDEO_SMOKE_GAP_SCREENSHOT"); !image.isEmpty())
                    line->grab().save(image);  // la linea del tempo con lo spazio selezionato
                window->deleteSelection();
                const bool closed = sequence.gapCount() == 0 && std::abs(sequence.totalLength() - total) < 1e-6;
                std::printf("Spazi vuoti: trascinato %s (%.3f -> %.3f s), annulla %s, ripeti %s, anteprima nello spazio %s "
                            "(arrivata a %.3f s, pezzo %d), chiuso con Canc %s\n",
                            dragged ? "sì" : "NO", lastStart, moved, undone ? "sì" : "NO", redone ? "sì" : "NO",
                            crossed ? "sì" : "NO", reached, piece + 1, closed ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && dragged && undone && redone && crossed && closed;
            }
            // Bordi dei pezzi: la fine del primo pezzo trascinata 2 fotogrammi indietro (resta uno spazio, il pezzo dopo
            // non si muove), poi con Alt 2 fotogrammi avanti (il pezzo dopo scorre); Annulla torna ogni volta com'era.
            if (sequence.fragments().size() >= 2) {
                auto* line = window->sequenceTimeline();
                window->programMonitor()->seekTo(0.0);  // il cursore lontano dai bordi: niente aggancio
                line->fitAll();
                QCoreApplication::processEvents();
                const double frame = window->frameDurationOf(sequence.fragments()[0].clipId);
                const double end = sequence.fragments()[0].end;
                const double nextStart = sequence.startOf(1);
                const double y = line->videoLaneCenter();
                const double pixelsPerSecond = line->xForSequenceTime(1.0) - line->xForSequenceTime(0.0);
                const auto dragEdge = [&](double seconds, Qt::KeyboardModifiers modifiers) {
                    const double x0 = line->xForBlockEdge(0, false) - 1;
                    const double x1 = x0 + seconds * pixelsPerSecond;
                    const auto mouse = [&](QEvent::Type type, double x, Qt::MouseButtons buttons) {
                        QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                                      type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, modifiers);
                        QCoreApplication::sendEvent(line, &e);
                    };
                    mouse(QEvent::MouseButtonPress, x0, Qt::LeftButton);
                    mouse(QEvent::MouseMove, (x0 + x1) / 2, Qt::LeftButton);
                    mouse(QEvent::MouseMove, x1, Qt::LeftButton);
                    mouse(QEvent::MouseButtonRelease, x1, Qt::NoButton);
                    QCoreApplication::processEvents();
                };
                dragEdge(-2 * frame, Qt::NoModifier);
                const double shortened = sequence.fragments()[0].end;
                const bool shortenOk = std::abs(shortened - (end - 2 * frame)) < 1e-6 && std::abs(sequence.startOf(1) - nextStart) < 1e-6
                                    && sequence.gapCount() == 1;
                const bool undone = window->undo() && std::abs(sequence.fragments()[0].end - end) < 1e-6 && sequence.gapCount() == 0;
                dragEdge(2 * frame, Qt::AltModifier);
                const double lengthened = sequence.fragments()[0].end;
                const bool rippleOk = std::abs(lengthened - (end + 2 * frame)) < 1e-6
                                   && std::abs(sequence.startOf(1) - (nextStart + 2 * frame)) < 1e-6;
                const bool rippleUndone = window->undo() && std::abs(sequence.fragments()[0].end - end) < 1e-6
                                       && std::abs(sequence.startOf(1) - nextStart) < 1e-6;
                std::printf("Bordi dei pezzi: accorciato %s (%.3f -> %.3f s, pezzo dopo fermo), con Alt allungato %s (%.3f s, "
                            "pezzo dopo spostato), annulla %s\n",
                            shortenOk ? "sì" : "NO", end, shortened, rippleOk ? "sì" : "NO", lengthened,
                            undone && rippleUndone ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && shortenOk && undone && rippleOk && rippleUndone;
            }
            // Tracce audio (step 18): "+" aggiunge A2 e A3, l'audio del file va su A2, trascinato col mouse su A3, accorciato,
            // diviso con S e tolto con Canc; "−" toglie una traccia; Annulla riporta tutto com'era.
            {
                auto* line = window->sequenceTimeline();
                const core::AudioTracks& audio = window->project().audioTracks();
                line->addTrackButton()->click();
                line->addTrackButton()->click();
                QCoreApplication::processEvents();
                const bool added = audio.tracks().size() == 2 && line->audioLanes().size() == 3;
                const int a2 = added ? audio.tracks()[0].id : 0;
                const int a3 = added ? audio.tracks()[1].id : 0;
                const int clips = window->addAudioFiles({window->currentFile()}, a2, 0.25);
                const core::AudioClip* clip = added && !audio.tracks()[0].clips.empty() ? &audio.tracks()[0].clips.front() : nullptr;
                const bool placed = clips == 1 && clip != nullptr && std::abs(clip->position - 0.25) < 1e-6;
                const int clipId = clip != nullptr ? clip->id : 0;
                const double length = clip != nullptr ? clip->length() : 0.0;
                // Trascinata col mouse dal centro della clip su A3, 0,5 s più avanti.
                window->programMonitor()->seekTo(0.0);
                line->fitAll();
                QCoreApplication::processEvents();
                const double pixelsPerSecond = line->xForSequenceTime(1.0) - line->xForSequenceTime(0.0);
                const double x0 = line->xForSequenceTime(0.25 + std::min(length / 2, 0.4));
                const double x1 = x0 + 0.5 * pixelsPerSecond;
                const auto mouse = [&](QEvent::Type type, double x, double y, Qt::MouseButtons buttons) {
                    QMouseEvent e(type, QPointF(x, y), line->mapToGlobal(QPointF(x, y)),
                                  type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
                    QCoreApplication::sendEvent(line, &e);
                };
                mouse(QEvent::MouseButtonPress, x0, line->audioLaneCenter(1), Qt::LeftButton);
                mouse(QEvent::MouseMove, (x0 + x1) / 2, line->audioLaneCenter(2), Qt::LeftButton);
                mouse(QEvent::MouseMove, x1, line->audioLaneCenter(2), Qt::LeftButton);
                mouse(QEvent::MouseButtonRelease, x1, line->audioLaneCenter(2), Qt::NoButton);
                QCoreApplication::processEvents();
                const core::AudioClip* moved = audio.find(clipId);
                const bool dragged = moved != nullptr && audio.trackOf(clipId) == a3 && std::abs(moved->position - 0.75) < 0.15;
                // Accorciata di 0,2 s alla fine, divisa a metà con S, la seconda parte tolta con Canc.
                const bool trimmed = moved != nullptr && window->trimAudioClip(clipId, false, moved->end - 0.2, false)
                                  && std::abs(audio.find(clipId)->length() - (length - 0.2)) < 1e-6;
                // Il cursore del montaggio non va oltre la fine dei pezzi video: la clip torna all'inizio e si divide dentro
                // quella parte.
                const bool back = window->moveAudioClip(clipId, a3, 0.0);
                const double middle = std::min(length - 0.2, sequence.totalLength()) / 2;
                line->setSelectedAudioClip(clipId);
                // Il monitor arriva al punto quando il fotogramma è decodificato: si aspetta che ci sia.
                window->programMonitor()->seekTo(middle);
                QElapsedTimer seeking;
                seeking.start();
                while (std::abs(window->programMonitor()->position() - middle) > 0.1 && seeking.elapsed() < 3000)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                const double splitAt = window->programMonitor()->position();
                window->splitAtPlayhead();
                const bool split = back && audio.track(a3) != nullptr && audio.track(a3)->clips.size() == 2;
                if (!split)
                    std::printf("  divisione: cursore a %.3f s (voluto %.3f s), clip audio scelta %d\n", splitAt, middle,
                                line->selectedAudioClip());
                line->setFocus(Qt::MouseFocusReason);
                if (split)
                    line->setSelectedAudioClip(audio.track(a3)->clips[1].id);
                window->deleteSelection();
                const bool deleted = audio.track(a3) != nullptr && audio.track(a3)->clips.size() == 1;
                // Livello, muto e solo dalle corsie.
                window->setTrackLevel(a3, -6.0);
                window->setTrackSolo(a3, true);
                const bool mix = audio.mix(a3).levelDb == -6.0 && !audio.audible(core::AudioTracks::kOriginalTrack) && audio.audible(a3);
                if (const QString image = qEnvironmentVariable("EDITORVIDEO_SMOKE_TRACKS_SCREENSHOT"); !image.isEmpty())
                    line->grab().save(image);
                // "−" toglie la traccia scelta (A3, quella dell'ultima clip toccata).
                line->removeTrackButton()->click();
                QCoreApplication::processEvents();
                const bool removed = audio.tracks().size() == 1 && audio.track(a3) == nullptr && line->audioLanes().size() == 2;
                // Annulla tutto: tracce e clip spariscono, il montaggio torna com'era.
                bool undone = true;
                for (int i = 0; i < 12 && !audio.isDefault(); ++i)
                    undone = window->undo() && undone;
                undone = undone && audio.isDefault() && line->audioLanes().size() == 1;
                std::printf("Tracce audio: aggiunte %s, file su A2 %s, trascinato su A3 %s, accorciato %s, diviso %s, tolto con "
                            "Canc %s, livello e solo %s, traccia tolta %s, annulla %s\n",
                            added ? "sì" : "NO", placed ? "sì" : "NO", dragged ? "sì" : "NO", trimmed ? "sì" : "NO",
                            split ? "sì" : "NO", deleted ? "sì" : "NO", mix ? "sì" : "NO", removed ? "sì" : "NO",
                            undone ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && added && placed && dragged && trimmed && split && deleted && mix && removed && undone;
            }
            // Mix e file solo audio (step 19): un file solo audio non va mai nella traccia video (né rilasciato né con A),
            // e livello, muto e solo si cambiano suonando senza fermare l'anteprima.
            {
                const core::AudioTracks& audio = window->project().audioTracks();
                const QString wav = QDir::temp().filePath(QStringLiteral("EditorVideoProvaAudio.wav"));
                {
                    QFile file(wav);
                    const int rate = 8000;
                    if (file.open(QIODevice::WriteOnly)) {
                        QDataStream out(&file);
                        out.setByteOrder(QDataStream::LittleEndian);
                        out.writeRawData("RIFF", 4);
                        out << quint32(36 + rate * 2);
                        out.writeRawData("WAVEfmt ", 8);
                        out << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
                        out.writeRawData("data", 4);
                        out << quint32(rate * 2);
                        for (int i = 0; i < rate; ++i)
                            out << qint16(8000 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));  // 1 s, 440 Hz
                    }
                }
                const QString original = window->currentFile();
                int originalId = 0;
                for (const core::Clip& c : window->project().clips()) {
                    if (QString::fromStdString(c.path) == original || QDir::toNativeSeparators(QString::fromStdString(c.path)) == QDir::toNativeSeparators(original))
                        originalId = c.id;
                }
                const std::size_t pieces = sequence.fragments().size();
                window->dropFiles({QDir::toNativeSeparators(wav)}, MainWindow::DropTarget::Sequence, 0);
                const bool dropped = sequence.fragments().size() == pieces && audio.tracks().size() == 1
                                  && audio.tracks()[0].clips.size() == 1;
                QElapsedTimer analysis;
                analysis.start();
                while (analysis.elapsed() < 20000 && !window->project().allAnalyzed())
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                int wavId = 0;
                for (const core::Clip& c : window->project().clips()) {
                    if (QString::fromStdString(c.path).endsWith(QStringLiteral("EditorVideoProvaAudio.wav")))
                        wavId = c.id;
                }
                bool withA = false;
                if (wavId != 0 && !audio.tracks().empty()) {
                    window->selectClip(wavId);
                    window->addToSequence();
                    withA = sequence.fragments().size() == pieces && audio.tracks()[0].clips.size() == 2;
                }
                if (originalId != 0)
                    window->selectClip(originalId);
                // Livello, muto e solo mentre suona: la riproduzione non si ferma.
                window->programMonitor()->seekTo(0.0);
                window->programMonitor()->setPlaying(true);
                const int track = audio.tracks().empty() ? 0 : audio.tracks()[0].id;
                window->setTrackLevel(track, -12.0);
                window->setTrackSolo(track, true);
                window->setTrackMuted(core::AudioTracks::kOriginalTrack, true);
                const bool keepsPlaying = window->programMonitor()->isPlaying() && audio.audible(track)
                                       && !audio.audible(core::AudioTracks::kOriginalTrack);
                window->programMonitor()->setPlaying(false);
                bool undone = true;
                for (int i = 0; i < 12 && !(audio.isDefault() && window->project().find(wavId) == nullptr); ++i)
                    undone = window->undo() && undone;
                undone = undone && audio.isDefault() && sequence.fragments().size() == pieces;
                std::printf("Mix audio: file solo audio rilasciato nel montaggio %s, con A %s, livello/solo/muto suonando %s, annulla %s\n",
                            dropped ? "su una traccia audio (non su quella video)" : "NO", withA ? "sì" : "NO",
                            keepsPlaying ? "senza fermare la riproduzione" : "NO", undone ? "sì" : "NO");
                std::fflush(stdout);
                ok = ok && dropped && withA && keepsPlaying && undone;
                QFile::remove(wav);
            }
            const double expectedLength = window->exportLength();

            // Esportazione dei tratti come dal menu (senza chiedere dove salvare), con il messaggio finale.
            const QString target = QDir::temp().filePath(QStringLiteral("EditorVideoProvaEsportazione.")
                                                          + QFileInfo(window->currentFile()).suffix());
            QFile::remove(target);
            // Rallentata apposta, perché la finestra di avanzamento faccia in tempo a comparire, come con un file grande.
            if (!window->startExport(QDir::toNativeSeparators(target), true, std::chrono::milliseconds(25))) {
                std::printf("Esportazione: non partita\n");
                finish(false);
                return;
            }
            // Quando compare il messaggio finale si aspetta ancora un po' (più del ritardo con cui compare la
            // finestra di avanzamento) e si controlla che nessuna finestra di avanzamento sia rimasta aperta.
            auto* poll = new QTimer(&app);
            auto messageSeen = std::make_shared<QElapsedTimer>();
            auto waiting = std::make_shared<QElapsedTimer>();
            waiting->start();
            QObject::connect(poll, &QTimer::timeout, &app, [=] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (box == nullptr) {
                    if (waiting->elapsed() < 15000)
                        return;
                    // Il messaggio finale non è mai arrivato in primo piano: di solito perché la finestra di
                    // avanzamento è rimasta aperta sopra.
                    poll->stop();
                    const QWidget* modal = QApplication::activeModalWidget();
                    std::printf("Esportazione: messaggio finale mai in primo piano (finestra attiva: %s, "
                                "esportazione %s, riuscita: %s)\n",
                                modal != nullptr ? qPrintable(modal->windowTitle()) : "nessuna",
                                window->isExporting() ? "ancora in corso" : "finita",
                                window->lastExportSucceeded() ? "sì" : qPrintable(QStringLiteral("no: ") + window->lastExportProblem()));
                    for (QWidget* w : QApplication::topLevelWidgets()) {
                        if (w->isVisible())
                            std::printf("  finestra visibile: \"%s\" (%s)\n", qPrintable(w->windowTitle()),
                                        w->metaObject()->className());
                    }
                    std::fflush(stdout);
                    if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                        dialog->reject();
                    finish(false);
                    return;
                }
                if (!messageSeen->isValid())
                    messageSeen->start();
                if (messageSeen->elapsed() < 700)
                    return;
                poll->stop();
                const auto dialogs = window->findChildren<QProgressDialog*>();
                const auto stuck = std::count_if(dialogs.begin(), dialogs.end(),
                                                 [](QProgressDialog* d) { return d->isVisible(); });
                bool exported = window->lastExportSucceeded() && QFileInfo::exists(target);
                if (exported) {
                    const double length = core::probeFile(target.toStdString()).durationSeconds;
                    std::printf("File esportato: %.3f s (attesi %.3f s)\n", length, expectedLength);
                    exported = std::abs(length - expectedLength) < 0.15;
                }
                std::printf("Esportazione: %s (\"%s\"), finestre di avanzamento rimaste aperte: %d\n",
                            exported ? "riuscita" : "NON riuscita", qPrintable(box->windowTitle()),
                            static_cast<int>(stuck));
                std::fflush(stdout);
                box->accept();
                QFile::remove(target);
                finish(ok && exported && stuck == 0);
            });
            poll->start(50);
        });
    }
}

std::optional<int> runWindowTest(QApplication& app, const QStringList& arguments)
{
    // Posizione dell'opzione, se è seguita da progetto e destinazione; -1 altrimenti.
    const auto withTwoArguments = [&arguments](const char* option) {
        const qsizetype at = arguments.indexOf(QLatin1String(option));
        return at >= 0 && at + 2 < arguments.size() ? at : qsizetype{-1};
    };
    std::optional<int> code;
    if (const qsizetype at = withTwoArguments("--project-export-test"); at >= 0)
        code = runProjectExportTest(app, arguments[at + 1], arguments[at + 2]);
    else if (const qsizetype other = withTwoArguments("--export-dialog-test"); other >= 0)
        code = runExportDialogTest(app, arguments[other + 1], arguments[other + 2]);
    else if (arguments.contains(QStringLiteral("--dock-test")))
        code = runDockTest(app);
    if (code)
        std::fflush(stdout);
    return code;
}

}  // namespace editor::app
