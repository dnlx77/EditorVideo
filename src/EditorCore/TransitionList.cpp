#include "EditorCore/TransitionList.h"

#include <algorithm>

namespace editor::core {

int TransitionList::add(TransitionClip clip)
{
    clip.id = nextId_++;
    clips_.push_back(std::move(clip));
    return clips_.back().id;
}

bool TransitionList::remove(int id)
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [id](const TransitionClip& c) { return c.id == id; });
    if (it == clips_.end())
        return false;
    clips_.erase(it);
    return true;
}

void TransitionList::clear()
{
    clips_.clear();
}

void TransitionList::restore(std::vector<TransitionClip> clips)
{
    clips_.clear();
    for (TransitionClip& clip : clips)
        add(std::move(clip));
}

void TransitionList::restoreEdits(const TransitionList& snapshot)
{
    clips_ = snapshot.clips_;
    nextId_ = std::max(nextId_, snapshot.nextId_);  // una transizione aggiunta dopo non riprende l'id di una tolta
}

TransitionClip* TransitionList::find(int id)
{
    const auto it = std::find_if(clips_.begin(), clips_.end(), [id](const TransitionClip& c) { return c.id == id; });
    return it == clips_.end() ? nullptr : &*it;
}

const TransitionClip* TransitionList::find(int id) const
{
    return const_cast<TransitionList*>(this)->find(id);
}

const TransitionClip* TransitionList::atJunction(int beforeFragment) const
{
    const auto it = std::find_if(clips_.begin(), clips_.end(),
                                 [beforeFragment](const TransitionClip& c) { return c.beforeFragment == beforeFragment; });
    return it == clips_.end() ? nullptr : &*it;
}

void TransitionList::moveJunction(int fromBeforeFragment, int toBeforeFragment)
{
    for (TransitionClip& clip : clips_)
        if (clip.beforeFragment == fromBeforeFragment)
            clip.beforeFragment = toBeforeFragment;
}

}  // namespace editor::core
