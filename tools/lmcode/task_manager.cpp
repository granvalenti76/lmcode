#include "task_manager.h"
#include <algorithm>
#include <sstream>

int TaskManager::create_task(const std::string& description) {
    Task new_task;
    new_task.id = next_id_++;
    new_task.parent_id = 0;
    new_task.order = 0;
    new_task.description = description;
    new_task.status = TaskStatus::TODO;
    tasks_.push_back(new_task);
    return new_task.id;
}

int TaskManager::create_subtask(int parent_id, const std::string& description) {
    // Find parent to update its children list and get next order
    Task* parent = nullptr;
    for (auto& t : tasks_) {
        if (t.id == parent_id) {
            parent = &t;
            break;
        }
    }
    if (!parent) return -1; // Parent not found

    int order = (int)parent->children.size();

    Task new_task;
    new_task.id = next_id_++;
    new_task.parent_id = parent_id;
    new_task.order = order;
    new_task.description = description;
    new_task.status = TaskStatus::TODO;
    tasks_.push_back(new_task);

    parent->children.push_back(new_task.id);
    return new_task.id;
}

std::vector<int> TaskManager::decompose_task(int task_id, const std::vector<std::string>& steps) {
    std::vector<int> created_ids;

    // Mark the parent as IN_PROGRESS
    set_task_status(task_id, TaskStatus::IN_PROGRESS);

    for (const auto& step : steps) {
        int id = create_subtask(task_id, step);
        if (id >= 0) {
            created_ids.push_back(id);
        }
    }

    return created_ids;
}

bool TaskManager::set_task_status(int id, TaskStatus status) {
    for (auto& task : tasks_) {
        if (task.id == id) {
            task.status = status;
            return true;
        }
    }
    return false;
}

bool TaskManager::complete_task(int id) {
    return set_task_status(id, TaskStatus::DONE);
}

bool TaskManager::fail_task(int id) {
    return set_task_status(id, TaskStatus::FAILED);
}

const Task* TaskManager::get_task(int id) const {
    for (const auto& task : tasks_) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

Task* TaskManager::get_task_mutable(int id) {
    for (auto& task : tasks_) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

std::vector<const Task*> TaskManager::get_subtasks(int parent_id) const {
    std::vector<const Task*> result;
    for (const auto& task : tasks_) {
        if (task.parent_id == parent_id) {
            result.push_back(&task);
        }
    }
    // Sort by order
    std::sort(result.begin(), result.end(),
        [](const Task* a, const Task* b) { return a->order < b->order; });
    return result;
}

std::vector<const Task*> TaskManager::get_root_tasks() const {
    return get_subtasks(0);
}

bool TaskManager::has_subtasks(int id) const {
    const Task* task = get_task(id);
    return task && !task->children.empty();
}

std::string TaskManager::get_summary() const {
    std::stringstream ss;
    if (tasks_.empty()) {
        return "";
    }

    ss << "[Task Progress]\n";
    auto roots = get_root_tasks();
    for (size_t i = 0; i < roots.size(); i++) {
        format_tree_node(ss, *roots[i], "", i == roots.size() - 1);
    }
    return ss.str();
}

void TaskManager::format_tree_node(std::stringstream& ss, const Task& task,
                                    const std::string& prefix, bool is_last) const {
    // Choose connector
    std::string connector = is_last ? "\xe2\x94\x94\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80 "; // └─ / ├─
    std::string child_prefix = prefix + (is_last ? "    " : "\xe2\x94\x82   "); // "    " / "│   "

    ss << prefix << connector
       << task.status_icon() << " [" << task.id << "] "
       << task.description << " (" << task.status_to_string() << ")\n";

    // Format children in order
    auto children = get_subtasks(task.id);
    for (size_t i = 0; i < children.size(); i++) {
        format_tree_node(ss, *children[i], child_prefix, i == children.size() - 1);
    }
}

std::string TaskManager::format_tree() const {
    std::stringstream ss;
    if (tasks_.empty()) {
        ss << "No active tasks.\n";
        return ss.str();
    }

    ss << "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80 Task Progress "; // ┌── Task Progress
    ss << "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\n";
    auto roots = get_root_tasks();
    for (size_t i = 0; i < roots.size(); i++) {
        format_tree_node(ss, *roots[i], "", i == roots.size() - 1);
    }
    return ss.str();
}

std::string TaskManager::format_task_detail(int id) const {
    const Task* task = get_task(id);
    if (!task) return "Task not found.";

    std::stringstream ss;
    ss << "Task #" << task->id << ": " << task->description << "\n";
    ss << "  Status: " << task->status_icon() << " " << task->status_to_string() << "\n";
    ss << "  Parent: " << (task->parent_id == 0 ? "(root)" : "#" + std::to_string(task->parent_id)) << "\n";
    ss << "  Subtasks: " << task->children.size() << "\n";

    if (!task->children.empty()) {
        auto children = get_subtasks(id);
        for (size_t i = 0; i < children.size(); i++) {
            ss << "    " << (i + 1) << ". "
               << children[i]->status_icon() << " "
               << children[i]->description << "\n";
        }
    }

    return ss.str();
}
