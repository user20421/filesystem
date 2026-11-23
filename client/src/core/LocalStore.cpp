#include "LocalStore.h"

#include "LocalStoreMessageCodec.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>


LocalStore::LocalStore(QString dataDir, QObject *parent)
    : QObject(parent), m_root(QDir::cleanPath(QDir(dataDir).absolutePath())),
      m_valid(!dataDir.trimmed().isEmpty()) {}

bool LocalStore::initialize(QString *error)
{
    if (!m_valid || m_root.absolutePath().isEmpty() || m_root.isRoot()) {
        if (error)
            *error = QStringLiteral("数据目录不能为空或文件系统根目录");
        return false;
    }
    if (!m_root.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("无法创建数据目录：") + m_root.absolutePath();
        return false;
    }
    const QStringList directories{QStringLiteral("inbox"), QStringLiteral("outbox"),
                                  QStringLiteral("downloads"), QStringLiteral("temp"),
                                  QStringLiteral("keys"), QStringLiteral("logs"),
                                  QStringLiteral("config")};
    for (const QString &directory : directories) {
        if (!m_root.mkpath(directory)) {
            if (error)
                *error = QStringLiteral("无法创建目录：") + m_root.filePath(directory);
            return false;
        }
    }
    return true;
}

QString LocalStore::path(const QString &name) const
{
    return m_root.filePath(name);
}

bool LocalStore::writeJson(const QString &filePath, const QJsonObject &object, QString *error) const
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(json) != json.size()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool LocalStore::saveSession(const AuthSession &session, QString *error)
{
    const QJsonObject user{{QStringLiteral("id"), session.user.id},
                           {QStringLiteral("username"), session.user.username},
                           {QStringLiteral("display_name"), session.user.displayName}};
    // Session tokens are never logged.  This file is owner-only where the OS
    // supports Qt permission mapping; deployments can replace it with a keychain.
    return writeJson(path(QStringLiteral("config/session.json")),
                     {{QStringLiteral("access_token"), session.accessToken},
                      {QStringLiteral("refresh_token"), session.refreshToken},
                      {QStringLiteral("user"), user}}, error);
}

