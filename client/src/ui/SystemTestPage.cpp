#include "SystemTestPage.h"

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

SystemTestPage::SystemTestPage(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("systemTestPage"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(28, 28, 28, 28);
    auto *panel = new QFrame(this);
    panel->setProperty("card", true);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(34, 32, 34, 32);
    layout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("系统测试"), panel);
    title->setObjectName(QStringLiteral("brandTitle"));
    layout->addWidget(title);
    auto *badge = new QLabel(QStringLiteral("预留页面 · 暂未启用"), panel);
    badge->setObjectName(QStringLiteral("noticeLabel"));
    layout->addWidget(badge);
    auto *description = new QLabel(QStringLiteral(
        "后续用于模拟大量用户同时发送消息和传输文件，观察后端压力、成功率、延迟和资源使用情况。"), panel);
    description->setObjectName(QStringLiteral("mutedLabel"));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *button = new QPushButton(QStringLiteral("压力测试尚未启用"), panel);
    button->setObjectName(QStringLiteral("stressTestButton"));
    button->setEnabled(false);
    layout->addWidget(button);
    layout->addStretch();
    outer->addWidget(panel);
}
