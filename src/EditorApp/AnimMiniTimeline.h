#pragma once

#include "EditorCore/Animation.h"

#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

namespace editor::app {

// La mini linea del tempo del pannello Proprietà: una riga per ogni trasformazione animata del pezzo scelto, tutta la durata
// del pezzo in orizzontale, i rombi delle chiavi (stessi colori e forme della linea del tempo grande) e il cursore. Clic sul
// vuoto o sul rombo: porta il cursore lì (si può trascinare per scorrere); trascinare un rombo ne sposta la chiave (si
// aggancia al cursore); clic destro: tipo di passaggio o «Togli la chiave». Il progetto non usa moc: callback std::function.
class AnimMiniTimeline : public QWidget
{
public:
    struct Mark
    {
        core::AnimGroup group = core::AnimGroup::Scale;
        double time = 0.0;  // nel file
        core::Interp interp = core::Interp::Linear;
    };
    explicit AnimMiniTimeline(QWidget* parent = nullptr);

    // Le chiavi del pezzo (tempi nel file) che va da `from` a `to`; `cursor` = il cursore nel file (negativo: fuori dal pezzo).
    // Senza chiavi il widget si nasconde.
    // `reversed`: il pezzo è inverso, quindi la fine del file sta a sinistra.
    void setData(double from, double to, double cursor, std::vector<Mark> marks, bool reversed = false);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    std::function<void(double fileTime)> onSeek;
    std::function<void(core::AnimGroup group, double from, double to)> onKeyMoved;
    std::function<void(core::AnimGroup group, double time)> onKeyRemoved;
    std::function<void(core::AnimGroup group, double time, core::Interp interp)> onKeyInterp;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    static constexpr int kLabelWidth = 62;
    static constexpr int kRowHeight = 13;
    std::vector<core::AnimGroup> rows() const;
    double x(double time) const;
    double timeAt(double x) const;
    double y(core::AnimGroup group) const;
    int markAt(const QPointF& point) const;

    double from_ = 0.0;
    double to_ = 0.0;
    double cursor_ = -1.0;
    bool reversed_ = false;
    std::vector<Mark> marks_;
    int dragIndex_ = -1;
    bool dragMoved_ = false;
    bool scrubbing_ = false;
    double dragValue_ = 0.0;

private:
    // Solo per le prove (EditorVideoTests.exe e --smoke-test), che li raggiungono attraverso TestAccess (TestAccess.h).
    friend class TestAccess;
    // Il centro del rombo n (nell'ordine delle chiavi) in coordinate del widget; la chiave scelta.
    QPointF markCenterForTest(int index) const;
    QPointF pointForTimeForTest(core::AnimGroup group, double time) const;
};

}  // namespace editor::app
