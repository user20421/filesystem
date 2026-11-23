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
QMainWindow, QWidget#sessionPage, QWidget#loginPage, QWidget#systemTestPage, QScrollArea, QScrollArea > QWidget > QWidget { background: #f4f7fb; color: #24324a; }
QFrame[card="true"] { background: #ffffff; border: 1px solid #e4eaf3; border-radius: 14px; }
QLabel { background: transparent; border: none; }
QLabel#brandTitle { color: #4666dd; font-size: 32px; font-weight: 700; }
QLabel#sessionTitle { color: #1e2c45; font-size: 17px; font-weight: 650; }
QLabel#stressTitle { color: #263c72; font-size: 25px; font-weight: 700; }
QLabel#stressPhaseLabel { color: #4666dd; background: #edf2ff; border: 1px solid #d9e2fb; border-radius: 11px; padding: 6px 12px; font-weight: 600; }
QLabel#stressDistributionLabel { color: #536784; font-size: 12px; }
QLabel#stressResourceLabel { color: #73829a; background: #f7f9fc; border-radius: 7px; padding: 7px 9px; font-size: 12px; }
QLabel[metricValue="true"] { color: #253c67; font-size: 18px; font-weight: 650; }
QLabel#stressConclusionLabel { border-radius: 9px; padding: 7px 13px; font-size: 15px; font-weight: 650; }
QLabel#stressConclusionLabel[state="idle"] { color: #64748b; background: #f0f3f7; }
QLabel#stressConclusionLabel[state="running"] { color: #3564c8; background: #eaf1ff; }
QLabel#stressConclusionLabel[state="success"] { color: #23785b; background: #e8f7ef; }
QLabel#stressConclusionLabel[state="warning"] { color: #946315; background: #fff4dc; }
QLabel#stressConclusionLabel[state="failed"] { color: #b84040; background: #fff0f0; }
QLabel#stressAdviceLabel { color: #62728a; font-size: 12px; }
QTreeWidget#stressHistoryList { background: #fbfcfe; border: 1px solid #e7ecf4; border-radius: 8px; alternate-background-color: #f5f8fc; padding: 3px; }
QTreeWidget#stressHistoryList::item { min-height: 25px; }
QHeaderView::section { background: #f2f5fa; color: #687991; border: none; padding: 6px; font-size: 12px; }
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
QLabel#transferStats { color: #708098; font-size: 12px; }
QLabel#transferEmptyState { color: #8795aa; background: #fafbfe; border: 1px dashed #d9e1ed; border-radius: 12px; font-size: 14px; padding: 30px; }
QTreeWidget#transferTable { background: transparent; border: none; outline: none; padding: 0px; }
QTreeWidget#transferTable::item { background: transparent; border: none; padding: 0px; }
QTreeWidget#transferTable::item:selected { background: transparent; color: #24324a; }
QFrame#transferTaskCard { background: #ffffff; border: 1px solid #e2e8f1; border-radius: 11px; }
QFrame#transferTaskCard:hover { background: #fcfdff; border-color: #cbd8ee; }
QFrame#transferTaskCard[selected="true"] { background: #f8faff; border: 1px solid #8ea8ee; }
QLabel#transferDirection { border-radius: 8px; padding: 4px 8px; font-size: 12px; font-weight: 650; }
QLabel#transferDirection[direction="upload"] { color: #4666d8; background: #edf2ff; }
QLabel#transferDirection[direction="download"] { color: #167c72; background: #eaf8f5; }
QLabel#transferFileName { color: #22314b; font-size: 14px; font-weight: 650; }
QLabel#transferFileName:hover, QLabel#transferPath:hover { color: #4666dd; }
QLabel#transferPeer, QLabel#transferBytes { color: #6d7c93; font-size: 12px; }
QLabel#transferPercent { color: #334c7c; font-size: 13px; font-weight: 650; }
QLabel#transferPath { color: #7b899d; font-size: 11px; }
QLabel#transferError { color: #c44545; font-size: 12px; }
QLabel#transferState { border-radius: 8px; padding: 4px 9px; font-size: 12px; font-weight: 600; }
QLabel#transferState[state="active"] { color: #3564c8; background: #eaf1ff; }
QLabel#transferState[state="queued"] { color: #64748b; background: #f0f3f7; }
QLabel#transferState[state="paused"] { color: #9a6715; background: #fff4dc; }
QLabel#transferState[state="completed"] { color: #238061; background: #e8f7ef; }
QLabel#transferState[state="failed"] { color: #bd4141; background: #fff0f0; }
QLabel#transferState[state="cancelled"] { color: #758195; background: #f0f2f5; }
QPushButton#openTransferCardDirectoryButton { padding: 5px 10px; font-size: 12px; }
QProgressBar#transferProgress { background: #e9edf4; border: none; border-radius: 4px; min-height: 8px; max-height: 8px; }
QProgressBar#transferProgress::chunk { background: #6685ed; border-radius: 4px; }
QProgressBar#transferProgress[state="paused"]::chunk { background: #d8a43e; }
QProgressBar#transferProgress[state="completed"]::chunk { background: #42ad84; }
QProgressBar#transferProgress[state="failed"]::chunk { background: #dd7070; }
QProgressBar#transferProgress[state="cancelled"]::chunk { background: #a5afbd; }
QSplitter::handle { background: transparent; width: 8px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #cbd4e2; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #aebbd0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QToolTip { color: #ffffff; background: #334155; border: none; padding: 5px; }
)QSS"));
}
