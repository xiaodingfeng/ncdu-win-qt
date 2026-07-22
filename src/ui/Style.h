#pragma once

// Theme + style for the disk analyzer.
//
// Color palette, file-type classification, and QSS stylesheet. Colors live in
// a ThemeColors struct so the active palette can be swapped at runtime (light
// / dark / follow system). The ``C::`` namespace exposes the active palette
// via function accessors (e.g. ``C::FG()``); call sites read the current
// theme every time they paint, so a theme switch takes effect on the next
// refresh. The ``Theme::`` namespace persists the user's choice and resolves
// "system" against the OS color scheme.

#include <QString>
#include <QSet>
#include <QList>
#include <QPair>
#include <QSettings>
#include <QGuiApplication>
#include <QStyleHints>

#include "I18n.h"

// --------------------------------------------------------------------------- //
// Color palette
#include <QPalette>
#include <QColor>

// --------------------------------------------------------------------------- //
// Color palette
// --------------------------------------------------------------------------- //

// All colors used by the UI. Two instances are defined (LIGHT_THEME /
// DARK_THEME); g_currentTheme points at the active one.
struct ThemeColors {
    // Surfaces
    const char* BG;
    const char* SURFACE;
    const char* SURFACE_ALT;
    const char* SURFACE_DEEP;
    // Text
    const char* TEXT;
    const char* TEXT_SEC;
    const char* TEXT_MUTED;
    // Brand
    const char* PRIMARY;
    const char* PRIMARY_HOVER;
    const char* PRIMARY_SOFT;
    const char* ACCENT;
    // Status
    const char* DANGER;
    const char* WARNING;
    const char* SUCCESS;
    // Lines
    const char* BORDER;
    const char* BORDER_LIGHT;
    // Scrollbars
    const char* SCROLLBAR_THUMB;
    const char* SCROLLBAR_THUMB_HOVER;
    // File-type colors (for treemap)
    const char* TYPE_DIR;
    const char* TYPE_DOC;
    const char* TYPE_IMAGE;
    const char* TYPE_VIDEO;
    const char* TYPE_AUDIO;
    const char* TYPE_ARCHIVE;
    const char* TYPE_CODE;
    const char* TYPE_EXEC;
    const char* TYPE_DATA;
    const char* TYPE_OTHER;
    const char* TYPE_HIDDEN;
};

inline const ThemeColors LIGHT_THEME = {
    "#F5F8FB",   // BG            app background
    "#FFFFFF",   // SURFACE       cards / panels
    "#EEF3F8",   // SURFACE_ALT   hover / alt rows
    "#E4EBF3",   // SURFACE_DEEP  pressed / dividers
    "#1E2A3A",   // TEXT
    "#5B6B7E",   // TEXT_SEC
    "#94A3B8",   // TEXT_MUTED
    "#14B8A6",   // PRIMARY       mint-teal
    "#0D9488",   // PRIMARY_HOVER
    "#E3F0FF",   // PRIMARY_SOFT  light blue selection
    "#60A5FA",   // ACCENT
    "#EF4444",   // DANGER
    "#F59E0B",   // WARNING
    "#10B981",   // SUCCESS
    "#E2E8F0",   // BORDER
    "#EEF2F6",   // BORDER_LIGHT
    "#CBD5E1",   // SCROLLBAR_THUMB
    "#94A3B8",   // SCROLLBAR_THUMB_HOVER
    "#2DD4BF",   // TYPE_DIR
    "#60A5FA",   // TYPE_DOC
    "#A78BFA",   // TYPE_IMAGE
    "#F472B6",   // TYPE_VIDEO
    "#34D399",   // TYPE_AUDIO
    "#FBBF24",   // TYPE_ARCHIVE
    "#22D3EE",   // TYPE_CODE
    "#FB923C",   // TYPE_EXEC
    "#818CF8",   // TYPE_DATA
    "#94A3B8",   // TYPE_OTHER
    "#CBD5E1",   // TYPE_HIDDEN
};

