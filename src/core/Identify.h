#pragma once

#include <QString>
#include <memory>
#include <functional>

#include "FileNode.h"

// Heuristic identification of what a file or folder is "for".
//
// Given a FileNode, returns a short human label (e.g. "Likely: Adobe
// Photoshop", "Node.js dependencies (node_modules)", "Git repository").
// The detection is purely path/name/marker based and intentionally cheap -
// it never reads file contents beyond a couple of existence checks.
//
// ``describe`` runs the ordered rule chain
// (system path -> project markers -> software name), then translates the
// resulting i18n key through the supplied ``tr`` callback and interpolates
// any {placeholder} parameters produced by the rule.
namespace Identify {

// Return the translated description string for ``node``, or an empty string
// if nothing was recognized. ``tr`` maps an i18n key to its translated
// template (language fallback handled by the caller, typically I18n::tr).
QString describe(const std::shared_ptr<FileNode>& node,
                 const std::function<QString(const QString&)>& tr);

}  // namespace Identify
