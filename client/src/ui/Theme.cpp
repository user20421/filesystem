#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

void Theme::apply(QApplication &application)
{
    application.setStyle(QStringLiteral("Fusion"));
    QFont font(QStringLiteral("Noto Sans SC"));
    font.setPointSize(10);
    application.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#f4f7fb")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#24324a")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#24324a")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#24324a")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#5577ee")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    application.setPalette(palette);
    application.setStyleSheet(QStringLiteral(R"QSS(
* { font-family: "Noto Sans SC", "Microsoft YaHei UI", "Segoe UI", sans-serif; font-size: 13px; }
QMainWindow, QWidget#sessionPage, QWidget#loginPage { background: #f4f7fb; color: #24324a; }
QFrame[card="true"] { background: #ffffff; border: 1px solid #e4eaf3; border-radius: 14px; }
QLabel { background: transparent; border: none; }
QLabel#brandTitle { color: #4666dd; font-size: 32px; font-weight: 700; }
QLabel#sessionTitle { color: #1e2c45; font-size: 17px; font-weight: 650; }
QLabel#sectionTitle { color: #25324b; font-size: 16px; font-weight: 650; }
QLabel#mutedLabel, QLabel#serverLabel, QLabel#contactPresenceLabel { color: #7a89a3; font-size: 12px; }
QLabel#connectionBadge { color: #328064; background: #eaf8f1; border: 1px solid #d3f0e2; border-radius: 11px; padding: 5px 10px; font-size: 12px; }
QLabel#noticeLabel { color: #9a5a18; background: #fff7e8; border: 1px solid #fde7bd; border-radius: 8px; padding: 7px; }
QLabel#loginStatus { color: #a14949; }
QLineEdit, QSpinBox { background: #f9fbfe; border: 1px solid #dbe3ef; border-radius: 8px; padding: 8px 10px; selection-background-color: #6685ed; }
QLineEdit:focus, QSpinBox:focus { border: 1px solid #6685ed; background: #ffffff; }
QLineEdit:disabled, QSpinBox:disabled { color: #a6b0c0; background: #f0f2f6; }
QPushButton, QToolButton { background: #ffffff; color: #40506a; border: 1px solid #dbe3ef; border-radius: 8px; padding: 8px 13px; }
QPushButton:hover, QToolButton:hover { background: #f0f4ff; border-color: #b9c8f3; color: #4666dd; }
QPushButton:pressed, QToolButton:pressed { background: #e2e9fb; padding-top: 9px; padding-bottom: 7px; }
QPushButton:disabled, QToolButton:disabled { color: #a6b0c0; background: #f0f2f6; border-color: #e5e8ee; }
QPushButton[primary="true"] { background: #5577ee; color: white; border: none; font-weight: 600; }
QPushButton[primary="true"]:hover { background: #4669e3; }
QPushButton[primary="true"]:pressed { background: #3b5ccd; }
QPushButton[navigation="true"] { border: none; background: transparent; padding: 7px 14px; color: #7a89a3; }
QPushButton[navigation="true"]:checked { color: #4666dd; background: #edf2ff; font-weight: 600; }
QListWidget#contactList { background: transparent; border: none; outline: none; }
QListWidget#contactList::item { background: #f7f9fc; border: 1px solid transparent; border-radius: 9px; padding: 0px; margin: 2px 0; color: #35445e; }
QListWidget#contactList::item:hover { background: #edf2ff; border-color: #dbe4fc; }
QListWidget#contactList::item:selected { background: #e6edff; border-color: #cbd8fb; color: #3654bd; }
QTextBrowser#chatTranscript { background: #fbfcfe; border: 1px solid #edf0f5; border-radius: 10px; padding: 9px; }
QFrame#composer { background: #f7f9fd; border: 1px solid #e2e8f2; border-radius: 11px; }
QFrame#composer QLineEdit { border: none; background: transparent; }
QTreeWidget#transferTable { background: #fbfcfe; alternate-background-color: #f5f8fc; border: 1px solid #e7ecf3; border-radius: 9px; outline: none; }
QHeaderView::section { background: #f2f5fa; color: #708098; border: none; border-bottom: 1px solid #e2e8f1; padding: 8px; font-size: 12px; }
QTreeWidget::item { min-height: 34px; }
QTreeWidget::item:selected { background: #e9efff; color: #314fba; }
QProgressBar { background: #e9edf5; border: none; border-radius: 6px; min-height: 12px; max-height: 18px; text-align: center; color: #46556e; font-size: 10px; }
QProgressBar::chunk { background: #6685ed; border-radius: 6px; }
QTabWidget::pane { border: none; background: #f4f7fb; }
QTabBar { background: #f4f7fb; }
QTabBar::tab { background: #e9eef6; color: #65758f; border: 1px solid #e0e6f0; border-bottom: none; border-top-left-radius: 9px; border-top-right-radius: 9px; padding: 10px 18px; margin-right: 4px; min-width: 105px; }
QTabBar::tab:hover { background: #f5f7ff; color: #4666dd; }
QTabBar::tab:selected { background: #ffffff; color: #3658d2; font-weight: 600; }
QToolButton#addTabButton { font-size: 20px; font-weight: 400; padding: 3px 11px; margin: 3px; }
QSplitter::handle { background: transparent; width: 8px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #cbd4e2; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #aebbd0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QToolTip { color: #ffffff; background: #334155; border: none; padding: 5px; }
)QSS"));
}
