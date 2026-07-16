#pragma once

// Light, fresh theme for the disk analyzer.
//
// Aesthetic direction: "fresh morning" - soft blue-white backgrounds, a
// mint-teal primary, pastel file-type colors for the treemap. Mirrors
// Color palette, file-type classification, and QSS stylesheet.

#include <QString>
#include <QSet>
#include <QList>
#include <QPair>

// --------------------------------------------------------------------------- //
// Color palette
// --------------------------------------------------------------------------- //
namespace C {
    // Surfaces
    inline const char* const BG            = "#F5F8FB";   // app background
    inline const char* const SURFACE       = "#FFFFFF";   // cards / panels
    inline const char* const SURFACE_ALT   = "#EEF3F8";   // hover / alt rows
    inline const char* const SURFACE_DEEP  = "#E4EBF3";   // pressed / dividers

    // Text
    inline const char* const TEXT          = "#1E2A3A";
    inline const char* const TEXT_SEC      = "#5B6B7E";
    inline const char* const TEXT_MUTED    = "#94A3B8";

    // Brand
    inline const char* const PRIMARY       = "#14B8A6";   // mint-teal
    inline const char* const PRIMARY_HOVER = "#0D9488";
    inline const char* const PRIMARY_SOFT  = "#E3F0FF";   // light blue selection
    inline const char* const ACCENT        = "#60A5FA";

    // Status
    inline const char* const DANGER        = "#EF4444";
    inline const char* const WARNING       = "#F59E0B";
    inline const char* const SUCCESS       = "#10B981";

    // Lines
    inline const char* const BORDER        = "#E2E8F0";
    inline const char* const BORDER_LIGHT  = "#EEF2F6";

    // File-type colors (pastel set for treemap)
    inline const char* const TYPE_DIR      = "#2DD4BF";
    inline const char* const TYPE_DOC      = "#60A5FA";
    inline const char* const TYPE_IMAGE    = "#A78BFA";
    inline const char* const TYPE_VIDEO    = "#F472B6";
    inline const char* const TYPE_AUDIO    = "#34D399";
    inline const char* const TYPE_ARCHIVE  = "#FBBF24";
    inline const char* const TYPE_CODE     = "#22D3EE";
    inline const char* const TYPE_EXEC     = "#FB923C";
    inline const char* const TYPE_DATA     = "#818CF8";
    inline const char* const TYPE_OTHER    = "#94A3B8";
    inline const char* const TYPE_HIDDEN   = "#CBD5E1";
}  // namespace C

// Default UI font family.
inline const char* const DEFAULT_FAMILY = "Segoe UI";

// --------------------------------------------------------------------------- //
// File-type classification helpers
// --------------------------------------------------------------------------- //

