#include "SimulatorWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("secure-im-simulator"));
    Theme::apply(application);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("单窗口多用户安全 IM 工作台"));
    parser.addHelpOption();
    const QCommandLineOption serverOption(QStringLiteral("server-url"),
        QStringLiteral("后端 HTTP 地址"), QStringLiteral("url"));
    const QCommandLineOption previewOption(QStringLiteral("preview"),
        QStringLiteral("创建两个演示标签并保存 PNG 后退出"), QStringLiteral("png"));
    parser.addOption(serverOption);
    parser.addOption(previewOption);
    parser.process(application);

    SimulatorWindow window({}, QUrl(parser.value(serverOption)));
    if (parser.isSet(previewOption)) {
        window.addSessions(2, true);
        window.show();
        QTimer::singleShot(250, &application, [&application, &window, path = parser.value(previewOption)] {
            const bool saved = window.savePreview(path);
            application.exit(saved ? 0 : 4);
        });
    } else {
        window.show();
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect available = screen->availableGeometry();
            window.move(available.center() - window.rect().center());
        }
    }
    return QApplication::exec();
}
