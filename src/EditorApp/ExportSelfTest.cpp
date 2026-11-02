#include "EditorApp/ExportDialog.h"
#include "EditorApp/SelfTests.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtCore/QVariant>
#include <QtWidgets/QSlider>

#include <cstdio>
#include <vector>

namespace editor::app {

namespace core = editor::core;

// --export-panel-test: la finestra "Esporta" con un riepilogo inventato: l'avviso se il file c'è già, le destinazioni
// che non si possono usare, le scelte che arrivano alle opzioni dell'esportazione. Codice 0 se tutto è giusto, 3 se un
// controllo non riesce.
int runExportPanelTest(const QStringList&, qsizetype)
{
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("  %s: %s\n", ok ? "OK" : "NO", what);
        if (!ok)
            ++failures;
    };
    std::printf("Prova della finestra Esporta\n");

    QTemporaryDir folder;
    if (!folder.isValid()) {
        std::printf("  cartella temporanea non creata\n");
        return 2;
    }
    const auto file = [&](const char* name) { return QDir(folder.path()).filePath(QString::fromLatin1(name)); };
    for (const char* name : {"esiste.mkv", "origine.mkv"}) {
        QFile f(file(name));
        f.open(QIODevice::WriteOnly);
        f.write("x");
    }

    std::vector<core::ReencodeOptions> asked;  // le scelte con cui la finestra ha chiesto il riepilogo
    ExportDialogInput input;
    input.suggestedPath = file("nuovo.mkv");
    input.extension = QStringLiteral("mkv");
    input.sources = {file("origine.mkv")};
    input.montage = true;
    input.gapCount = 1;
    input.mixedAudioFiles = 2;
    input.projectCodec = core::VideoCodec::H264;
    input.sourceWidth = 1920;
    input.sourceHeight = 1080;
    input.sourceFrameRate = 60.0;
    input.preview = [&](const core::ReencodeOptions& options) {
        asked.push_back(options);
        core::ExportPreview preview;
        preview.copiedSegments = 1;
        preview.reencodedSegments = 1;
        preview.copiedSeconds = 2.0;
        preview.reencodedSeconds = 3.0;
        preview.outputSeconds = 5.0;
        return preview;
    };

