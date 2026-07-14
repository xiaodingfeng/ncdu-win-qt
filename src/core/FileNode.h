#pragma once

#include <algorithm>
#include <memory>
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
//   error        -> error
//   skipped      -> skipped
class FileNode : public std::enable_shared_from_this<FileNode> {
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
    QString error;
    bool skipped = false;

    bool isDir() const
    {
        return nodeType == NodeType::Directory || nodeType == NodeType::Drive;
    }

    int totalItems() const { return fileCount + dirCount; }

    // Sort children largest-first (descending size, ascending lowercase name),
    // then recurse into subdirectories. Sorts children by size descending.
    void sortBySizeDesc();
};

inline void FileNode::sortBySizeDesc()
{
    std::sort(children.begin(), children.end(),
              [](const std::shared_ptr<FileNode>& a,
                 const std::shared_ptr<FileNode>& b) {
                  if (a->size != b->size)
                      return a->size > b->size;  // larger first
                  return a->name.toLower() < b->name.toLower();
              });
    for (auto& c : children) {
        if (c && c->isDir())
            c->sortBySizeDesc();
    }
}
