#include "SelfTests.h"

#include "EditorApp/GroupListPanel.h"
#include "EditorApp/ExportDialog.h"
#include "EditorApp/MainWindow.h"
#include "EditorApp/MixerPanel.h"
#include "EditorApp/PlayerWidget.h"
#include "EditorApp/ProgramMonitor.h"
#include "EditorApp/PropertiesPanel.h"
#include "EditorApp/TextEditorPanel.h"
#include "EditorApp/TransitionMime.h"
#include "EditorApp/SequenceTimeline.h"
#include "EditorApp/TimelineWidget.h"
#include "EditorCore/AudioDecoder.h"
#include "EditorCore/Compatibility.h"
#include "EditorCore/CutSegment.h"
#include "EditorCore/FileDrop.h"
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
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
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
    {"--join-test", runJoinTest},         {"--text-test", runTextTest},
    {"--transition-test", runTransitionTest},
    {"--group-test", runGroupTest},
    {"--transform-test", runTransformTest},
    {"--manip-test", runManipTest},
    {"--layer-test", runLayerTest},
    {"--layer-monitor-test", runLayerMonitorTest},
    {"--image-test", runImageTest},
    {"--export-panel-test", runExportPanelTest},
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

// --export-dialog-test <progetto> <destinazione>: come l'utente: apre il progetto, Esporta, nella finestra "Esporta" il
// nome del file e il pulsante Esporta, chiude il messaggio finale. La finestra "Esporta" la guida un timer dentro
// l'applicazione (è modale: gli eventi li elabora il suo ciclo), il messaggio finale un thread a parte (senza mouse).
// Dice a che punto si ferma. Codice 0 se arriva in fondo, 3 se si blocca.
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
    std::atomic<int> stage{0};  // 2 finestra "Esporta" compilata, 3 messaggio finale chiuso
    std::atomic<bool> stop{false};
    bool warnedExisting = false;
    QTimer dialogDriver;
    QObject::connect(&dialogDriver, &QTimer::timeout, &window, [&] {
        if (stage != 0)
            return;
        auto* dialog = dynamic_cast<ExportDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr)
            return;
        std::printf("  [%.1f s] finestra Esporta: destinazione e Esporta\n", waiting.elapsed() / 1000.0);
        dialog->setDestination(output);
        warnedExisting = !dialog->warningText().isEmpty();
        if (!dialog->canExport()) {
            std::printf("  il pulsante Esporta non è attivo: %s\n", qPrintable(dialog->warningText()));
            dialog->reject();
            stage = 1;
            return;
        }
        dialog->exportButton()->click();
        stage = 2;
    });
    dialogDriver.start(300);
    std::thread driver([&] {
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (stage == 2) {
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
    while (stage < 3 && stage != 1 && waiting.elapsed() < 120000)
        app.processEvents(QEventLoop::AllEvents, 50);
    // Ancora un po' perché il messaggio si chiuda e l'esportazione sia registrata.
    QElapsedTimer tail;
    tail.start();
    while (tail.elapsed() < 1000)
        app.processEvents(QEventLoop::AllEvents, 50);
    stop = true;
    driver.join();
    dialogDriver.stop();
    const char* stages[] = {"finestra Esporta mai comparsa", "finestra Esporta senza pulsante attivo",
                            "messaggio finale mai comparso", "arrivata in fondo"};
    std::printf("Esportazione dal pulsante: %s, %s (file %s%s)\n", stages[stage.load()],
                window.lastExportSucceeded() ? "riuscita" : qPrintable(QStringLiteral("non riuscita: ") + window.lastExportProblem()),
                QFile::exists(output) ? "creato" : "assente", warnedExisting ? ", avviso comparso" : "");
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
