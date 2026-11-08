#include "EditorApp/CurveEditorDialog.h"

#include "EditorApp/MainWindowShared.h"
#include "EditorApp/PropertiesPanel.h"

namespace editor::app {
using namespace detail;

namespace {

// I pezzi video fra gli oggetti scelti, nell'ordine di scelta.
std::vector<int> selectedPieceIds(const std::vector<core::ObjectRef>& objects)
{
    std::vector<int> ids;
    for (const core::ObjectRef& o : objects) {
        if (o.kind == core::ObjectRef::Kind::Piece && std::find(ids.begin(), ids.end(), o.id) == ids.end())
            ids.push_back(o.id);
    }
    return ids;
}

// Lo stato dei rombi del pannello Proprietà per l'animazione del pezzo o della scritta scelti, col cursore a `fileTime`.
PropertiesPanel::AnimationState animationState(const core::TransformAnimation& animation, double fileTime, double from, double to,
                                               bool inside)
{
    PropertiesPanel::AnimationState state;
    state.enabled = inside;
    state.presets = true;
    for (int g = 0; g < core::kAnimGroupCount; ++g)
        state.groups[static_cast<std::size_t>(g)] = core::groupState(animation, static_cast<core::AnimGroup>(g), fileTime, from, to);
    return state;
}

// I rombi della mini linea del tempo: una chiave per gruppo e istante, col suo tipo di passaggio.
std::vector<AnimMiniTimeline::Mark> keyMarks(const core::TransformAnimation& animation)
{
    std::vector<AnimMiniTimeline::Mark> marks;
    for (int g = 0; g < core::kAnimGroupCount; ++g) {
        const auto group = static_cast<core::AnimGroup>(g);
        for (const double time : core::groupKeyTimes(animation, group))
            marks.push_back({group, time, core::groupInterpAt(animation, group, time)});
    }
    return marks;
}

}  // namespace

void MainWindow::refreshPropertiesPanel(bool cursorOnly)
{
    if (propertiesPanel_ == nullptr || timeline_ == nullptr)
        return;
    propertiesFollowCursor_ = false;
    refreshKeyRow();
    if (program_ != nullptr && !cursorOnly)
        program_->refreshManipulators();  // le maniglie del monitor seguono la scelta
    const std::vector<core::ObjectRef> objects = timeline_->allSelectedObjects();
    if (objects.size() == 1 && objects.front().kind == core::ObjectRef::Kind::TextClip
        && project_.textTracks().find(objects.front().id) != nullptr) {
        // La scritta: il suo editor (refreshTextPanel ne mette i valori) e in fondo la sezione «Trasforma» con rombi e mini linea.
        if (const AnimTarget target = animationTarget(); target.valid) {
            const PropertiesPanel::AnimationState state =
                animationState(target.animation, target.fileTime, target.from, target.to, target.inside);
            propertiesFollowCursor_ = true;
            const core::TextClip* textClip = project_.textTracks().find(target.id);
            const QSize frame = frameSize();
            propertiesPanel_->showTextTransform(core::evaluateTransform(target.base, target.animation, target.fileTime), state,
                                                textClip != nullptr ? textClip->style.centerX : 0.5,
                                                textClip != nullptr ? textClip->style.centerY : 0.5, frame.width(), frame.height());
            propertiesPanel_->miniTimeline()->setData(target.from, target.to, target.inside ? target.fileTime : -1.0,
                                                      keyMarks(target.animation));
        } else {
            propertiesPanel_->showText();
        }
        return;
    }
    if (const int transition = timeline_->selectedTransition(); transition != 0) {
        if (const core::TransitionClip* clip = project_.transitions().find(transition)) {
            const double limit = std::max(core::kMinTransition, project_.transitionLimits(clip->beforeFragment).max());
            propertiesPanel_->showTransitionClip(*clip, core::kMinTransition, limit);
            return;
        }
    }
    // Un pezzo scelto da solo (di V1 o di un livello): valori al cursore e rombi dei fotogrammi chiave.
    if (const AnimTarget target = animationTarget(); target.valid) {
        const PropertiesPanel::AnimationState state =
            animationState(target.animation, target.fileTime, target.from, target.to, target.inside);
        propertiesFollowCursor_ = true;
        propertiesPanel_->showTransform(core::evaluateTransform(target.base, target.animation, target.fileTime), 1, state);
        propertiesPanel_->miniTimeline()->setData(target.from, target.to, target.inside ? target.fileTime : -1.0,
                                                  keyMarks(target.animation));
        return;
    }
    const std::vector<int> pieces = selectedPieceIds(objects);
    for (int id : pieces) {
        if (const core::Fragment* f = project_.sequence().find(id)) {
            propertiesPanel_->showTransform(f->transform, static_cast<int>(pieces.size()));
            return;
        }
    }
    QString message = QStringLiteral("Scegli un pezzo video sulla linea del tempo per cambiarne scala, posizione, rotazione e ritaglio.");
    if (!objects.empty()) {
        const core::ObjectRef::Kind kind = objects.front().kind;
        if (kind == core::ObjectRef::Kind::AudioClip || kind == core::ObjectRef::Kind::PieceAudio)
            message = QStringLiteral("Per l'audio il livello si cambia con la linea sulla clip e con i comandi della corsia.");
    }
    propertiesPanel_->showNothing(message);
}

MainWindow::AnimTarget MainWindow::animationTarget() const
{
    AnimTarget target;
    if (timeline_ == nullptr || program_ == nullptr)
        return target;
    const std::vector<core::ObjectRef> objects = timeline_->allSelectedObjects();
    const double position = program_->position();
    if (objects.size() == 1 && objects.front().kind == core::ObjectRef::Kind::TextClip) {
        const core::TextClip* clip = project_.textTracks().find(objects.front().id);
        if (clip == nullptr)
            return target;
        target.text = true;
        target.id = clip->id;
        target.from = 0.0;  // i tempi delle chiavi sono dall'inizio della scritta
        target.to = clip->length;
        target.sequenceStart = clip->position;
        target.base = clip->transform;
        target.animation = clip->animation;
    } else if (objects.size() == 1 && objects.front().kind == core::ObjectRef::Kind::LayerPiece) {
        const core::LayerPiece* piece = project_.textTracks().findPiece(objects.front().id);
        if (piece == nullptr)
            return target;
        target.layer = true;
        target.id = piece->id;
        target.from = piece->start;
        target.to = piece->end;
        target.sequenceStart = piece->position;
        target.base = piece->transform;
        target.animation = piece->animation;
    } else {
        const std::vector<int> pieces = selectedPieceIds(objects);
        if (pieces.size() != 1)
            return target;
        const core::Fragment* f = project_.sequence().find(pieces.front());
        if (f == nullptr)
            return target;
        target.id = f->id;
        target.from = f->start;
        target.to = f->end;
        target.sequenceStart = project_.sequence().startOf(project_.sequence().indexOf(f->id));
        target.base = f->transform;
        target.animation = f->animation;
    }
    const double offset = position - target.sequenceStart;
    target.inside = offset >= -1e-9 && offset <= (target.to - target.from) + 1e-9;
    target.fileTime = target.from + std::clamp(offset, 0.0, target.to - target.from);
    target.valid = true;
    return target;
}

std::array<double, 4> MainWindow::keyCurve(core::AnimGroup group, double time) const
{
    const AnimTarget target = animationTarget();
    if (target.valid) {
        if (const core::AnimKey* key = core::firstGroupKey(target.animation, group, time)) {
            switch (key->interp) {
                case core::Interp::Bezier: return {key->c1x, key->c1y, key->c2x, key->c2y};
                case core::Interp::Smooth: return {0.42, 0.0, 0.58, 1.0};
                case core::Interp::Linear:
                case core::Interp::Hold: break;
            }
        }
    }
    return {0.0, 0.0, 1.0, 1.0};
}

bool MainWindow::applyKeyCurve(core::AnimGroup group, double time, const std::array<double, 4>& c)
{
    return editAnimationKeys(QStringLiteral("Curva del passaggio"), [&](core::TransformAnimation& animation, core::Transform&) {
        return core::setGroupKeysInterp(animation, group, time, core::Interp::Bezier, c[0], c[1], c[2], c[3]);
    });
}

void MainWindow::editKeyCurve(core::AnimGroup group, double time)
{
    CurveEditorDialog dialog(keyCurve(group, time), this);
    if (dialog.exec() == QDialog::Accepted)
        applyKeyCurve(group, time, dialog.values());
}

void MainWindow::refreshKeyRow()
{
    if (timeline_ == nullptr)
        return;
    SequenceTimeline::KeyRow row;
    if (const AnimTarget target = animationTarget(); target.valid) {
        row.layerPiece = target.layer;
        row.text = target.text;
        row.id = target.id;
        for (int g = 0; g < core::kAnimGroupCount; ++g) {
            const auto group = static_cast<core::AnimGroup>(g);
            for (const double time : core::groupKeyTimes(target.animation, group))
                row.keys.push_back({group, time, core::groupInterpAt(target.animation, group, time)});
        }
    }
    timeline_->setKeyRow(std::move(row));
}

bool MainWindow::editAnimationKeys(const QString& label, const std::function<bool(core::TransformAnimation&, core::Transform&)>& edit)
{
    const AnimTarget target = animationTarget();
    if (!target.valid)
        return false;
    core::Transform base = target.base;
    core::TransformAnimation animation = target.animation;
    if (!edit(animation, base))
        return false;
    const core::Project before = project_;
    if (!writeAnimTarget(target, base, animation))
        return false;
    transformMergeKey_.clear();
    recordEdit(label, before);
    return true;
}

bool MainWindow::writeAnimTarget(const AnimTarget& target, const core::Transform& base, const core::TransformAnimation& animation)
{
    if (target.text) {
        const bool a = project_.textTracks().setTransform(target.id, core::normalized(base));
        const bool b = project_.textTracks().setAnimation(target.id, animation);
        return a || b;
    }
    if (target.layer) {
        const bool a = project_.textTracks().setPieceTransform(target.id, core::normalized(base));
        const bool b = project_.textTracks().setPieceAnimation(target.id, animation);
        return a || b;
    }
    const int a = project_.setTransform({target.id}, base);
    const int b = project_.setAnimation({target.id}, animation);
    return a + b > 0;
}

bool MainWindow::setTextPosition(std::optional<double> x, std::optional<double> y)
{
    const AnimTarget target = animationTarget();
    if (!target.valid || !target.text || (!x && !y))
        return false;
    const core::TextClip* clip = project_.textTracks().find(target.id);
    if (clip == nullptr)
        return false;
    const QSize frame = frameSize();
    const bool animX = target.animation.animated(core::AnimParam::PosX);
    const bool animY = target.animation.animated(core::AnimParam::PosY);
    core::Transform base = target.base;
    core::TransformAnimation animation = target.animation;
    core::TextStyle style = clip->style;
    // Un asse non animato: la posizione va nel centro di stile (la scritta resta dove la si vede) e lo spostamento si azzera.
    if (x && !animX) {
        style.centerX = *x;
        base.posX = 0.0;
    }
    if (y && !animY) {
        style.centerY = *y;
        base.posY = 0.0;
    }
    // Un asse animato: la chiave sul cursore (lo spostamento è la differenza dal centro di stile).
    if ((x && animX) || (y && animY)) {
        if (!target.inside)
            return false;
        const core::Transform shown = core::evaluateTransform(base, animation, target.fileTime);
        core::Transform edited = shown;
        if (x && animX)
            edited.posX = (*x - style.centerX) * frame.width();
        if (y && animY)
            edited.posY = (*y - style.centerY) * frame.height();
        core::applyEditWithKeys(animation, base, shown, edited, target.fileTime);
    }
    base = core::normalized(base);
    if (style == clip->style && base == clip->transform && animation == clip->animation)
        return false;
    const core::Project before = project_;
    core::TextTracks& tracks = project_.textTracks();
    tracks.setStyle(target.id, style);
    tracks.setTransform(target.id, base);
    tracks.setAnimation(target.id, animation);
    const std::string key = std::string("posizioneScritta") + (x ? "X" : "") + (y ? "Y" : "") + ":" + std::to_string(target.id) + ":"
                            + std::to_string(std::lround(target.fileTime * 1000.0));
    const bool continues = transformMergeKey_ == key && transformMergeCount_ == history_.undoCount() && history_.canUndo()
                        && history_.undoLabel() == "Posizione della scritta" && !history_.canRedo();
    if (continues && history_.amend())
        sequenceChanged();
    else
        recordEdit(QStringLiteral("Posizione della scritta"), before);
    transformMergeKey_ = key;
    transformMergeCount_ = history_.undoCount();
    return true;
}

bool MainWindow::applyTransformEdit(const std::function<void(core::Transform&)>& edit, const std::string& field)
{
    // Un pezzo scelto da solo (di V1 o di un livello): i parametri animati ricevono una chiave sul cursore (auto-key), il resto
    // cambia la trasformazione fissa.
    if (const AnimTarget target = animationTarget(); target.valid) {
        const core::Transform shown = core::evaluateTransform(target.base, target.animation, target.fileTime);
        core::Transform edited = shown;
        edit(edited);
        core::Transform base = target.base;
        core::TransformAnimation animation = target.animation;
        if (!core::applyEditWithKeys(animation, base, shown, edited, target.fileTime))
            return false;
        const core::Project before = project_;
        if (!writeAnimTarget(target, base, animation))
            return false;
        // Un cursore o una casella girati più volte di fila sullo stesso campo (e sullo stesso istante) sono una sola voce di Annulla.
        const std::string key = field + (target.text ? ":T" : target.layer ? ":L" : ":") + std::to_string(target.id) + ":"
                                + std::to_string(std::lround(target.fileTime * 1000.0));
        const bool continues = transformMergeKey_ == key && transformMergeCount_ == history_.undoCount() && history_.canUndo()
                            && history_.undoLabel() == "Cambia trasformazione" && !history_.canRedo();
        if (continues && history_.amend())
            sequenceChanged();
        else
            recordEdit(QStringLiteral("Cambia trasformazione"), before);
        transformMergeKey_ = key;
        transformMergeCount_ = history_.undoCount();
        return true;
    }
    const std::vector<int> ids = selectedPieceIds(timeline_->allSelectedObjects());
    if (ids.empty())
        return false;
    const core::Project before = project_;
    std::string key = field;
    int changed = 0;
    for (int id : ids) {
        const core::Fragment* f = project_.sequence().find(id);
        if (f == nullptr)
            continue;
        core::Transform t = f->transform;
        edit(t);
        changed += project_.setTransform({id}, t);
        key += ':' + std::to_string(id);
    }
    if (changed == 0)
        return false;
    // Un cursore o una casella girati più volte di fila sullo stesso campo sono una sola voce di Annulla.
    const bool continues = transformMergeKey_ == key && transformMergeCount_ == history_.undoCount() && history_.canUndo()
                        && history_.undoLabel() == "Cambia trasformazione" && !history_.canRedo();
    if (continues && history_.amend())
        sequenceChanged();
    else
        recordEdit(QStringLiteral("Cambia trasformazione"), before);
    transformMergeKey_ = key;
    transformMergeCount_ = history_.undoCount();
    return true;
}

bool MainWindow::commitMonitorTransform(int fragmentId, const core::Transform& transform)
{
    if (project_.sequence().find(fragmentId) == nullptr)
        return false;
    if (commitAnimatedMonitorTransform(false, fragmentId, transform))
        return true;
    const core::Project before = project_;
    if (project_.setTransform({fragmentId}, transform) == 0)
        return false;
    transformMergeKey_.clear();  // un gesto è una voce di Annulla a sé: non si unisce alle modifiche del pannello
    recordEdit(QStringLiteral("Trasforma sul monitor"), before);
    return true;
}

bool MainWindow::commitMonitorTextTransform(int clipId, const core::Transform& transform)
{
    const core::TextClip* clip = project_.textTracks().find(clipId);
    if (clip == nullptr)
        return false;
    if (commitAnimatedMonitorTransform(false, clipId, transform, true))
        return true;
    if (!clip->animation.empty())
        return false;  // (con l'animazione serve un solo oggetto scelto: così non si cambierebbe quello che si vede)
    const core::Project before = project_;
    if (!project_.textTracks().setTransform(clipId, core::normalized(transform)))
        return false;
    transformMergeKey_.clear();
    recordEdit(QStringLiteral("Trasforma sul monitor"), before);
    return true;
}

bool MainWindow::commitMonitorLayerTransform(int pieceId, const core::Transform& transform)
{
    if (project_.textTracks().findPiece(pieceId) == nullptr)
        return false;
    if (commitAnimatedMonitorTransform(true, pieceId, transform))
        return true;
    const core::Project before = project_;
    if (!project_.textTracks().setPieceTransform(pieceId, transform))
        return false;
    transformMergeKey_.clear();
    recordEdit(QStringLiteral("Trasforma sul monitor"), before);
    return true;
}

bool MainWindow::commitAnimatedMonitorTransform(bool layer, int id, const core::Transform& edited, bool text)
{
    // Un pezzo con l'animazione: il monitor mostrava la trasformazione dell'istante sul cursore e `edited` è quella dopo il
    // gesto. I parametri animati che sono cambiati ricevono una chiave sul cursore (come nel pannello), gli altri cambiano la
    // trasformazione fissa del pezzo.
    const AnimTarget target = animationTarget();
    if (!target.valid || target.text != text || target.layer != layer || target.id != id || target.animation.empty() || !target.inside)
        return false;
    const core::Transform shown = core::evaluateTransform(target.base, target.animation, target.fileTime);
    core::Transform base = target.base;
    core::TransformAnimation animation = target.animation;
    if (!core::applyEditWithKeys(animation, base, shown, edited, target.fileTime))
        return false;
    const core::Project before = project_;
    if (!writeAnimTarget(target, base, animation))
        return false;
    transformMergeKey_.clear();
    recordEdit(QStringLiteral("Trasforma sul monitor"), before);
    return true;
}

void MainWindow::applyAnimationAction(core::AnimGroup group, int actionValue)
{
    const auto action = static_cast<PropertiesPanel::AnimAction>(actionValue);
    const AnimTarget target = animationTarget();
    if (!target.valid)
        return;
    if (action == PropertiesPanel::AnimAction::PrevKey || action == PropertiesPanel::AnimAction::NextKey) {
        const core::GroupState state = core::groupState(target.animation, group, target.fileTime, target.from, target.to);
        const bool previous = action == PropertiesPanel::AnimAction::PrevKey;
        if (previous ? !state.hasPrev : !state.hasNext)
            return;
        const double fileTime = previous ? state.prevTime : state.nextTime;
        setProgramActive(true);
        program_->seekTo(target.sequenceStart + (fileTime - target.from));
        return;
    }
    core::Transform base = target.base;
    core::TransformAnimation animation = target.animation;
    QString label;
    if (action == PropertiesPanel::AnimAction::ToggleKey) {
        if (!target.inside)
            return;
        core::toggleGroupKey(animation, base, group, target.fileTime);
        label = QStringLiteral("Fotogramma chiave");
    } else {
        for (int g = 0; g < core::kAnimGroupCount; ++g) {
            if (action == PropertiesPanel::AnimAction::Reset && g != static_cast<int>(group))
                continue;
            core::resetGroup(animation, base, static_cast<core::AnimGroup>(g));
        }
        if (action == PropertiesPanel::AnimAction::ResetAll || group == core::AnimGroup::Opacity)
            base.flipHorizontal = base.flipVertical = false;
        label = QStringLiteral("Ripristina l'animazione");
    }
    const core::Project before = project_;
    if (!writeAnimTarget(target, base, animation))
        return;
    transformMergeKey_.clear();
    recordEdit(label, before);
}

void MainWindow::fitSelectedPieces(bool fill)
{
    const core::StreamSignature* target = project_.format().video();
    const int projectW = target != nullptr ? target->width : 0;
    const int projectH = target != nullptr ? target->height : 0;
    const std::vector<int> ids = selectedPieceIds(timeline_->allSelectedObjects());
    if (ids.empty() || projectW <= 0 || projectH <= 0)
        return;
    const core::Project before = project_;
    int changed = 0;
    for (int id : ids) {
        const core::Fragment* f = project_.sequence().find(id);
        const core::Clip* clip = f != nullptr ? project_.find(f->clipId) : nullptr;
        if (clip == nullptr)
            continue;
        double factor = 1.0;
        if (fill) {
            const core::MediaSignature* signature = signatureFor(clip->path);
            const core::StreamSignature* video =
                signature != nullptr ? core::findStream(signature->streams, core::StreamKind::Video) : nullptr;
            if (video != nullptr && video->width > 0 && video->height > 0) {
                const double fx = static_cast<double>(projectW) / video->width;
                const double fy = static_cast<double>(projectH) / video->height;
                factor = std::max(fx, fy) / std::min(fx, fy);
            }
        }
        core::Transform t = f->transform;
        t.scaleX = t.scaleY = factor;
        t.posX = t.posY = 0.0;
        t.cropLeft = t.cropRight = t.cropTop = t.cropBottom = 0.0;
        changed += project_.setTransform({id}, t);
        // Scala, posizione e ritaglio animati: l'animazione si spegne, altrimenti coprirebbe il valore appena messo.
        core::TransformAnimation animation = f->animation;
        for (const core::AnimGroup g : {core::AnimGroup::Scale, core::AnimGroup::Position, core::AnimGroup::Crop})
            for (const core::AnimParam p : core::groupParams(g))
                core::clearTrack(animation, p);
        changed += project_.setAnimation({id}, animation);
    }
    if (changed == 0)
        return;
    transformMergeKey_.clear();
    recordEdit(fill ? QStringLiteral("Riempi il fotogramma") : QStringLiteral("Adatta al fotogramma"), before);
}

}  // namespace editor::app
