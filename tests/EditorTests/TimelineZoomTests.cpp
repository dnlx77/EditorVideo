#include "EditorCore/TimelineZoom.h"

#include <gtest/gtest.h>

using editor::core::TimelineZoom;

namespace {

// 100 s in 1000 pixel, zoom massimo 12 pixel per fotogramma a 25 fps.
TimelineZoom sample()
{
    TimelineZoom zoom;
    zoom.setDuration(100.0);
    zoom.setWidth(1000.0);
    zoom.setMaxPixelsPerSecond(12.0 * 25.0);
    return zoom;
}

TEST(TimelineZoom, FitShowsEverything)
{
    const TimelineZoom zoom = sample();
    EXPECT_TRUE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 10.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);
    EXPECT_DOUBLE_EQ(zoom.xForTime(0.0), 0.0);
    EXPECT_DOUBLE_EQ(zoom.xForTime(100.0), 1000.0);
    EXPECT_DOUBLE_EQ(zoom.timeForX(250.0), 25.0);
    EXPECT_DOUBLE_EQ(zoom.timeForX(-20.0), 0.0);    // fuori dalla traccia: limitato alla durata
    EXPECT_DOUBLE_EQ(zoom.timeForX(5000.0), 100.0);
}

TEST(TimelineZoom, ConversionsWithZoomAndScroll)
{
    TimelineZoom zoom = sample();
    zoom.zoomAt(4.0, 0.0);  // 40 px/s, 25 s visibili, dall'inizio
    EXPECT_FALSE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 40.0);
    EXPECT_DOUBLE_EQ(zoom.visibleSeconds(), 25.0);
    zoom.scrollTo(30.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 30.0);
    EXPECT_DOUBLE_EQ(zoom.xForTime(30.0), 0.0);
    EXPECT_DOUBLE_EQ(zoom.xForTime(40.0), 400.0);
    EXPECT_DOUBLE_EQ(zoom.xForTime(10.0), -800.0);  // prima della parte visibile
    for (double x : {0.0, 123.0, 500.0, 999.0})
        EXPECT_NEAR(zoom.xForTime(zoom.timeForX(x)), x, 1e-9);
    EXPECT_DOUBLE_EQ(zoom.contentWidth(), 4000.0);
    zoom.scrollBy(400.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 40.0);
}

TEST(TimelineZoom, ZoomKeepsTheTimeUnderTheMouse)
{
    TimelineZoom zoom = sample();
    for (double x : {0.0, 137.0, 500.0, 999.0}) {
        for (double factor : {1.25, 2.0, 1.0 / 1.25}) {
            const double before = zoom.timeForX(x);
            zoom.zoomAt(factor, x);
            // Riducendo, vicino all'inizio o alla fine la vista può dover scorrere (prima di 0 non c'è niente):
            // lì il tempo sotto il mouse può spostarsi; ingrandendo mai.
            if (!zoom.isFit() && factor > 1.0)
                EXPECT_NEAR(zoom.timeForX(x), before, 1e-9) << "x " << x << " fattore " << factor;
        }
        zoom.zoomAt(3.0, x);
    }
    // Anche vicino alla fine (dove lo scorrimento è al limite) il tempo sotto il mouse non si sposta.
    TimelineZoom end = sample();
    end.zoomAt(5.0, 990.0);
    EXPECT_NEAR(end.timeForX(990.0), 99.0, 1e-9);
}

TEST(TimelineZoom, Limits)
{
    TimelineZoom zoom = sample();
    for (int i = 0; i < 100; ++i)
        zoom.zoomAt(2.0, 500.0);
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 300.0);  // 12 px per fotogramma
    for (int i = 0; i < 100; ++i)
        zoom.zoomAt(0.5, 500.0);
    // Oltre "Adatta": tutto in un decimo della larghezza, a sinistra, senza scorrimento.
    EXPECT_FALSE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 1.0);
    EXPECT_DOUBLE_EQ(zoom.contentWidth(), 100.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);
    EXPECT_FALSE(zoom.canScroll());
    EXPECT_DOUBLE_EQ(zoom.xForTime(100.0), 100.0);
    EXPECT_DOUBLE_EQ(zoom.timeForX(600.0), 100.0);  // nello spazio vuoto a destra: la fine
    zoom.scrollTo(30.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);
    zoom.fit();

    zoom.zoomAt(4.0, 0.0);
    zoom.scrollTo(-5.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);
    zoom.scrollTo(1000.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 75.0);  // l'ultima parte visibile finisce alla fine
    zoom.fit();
    zoom.scrollTo(30.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);  // con "Adatta" non si scorre

    // Un file brevissimo: anche a "Adatta" si supera il massimo, e lo zoom non può andare oltre.
    TimelineZoom shortClip;
    shortClip.setDuration(0.5);
    shortClip.setWidth(1000.0);
    shortClip.setMaxPixelsPerSecond(300.0);
    shortClip.zoomAt(2.0, 100.0);
    EXPECT_TRUE(shortClip.isFit());
    EXPECT_DOUBLE_EQ(shortClip.pixelsPerSecond(), 2000.0);
    shortClip.zoomAt(0.5, 100.0);  // ma si può rimpicciolire
    EXPECT_DOUBLE_EQ(shortClip.pixelsPerSecond(), 1000.0);

    // Una durata a zero non deve dare divisioni per zero.
    TimelineZoom empty;
    empty.zoomAt(2.0, 10.0);
    EXPECT_DOUBLE_EQ(empty.timeForX(500.0), 0.0);
}