inline const ThemeColors DARK_THEME = {
    "#1E2330",   // BG
    "#252B3A",   // SURFACE
    "#2D3445",   // SURFACE_ALT
    "#353D50",   // SURFACE_DEEP
    "#F0F4F8",   // TEXT          (high-contrast crisp white-gray text)
    "#A6B4C9",   // TEXT_SEC      (clear secondary text)
    "#8595A8",   // TEXT_MUTED    (clear muted text)
    "#14B8A6",   // PRIMARY       (mint-teal reads well on dark)
    "#0D9488",   // PRIMARY_HOVER
    "#1F3A3A",   // PRIMARY_SOFT  selection bg
    "#60A5FA",   // ACCENT
    "#EF4444",   // DANGER
    "#F59E0B",   // WARNING
    "#10B981",   // SUCCESS
    "#353D50",   // BORDER
    "#2D3445",   // BORDER_LIGHT
    "#4B5563",   // SCROLLBAR_THUMB       (clearly visible slate thumb)
    "#718096",   // SCROLLBAR_THUMB_HOVER
    "#2DD4BF",   // TYPE_DIR      (saturated colors stay vivid on dark)
    "#60A5FA",   // TYPE_DOC
    "#A78BFA",   // TYPE_IMAGE
    "#F472B6",   // TYPE_VIDEO
    "#34D399",   // TYPE_AUDIO
    "#FBBF24",   // TYPE_ARCHIVE
    "#22D3EE",   // TYPE_CODE
    "#FB923C",   // TYPE_EXEC
    "#818CF8",   // TYPE_DATA
    "#94A3B8",   // TYPE_OTHER
    "#4A5366",   // TYPE_HIDDEN   (darkened so "hidden" reads as muted on dark)
};

// The active palette. Switched by Theme::applyEffective() on the main thread.
inline const ThemeColors* g_currentTheme = &LIGHT_THEME;

// Function accessors for the active palette. Reading through these (rather
// than a fixed constant) is what makes runtime theme switching work.
namespace C {
    inline const char* BG()                    { return g_currentTheme->BG; }
    inline const char* SURFACE()               { return g_currentTheme->SURFACE; }
    inline const char* SURFACE_ALT()           { return g_currentTheme->SURFACE_ALT; }
    inline const char* SURFACE_DEEP()          { return g_currentTheme->SURFACE_DEEP; }
    inline const char* FG()                    { return g_currentTheme->TEXT; }
    inline const char* TEXT_SEC()              { return g_currentTheme->TEXT_SEC; }
    inline const char* TEXT_MUTED()            { return g_currentTheme->TEXT_MUTED; }
    inline const char* PRIMARY()               { return g_currentTheme->PRIMARY; }
    inline const char* PRIMARY_HOVER()         { return g_currentTheme->PRIMARY_HOVER; }
    inline const char* PRIMARY_SOFT()          { return g_currentTheme->PRIMARY_SOFT; }
    inline const char* ACCENT()                { return g_currentTheme->ACCENT; }
    inline const char* DANGER()                { return g_currentTheme->DANGER; }
    inline const char* WARNING()               { return g_currentTheme->WARNING; }
    inline const char* SUCCESS()               { return g_currentTheme->SUCCESS; }
    inline const char* BORDER()                { return g_currentTheme->BORDER; }
    inline const char* BORDER_LIGHT()          { return g_currentTheme->BORDER_LIGHT; }
    inline const char* SCROLLBAR_THUMB()       { return g_currentTheme->SCROLLBAR_THUMB; }
    inline const char* SCROLLBAR_THUMB_HOVER() { return g_currentTheme->SCROLLBAR_THUMB_HOVER; }
    inline const char* TYPE_DIR()              { return g_currentTheme->TYPE_DIR; }
    inline const char* TYPE_DOC()              { return g_currentTheme->TYPE_DOC; }
    inline const char* TYPE_IMAGE()            { return g_currentTheme->TYPE_IMAGE; }
    inline const char* TYPE_VIDEO()            { return g_currentTheme->TYPE_VIDEO; }
    inline const char* TYPE_AUDIO()            { return g_currentTheme->TYPE_AUDIO; }
    inline const char* TYPE_ARCHIVE()          { return g_currentTheme->TYPE_ARCHIVE; }
    inline const char* TYPE_CODE()             { return g_currentTheme->TYPE_CODE; }
    inline const char* TYPE_EXEC()             { return g_currentTheme->TYPE_EXEC; }
    inline const char* TYPE_DATA()             { return g_currentTheme->TYPE_DATA; }
    inline const char* TYPE_OTHER()            { return g_currentTheme->TYPE_OTHER; }
    inline const char* TYPE_HIDDEN()           { return g_currentTheme->TYPE_HIDDEN; }
}  // namespace C

