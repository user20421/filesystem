#include "LocalStore.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace {
QJsonObject messageToJson(const ChatMessage &message)
{
    return {{QStringLiteral("message_id"), message.id},
            {QStringLiteral("cursor"), message.cursor},
            {QStringLiteral("conversation_id"), message.conversationId},
            {QStringLiteral("sender_id"), message.senderId},
            {QStringLiteral("recipient_id"), message.recipientId},
            {QStringLiteral("type"), message.kind},
            {QStringLiteral("text"), message.content},
            {QStringLiteral("file_id"), message.fileId},
            {QStringLiteral("file_name"), message.fileName},
            {QStringLiteral("size"), message.fileSize},
            {QStringLiteral("timestamp"), message.timestamp},
            {QStringLiteral("read_at"), message.readAt}};
}

ChatMessage messageFromJson(const QJsonObject &object)
{
    ChatMessage message;
    message.id = object.value(QStringLiteral("message_id")).toString();
    message.cursor = object.value(QStringLiteral("cursor")).toString();
    message.conversationId = object.value(QStringLiteral("conversation_id")).toString();
    message.senderId = object.value(QStringLiteral("sender_id")).toString();
    message.recipientId = object.value(QStringLiteral("recipient_id")).toString();
    message.kind = object.value(QStringLiteral("type")).toString(QStringLiteral("text"));
    message.content = object.value(QStringLiteral("text")).toString();
    message.fileId = object.value(QStringLiteral("file_id")).toString();
    message.fileName = object.value(QStringLiteral("file_name")).toString();
    message.fileSize = object.value(QStringLiteral("size")).toVariant().toLongLong();
    message.timestamp = object.value(QStringLiteral("timestamp")).toVariant().toLongLong();
    message.readAt = object.value(QStringLiteral("read_at")).toVariant().toLongLong();
    return message;
}
} // namespace

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
    if (file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
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

QString LocalStore::knownAccountsPath() const
{
    return path(QStringLiteral("config/known_accounts.json"));
}

QJsonArray LocalStore::readKnownAccounts() const
{
    QFile file(knownAccountsPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isArray() ? document.array() : QJsonArray{};
}

bool LocalStore::writeKnownAccounts(const QJsonArray &accounts, QString *error) const
{
    QSaveFile file(knownAccountsPath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(QJsonDocument(accounts).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QFile::setPermissions(knownAccountsPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

QSet<QString> LocalStore::knownAccountIds() const
{
    QSet<QString> result;
    const QJsonArray accounts = readKnownAccounts();
    for (const QJsonValue &value : accounts) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            result.insert(id);
    }
    return result;
}

bool LocalStore::rememberAccount(const User &user, QString *error) const
{
    if (user.id.isEmpty() || user.username.isEmpty()) {
        if (error)
            *error = QStringLiteral("已知账号必须包含用户 ID 和用户名");
        return false;
    }
    QJsonArray accounts = readKnownAccounts();
    const QJsonObject account{{QStringLiteral("id"), user.id},
                              {QStringLiteral("username"), user.username}};
    for (int index = 0; index < accounts.size(); ++index) {
        if (accounts.at(index).toObject().value(QStringLiteral("id")).toString() != user.id)
            continue;
        if (accounts.at(index).toObject() == account)
            return true;
        accounts.replace(index, account);
        return writeKnownAccounts(accounts, error);
    }
    accounts.append(account);
    return writeKnownAccounts(accounts, error);
}

bool LocalStore::forgetAccount(const QString &userId, QString *error) const
{
    QJsonArray accounts = readKnownAccounts();
    for (int index = 0; index < accounts.size(); ++index) {
        if (accounts.at(index).toObject().value(QStringLiteral("id")).toString() != userId)
            continue;
        accounts.removeAt(index);
        return writeKnownAccounts(accounts, error);
    }
    return true;
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

bool LocalStore::appendMessage(const QString &peerId, const ChatMessage &message)
{
    if (peerId.isEmpty())
        return false;
    QJsonObject root = readMessageDocument();
    QJsonArray array = root.value(peerId).toArray();
    if (!message.id.isEmpty()) {
        for (int index = 0; index < array.size(); ++index) {
            QJsonObject existing = array.at(index).toObject();
            if (existing.value(QStringLiteral("message_id")).toString() != message.id)
                continue;
            const ChatMessage merged = messageFromJson(existing);
            ChatMessage update = message;
            if (update.cursor.isEmpty())
                update.cursor = merged.cursor;
            if (update.conversationId.isEmpty())
                update.conversationId = merged.conversationId;
            if (update.senderId.isEmpty())
                update.senderId = merged.senderId;
            if (update.recipientId.isEmpty())
                update.recipientId = merged.recipientId;
            if (update.kind.isEmpty())
                update.kind = merged.kind;
            if (update.content.isEmpty())
                update.content = merged.content;
            if (update.fileId.isEmpty())
                update.fileId = merged.fileId;
            if (update.fileName.isEmpty())
                update.fileName = merged.fileName;
            if (update.fileSize == 0)
                update.fileSize = merged.fileSize;
            if (update.timestamp == 0)
                update.timestamp = merged.timestamp;
            update.readAt = qMax(update.readAt, merged.readAt);
            const QJsonObject replacement = messageToJson(update);
            if (replacement == existing)
                return false;
            array.replace(index, replacement);
            root.insert(peerId, array);
            writeJson(messagesPath(), root);
            return true;
        }
    }
    array.append(messageToJson(message));
    // Bound local history so a long-running client does not grow without limit.
    while (array.size() > 2000)
        array.removeFirst();
    root.insert(peerId, array);
    writeJson(messagesPath(), root);
    return true;
}

bool LocalStore::markMessagesRead(const QString &peerId, const QString &senderId,
                                  const QString &cursor, qint64 readAt)
{
    bool cursorOk = false;
    const qulonglong readCursor = cursor.toULongLong(&cursorOk);
    if (peerId.isEmpty() || senderId.isEmpty() || !cursorOk)
        return false;
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
    writeJson(messagesPath(), root);
    return true;
}

QList<ChatMessage> LocalStore::messages(const QString &peerId, const QString &afterId) const
{
    const QJsonArray array = readMessageDocument().value(peerId).toArray();
    QList<ChatMessage> result;
    bool afterSeen = afterId.isEmpty();
    for (const QJsonValue &value : array) {
        ChatMessage message = messageFromJson(value.toObject());
        if (afterSeen)
            result.append(message);
        if (!afterId.isEmpty() && (message.cursor == afterId || message.id == afterId))
            afterSeen = true;
    }
    return result;
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
