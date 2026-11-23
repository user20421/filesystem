// 传输任务视图、汇总与目录打开；不改变 TransferManager 状态机。
#include "MainWindow.h"

#include "TransferTaskCard.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QTreeWidget>
void MainWindow::addTransferRow(TransferTask *task)
{
    if (m_closing)
        return;
    auto *item = new QTreeWidgetItem(m_transferList);
    item->setData(0, Qt::UserRole, task->id());
    item->setData(0, Qt::UserRole + 1, QDir::cleanPath(task->localPath()));
    auto *widget = new TransferTaskCard(m_transferList);
    m_transferList->setItemWidget(item, 0, widget);
    connect(widget, &TransferTaskCard::clicked, this, [this, item] {
        if (!m_closing)
            m_transferList->setCurrentItem(item);
    });
    connect(widget, &TransferTaskCard::openDirectoryRequested,
            this, &MainWindow::openTransferDirectory);
    updateTransferRow(task);
    if (!m_transferList->currentItem())
        m_transferList->setCurrentItem(item);
}

void MainWindow::updateTransferRow(TransferTask *task)
{
    if (m_closing)
        return;
    QTreeWidgetItem *found = nullptr;
    for (int index = 0; index < m_transferList->topLevelItemCount(); ++index) {
        auto *item = m_transferList->topLevelItem(index);
        if (item->data(0, Qt::UserRole).toString() == task->id()) {
            found = item;
            break;
        }
    }
    if (!found)
        return;
    auto *widget = qobject_cast<TransferTaskCard *>(m_transferList->itemWidget(found, 0));
    if (!widget)
        return;
    const QString peerName = m_userNames.value(task->peerId(), task->peerId().toUpper());
    widget->setTask(*task, peerName);
    found->setData(0, Qt::UserRole + 1, QDir::cleanPath(task->localPath()));
    found->setData(0, Qt::UserRole + 2, widget->property("stateKey"));
    found->setText(0, QStringLiteral("%1 · %2 · %3").arg(task->fileName(), peerName, task->stateText()));
    found->setToolTip(0, QDir::toNativeSeparators(task->localPath()));
    found->setSizeHint(0, widget->sizeHint());
    updateTransferSummary();
}

void MainWindow::updateTransferSummary()
{
    if (!m_transferStats || !m_transferList || !m_transferEmpty)
        return;
    int active = 0;
    int completed = 0;
    int failed = 0;
    int paused = 0;
    const int count = m_transferList->topLevelItemCount();
    for (int index = 0; index < count; ++index) {
        const QString state = m_transferList->topLevelItem(index)
            ->data(0, Qt::UserRole + 2).toString();
        if (state == QLatin1String("completed"))
            ++completed;
        else if (state == QLatin1String("failed"))
            ++failed;
        else if (state == QLatin1String("paused"))
            ++paused;
        else if (state != QLatin1String("cancelled"))
            ++active;
    }
    QStringList parts{QStringLiteral("共 %1 项").arg(count)};
    if (active > 0)
        parts.append(QStringLiteral("%1 进行中").arg(active));
    if (paused > 0)
        parts.append(QStringLiteral("%1 已暂停").arg(paused));
    if (completed > 0)
        parts.append(QStringLiteral("%1 已完成").arg(completed));
    if (failed > 0)
        parts.append(QStringLiteral("%1 失败").arg(failed));
    m_transferStats->setText(parts.join(QStringLiteral("  ·  ")));
    m_transferList->setVisible(count > 0);
    m_transferEmpty->setVisible(count == 0);
}

void MainWindow::openTransferDirectory(const QString &localPath)
{
    if (m_closing || localPath.trimmed().isEmpty())
        return;
    const QString normalized = QDir::cleanPath(QFileInfo(localPath).absoluteFilePath());
    const QString directory = QDir::cleanPath(QFileInfo(normalized).absolutePath());
    if (directory.isEmpty())
        return;
    const QUrl url = QUrl::fromLocalFile(directory);
    if (s_directoryOpener)
        s_directoryOpener(url);
    else
        QDesktopServices::openUrl(url);
}

QString MainWindow::selectedTransferId() const
{
    auto *item = m_transferList->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString{};
}
