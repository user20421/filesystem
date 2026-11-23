#include "SimulatorWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QMessageBox>
#include <QScreen>

// 保留独立 ROOT 压测入口，与普通聊天会话分离；关闭窗口时会停止活动 runner。
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("secure-im-simulator"));
    Theme::apply(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("ROOT 独立系统压力测试工具"));
    parser.addHelpOption();
    const QCommandLineOption serverOption(QStringLiteral("server-url"),
        QStringLiteral("压测后端 HTTP 地址"), QStringLiteral("url"));
    parser.addOption(serverOption);
    parser.process(application);

    SimulatorWindow window({}, QUrl(parser.value(serverOption)));
    if (!window.isOpen()) {
        QMessageBox::critical(nullptr, QStringLiteral("压测工具启动失败"), window.startupError());
        return 2;
    }
    window.show();
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        window.move(available.center() - window.rect().center());
    }
    return QApplication::exec();
}