// Extension sets (built once, thread-safe via magic statics).
inline const QSet<QString>& docExt() {
    static const QSet<QString> s = {
        QStringLiteral(".txt"), QStringLiteral(".pdf"), QStringLiteral(".doc"),
        QStringLiteral(".docx"), QStringLiteral(".rtf"), QStringLiteral(".odt"),
        QStringLiteral(".pages"), QStringLiteral(".md"), QStringLiteral(".tex"),
        QStringLiteral(".epub"), QStringLiteral(".mobi")
    };
    return s;
}
inline const QSet<QString>& imgExt() {
    static const QSet<QString> s = {
        QStringLiteral(".jpg"), QStringLiteral(".jpeg"), QStringLiteral(".png"),
        QStringLiteral(".gif"), QStringLiteral(".bmp"), QStringLiteral(".tiff"),
        QStringLiteral(".tif"), QStringLiteral(".webp"), QStringLiteral(".svg"),
        QStringLiteral(".heic"), QStringLiteral(".raw"), QStringLiteral(".psd"),
        QStringLiteral(".ai"), QStringLiteral(".ico")
    };
    return s;
}
inline const QSet<QString>& vidExt() {
    static const QSet<QString> s = {
        QStringLiteral(".mp4"), QStringLiteral(".mkv"), QStringLiteral(".avi"),
        QStringLiteral(".mov"), QStringLiteral(".wmv"), QStringLiteral(".flv"),
        QStringLiteral(".webm"), QStringLiteral(".m4v"), QStringLiteral(".mpg"),
        QStringLiteral(".mpeg"), QStringLiteral(".3gp")
    };
    return s;
}
inline const QSet<QString>& audExt() {
    static const QSet<QString> s = {
        QStringLiteral(".mp3"), QStringLiteral(".wav"), QStringLiteral(".flac"),
        QStringLiteral(".aac"), QStringLiteral(".ogg"), QStringLiteral(".wma"),
        QStringLiteral(".m4a"), QStringLiteral(".alac"), QStringLiteral(".aiff")
    };
    return s;
}
inline const QSet<QString>& arcExt() {
    static const QSet<QString> s = {
        QStringLiteral(".zip"), QStringLiteral(".rar"), QStringLiteral(".7z"),
        QStringLiteral(".tar"), QStringLiteral(".gz"), QStringLiteral(".bz2"),
        QStringLiteral(".xz"), QStringLiteral(".iso"), QStringLiteral(".dmg"),
        QStringLiteral(".tgz"), QStringLiteral(".lz")
    };
    return s;
}
inline const QSet<QString>& codeExt() {
    static const QSet<QString> s = {
        QStringLiteral(".py"), QStringLiteral(".js"), QStringLiteral(".ts"),
        QStringLiteral(".jsx"), QStringLiteral(".tsx"), QStringLiteral(".java"),
        QStringLiteral(".c"), QStringLiteral(".cpp"), QStringLiteral(".h"),
        QStringLiteral(".hpp"), QStringLiteral(".cs"), QStringLiteral(".go"),
        QStringLiteral(".rs"), QStringLiteral(".rb"), QStringLiteral(".php"),
        QStringLiteral(".swift"), QStringLiteral(".kt"), QStringLiteral(".scala"),
        QStringLiteral(".sh"), QStringLiteral(".bash"), QStringLiteral(".ps1"),
        QStringLiteral(".bat"), QStringLiteral(".cmd"), QStringLiteral(".html"),
        QStringLiteral(".css"), QStringLiteral(".scss"), QStringLiteral(".less"),
        QStringLiteral(".vue"), QStringLiteral(".svelte"), QStringLiteral(".json"),
        QStringLiteral(".xml"), QStringLiteral(".yaml"), QStringLiteral(".yml"),
        QStringLiteral(".toml"), QStringLiteral(".ini"), QStringLiteral(".cfg")
    };
    return s;
}
inline const QSet<QString>& execExt() {
    static const QSet<QString> s = {
        QStringLiteral(".exe"), QStringLiteral(".msi"), QStringLiteral(".app"),
        QStringLiteral(".appimage"), QStringLiteral(".deb"), QStringLiteral(".rpm"),
        QStringLiteral(".apk"), QStringLiteral(".dll"), QStringLiteral(".so"),
        QStringLiteral(".dylib")
    };
    return s;
}
inline const QSet<QString>& dataExt() {
    static const QSet<QString> s = {
        QStringLiteral(".csv"), QStringLiteral(".tsv"), QStringLiteral(".db"),
        QStringLiteral(".sqlite"), QStringLiteral(".sqlite3"), QStringLiteral(".mdb"),
        QStringLiteral(".accdb"), QStringLiteral(".parquet"), QStringLiteral(".feather"),
        QStringLiteral(".h5"), QStringLiteral(".hdf5")
    };
    return s;
}

// Extract the lowercased extension (including the dot) from a file name.
// Returns "" when the name has no extension.
//   "." + name.rsplit(".", 1)[-1].lower() if "." in name else ""
inline QString extOf(const QString& name) {
    const int idx = name.lastIndexOf('.');
    if (idx < 0)
        return {};
    return name.mid(idx).toLower();
}

// Return the hex color for a file based on its extension.
inline QString typeColor(const QString& name, bool isDir = false, bool isHidden = false) {
    if (isDir)
        return QString::fromLatin1(C::TYPE_DIR);
    if (isHidden)
        return QString::fromLatin1(C::TYPE_HIDDEN);
    const QString ext = extOf(name);
    if (docExt().contains(ext))    return QString::fromLatin1(C::TYPE_DOC);
    if (imgExt().contains(ext))   return QString::fromLatin1(C::TYPE_IMAGE);
    if (vidExt().contains(ext))   return QString::fromLatin1(C::TYPE_VIDEO);
    if (audExt().contains(ext))   return QString::fromLatin1(C::TYPE_AUDIO);
    if (arcExt().contains(ext))   return QString::fromLatin1(C::TYPE_ARCHIVE);
    if (codeExt().contains(ext))  return QString::fromLatin1(C::TYPE_CODE);
    if (execExt().contains(ext))  return QString::fromLatin1(C::TYPE_EXEC);
    if (dataExt().contains(ext))  return QString::fromLatin1(C::TYPE_DATA);
    return QString::fromLatin1(C::TYPE_OTHER);
}

