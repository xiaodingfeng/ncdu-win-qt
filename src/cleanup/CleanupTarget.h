#pragma once

#include <QString>
#include <memory>

// DangerLevel - safety classification for cleanup targets.
//
// DangerLevel enum for safety classification.
//
// Level | Meaning                          | Default behavior
// ------+----------------------------------+---------------------------
// S     | System-generated temp/cache      | Auto-delete, no prompt
// A     | User cache (regeneratable)       | Default checked, confirm
// B     | Large but potentially useful     | Unchecked, user must opt in
// C     | System state / recovery / update | Shown but disabled
// D     | User data                        | Hidden from cleanup entirely
enum class DangerLevel { S, A, B, C, D };

// CleanupTarget - a single cleanable item (file or directory).
//
// A single cleanup target (file or directory).
struct CleanupTarget {
    QString key;           // i18n key
    QString path;          // filesystem path
    qint64 size = 0;
    int fileCount = 0;
    DangerLevel danger = DangerLevel::A;
    bool isDir = true;
    QString remark;        // extra i18n key for warning/explanation
    bool checked = true;   // default checked state
    bool enabled = true;   // can user toggle? (C level = false)
};

// LargeFile - a single large file found during scanning.
//
// A single large file found during scanning.
struct LargeFile {
    QString path;
    QString name;
    qint64 size = 0;
    DangerLevel danger = DangerLevel::B;  // default: needs user choice
    QString remark;
};
