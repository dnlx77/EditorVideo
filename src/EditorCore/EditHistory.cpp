#include "EditorCore/EditHistory.h"

namespace editor::core {

EditHistory::EditHistory(std::size_t limit)
    : limit_(limit > 0 ? limit : 1)
{
}

void EditHistory::record(std::string label, const Project& before)
{
    // Lo stato salvato era fra quelli da ripetere: dopo questa modifica non si ritrova più.
    if (clean_ > static_cast<long long>(undo_.size()))
        clean_ = -1;
    undo_.push_back({std::move(label), before});
    redo_.clear();
    if (undo_.size() > limit_) {
        undo_.erase(undo_.begin());
        if (clean_ >= 0)
            --clean_;  // a 0 diventa -1: lo stato salvato era proprio quello tolto
    }
}

bool EditHistory::amend()
{
    if (undo_.empty() || !redo_.empty())
        return false;
    if (clean_ == static_cast<long long>(undo_.size()))
        clean_ = -1;
    return true;
}

std::string EditHistory::undoLabel() const
{
    return undo_.empty() ? std::string() : undo_.back().label;
}

std::string EditHistory::redoLabel() const
{
    return redo_.empty() ? std::string() : redo_.back().label;
}

bool EditHistory::undo(Project& project)
{
    if (undo_.empty())
        return false;
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({entry.label, project});
    project.restoreEdits(entry.state);
    return true;
}

bool EditHistory::redo(Project& project)
{
    if (redo_.empty())
        return false;
    Entry entry = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({entry.label, project});
    project.restoreEdits(entry.state);
    return true;
}

void EditHistory::clear()
{
    undo_.clear();
    redo_.clear();
    clean_ = 0;
}

void EditHistory::setClean()
{
    clean_ = static_cast<long long>(undo_.size());
}

bool EditHistory::isClean() const
{
    return clean_ == static_cast<long long>(undo_.size());
}

}  // namespace editor::core
