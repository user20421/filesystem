#pragma once

#include "SessionContext.h"

#include <QMainWindow>
#include <memory>

class SessionPage;

// SingleUserWindow 是一进程一用户的顶层窗口。
// 普通模式固定为 560×390，进入 ROOT 压测后切换为可缩放尺寸并居中，返回时恢复普通布局。
class SingleUserWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit SingleUserWindow(std::unique_ptr<SessionContext> context, QWidget *parent = nullptr);

    SessionPage *sessionPage() const { return m_page; }

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    SessionPage *m_page;
};