inline QPalette makePalette(const ThemeColors& colors) {
    QPalette p;
    QColor bg(colors.BG);
    QColor surface(colors.SURFACE);
    QColor surfaceAlt(colors.SURFACE_ALT);
    QColor text(colors.TEXT);
    QColor textSec(colors.TEXT_SEC);
    QColor textMuted(colors.TEXT_MUTED);
    QColor primary(colors.PRIMARY);
    QColor primarySoft(colors.PRIMARY_SOFT);

    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, surface);
    p.setColor(QPalette::AlternateBase, surfaceAlt);
    p.setColor(QPalette::ToolTipBase, QColor("#1E2A3A"));
    p.setColor(QPalette::ToolTipText, QColor("#FFFFFF"));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, surface);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(colors.DANGER));
    p.setColor(QPalette::Link, primary);
    p.setColor(QPalette::Highlight, primarySoft);
    p.setColor(QPalette::HighlightedText, text);
    p.setColor(QPalette::PlaceholderText, textMuted);

    p.setColor(QPalette::Disabled, QPalette::WindowText, textMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textMuted);

    return p;
}

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
        return QString::fromLatin1(C::TYPE_DIR());
    if (isHidden)
        return QString::fromLatin1(C::TYPE_HIDDEN());
    const QString ext = extOf(name);
    if (docExt().contains(ext))    return QString::fromLatin1(C::TYPE_DOC());
    if (imgExt().contains(ext))   return QString::fromLatin1(C::TYPE_IMAGE());
    if (vidExt().contains(ext))   return QString::fromLatin1(C::TYPE_VIDEO());
    if (audExt().contains(ext))   return QString::fromLatin1(C::TYPE_AUDIO());
    if (arcExt().contains(ext))   return QString::fromLatin1(C::TYPE_ARCHIVE());
    if (codeExt().contains(ext))  return QString::fromLatin1(C::TYPE_CODE());
    if (execExt().contains(ext))  return QString::fromLatin1(C::TYPE_EXEC());
    if (dataExt().contains(ext))  return QString::fromLatin1(C::TYPE_DATA());
    return QString::fromLatin1(C::TYPE_OTHER());
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
        { QStringLiteral("type.folder"),     QString::fromLatin1(C::TYPE_DIR()) },
        { QStringLiteral("type.document"),    QString::fromLatin1(C::TYPE_DOC()) },
        { QStringLiteral("type.image"),       QString::fromLatin1(C::TYPE_IMAGE()) },
        { QStringLiteral("type.video"),       QString::fromLatin1(C::TYPE_VIDEO()) },
        { QStringLiteral("type.audio"),       QString::fromLatin1(C::TYPE_AUDIO()) },
        { QStringLiteral("type.archive"),     QString::fromLatin1(C::TYPE_ARCHIVE()) },
        { QStringLiteral("type.code"),        QString::fromLatin1(C::TYPE_CODE()) },
        { QStringLiteral("type.executable"),  QString::fromLatin1(C::TYPE_EXEC()) },
        { QStringLiteral("type.data"),        QString::fromLatin1(C::TYPE_DATA()) },
        { QStringLiteral("type.other"),       QString::fromLatin1(C::TYPE_OTHER()) },
    };
}

// --------------------------------------------------------------------------- //
// QSS stylesheet
// --------------------------------------------------------------------------- //

// The full application stylesheet. Color values are injected from the active
// palette via @TOKEN placeholders so a theme switch (qApp->setStyleSheet(
// loadQSS())) re-reads the current theme. A few non-palette literals are left
// baked in: the tooltip is a dark pill in both themes (an overlay element),
// and #0B7C72 (primary pressed) reads on both light and dark.
inline QString loadQSS() {
    QString s = QString::fromLatin1(R"(
* {
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 13px;
    color: @TEXT;
    outline: none;
}

QWidget#root {
    background-color: @BG;
}

QDialog, QMessageBox, QProgressDialog, QFileDialog, QInputDialog {
    background-color: @BG;
    color: @TEXT;
}

QDialog QLabel, QMessageBox QLabel, QProgressDialog QLabel {
    color: @TEXT;
}

QMenuBar {
    background-color: @SURFACE;
    border-bottom: 1px solid @BORDER;
    padding: 2px 4px;
    spacing: 2px;
}
QMenuBar::item {
    background: transparent;
    padding: 6px 10px;
    border-radius: 6px;
    color: @TEXT;
}
QMenuBar::item:selected {
    background-color: @PRIMARY_SOFT;
    color: @TEXT;
}
QMenuBar::item:pressed {
    background-color: @SURFACE_DEEP;
}

/* ---- top bar ---- */
QFrame#topbar {
    background-color: @SURFACE;
    border-bottom: 1px solid @BORDER;
}

