// Recursively moves the contents of *src* into *dst* and removes every source
// file the moment its copy is confirmed on the new drive — a move in Explorer
// semantics, safe to interrupt: whatever has not been moved yet stays exactly
// where it was. Reparse points are left alone (they are links, not data).
// Returns the number of bytes it relocated.
qint64 moveContentsRecursive(const QString& src, const QString& dst,
                             std::atomic<qint64>* copied, std::atomic<bool>* cancelled)
{
    const QFileInfo info(src);
    if (WinApi::isReparsePointAt(src))
        return 0;

    // Shortcuts (and true symlinks that carry no reparse attribute: .lnk
    // files) must bypass every Qt existence test below: Qt resolves .lnk to
    // its TARGET, so a dangling shortcut reports exists()==false and a valid
    // one reports the target's size and content — either way the shortcut
    // file itself would be skipped or corrupted. Copy it byte for byte with
    // the native API instead, exactly like Explorer's move does.
    const bool isShortcut = info.isSymLink()
        || info.fileName().endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
    if (isShortcut) {
        if (cancelled->load())
            return 0;
        const QString nativeSrc = QDir::toNativeSeparators(src);
        const QString nativeDst = QDir::toNativeSeparators(dst);
#ifdef _WIN32
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;
        const qint64 sz = (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        FindClose(hFind);
        if (!CopyFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()),
                       reinterpret_cast<const wchar_t*>(nativeDst.utf16()), FALSE))
            return 0;   // in use / access denied: the shortcut stays put
        DeleteFileW(reinterpret_cast<const wchar_t*>(nativeSrc.utf16()));
#else
        const qint64 sz = info.size();
        if (!QFile::copy(src, dst))
            return 0;
        QFile::remove(src);
#endif
        copied->fetch_add(sz);
        return sz;
    }

    if (!info.exists())
        return 0;

    if (info.isFile()) {
        if (cancelled->load())
            return 0;
        // dst is this entry's FULL target path (the caller already appended
        // this file's name); appending it again here produced
        // "newDir\name\name" — a path inside a folder that does not exist —
        // so every plain file silently failed to move while the folder tree
        // around it was rebuilt fine.
        const QString target = QDir::toNativeSeparators(dst);
        if (QFile::exists(target))
            QFile::remove(target);
        if (QFile::copy(src, target)) {
            QFile::remove(src);          // the file now lives at the target
            copied->fetch_add(info.size());
            return info.size();
        }
        return 0;  // in use / access denied: the file stays where it was
    }

    QDir().mkpath(dst);
    qint64 moved = 0;
    const auto entries = QDir(src).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        if (cancelled->load())
            return moved;
        moved += moveContentsRecursive(e.absoluteFilePath(),
                                       dst + QLatin1Char('/') + e.fileName(),
                                       copied, cancelled);
    }
    // Empty by now, unless some files could not be moved — those keep the
    // folder (and everything above it) alive on purpose.
    QDir().rmdir(src);
    return moved;
}
