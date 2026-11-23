#include "SessionContext.h"
#include "SingleUserWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>

// --data-dir 是共享 base；登录后由后端用户名决定 users/ 下的独立目录和用户锁。
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("secure-im-client"));
    Theme::apply(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("单用户安全 IM Qt 客户端"));
    parser.addHelpOption();
    const QCommandLineOption dataDirOption(QStringLiteral("data-dir"),
        QStringLiteral("客户端数据 base 目录（必填）"), QStringLiteral("directory"));
    const QCommandLineOption serverOption(QStringLiteral("server-url"),
        QStringLiteral("后端 HTTP 地址"), QStringLiteral("url"),
        QStringLiteral("http://127.0.0.1:8080"));
    const QCommandLineOption usernameOption(QStringLiteral("username"),
        QStringLiteral("预填登录名，并定向选择唯一匹配的可恢复会话（可选）"), QStringLiteral("name"));
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