QLabel#title {
    font-size: 16px;
    font-weight: 600;
    color: @TEXT;
}

QLabel#subtitle {
    font-size: 11px;
    color: @TEXT_MUTED;
}

/* ---- skip checkbox (button-like) ---- */
QCheckBox {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 8px;
    padding: 7px 14px;
    color: @TEXT;
    font-weight: 500;
    spacing: 6px;
}
QCheckBox:hover {
    background-color: @SURFACE_ALT;
    border-color: @PRIMARY;
}
QCheckBox:checked {
    background-color: @PRIMARY_SOFT;
    border-color: @ACCENT;
}

/* ---- buttons ---- */
QPushButton {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 8px;
    padding: 7px 14px;
    color: @TEXT;
    font-weight: 500;
}
QPushButton:hover {
    background-color: @SURFACE_ALT;
    border-color: @PRIMARY;
}
QPushButton:pressed {
    background-color: @SURFACE_DEEP;
}
QPushButton:disabled {
    color: @TEXT_MUTED;
    background-color: @SURFACE_ALT;
}

QPushButton#primary {
    background-color: @PRIMARY;
    border: none;
    color: white;
    font-weight: 600;
}
QPushButton#primary:hover {
    background-color: @PRIMARY_HOVER;
}
QPushButton#primary:pressed {
    background-color: #0B7C72;
}
QPushButton#primary:disabled {
    background-color: @SURFACE_DEEP;
    color: @TEXT_MUTED;
}

QPushButton#danger {
    background-color: transparent;
    border: 1px solid @DANGER;
    color: @DANGER;
    font-weight: 500;
}
QPushButton#danger:hover {
    background-color: @DANGER;
    color: white;
}

/* ghost / icon-only button */
QPushButton#ghost {
    background-color: transparent;
    border: none;
    padding: 6px;
    color: @TEXT_SEC;
    font-weight: 500;
}
QPushButton#ghost:hover {
    background-color: @SURFACE_ALT;
    color: @PRIMARY;
    border-radius: 6px;
}

/* ---- combo ---- */
QComboBox {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 8px;
    padding: 6px 12px;
    min-height: 20px;
    color: @TEXT;
}
QComboBox:hover {
    border-color: @PRIMARY;
}
QComboBox::drop-down {
    border: none;
    width: 28px;
    background: @SURFACE_ALT;
    border-left: 1px solid @BORDER;
    border-radius: 0px 8px 8px 0px;
}
QComboBox::down-arrow {
    image: none;
    width: 0;
    height: 0;
    border-left: 6px solid transparent;
    border-right: 6px solid transparent;
    border-top: 7px solid @TEXT;
    margin-right: 8px;
}
QComboBox QAbstractItemView {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 6px;
    selection-background-color: @PRIMARY_SOFT;
    selection-color: @TEXT;
    padding: 4px;
    outline: none;
}

/* ---- line edit / search ---- */
QLineEdit {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 8px;
    padding: 7px 12px 7px 34px;
    color: @TEXT;
    selection-background-color: @PRIMARY_SOFT;
    selection-color: @TEXT;
}
QLineEdit:focus {
    border-color: @PRIMARY;
    background-color: @SURFACE;
}
QLineEdit::placeholder {
    color: @TEXT_MUTED;
}

/* ---- breadcrumb ---- */
QFrame#breadcrumb {
    background-color: transparent;
}
QPushButton#crumb {
    background-color: transparent;
    border: none;
    color: @TEXT_SEC;
    padding: 4px 8px;
    font-weight: 500;
}
QPushButton#crumb:hover {
    color: @PRIMARY;
    text-decoration: underline;
}
QLabel#crumb-sep {
    color: @TEXT_MUTED;
}