    {
        ExportDialog dialog(input, nullptr);
        check(dialog.canExport() && dialog.warningText().isEmpty(), "destinazione nuova: si può esportare, nessun avviso");
        check(!asked.empty(), "il riepilogo si calcola all'apertura");

        dialog.setDestination(file("esiste.mkv"));
        check(dialog.canExport() && dialog.warningText().contains(QStringLiteral("sovrascritto")),
              "il file che esiste già: avviso di sovrascrittura, ma si può esportare");
        dialog.setDestination(file("origine.mkv"));
        check(!dialog.canExport(), "uno dei file originali non è una destinazione");
        dialog.setDestination(QDir(folder.path()).filePath(QStringLiteral("non_esiste/x.mkv")));
        check(!dialog.canExport(), "la cartella che non esiste non va bene");
        dialog.setDestination(QString());
        check(!dialog.canExport(), "senza destinazione non si esporta");
        dialog.setDestination(file("senza_estensione"));
        check(dialog.canExport() && dialog.target().endsWith(QStringLiteral("senza_estensione.mkv")),
              "senza l'estensione giusta si aggiunge quella del contenitore");

        // Le scelte di partenza sono quelle di oggi.
        core::ReencodeOptions options = dialog.options();
        check(options.includeAudio && !options.backend && options.cq == 0 && options.preset.empty(),
              "di partenza: con audio, encoder automatico, qualità e preset predefiniti");
        check(options.audioCodec == core::AudioCodec::Aac && options.audioBitRate == 256000,
              "l'audio mixato parte in AAC a 256 kb/s");

        // Le scelte arrivano alle opzioni e al riepilogo.
        const std::size_t before = asked.size();
        dialog.includeAudioCheck()->setChecked(false);
        check(!dialog.options().includeAudio && asked.size() > before && !asked.back().includeAudio,
              "togliere l'audio arriva alle opzioni e ricalcola il riepilogo");
        dialog.includeAudioCheck()->setChecked(true);
        dialog.reencodeAllCheck()->setChecked(true);
        check(dialog.options().all && asked.back().all, "\"Ricodifica tutto\" arriva alle opzioni e al riepilogo");
        dialog.cqSlider()->setValue(dialog.cqSlider()->minimum() + 10);
        check(dialog.options().cq == dialog.cqSlider()->minimum() + 10, "il cursore della qualità arriva alle opzioni");

        const QComboBox* encoders = dialog.encoderCombo();
        if (encoders->count() > 2) {
            dialog.encoderCombo()->setCurrentIndex(1);
            check(dialog.options().backend.has_value(), "scegliere un encoder lo mette nelle opzioni");
            dialog.encoderCombo()->setCurrentIndex(0);
            check(!dialog.options().backend.has_value(), "\"Automatico\" lascia scegliere al programma");
        } else {
            std::printf("  (un solo encoder su questo PC: scelta dell'encoder non provata)\n");
        }

        // Formato del video: di partenza come i file; risoluzione in percentuale o in altezza (misure pari), fotogrammi, codec.
        check(dialog.options().video.width == 0 && !dialog.options().video.codec && dialog.options().video.frameRate == 0.0,
              "di partenza il video è come quello dei file");
        QComboBox* sizes = dialog.resolutionCombo();
        const auto choose = [](QComboBox* combo, const QVariant& data) {
            const int at = combo->findData(data);
            if (at >= 0)
                combo->setCurrentIndex(at);
            return at >= 0;
        };
        check(choose(sizes, QStringLiteral("p50")) && dialog.options().video.width == 960 && dialog.options().video.height == 540,
              "50% di 1920x1080 = 960x540");
        check(choose(sizes, QStringLiteral("h720")) && dialog.options().video.width == 1280 && dialog.options().video.height == 720,
              "720p tiene le proporzioni");
        check(choose(dialog.frameRateCombo(), 30.0) && dialog.options().video.frameRate == 30.0, "30 fotogrammi al secondo");
        check(choose(dialog.codecCombo(), static_cast<int>(core::VideoCodec::Hevc)) && dialog.options().video.codec == core::VideoCodec::Hevc,
              "il codec scelto arriva alle opzioni");
        check(asked.back().video.height == 720, "il riepilogo si ricalcola con la nuova misura");
        check(choose(dialog.containerCombo(), QStringLiteral("mp4")) && dialog.target().endsWith(QStringLiteral(".mp4")),
              "il contenitore scelto dà l'estensione della destinazione");
        check(choose(dialog.containerCombo(), QString()) && dialog.target().endsWith(QStringLiteral(".mkv")),
              "\"Come i file\" rimette il contenitore dei file");
        choose(sizes, QString());
        choose(dialog.frameRateCombo(), 0.0);
        choose(dialog.codecCombo(), -1);

        check(dialog.options().decoder == core::DecoderMode::Software, "di partenza il decoder è la CPU");
        dialog.decoderCombo()->setCurrentIndex(1);
        check(dialog.options().decoder == core::DecoderMode::Hardware && asked.back().decoder == core::DecoderMode::Hardware,
              "il decoder GPU arriva alle opzioni");
        dialog.decoderCombo()->setCurrentIndex(0);

        // Altro codec audio: il bitrate segue (FLAC è senza perdita).
        QComboBox* codecs = dialog.audioCodecCombo();
        for (int i = 0; i < codecs->count(); ++i) {
            if (codecs->itemData(i).toInt() == static_cast<int>(core::AudioCodec::Flac)) {
                codecs->setCurrentIndex(i);
                options = dialog.options();
                check(options.audioCodec == core::AudioCodec::Flac && options.audioBitRate == 0,
                      "FLAC nelle opzioni, senza bitrate");
            }
        }
    }

