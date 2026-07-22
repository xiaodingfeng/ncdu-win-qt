#pragma once

#include <algorithm>
#include <memory>
#include <stack>
#include <QString>
#include <vector>

// NodeType: distinguishes files, directories, and symlinks.
enum class NodeType {
    File,
    Directory,
    Drive,
    Symlink,
    Other
};

// Structured error kind for a FileNode. Replaces a per-node QString to shrink
// node footprint (~24B → 4B). The user-facing string is reconstructed by
// errorText() at display time.
enum class NodeError {
    None,
    AccessDenied,
    FindFirstFailed,
    EnumFailed
};

// A single node in the scanned file tree.
//
// Fields of a scanned file system entry:
//   name         -> name
//   path         -> path
//   nodeType     -> node_type
//   size         -> size
//   fileCount    -> file_count
//   dirCount     -> dir_count
//   children     -> children
//   parent       -> parent (weak_ptr to break reference cycles)
//   isHidden     -> is_hidden
//   error        -> error (enum; use errorText() for display)
//   skipped      -> skipped
class FileNode {
public:
    QString name;
    QString path;
    NodeType nodeType = NodeType::File;
    qint64 size = 0;
    int fileCount = 0;
    int dirCount = 0;
    std::vector<std::shared_ptr<FileNode>> children;
    std::weak_ptr<FileNode> parent;  // weak to avoid cycles
    bool isHidden = false;
    NodeError error = NodeError::None;
    int lastError = 0;  // Windows error code accompanying `error` (for diagnostics)
    bool skipped = false;

    bool isDir() const
    {
        return nodeType == NodeType::Directory || nodeType == NodeType::Drive;
    }

    int totalItems() const { return fileCount + dirCount; }

    // Reconstruct the user-facing error string from the structured enum.
    // Returns a non-empty QString only when `error != None`.
    QString errorText() const;

    // Sort children largest-first (descending size, ascending lowercase name),
    // then recurse into subdirectories. Iterative (explicit stack) to avoid
    // stack overflow on deeply nested trees.
    void sortBySizeDesc();
};

inline QString FileNode::errorText() const
{
    switch (error) {
    case NodeError::AccessDenied:
        return QStringLiteral("Access denied");
    case NodeError::FindFirstFailed:
        return QStringLiteral("FindFirstFile failed (error %1)").arg(lastError);
    case NodeError::EnumFailed:
        return QStringLiteral("Enumeration failed (error %1)").arg(lastError);
    case NodeError::None:
    default:
        return {};
    }
}

inline void FileNode::sortBySizeDesc()
{
    // Iterative traversal: explicit stack avoids O(depth) C++ stack frames,
    // which previously overflowed on deeply nested directory trees.
    std::stack<FileNode*> pending;
    pending.push(this);
    while (!pending.empty()) {
        FileNode* n = pending.top();
        pending.pop();
        std::sort(n->children.begin(), n->children.end(),
                  [](const std::shared_ptr<FileNode>& a,
                     const std::shared_ptr<FileNode>& b) {
                      if (a->size != b->size)
                          return a->size > b->size;  // larger first
                      return a->name.toLower() < b->name.toLower();
                  });
        for (auto& c : n->children) {
            if (c && c->isDir())
                pending.push(c.get());
        }
    }
}
