#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <string>
#include <vector>
#include <memory>
#include <sstream>

enum class TaskStatus {
    TODO,
    IN_PROGRESS,
    DONE,
    FAILED
};

struct Task {
    int id;
    int parent_id;          // 0 = root task
    int order;              // Order within parent's children
    std::string description;
    TaskStatus status;
    std::vector<int> children;  // IDs of child tasks

    std::string status_to_string() const {
        switch (status) {
            case TaskStatus::TODO: return "TODO";
            case TaskStatus::IN_PROGRESS: return "IN_PROGRESS";
            case TaskStatus::DONE: return "DONE";
            case TaskStatus::FAILED: return "FAILED";
            default: return "UNKNOWN";
        }
    }

    std::string status_icon() const {
        switch (status) {
            case TaskStatus::TODO: return "\xe2\x97\x8f";    // ● — open circle (needs work)
            case TaskStatus::IN_PROGRESS: return "\xe2\x8f\xb3"; // ⏳ — hourglass
            case TaskStatus::DONE: return "\xe2\x9c\x85";     // ✅ — check mark
            case TaskStatus::FAILED: return "\xe2\x9d\x8c";   // ❌ — cross mark
            default: return "?";
        }
    }
};

class TaskManager {
public:
    TaskManager() : next_id_(1) {}

    // Create a root task with no parent
    int create_task(const std::string& description);

    // Create a subtask under a parent
    int create_subtask(int parent_id, const std::string& description);

    // Decompose a task into steps (creates multiple subtasks)
    // Returns the IDs of the created subtasks
    std::vector<int> decompose_task(int task_id, const std::vector<std::string>& steps);

    // Set task status
    bool set_task_status(int id, TaskStatus status);

    // Complete a task (marks DONE)
    bool complete_task(int id);

    // Fail a task (marks FAILED)
    bool fail_task(int id);

    // Get a task by ID (returns nullptr if not found)
    const Task* get_task(int id) const;
    Task* get_task_mutable(int id);

    // Get subtasks of a parent
    std::vector<const Task*> get_subtasks(int parent_id) const;

    // Get all root tasks (parent_id == 0)
    std::vector<const Task*> get_root_tasks() const;

    // Check if a task has subtasks
    bool has_subtasks(int id) const;

    // Get formatted task summary (for system prompt injection)
    std::string get_summary() const;

    // Get formatted tree display (for CLI output)
    std::string format_tree() const;

    // Get detailed task info
    std::string format_task_detail(int id) const;

    const std::vector<Task>& get_tasks() const { return tasks_; }
    bool empty() const { return tasks_.empty(); }
    void clear() { tasks_.clear(); next_id_ = 1; }

private:
    // Recursive helper for tree formatting
    void format_tree_node(std::stringstream& ss, const Task& task, const std::string& prefix, bool is_last) const;

    std::vector<Task> tasks_;
    int next_id_;
};

#endif // TASK_MANAGER_H
