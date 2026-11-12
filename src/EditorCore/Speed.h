#pragma once

#include "EditorCore/TimeUtil.h"

#include <algorithm>
#include <cmath>

namespace editor::core {

// Velocità di un pezzo (o clip audio): costante, da kMinSpeed a kMaxSpeed volte la normale, con la lettura del file
// all'indietro se `reverse`. Il pezzo prende [start, end) del file e nel montaggio dura (end - start) / speed.
inline constexpr double kMinSpeed = 0.1;
inline constexpr double kMaxSpeed = 10.0;

inline double clampSpeed(double speed)
{
    return std::clamp(speed, kMinSpeed, kMaxSpeed);
}

// Il tratto [start, end) del file visto con `speed`: la durata nel montaggio, e le conversioni fra tempo del file e scostamento
// (secondi dall'inizio del pezzo nel montaggio). Con `reverse` l'inizio del pezzo nel montaggio è la fine del tratto del file.
struct SpeedMap
{
    double start = 0.0;
    double end = 0.0;
    double speed = 1.0;
    bool reverse = false;

    // Il tempo del file che corrisponderebbe al tempo 0 della sequenza se il pezzo, messo in `position`, si estendesse all'indietro:
    // due tratti con la stessa velocità, verso e origine sono sincronizzati (stesso istante del file nello stesso istante).
    double origin(double position) const { return reverse ? end + position * speed : start - position * speed; }

    double length() const { return (end - start) / speed; }
    double fileAt(double offset) const { return reverse ? end - offset * speed : start + offset * speed; }
    double offsetOf(double fileTime) const { return (reverse ? end - fileTime : fileTime - start) / speed; }
    // [from, to) del file come scostamenti nel montaggio (in ordine, anche con reverse).
    TimeRange offsetsOf(double fileFrom, double fileTo) const
    {
        const double a = offsetOf(fileFrom);
        const double b = offsetOf(fileTo);
        return {std::min(a, b), std::max(a, b)};
    }
    // [from, to) in scostamenti del montaggio come tratto del file (in ordine).
    TimeRange fileRangeOf(double offsetFrom, double offsetTo) const
    {
        const double a = fileAt(offsetFrom);
        const double b = fileAt(offsetTo);
        return {std::min(a, b), std::max(a, b)};
    }
};

// Due tratti (di file diversi o no) messi a `positionA` e `positionB` della sequenza sono sincronizzati: stessa velocità, stesso verso,
// e ad ogni istante mostrano lo stesso istante del file (es. un pezzo video e la sua clip audio scollegata).
inline bool inSync(const SpeedMap& a, double positionA, const SpeedMap& b, double positionB, double tolerance = 1e-6)
{
    return std::abs(a.speed - b.speed) < 1e-9 && a.reverse == b.reverse
           && std::abs(a.origin(positionA) - b.origin(positionB)) < tolerance;
}

}  // namespace editor::core