/* ---- list (file list) ---- */
QTreeWidget {
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 10px;
    padding: 4px;
    outline: none;
}
QTreeWidget::item {
    padding: 6px 4px;
    border-bottom: 1px solid @BORDER_LIGHT;
    min-height: 22px;
    color: @TEXT;
}
QTreeWidget::item:selected {
    background-color: @PRIMARY_SOFT;
    color: @TEXT;
}
QTreeWidget::item:selected:!active {
    background-color: @PRIMARY_SOFT;
    color: @TEXT;
}
QTreeWidget::item:hover {
    background-color: @SURFACE_ALT;
}
QHeaderView::section {
    background-color: @SURFACE_ALT;
    color: @TEXT_SEC;
    padding: 8px 10px;
    border: none;
    border-right: 1px solid @BORDER_LIGHT;
    border-bottom: 1px solid @BORDER;
    font-weight: 600;
    font-size: 12px;
}
QHeaderView::section:hover {
    color: @PRIMARY;
    background-color: @SURFACE_DEEP;
}
QHeaderView::section:first {
    border-top-left-radius: 10px;
}
QHeaderView::section:last {
    border-top-right-radius: 10px;
    border-right: none;
}

/* ---- tabs ---- */
QTabWidget::pane {
    border: 1px solid @BORDER;
    background-color: @SURFACE;
    border-radius: 8px;
    top: -1px;
}
QTabBar::tab {
    background-color: @SURFACE_ALT;
    color: @TEXT_SEC;
    border: 1px solid @BORDER;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 7px 16px;
    margin-right: 2px;
    font-weight: 500;
}
QTabBar::tab:hover {
    background-color: @SURFACE_DEEP;
    color: @PRIMARY;
}
QTabBar::tab:selected {
    background-color: @SURFACE;
    color: @PRIMARY;
    border-color: @BORDER;
    border-bottom: 2px solid @PRIMARY;
    font-weight: 600;
}

/* ---- scroll bars ---- */
QScrollBar:vertical {
    background: @SURFACE_ALT;
    width: 10px;
    margin: 2px;
    border-radius: 5px;
}
QScrollBar::handle:vertical {
    background: @SCROLLBAR_THUMB;
    border-radius: 4px;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: @SCROLLBAR_THUMB_HOVER;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: none;
}

QScrollBar:horizontal {
    background: @SURFACE_ALT;
    height: 10px;
    margin: 2px;
    border-radius: 5px;
}
QScrollBar::handle:horizontal {
    background: @SCROLLBAR_THUMB;
    border-radius: 4px;
    min-width: 24px;
}
QScrollBar::handle:horizontal:hover {
    background: @SCROLLBAR_THUMB_HOVER;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0px;
}
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: none;
}

/* ---- status bar ---- */
QFrame#statusbar {
    background-color: @SURFACE;
    border-top: 1px solid @BORDER;
}
QLabel#status {
    color: @TEXT_SEC;
    font-size: 12px;
}

/* ---- progress ---- */
QProgressBar {
    background-color: @SURFACE_ALT;
    border: none;
    border-radius: 4px;
    height: 6px;
    text-align: center;
    color: transparent;
}
QProgressBar::chunk {
    background-color: @PRIMARY;
    border-radius: 4px;
}

/* ---- tooltip (dark pill in both themes — an overlay element) ---- */
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
    background-color: @SURFACE;
    border: 1px solid @BORDER;
    border-radius: 8px;
    padding: 6px;
}
QMenu::item {
    padding: 6px 24px 6px 14px;
    border-radius: 6px;
    color: @TEXT;
    background-color: transparent;
}
QMenu::item:selected {
    background-color: @PRIMARY_SOFT;
    color: @TEXT;
}
QMenu::separator {
    height: 1px;
    background-color: @BORDER_LIGHT;
    margin: 4px 8px;
}