    // Le scelte ricordate dalla volta prima riappaiono.
    {
        input.options.cq = 27;
        input.options.audioBitRate = 128000;
        ExportDialog dialog(input, nullptr);
        check(dialog.options().cq == 27 && dialog.cqSlider()->value() == 27, "la qualità ricordata riappare");
        check(dialog.options().audioBitRate == 128000, "il bitrate audio ricordato riappare");
    }

    // Profili: quelli pronti mettono le scelte nella finestra; cambiarne una riporta a "Personalizzato"; c'è la stima della dimensione.
    {
        input.options = core::ReencodeOptions{};
        input.sourceBitRate = 8'000'000.0;
        ExportDialog dialog(input, nullptr);
        QComboBox* profiles = dialog.profileCombo();
        check(profiles->count() >= 4 && profiles->currentIndex() == 0, "i profili ci sono, si parte da Personalizzato");
        check(dialog.sizeText().startsWith(QStringLiteral("circa")), "la dimensione stimata si mostra");
        const int whatsapp = profiles->findData(QStringLiteral("Piccolo per WhatsApp"));
        check(whatsapp > 0, "c'è il profilo Piccolo per WhatsApp");
        const QString before = dialog.sizeText();
        profiles->setCurrentIndex(whatsapp);
        const core::ReencodeOptions options = dialog.options();
        check(options.video.height == 480 && options.video.width == 854, "il profilo mette 480p (854x480)");
        check(options.video.frameRate == 30.0 && options.cq == 28, "il profilo mette 30 fps e CQ 28");
        check(dialog.target().endsWith(QStringLiteral(".mp4")), "il profilo cambia il contenitore in MP4");
        check(dialog.audioCodecCombo()->currentData().toInt() == static_cast<int>(core::AudioCodec::Aac) && options.audioBitRate == 96000,
              "il profilo mette AAC a 96 kb/s");
        check(profiles->currentIndex() == whatsapp, "dopo il profilo resta scelto");
        check(dialog.sizeText() != before, "la stima cambia con le scelte");
        dialog.resolutionCombo()->setCurrentIndex(0);
        check(profiles->currentIndex() == 0, "cambiare una scelta riporta a Personalizzato");
        // Salvare un profilo (nuovo, poi con lo stesso nome) e toglierlo; con un nome di programma di prova per non toccare
        // i profili veri.
        {
            setExportProfilesStore(QStringLiteral("EditorVideoProva"));
            const int before = profiles->count();
            dialog.storeProfileAs(QStringLiteral("Mio uno"));
            dialog.storeProfileAs(QStringLiteral("Mio due"));
            check(profiles->count() == before + 2 && profiles->currentData().toString() == QStringLiteral("Mio due"),
                  "salvare due profili nuovi li aggiunge");
            dialog.storeProfileAs(QStringLiteral("mio UNO"));
            check(profiles->count() == before + 2, "salvare con lo stesso nome sostituisce");
            QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("EditorVideo"), QStringLiteral("EditorVideoProva")).clear();
            setExportProfilesStore(QStringLiteral("EditorVideo"));
        }
        // Il profilo YouTube non ingrandisce: con file da 1080 la misura resta quella dei file.
        profiles->setCurrentIndex(profiles->findData(QStringLiteral("YouTube 1080p")));
        check(dialog.options().video.width == 0 && dialog.options().video.height == 0, "YouTube 1080p non ingrandisce un video da 1080");
    }

    // Senza montaggio (taglio veloce): le scelte del video non si applicano.
    {
        ExportDialogInput quick = input;
        quick.montage = false;
        quick.gapCount = 0;
        quick.mixedAudioFiles = 0;
        quick.projectCodec.reset();
        ExportDialog dialog(quick, nullptr);
        check(!dialog.encoderCombo()->isEnabled() && !dialog.reencodeAllCheck()->isEnabled(),
              "nel taglio veloce niente scelte di ricodifica");
        check(dialog.canExport(), "il taglio veloce si esporta");
    }

    std::printf("%s\n", failures == 0 ? "Tutto giusto" : "Qualcosa non va");
    return failures == 0 ? 0 : 3;
}

}  // namespace editor::app
