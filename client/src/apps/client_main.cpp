#include "SessionContext.h"
#include "SessionPage.h"
#include "Theme.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCommandLineParser>
#include <QMainWindow>
#include <QMessageBox>

namespace {
class SingleUserWindow final : public QMainWindow {
public:
    SingleUserWindow(std::unique_ptr<SessionContext> context, QWidget *parent = nullptr)
        : QMainWindow(parent), m_page(new SessionPage(std::move(context), this))
    {
        setWindowTitle(QStringLiteral("Lumen IM · 安全消息"));
        resize(1120, 780);
        setCentralWidget(m_page);
        connect(m_page, &SessionPage::titleChanged, this, [this] {
            if (m_page->isAuthenticated())
                setWindowTitle(QStringLiteral("Lumen IM · %1")
                    .arg(m_page->context()->session().user.label()));
        });
        connect(m_page, &SessionPage::exitRequested, this, &QMainWindow::close);
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        m_page->shutdown();
        QMainWindow::closeEvent(event);
    }

private:
    SessionPage *m_page;
};
} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("secure-im-client"));
    Theme::apply(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("单用户安全 IM Qt 客户端"));
    parser.addHelpOption();
    const QCommandLineOption dataDirOption(QStringLiteral("data-dir"),
        QStringLiteral("此用户独占的数据目录（必填）"), QStringLiteral("directory"));
    const QCommandLineOption serverOption(QStringLiteral("server-url"),
        QStringLiteral("后端 HTTP 地址"), QStringLiteral("url"),
        QStringLiteral("http://127.0.0.1:8080"));
    const QCommandLineOption usernameOption(QStringLiteral("username"),
        QStringLiteral("预填登录用户名（可选）"), QStringLiteral("name"));
    parser.addOption(dataDirOption);
    parser.addOption(serverOption);
    parser.addOption(usernameOption);
    parser.process(application);
    if (!parser.isSet(dataDirOption) || parser.value(dataDirOption).trimmed().isEmpty())
        parser.showHelp(1);

    auto context = std::make_unique<SessionContext>(parser.value(dataDirOption),
        QUrl(parser.value(serverOption)), parser.value(usernameOption));
    QString error;
    if (!context->open(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("客户端启动失败"), error);
        return 2;
    }
    SingleUserWindow window(std::move(context));
    window.show();
    return QApplication::exec();
}