/* ---- about dialog text ---- */
QLabel#about-body {
    font-size: 13px;
    color: @TEXT_SEC;
}
QLabel#about-title {
    font-size: 20px;
    font-weight: 700;
    color: @TEXT;
}
)");

    // Inject the active palette. Each token maps to one palette entry.
    // IMPORTANT: Longer tokens MUST be replaced before shorter ones that are
    // their prefixes (e.g. @SURFACE_ALT before @SURFACE), otherwise the
    // shorter replace corrupts the longer token (e.g. @SURFACE inside
    // @SURFACE_ALT becomes #RRGGBB_ALT, producing invalid color names).
    s.replace(QStringLiteral("@SURFACE_ALT"),           QString::fromLatin1(C::SURFACE_ALT()));
    s.replace(QStringLiteral("@SURFACE_DEEP"),          QString::fromLatin1(C::SURFACE_DEEP()));
    s.replace(QStringLiteral("@SURFACE"),               QString::fromLatin1(C::SURFACE()));
    s.replace(QStringLiteral("@TEXT_SEC"),              QString::fromLatin1(C::TEXT_SEC()));
    s.replace(QStringLiteral("@TEXT_MUTED"),            QString::fromLatin1(C::TEXT_MUTED()));
    s.replace(QStringLiteral("@TEXT"),                  QString::fromLatin1(C::FG()));
    s.replace(QStringLiteral("@PRIMARY_HOVER"),         QString::fromLatin1(C::PRIMARY_HOVER()));
    s.replace(QStringLiteral("@PRIMARY_SOFT"),          QString::fromLatin1(C::PRIMARY_SOFT()));
    s.replace(QStringLiteral("@PRIMARY"),               QString::fromLatin1(C::PRIMARY()));
    s.replace(QStringLiteral("@BORDER_LIGHT"),          QString::fromLatin1(C::BORDER_LIGHT()));
    s.replace(QStringLiteral("@BORDER"),                QString::fromLatin1(C::BORDER()));
    s.replace(QStringLiteral("@SCROLLBAR_THUMB_HOVER"), QString::fromLatin1(C::SCROLLBAR_THUMB_HOVER()));
    s.replace(QStringLiteral("@SCROLLBAR_THUMB"),       QString::fromLatin1(C::SCROLLBAR_THUMB()));
    s.replace(QStringLiteral("@BG"),                    QString::fromLatin1(C::BG()));
    s.replace(QStringLiteral("@ACCENT"),                QString::fromLatin1(C::ACCENT()));
    s.replace(QStringLiteral("@DANGER"),                QString::fromLatin1(C::DANGER()));
    return s;
}

// --------------------------------------------------------------------------- //
// Theme management
// --------------------------------------------------------------------------- //
//
// Persists the user's choice ("light"/"dark"/"system") to the same registry
// key group as I18n (HKCU\Software\NcduWin). "system" resolves against
// QStyleHints::colorScheme() so the app follows the Windows light/dark
// setting automatically.

namespace Theme {

inline QSettings settings() {
    return QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\NcduWin"),
                    QSettings::NativeFormat);
}

// The persisted choice. Defaults to "system".
inline QString g_code = QStringLiteral("system");

inline QString load() {
    const QString stored = settings().value(QStringLiteral("theme")).toString();
    if (stored == QStringLiteral("light") ||
        stored == QStringLiteral("dark") ||
        stored == QStringLiteral("system")) {
        g_code = stored;
    } else {
        g_code = QStringLiteral("system");
    }
    return g_code;
}

inline QString current() { return g_code; }

// Resolves "system" to "light"/"dark" using the OS color scheme.
inline QString effective() {
    if (g_code == QStringLiteral("dark"))
        return QStringLiteral("dark");
    if (g_code == QStringLiteral("light"))
        return QStringLiteral("light");
    // system
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return (scheme == Qt::ColorScheme::Dark) ? QStringLiteral("dark")
                                             : QStringLiteral("light");
}

// Points g_currentTheme at the palette for the effective theme. Call on the
// main thread (reads QGuiApplication).
inline void applyEffective() {
    g_currentTheme = (effective() == QStringLiteral("dark")) ? &DARK_THEME
                                                             : &LIGHT_THEME;
    QGuiApplication::setPalette(makePalette(*g_currentTheme));
}

// Persist the choice and apply it. Does NOT refresh widgets — callers do that
// (MainWindow::refreshTheme) so repainting happens once, on the main thread.
inline void set(const QString& code) {
    if (code != QStringLiteral("light") &&
        code != QStringLiteral("dark") &&
        code != QStringLiteral("system"))
        return;
    g_code = code;
    settings().setValue(QStringLiteral("theme"), code);
    applyEffective();
}

inline QString displayName(const QString& code) {
    return I18n::tr(QStringLiteral("theme.") + code);
}

}  // namespace Theme
