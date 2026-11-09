#pragma once

#include <utility>

namespace editor::app {

// Le prove (EditorVideoTests.exe e --smoke-test di EditorVideo.exe) leggono e pilotano la finestra anche con funzioni che
// al programma non servono: le classi le tengono private e dichiarano TestAccess come friend, così mostrano solo quello
// che usa il programma. Qui ognuna diventa una funzione statica con l'oggetto come primo argomento:
//     TestAccess::programMonitor(window)   al posto di   window.programMonitor()
class TestAccess
{
public:
#define EDITORVIDEO_TEST_ACCESS(name)                                        \
    template <class Object, class... Args>                                   \
    static decltype(auto) name(Object& object, Args&&... args)               \
    {                                                                        \
        return object.name(std::forward<Args>(args)...);                     \
    }

    // TextOverlay
    EDITORVIDEO_TEST_ACCESS(renderCount)
    // MainWindow
    EDITORVIDEO_TEST_ACCESS(programMonitor)
    EDITORVIDEO_TEST_ACCESS(sequenceTimeline)
    EDITORVIDEO_TEST_ACCESS(setTimelinePollAutomatic)
    EDITORVIDEO_TEST_ACCESS(setTestCursor)
    EDITORVIDEO_TEST_ACCESS(setNativeDragForTest)
    EDITORVIDEO_TEST_ACCESS(isDockPreviewVisible)
    EDITORVIDEO_TEST_ACCESS(isPanelFullScreen)
    EDITORVIDEO_TEST_ACCESS(sourceDock)
    EDITORVIDEO_TEST_ACCESS(programDock)
    EDITORVIDEO_TEST_ACCESS(filesDock)
    EDITORVIDEO_TEST_ACCESS(timelineDock)
    EDITORVIDEO_TEST_ACCESS(propertiesDock)
    EDITORVIDEO_TEST_ACCESS(currentFile)
    EDITORVIDEO_TEST_ACCESS(previewQuality)
    EDITORVIDEO_TEST_ACCESS(muteRangeForTest)
    EDITORVIDEO_TEST_ACCESS(textPanel)
    EDITORVIDEO_TEST_ACCESS(propertiesPanel)
    EDITORVIDEO_TEST_ACCESS(mixerPanel)
    EDITORVIDEO_TEST_ACCESS(leftTabs)
    EDITORVIDEO_TEST_ACCESS(transitionPanel)
    EDITORVIDEO_TEST_ACCESS(groupPanel)
    EDITORVIDEO_TEST_ACCESS(sourceStack)
    EDITORVIDEO_TEST_ACCESS(transitionPreview)
    EDITORVIDEO_TEST_ACCESS(pendingWaveforms)
    EDITORVIDEO_TEST_ACCESS(isModified)
    EDITORVIDEO_TEST_ACCESS(pendingThumbnails)
    EDITORVIDEO_TEST_ACCESS(readyThumbnails)
    EDITORVIDEO_TEST_ACCESS(isExporting)
    EDITORVIDEO_TEST_ACCESS(lastExportSucceeded)
    EDITORVIDEO_TEST_ACCESS(lastExportProblem)
    EDITORVIDEO_TEST_ACCESS(exportLength)
    // ExportDialog
    EDITORVIDEO_TEST_ACCESS(exportButton)
    EDITORVIDEO_TEST_ACCESS(warningText)
    EDITORVIDEO_TEST_ACCESS(encoderCombo)
    EDITORVIDEO_TEST_ACCESS(profileCombo)
    EDITORVIDEO_TEST_ACCESS(includeAudioCheck)
    EDITORVIDEO_TEST_ACCESS(reencodeAllCheck)
    EDITORVIDEO_TEST_ACCESS(cqSlider)
    // PropertiesPanel
    EDITORVIDEO_TEST_ACCESS(showingTransform)
    EDITORVIDEO_TEST_ACCESS(showingText)
    EDITORVIDEO_TEST_ACCESS(showingTransitionClip)
    // PlayerWidget
    EDITORVIDEO_TEST_ACCESS(statusText)
    // MixerPanel
    EDITORVIDEO_TEST_ACCESS(resetMeters)
    EDITORVIDEO_TEST_ACCESS(meterDb)
    EDITORVIDEO_TEST_ACCESS(masterMeterDb)
    EDITORVIDEO_TEST_ACCESS(stripCount)
    EDITORVIDEO_TEST_ACCESS(trackAt)
    // ProgramMonitor
    EDITORVIDEO_TEST_ACCESS(inGap)
    EDITORVIDEO_TEST_ACCESS(transitionShown)
    EDITORVIDEO_TEST_ACCESS(transitionFrame)
    EDITORVIDEO_TEST_ACCESS(textOverlay)
    EDITORVIDEO_TEST_ACCESS(layerFramesShown)
    EDITORVIDEO_TEST_ACCESS(transformMode)
    EDITORVIDEO_TEST_ACCESS(cropMode)
    EDITORVIDEO_TEST_ACCESS(thirdsGrid)
    EDITORVIDEO_TEST_ACCESS(manipQuad)
    // TextEditorPanel
    EDITORVIDEO_TEST_ACCESS(hasClip)
    // AnimMiniTimeline
    EDITORVIDEO_TEST_ACCESS(markCenterForTest)
    EDITORVIDEO_TEST_ACCESS(pointForTimeForTest)
    // GroupListPanel
    EDITORVIDEO_TEST_ACCESS(groupCount)
    // SequenceTimeline
    EDITORVIDEO_TEST_ACCESS(keyRow)
    EDITORVIDEO_TEST_ACCESS(keyCenterForTest)
    EDITORVIDEO_TEST_ACCESS(audioLanes)
    EDITORVIDEO_TEST_ACCESS(xForSequenceTime)
    EDITORVIDEO_TEST_ACCESS(videoLaneHeight)
    EDITORVIDEO_TEST_ACCESS(audioLaneHeight)
    EDITORVIDEO_TEST_ACCESS(videoLaneCenter)
    EDITORVIDEO_TEST_ACCESS(audioLaneCenter)
    EDITORVIDEO_TEST_ACCESS(addTrackButton)
    EDITORVIDEO_TEST_ACCESS(removeTrackButton)
    EDITORVIDEO_TEST_ACCESS(transitionRectForTest)
    EDITORVIDEO_TEST_ACCESS(transformBadgeRectForTest)
    EDITORVIDEO_TEST_ACCESS(xForJunction)
    EDITORVIDEO_TEST_ACCESS(transitionDropBlock)
    EDITORVIDEO_TEST_ACCESS(textLaneCenter)

#undef EDITORVIDEO_TEST_ACCESS
};

}  // namespace editor::app