AuthSession LocalStore::loadSession() const
{
    QFile file(path(QStringLiteral("config/session.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    AuthSession session;
    session.accessToken = root.value(QStringLiteral("access_token")).toString();
    session.refreshToken = root.value(QStringLiteral("refresh_token")).toString();
    const QJsonObject user = root.value(QStringLiteral("user")).toObject();
    session.user.id = user.value(QStringLiteral("id")).toString();
    session.user.username = user.value(QStringLiteral("username")).toString();
    session.user.displayName = user.value(QStringLiteral("display_name")).toString();
    return session;
}

void LocalStore::clearSession()
{
    QFile::remove(path(QStringLiteral("config/session.json")));
}

QString LocalStore::messagesPath() const
{
    return path(QStringLiteral("config/messages.json"));
}

QJsonObject LocalStore::readMessageDocument() const
{
    QFile file(messagesPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool LocalStore::appendMessage(const QString &peerId, const ChatMessage &message,
                               QString *error)
{
    if (error)
        error->clear();
    if (peerId.isEmpty()) {
        if (error)
            *error = QStringLiteral("消息缺少联系人 ID");
        return false;
    }
    QJsonObject root = readMessageDocument();
    QJsonArray array = root.value(peerId).toArray();
    if (!message.id.isEmpty()) {
        for (int index = 0; index < array.size(); ++index) {
            const QJsonObject existing = array.at(index).toObject();
            if (existing.value(QStringLiteral("message_id")).toString() != message.id)
                continue;
            const ChatMessage update = LocalStoreMessageCodec::mergeMessage(LocalStoreMessageCodec::messageFromJson(existing), message);
            const QJsonObject replacement = LocalStoreMessageCodec::messageToJson(update);
            if (replacement == existing)
                return false;
            array.replace(index, replacement);
            root.insert(peerId, array);
            return writeJson(messagesPath(), root, error);
        }
    }
    array.append(LocalStoreMessageCodec::messageToJson(message));
    // Bound acknowledged history without ever dropping work that still needs
    // transport recovery. An all-pending conversation may exceed the cap.
    while (array.size() > 2000) {
        int removable = -1;
        for (int index = 0; index < array.size(); ++index) {
            const ChatMessage candidate = LocalStoreMessageCodec::messageFromJson(array.at(index).toObject());
            if (!candidate.pending && !candidate.failed) {
                removable = index;
                break;
            }
        }
        if (removable < 0)
            break;
        array.removeAt(removable);
    }
    root.insert(peerId, array);
    return writeJson(messagesPath(), root, error);
}

bool LocalStore::markMessagesRead(const QString &peerId, const QString &senderId,
                                  const QString &cursor, qint64 readAt, QString *error)
{
    if (error)
        error->clear();
    bool cursorOk = false;
    const qulonglong readCursor = cursor.toULongLong(&cursorOk);
    if (peerId.isEmpty() || senderId.isEmpty() || !cursorOk) {
        if (error)
            *error = QStringLiteral("已读回执缺少有效的联系人、发送者或 cursor");
        return false;
    }
    QJsonObject root = readMessageDocument();
    QJsonArray array = root.value(peerId).toArray();
    bool changed = false;
    for (int index = 0; index < array.size(); ++index) {
        QJsonObject object = array.at(index).toObject();
        if (object.value(QStringLiteral("sender_id")).toString() != senderId)
            continue;
        bool messageCursorOk = false;
        const qulonglong messageCursor = object.value(QStringLiteral("cursor")).toString()
            .toULongLong(&messageCursorOk);
        if (!messageCursorOk || messageCursor > readCursor)
            continue;
        const qint64 previous = object.value(QStringLiteral("read_at")).toVariant().toLongLong();
        if (previous > 0 && (readAt <= 0 || previous >= readAt))
            continue;
        object.insert(QStringLiteral("read_at"), readAt > 0 ? readAt : previous);
        array.replace(index, object);
        changed = true;
    }
    if (!changed)
        return false;
    root.insert(peerId, array);
    // READ changes read_at only; transport ACK/pending/failed stay untouched.
    return writeJson(messagesPath(), root, error);
}

QList<ChatMessage> LocalStore::messages(const QString &peerId, const QString &afterId) const
{
    const QJsonArray array = readMessageDocument().value(peerId).toArray();
    QList<ChatMessage> result;
    bool afterSeen = afterId.isEmpty();
    for (const QJsonValue &value : array) {
        ChatMessage message = LocalStoreMessageCodec::messageFromJson(value.toObject());
        if (afterSeen)
            result.append(message);
        if (!afterId.isEmpty() && (message.cursor == afterId || message.id == afterId))
            afterSeen = true;
    }
    return result;
}

QList<ChatMessage> LocalStore::outbox(const QString &senderId) const
{
    QList<ChatMessage> result;
    if (senderId.isEmpty())
        return result;
    const QJsonObject root = readMessageDocument();
    for (auto peer = root.constBegin(); peer != root.constEnd(); ++peer) {
        const QJsonArray array = peer.value().toArray();
        for (const QJsonValue &value : array) {
            const ChatMessage message = LocalStoreMessageCodec::messageFromJson(value.toObject());
            if (message.senderId == senderId && !message.acked)
                result.append(message);
        }
    }
    return result;
}

bool LocalStore::findMessage(const QString &messageId, const QString &senderId,
                             ChatMessage *message, QString *peerId) const
{
    if (messageId.isEmpty())
        return false;
    const QJsonObject root = readMessageDocument();
    for (auto peer = root.constBegin(); peer != root.constEnd(); ++peer) {
        const QJsonArray array = peer.value().toArray();
        for (const QJsonValue &value : array) {
            const ChatMessage candidate = LocalStoreMessageCodec::messageFromJson(value.toObject());
            if (candidate.id != messageId
                || (!senderId.isEmpty() && candidate.senderId != senderId))
                continue;
            if (message)
                *message = candidate;
            if (peerId)
                *peerId = peer.key();
            return true;
        }
    }
    return false;
}

bool LocalStore::recoverOutbox(const QString &senderId, qint64 nextAttemptAt,
                               QString *error)
{
    if (error)
        error->clear();
    if (senderId.isEmpty()) {
        if (error)
            *error = QStringLiteral("无法恢复没有发送者 ID 的 outbox");
        return false;
    }
    QJsonObject root = readMessageDocument();
    bool changed = false;
    for (auto peer = root.begin(); peer != root.end(); ++peer) {
        QJsonArray array = peer.value().toArray();
        bool peerChanged = false;
        for (int index = 0; index < array.size(); ++index) {
            ChatMessage message = LocalStoreMessageCodec::messageFromJson(array.at(index).toObject());
            if (message.senderId != senderId || message.acked)
                continue;
            if (!message.cursor.isEmpty()) {
                // Pre-outbox rows used the server cursor as their only proof
                // of persistence. Migrate that proof to the explicit ACK bit.
                message.acked = true;
                message.pending = false;
                message.failed = false;
                message.nextAttemptAt = 0;
                message.deliveryError.clear();
            } else {
                const bool restartFailed = message.failed;
                message.pending = true;
                message.failed = false;
                message.acked = false;
                if (restartFailed) {
                    // An explicitly recovered terminal failure starts a new
                    // delivery cycle. Ordinary pending retries keep their
                    // persisted attempt budget across process restarts.
                    message.attemptCount = 0;
                    message.nextAttemptAt = nextAttemptAt;
                    message.deliveryError.clear();
                } else if (message.nextAttemptAt <= 0) {
                    message.nextAttemptAt = nextAttemptAt;
                }
            }
            array.replace(index, LocalStoreMessageCodec::messageToJson(message));
            changed = true;
            peerChanged = true;
        }
        if (peerChanged)
            peer.value() = array;
    }
    if (!changed)
        return false;
    return writeJson(messagesPath(), root, error);
}

QString LocalStore::sanitizeFileName(const QString &originalName)
{
    QString name = originalName;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1);
    name.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]")));
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = QStringLiteral("file");
    const QString base = name.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> reserved{QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"), QStringLiteral("COM1"),
        QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"),
        QStringLiteral("COM8"), QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"), QStringLiteral("LPT4"),
        QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    if (reserved.contains(base))
        name.prepend(QLatin1Char('_'));
    if (name.size() > 120)
        name = name.left(120);
    return name;
}

QString LocalStore::uniqueDownloadPath(const QString &originalName) const
{
    const QString safeName = sanitizeFileName(originalName);
    const QFileInfo info(safeName);
    const QString base = info.completeBaseName().isEmpty() ? QStringLiteral("file")
                                                           : info.completeBaseName();
    const QString suffix = info.suffix();
    for (int index = 0; index < 10000; ++index) {
        const QString number = index == 0 ? QString{} : QStringLiteral(" (%1)").arg(index);
        const QString candidate = base + number
            + (suffix.isEmpty() ? QString{} : QLatin1Char('.') + suffix);
        const QString fullPath = path(QStringLiteral("downloads/") + candidate);
        if (!QFileInfo::exists(fullPath))
            return fullPath;
    }
    return path(QStringLiteral("downloads/") + QUuid::createUuid().toString(QUuid::WithoutBraces));
}
