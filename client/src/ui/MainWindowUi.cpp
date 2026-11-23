// MainWindow UI 构建；objectName、QSS、布局及信号连接顺序保持原样。
#include "MainWindow.h"

#include "MainWindowInternal.h"
#include "TransferTaskCard.h"

#include <QAbstractItemView>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
using namespace MainWindowInternal;

void MainWindow::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(12);

    auto *splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("chatSplitter"));
    splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter, 1);

    auto *left = card(splitter);
    left->setMinimumWidth(180);
    left->setMaximumWidth(230);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(16, 16, 16, 16);
    leftLayout->setSpacing(12);
    auto *contactsTitle = new QLabel(QStringLiteral("联系人"), left);
    contactsTitle->setObjectName(QStringLiteral("sectionTitle"));
    leftLayout->addWidget(contactsTitle);
    m_search = new QLineEdit(left);
    m_search->setPlaceholderText(QStringLiteral("搜索姓名或用户 ID"));
    m_search->setClearButtonEnabled(true);
    leftLayout->addWidget(m_search);
    m_users = new QListWidget(left);
    m_users->setObjectName(QStringLiteral("contactList"));
    m_users->setSpacing(4);
    leftLayout->addWidget(m_users, 1);
    auto *summary = new QLabel(QStringLiteral("每个用户拥有独立会话与本地历史"), left);
    summary->setObjectName(QStringLiteral("mutedLabel"));
    summary->setWordWrap(true);
    leftLayout->addWidget(summary);

    auto *right = card(splitter);
    auto *layout = new QVBoxLayout(right);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);
    auto *top = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    m_sessionLabel = new QLabel(right);
    m_sessionLabel->setObjectName(QStringLiteral("sessionTitle"));
    m_conversationLabel = new QLabel(QStringLiteral("选择联系人开始会话"), right);
    m_conversationLabel->setObjectName(QStringLiteral("mutedLabel"));
    titles->addWidget(m_sessionLabel);
    titles->addWidget(m_conversationLabel);
    top->addLayout(titles, 1);
    m_socketLabel = new QLabel(right);
    m_socketLabel->setObjectName(QStringLiteral("connectionBadge"));
    top->addWidget(m_socketLabel);
    m_exitButton = new QPushButton(QStringLiteral("退出"), right);
    m_exitButton->setObjectName(QStringLiteral("exitButton"));
    m_exitButton->setToolTip(QStringLiteral("撤销当前登录凭据并退出，不删除服务器账号或历史文件"));
    connect(m_exitButton, &QPushButton::clicked, this, &MainWindow::requestExit);
    m_deleteAccountButton = new QPushButton(QStringLiteral("注销"), right);
    m_deleteAccountButton->setObjectName(QStringLiteral("deleteAccountButton"));
    m_deleteAccountButton->setToolTip(QStringLiteral("永久删除服务器账号及关联消息和文件，不可恢复"));
    connect(m_deleteAccountButton, &QPushButton::clicked,
            this, &MainWindow::requestAccountDeletion);
    layout->addLayout(top);

    auto *navigation = new QHBoxLayout;
    m_chatNav = new QPushButton(QStringLiteral("消息"), right);
    m_transferNav = new QPushButton(QStringLiteral("文件传输"), right);
    for (QPushButton *button : {m_chatNav, m_transferNav}) {
        button->setCheckable(true);
        button->setProperty("navigation", true);
        navigation->addWidget(button);
    }
    m_chatNav->setChecked(true);
    navigation->addStretch();
    navigation->addWidget(m_exitButton);
    navigation->addWidget(m_deleteAccountButton);
    layout->addLayout(navigation);

    m_views = new QStackedWidget(right);
    layout->addWidget(m_views, 1);
    auto *chatView = new QWidget(m_views);
    auto *chatLayout = new QVBoxLayout(chatView);
    chatLayout->setContentsMargins(0, 0, 0, 0);
    chatLayout->setSpacing(10);
    m_chat = new QTextBrowser(chatView);
    m_chat->setObjectName(QStringLiteral("chatTranscript"));
    m_chat->setOpenExternalLinks(false);
    chatLayout->addWidget(m_chat, 1);
    m_composer = new QFrame(chatView);
    m_composer->setObjectName(QStringLiteral("composer"));
    m_composer->setVisible(false); // No message can be sent until a peer is selected.
    auto *composerLayout = new QHBoxLayout(m_composer);
    composerLayout->setContentsMargins(8, 6, 8, 6);
    m_input = new QLineEdit(m_composer);
    m_input->setPlaceholderText(QStringLiteral("选择用户后输入消息，按 Enter 发送"));
    auto *send = new QPushButton(QStringLiteral("发送"), m_composer);
    send->setProperty("primary", true);
    auto *attach = new QPushButton(QStringLiteral("添加文件"), m_composer);
    composerLayout->addWidget(m_input, 1);
    composerLayout->addWidget(attach);
    composerLayout->addWidget(send);
    chatLayout->addWidget(m_composer);
    m_views->addWidget(chatView);

    auto *transferView = new QWidget(m_views);
    auto *transferLayout = new QVBoxLayout(transferView);
    transferLayout->setContentsMargins(0, 0, 0, 0);
    transferLayout->setSpacing(10);
    auto *transferHeadingRow = new QHBoxLayout;
    auto *transferHeading = new QLabel(QStringLiteral("传输任务"), transferView);
    transferHeading->setObjectName(QStringLiteral("sectionTitle"));
    transferHeadingRow->addWidget(transferHeading);
    transferHeadingRow->addStretch(1);
    m_transferStats = new QLabel(transferView);
    m_transferStats->setObjectName(QStringLiteral("transferStats"));
    transferHeadingRow->addWidget(m_transferStats);
    transferLayout->addLayout(transferHeadingRow);

    m_transferEmpty = new QLabel(QStringLiteral(
        "还没有传输任务\n在消息页选择联系人并点击“添加文件”，或等待接收文件"), transferView);
    m_transferEmpty->setObjectName(QStringLiteral("transferEmptyState"));
    m_transferEmpty->setAlignment(Qt::AlignCenter);
    transferLayout->addWidget(m_transferEmpty, 1);

    m_transferList = new QTreeWidget(transferView);
    m_transferList->setObjectName(QStringLiteral("transferTable"));
    m_transferList->setColumnCount(1);
    m_transferList->setHeaderHidden(true);
    m_transferList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_transferList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_transferList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_transferList->setRootIsDecorated(false);
    m_transferList->setIndentation(0);
    m_transferList->setUniformRowHeights(false);
    m_transferList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_transferList->setFocusPolicy(Qt::StrongFocus);
    connect(m_transferList, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        for (int index = 0; index < m_transferList->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = m_transferList->topLevelItem(index);
            if (auto *widget = qobject_cast<TransferTaskCard *>(m_transferList->itemWidget(item, 0)))
                widget->setSelected(item == current);
        }
    });
    transferLayout->addWidget(m_transferList, 1);
    updateTransferSummary();

    auto *controls = new QHBoxLayout;
    auto *openDirectory = new QPushButton(QStringLiteral("打开目录"), transferView);
    openDirectory->setObjectName(QStringLiteral("openTransferDirectoryButton"));
    openDirectory->setToolTip(QStringLiteral("打开选中文件所在的本地目录"));
    controls->addWidget(openDirectory);
    connect(openDirectory, &QPushButton::clicked, this, [this] {
        if (m_closing)
            return;
        const QTreeWidgetItem *item = m_transferList->currentItem();
        if (item)
            openTransferDirectory(item->data(0, Qt::UserRole + 1).toString());
    });
    const QList<QPair<QString, QString>> actions{
        {QStringLiteral("暂停"), QStringLiteral("pause")},
        {QStringLiteral("继续"), QStringLiteral("resume")},
        {QStringLiteral("重试"), QStringLiteral("retry")},
        {QStringLiteral("取消"), QStringLiteral("cancel")}};
    for (const auto &action : actions) {
        auto *button = new QPushButton(action.first, transferView);
        button->setObjectName(QStringLiteral("transfer%1Button").arg(
            action.second.at(0).toUpper() + action.second.mid(1)));
        controls->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, action]() {
            if (m_closing)
                return;
            const QString id = selectedTransferId();
            if (id.isEmpty())
                return;
            if (action.second == QLatin1String("pause"))
                m_transfers->pause(id);
            else if (action.second == QLatin1String("resume"))
                m_transfers->resume(id);
            else if (action.second == QLatin1String("retry"))
                m_transfers->retry(id);
            else
                m_transfers->cancel(id);
        });
    }
    controls->addStretch();
    transferLayout->addLayout(controls);
    m_views->addWidget(transferView);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setHandleWidth(0);
    splitter->handle(1)->setEnabled(false);
    splitter->setSizes({190, 346});

    m_notice = new QLabel(right);
    m_notice->setObjectName(QStringLiteral("noticeLabel"));
    m_notice->setWordWrap(true);
    m_notice->hide();
    layout->addWidget(m_notice);

    m_usersTimer = new QTimer(this);
    m_usersTimer->setInterval(10000);
    m_historyTimer = new QTimer(this);
    m_historyTimer->setInterval(3000);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    m_reconnectTimer->setSingleShot(true);

    connect(send, &QPushButton::clicked, this, &MainWindow::sendText);
    connect(m_input, &QLineEdit::returnPressed, this, &MainWindow::sendText);
    connect(attach, &QPushButton::clicked, this, &MainWindow::chooseFile);
    connect(m_chatNav, &QPushButton::clicked, this, &MainWindow::showChatView);
    connect(m_transferNav, &QPushButton::clicked, this, &MainWindow::showTransfersView);
}