// Return the i18n key for a file's type label.
inline QString typeKey(const QString& name, bool isDir = false) {
    if (isDir)
        return QStringLiteral("type.folder");
    const QString ext = extOf(name);
    if (docExt().contains(ext))    return QStringLiteral("type.document");
    if (imgExt().contains(ext))   return QStringLiteral("type.image");
    if (vidExt().contains(ext))   return QStringLiteral("type.video");
    if (audExt().contains(ext))   return QStringLiteral("type.audio");
    if (arcExt().contains(ext))   return QStringLiteral("type.archive");
    if (codeExt().contains(ext))  return QStringLiteral("type.code");
    if (execExt().contains(ext))  return QStringLiteral("type.executable");
    if (dataExt().contains(ext))  return QStringLiteral("type.data");
    return QStringLiteral("type.other");
}

// (i18n key, color) pairs for the legend.
inline QList<QPair<QString, QString>> legendItems() {
    return {
        { QStringLiteral("type.folder"),     QString::fromLatin1(C::TYPE_DIR) },
        { QStringLiteral("type.document"),    QString::fromLatin1(C::TYPE_DOC) },
        { QStringLiteral("type.image"),       QString::fromLatin1(C::TYPE_IMAGE) },
        { QStringLiteral("type.video"),       QString::fromLatin1(C::TYPE_VIDEO) },
        { QStringLiteral("type.audio"),       QString::fromLatin1(C::TYPE_AUDIO) },
        { QStringLiteral("type.archive"),     QString::fromLatin1(C::TYPE_ARCHIVE) },
        { QStringLiteral("type.code"),        QString::fromLatin1(C::TYPE_CODE) },
        { QStringLiteral("type.executable"),  QString::fromLatin1(C::TYPE_EXEC) },
        { QStringLiteral("type.data"),        QString::fromLatin1(C::TYPE_DATA) },
        { QStringLiteral("type.other"),       QString::fromLatin1(C::TYPE_OTHER) },
    };
}

// --------------------------------------------------------------------------- //
// QSS stylesheet
// --------------------------------------------------------------------------- //

// The full application stylesheet. Color values are baked in (matching the
// constants above) so it can be applied directly via qApp->setStyleSheet().
inline QString loadQSS() {
    // The stylesheet is pure ASCII, so fromLatin1 is a safe, portable way to
    // wrap a raw string literal without the u""-concatenation quirks of
    // QStringLiteral on some compilers.
    return QString::fromLatin1(R"(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 13px;
    color: #1E2A3A;
    outline: none;
}

QWidget#root {
    background-color: #F5F8FB;
}

QMenuBar {
    background-color: #FFFFFF;
    border-bottom: 1px solid #E2E8F0;
    padding: 2px 4px;
    spacing: 2px;
}
QMenuBar::item {
    background: transparent;
    padding: 6px 10px;
    border-radius: 6px;
    color: #1E2A3A;
}
QMenuBar::item:selected {
    background-color: #EEF3F8;
}
QMenuBar::item:pressed {
    background-color: #E3F0FF;
}

/* ---- top bar ---- */
QFrame#topbar {
    background-color: #FFFFFF;
    border-bottom: 1px solid #E2E8F0;
}

QLabel#title {
    font-size: 16px;
    font-weight: 600;
    color: #1E2A3A;
}

QLabel#subtitle {
    font-size: 11px;
    color: #94A3B8;
}

/* ---- skip checkbox (button-like) ---- */
QCheckBox {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 7px 14px;
    color: #1E2A3A;
    font-weight: 500;
    spacing: 6px;
}
QCheckBox:hover {
    background-color: #EEF3F8;
    border-color: #14B8A6;
}
QCheckBox:checked {
    background-color: #E3F0FF;
    border-color: #60A5FA;
}

/* ---- buttons ---- */
QPushButton {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 7px 14px;
    color: #1E2A3A;
    font-weight: 500;
}
QPushButton:hover {
    background-color: #EEF3F8;
    border-color: #14B8A6;
}
QPushButton:pressed {
    background-color: #E4EBF3;
}
QPushButton:disabled {
    color: #94A3B8;
    background-color: #EEF3F8;
}

QPushButton#primary {
    background-color: #14B8A6;
    border: none;
    color: white;
    font-weight: 600;
}
QPushButton#primary:hover {
    background-color: #0D9488;
}
QPushButton#primary:pressed {
    background-color: #0B7C72;
}
QPushButton#primary:disabled {
    background-color: #E4EBF3;
    color: #94A3B8;
}

QPushButton#danger {
    background-color: transparent;
    border: 1px solid #EF4444;
    color: #EF4444;
    font-weight: 500;
}
QPushButton#danger:hover {
    background-color: #EF4444;
    color: white;
}

/* ghost / icon-only button */
QPushButton#ghost {
    background-color: transparent;
    border: none;
    padding: 6px;
    color: #5B6B7E;
    font-weight: 500;
}
QPushButton#ghost:hover {
    background-color: #EEF3F8;
    color: #14B8A6;
    border-radius: 6px;
}