TEST(TimelineZoom, ResizeAndDurationChanges)
{
    TimelineZoom zoom = sample();
    zoom.zoomAt(4.0, 0.0);
    zoom.scrollTo(75.0);
    zoom.setWidth(2000.0);  // più largo: stessi pixel per secondo, più secondi visibili
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 40.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 50.0);  // limitato perché la fine resti in fondo
    zoom.setWidth(5000.0);  // ora tutto ci sta largamente: stesso zoom, spazio vuoto a destra, niente scorrimento
    EXPECT_FALSE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 40.0);
    EXPECT_FALSE(zoom.canScroll());
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);

    TimelineZoom fit = sample();
    fit.setDuration(200.0);  // con "Adatta" si resta ad "Adatta"
    EXPECT_TRUE(fit.isFit());
    EXPECT_DOUBLE_EQ(fit.pixelsPerSecond(), 5.0);
}

TEST(TimelineZoom, ZoomingOutBeyondFit)
{
    TimelineZoom zoom = sample();
    zoom.zoomAt(0.5, 700.0);  // metà: 50 s in 500 px, il resto vuoto
    EXPECT_FALSE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 5.0);
    EXPECT_DOUBLE_EQ(zoom.contentWidth(), 500.0);
    EXPECT_DOUBLE_EQ(zoom.offset(), 0.0);
    EXPECT_FALSE(zoom.canScroll());
    EXPECT_DOUBLE_EQ(zoom.xForTime(50.0), 250.0);
    // Il contenuto parte dal bordo sinistro: lì il tempo sotto il mouse resta fermo anche rimpicciolendo.
    zoom.zoomAt(0.5, 0.0);
    EXPECT_DOUBLE_EQ(zoom.timeForX(0.0), 0.0);
    EXPECT_FALSE(zoom.follow(90.0));  // tutto visibile: niente pagine
    // Riingrandendo oltre "Adatta" il tempo sotto il mouse torna a restare fermo.
    zoom.zoomAt(8.0, 0.0);  // 20 px/s: più largo della vista
    EXPECT_TRUE(zoom.canScroll());
    const double before = zoom.timeForX(300.0);
    zoom.zoomAt(1.5, 300.0);
    EXPECT_NEAR(zoom.timeForX(300.0), before, 1e-9);
    zoom.fit();
    EXPECT_TRUE(zoom.isFit());
    EXPECT_DOUBLE_EQ(zoom.pixelsPerSecond(), 10.0);
}

TEST(TimelineZoom, FollowTurnsPages)
{
    TimelineZoom zoom = sample();
    EXPECT_FALSE(zoom.follow(80.0));  // con "Adatta" si vede già tutto
    zoom.zoomAt(4.0, 0.0);            // 0 .. 25 s
    EXPECT_FALSE(zoom.follow(10.0));
    EXPECT_FALSE(zoom.follow(24.9));
    EXPECT_TRUE(zoom.follow(25.0));   // pagina successiva: il cursore poco dopo il bordo sinistro
    EXPECT_NEAR(zoom.offset(), 25.0 - 25.0 * 0.05, 1e-9);
    EXPECT_FALSE(zoom.follow(30.0));  // resta sulla stessa pagina finché non ci esce
    EXPECT_TRUE(zoom.follow(5.0));    // all'indietro (es. dopo un salto)
    EXPECT_LE(zoom.offset(), 5.0);
    EXPECT_TRUE(zoom.follow(99.0));   // verso la fine: l'ultima pagina
    EXPECT_DOUBLE_EQ(zoom.offset(), 75.0);
}

}  // namespace
