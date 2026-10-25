#include "EditorCore/Groups.h"

#include <algorithm>

namespace editor::core {

int Groups::create(const std::vector<ObjectRef>& objects)
{
    std::vector<ObjectRef> members;
    for (const ObjectRef& o : objects) {
        if (std::find(members.begin(), members.end(), o) == members.end())
            members.push_back(o);
    }
    if (members.size() < 2)
        return 0;
    prune([&](const ObjectRef& o) { return std::find(members.begin(), members.end(), o) == members.end(); });
    ObjectGroup group;
    group.id = nextId_++;
    group.members = std::move(members);
    groups_.push_back(std::move(group));
    return groups_.back().id;
}

bool Groups::dissolve(int groupId)
{
    return std::erase_if(groups_, [groupId](const ObjectGroup& g) { return g.id == groupId; }) > 0;
}

int Groups::groupOf(ObjectRef object) const
{
    for (const ObjectGroup& g : groups_) {
        if (std::find(g.members.begin(), g.members.end(), object) != g.members.end())
            return g.id;
    }
    return 0;
}

const ObjectGroup* Groups::find(int groupId) const
{
    const auto it = std::find_if(groups_.begin(), groups_.end(), [groupId](const ObjectGroup& g) { return g.id == groupId; });
    return it == groups_.end() ? nullptr : &*it;
}

void Groups::restore(const std::vector<std::vector<ObjectRef>>& groups)
{
    groups_.clear();
    for (const std::vector<ObjectRef>& members : groups)
        create(members);
}

void Groups::restoreEdits(const Groups& snapshot)
{
    const int next = std::max(nextId_, snapshot.nextId_);
    *this = snapshot;
    nextId_ = next;
}

}  // namespace editor::core