/* ---- combo ---- */
QComboBox {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 6px 12px;
    min-height: 20px;
    color: #1E2A3A;
}
QComboBox:hover {
    border-color: #14B8A6;
}
QComboBox::drop-down {
    border: none;
    width: 28px;
    background: #F1F5F9;
    border-left: 1px solid #E2E8F0;
    border-radius: 0px 8px 8px 0px;
}
QComboBox::down-arrow {
    image: none;
    width: 0;
    height: 0;
    border-left: 6px solid transparent;
    border-right: 6px solid transparent;
    border-top: 7px solid #1E2A3A;
    margin-right: 8px;
}
QComboBox QAbstractItemView {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 6px;
    selection-background-color: #E3F0FF;
    selection-color: #1E2A3A;
    padding: 4px;
    outline: none;
}

/* ---- line edit / search ---- */
QLineEdit {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 7px 12px 7px 34px;
    color: #1E2A3A;
    selection-background-color: #E3F0FF;
    selection-color: #1E2A3A;
}
QLineEdit:focus {
    border-color: #14B8A6;
    background-color: #FFFFFF;
}
QLineEdit::placeholder {
    color: #94A3B8;
}

/* ---- breadcrumb ---- */
QFrame#breadcrumb {
    background-color: transparent;
}
QPushButton#crumb {
    background-color: transparent;
    border: none;
    color: #5B6B7E;
    padding: 4px 8px;
    font-weight: 500;
}
QPushButton#crumb:hover {
    color: #14B8A6;
    text-decoration: underline;
}
QLabel#crumb-sep {
    color: #94A3B8;
}

/* ---- list (file list) ---- */
QTreeWidget {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 10px;
    padding: 4px;
    outline: none;
}
QTreeWidget::item {
    padding: 6px 4px;
    border-bottom: 1px solid #EEF2F6;
    min-height: 22px;
}
QTreeWidget::item:selected {
    background-color: #E3F0FF;
    color: #1E2A3A;
}
QTreeWidget::item:selected:!active {
    background-color: #E3F0FF;
    color: #1E2A3A;
}
QTreeWidget::item:hover {
    background-color: #EEF3F8;
}
QHeaderView::section {
    background-color: #EEF3F8;
    color: #5B6B7E;
    padding: 8px 10px;
    border: none;
    border-right: 1px solid #EEF2F6;
    border-bottom: 1px solid #E2E8F0;
    font-weight: 600;
    font-size: 12px;
}
QHeaderView::section:hover {
    color: #14B8A6;
    background-color: #E4EBF3;
}
QHeaderView::section:first {
    border-top-left-radius: 10px;
}
QHeaderView::section:last {
    border-top-right-radius: 10px;
    border-right: none;
}

/* ---- scroll bars ---- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px 2px;
}
QScrollBar::handle:vertical {
    background: #E4EBF3;
    border-radius: 4px;
    min-height: 32px;
}
QScrollBar::handle:vertical:hover {
    background: #94A3B8;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px 4px;
}
QScrollBar::handle:horizontal {
    background: #E4EBF3;
    border-radius: 4px;
    min-width: 32px;
}
QScrollBar::handle:horizontal:hover {
    background: #94A3B8;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0;
}

/* ---- status bar ---- */
QFrame#statusbar {
    background-color: #FFFFFF;
    border-top: 1px solid #E2E8F0;
}
QLabel#status {
    color: #5B6B7E;
    font-size: 12px;
}

/* ---- progress ---- */
QProgressBar {
    background-color: #EEF3F8;
    border: none;
    border-radius: 4px;
    height: 6px;
    text-align: center;
    color: transparent;
}
QProgressBar::chunk {
    background-color: #14B8A6;
    border-radius: 4px;
}

/* ---- tooltip ---- */
QToolTip {
    background-color: #1E2A3A;
    color: #FFFFFF;
    border: none;
    border-radius: 6px;
    padding: 6px 10px;
    font-size: 12px;
}

/* ---- menu ---- */
QMenu {
    background-color: #FFFFFF;
    border: 1px solid #E2E8F0;
    border-radius: 8px;
    padding: 6px;
}
QMenu::item {
    padding: 6px 24px 6px 14px;
    border-radius: 6px;
}
QMenu::item:selected {
    background-color: #E3F0FF;
    color: #1E2A3A;
}
QMenu::separator {
    height: 1px;
    background-color: #EEF2F6;
    margin: 4px 8px;
}

/* ---- about dialog text ---- */
QLabel#about-body {
    font-size: 13px;
    color: #5B6B7E;
}
QLabel#about-title {
    font-size: 20px;
    font-weight: 700;
    color: #1E2A3A;
}
)");
}
