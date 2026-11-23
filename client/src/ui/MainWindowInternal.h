#pragma once

// MainWindow 私有 UI helper：仅用于实现文件拆分，不扩展公共 C++ 接口。
#include "ApiTypes.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QVBoxLayout>

namespace MainWindowInternal {
inline int execCenteredMessageBox(QMessageBox &dialog, QWidget *parent)
{
    dialog.adjustSize();
    QWidget *window = parent ? parent->window() : nullptr;
    if (window)
        dialog.move(window->frameGeometry().center() - dialog.rect().center());
    return dialog.exec();
}

inline QString formatTime(qint64 timestamp)
{
    if (timestamp > 0 && timestamp < 1000000000000LL)
        timestamp *= 1000;
    return timestamp > 0
        ? QDateTime::fromMSecsSinceEpoch(timestamp).toString(QStringLiteral("MM-dd HH:mm"))
        : QString{};
}

inline bool cursorAtMost(const QString &value, const QString &limit)
{
    bool valueOk = false;
    bool limitOk = false;
    const qulonglong left = value.toULongLong(&valueOk);
    const qulonglong right = limit.toULongLong(&limitOk);
    return valueOk && limitOk && left <= right;
}

inline QFrame *card(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setProperty("card", true);
    return frame;
}

class ContactItemWidget final : public QWidget {
public:
    ContactItemWidget(const QString &name, const QString &presence, int unread, QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 7, 10, 7);
        layout->setSpacing(3);
        auto *nameRow = new QHBoxLayout;
        nameRow->setSpacing(7);
        auto *nameLabel = new QLabel(name, this);
        nameLabel->setObjectName(QStringLiteral("contactNameLabel"));
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameRow->addWidget(nameLabel);
        m_unreadDot = new QLabel(this);
        m_unreadDot->setObjectName(QStringLiteral("contactUnreadDot"));
        m_unreadDot->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_unreadDot->setFixedSize(10, 10);
        m_unreadDot->setStyleSheet(QStringLiteral(
            "background: #ef4444; border: none; border-radius: 5px;"));
        nameRow->addWidget(m_unreadDot, 0, Qt::AlignVCenter);
        nameRow->addStretch(1);
        layout->addLayout(nameRow);
        auto *presenceLabel = new QLabel(presence, this);
        presenceLabel->setObjectName(QStringLiteral("contactPresenceLabel"));
        presenceLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(presenceLabel);
        setUnread(unread);
    }

    void setUnread(int count)
    {
        m_unreadDot->setVisible(count > 0);
        m_unreadDot->setToolTip(QStringLiteral("%1 条未读消息").arg(count));
    }

private:
    QLabel *m_unreadDot = nullptr;
};

inline QListWidgetItem *createContactItem(QListWidget *list, const User &user,
                                   const QString &presence, int unread)
{
    auto *item = new QListWidgetItem(list);
    item->setData(Qt::AccessibleTextRole, QStringLiteral("%1, %2").arg(user.label(), presence));
    item->setData(Qt::UserRole, user.id);
    item->setData(Qt::UserRole + 1, QStringList{user.label(), user.username,
                                               user.displayName, user.id}.join(QLatin1Char(' ')));
    item->setToolTip(QStringLiteral("与 %1 发起安全会话 · %2")
        .arg(user.label(), presence));
    auto *widget = new ContactItemWidget(user.label(), presence, unread, list);
    list->setItemWidget(item, widget);
    QSize sizeHint = widget->sizeHint();
    sizeHint.setHeight(58);
    item->setSizeHint(sizeHint);
    return item;
}
} // namespace MainWindowInternal
