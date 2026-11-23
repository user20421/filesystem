#include "SessionContext.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

namespace {
bool validSession(const AuthSession &session)
{
    return !session.accessToken.isEmpty() && !session.user.id.isEmpty()
        && !session.user.username.trimmed().isEmpty();
}

bool hasEntries(const QString &path, const QSet<QString> &excluded = {})
{
    QDir dir(path);
    if (!dir.exists())
        return false;
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                    | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (!excluded.contains(entry.fileName()))
            return true;
    }
    return false;
}

bool prepareBase(const QString &path, QString *error)
{
    const QDir base(path);
    if (path.trimmed().isEmpty() || base.absolutePath().isEmpty() || base.isRoot()) {
        if (error)
            *error = QStringLiteral("数据目录不能为空或文件系统根目录");
        return false;
    }
    if (!base.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("无法创建数据目录：") + base.absolutePath();
        return false;
    }
    return true;
}
} // namespace

SessionContext::SessionContext(const QString &dataDir, const QUrl &serverUrl,
                               const QString &preferredUsername, StorageMode mode,
                               QObject *parent)
    : QObject(parent), m_baseDir(QDir::cleanPath(QDir(dataDir).absolutePath())),
      m_api(new ApiClient(this)), m_codec(createDefaultWireCodec()),
      m_preferredUsername(preferredUsername), m_mode(mode)
{
    m_api->setServerUrl(serverUrl);
    connect(m_api, &ApiClient::sessionRefreshed, this,
            [this](const AuthSession &session) {
        if (m_mode == StorageMode::Direct
            || (m_store && session.user.id == m_session.user.id))
            setSession(session);
    });
    connect(m_api, &ApiClient::authenticationFailed, this, [this](const QString &) {
        m_session = {};
    });
}

SessionContext::~SessionContext()
{
    shutdown();
}

bool SessionContext::open(QString *error)
{
    if (m_open)
        return true;
    if (m_closing) {
        if (error)
            *error = QStringLiteral("会话已经关闭");
        return false;
    }
    if (!prepareBase(m_baseDir, error))
        return false;
    m_open = true;
    if (m_mode == StorageMode::Direct) {
        m_store = new LocalStore(m_baseDir, this);
        if (!m_store->initialize(error) || !lockStore(m_store, true, error)) {
            delete m_store;
            m_store = nullptr;
            m_open = false;
            return false;
        }
        m_api->setSessionStore(m_store);
    }
    return true;
}

bool SessionContext::lockStore(LocalStore *store, bool direct, QString *error)
{
    const QString lockPath = direct
        ? store->path(QStringLiteral("config/client.lock"))
        : store->path(QStringLiteral("client.lock"));
    m_lock = std::make_unique<QLockFile>(lockPath);
    m_lock->setStaleLockTime(30000);
    if (m_lock->tryLock(0)) {
        m_userLocked = true;
        return true;
    }
    if (error)
        *error = QStringLiteral("该用户数据目录已被使用：%1").arg(store->root().absolutePath());
    return false;
}

QString SessionContext::normalizedUsername(const QString &username)
{
    return username.trimmed().toUpper();
}

QString SessionContext::userDirectoryName(const QString &username)
{
    const QString normalized = normalizedUsername(username);
    QString safe = normalized;
    safe.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]")),
                 QStringLiteral("_"));
    while (safe.endsWith(QLatin1Char('.')) || safe.endsWith(QLatin1Char(' ')))
        safe.chop(1);
    if (safe.isEmpty() || safe == QLatin1String(".") || safe == QLatin1String(".."))
        safe = QStringLiteral("_");
    else if (safe.startsWith(QStringLiteral("..")))
        safe.prepend(QLatin1Char('_'));
    const QString deviceBase = safe.section(QLatin1Char('.'), 0, 0);
    static const QSet<QString> reserved{QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"), QStringLiteral("COM1"),
        QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"),
        QStringLiteral("COM8"), QStringLiteral("COM9"), QStringLiteral("LPT1"),
        QStringLiteral("LPT2"), QStringLiteral("LPT3"), QStringLiteral("LPT4"),
        QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9")};
    if (reserved.contains(deviceBase))
        safe.prepend(QLatin1Char('_'));
    // Escaping alone could make different backend names share a directory.
    // A stable suffix keeps sanitized names isolated and deterministic.
    QString suffix;
    if (safe != normalized) {
        const QByteArray digest = QCryptographicHash::hash(normalized.toUtf8(),
                                                           QCryptographicHash::Sha256);
        suffix = QLatin1Char('-') + QString::fromLatin1(digest.toHex().left(8));
    }
    if (safe.size() + suffix.size() > 100)
        safe.truncate(100 - suffix.size());
    return safe + suffix;
}

QString SessionContext::userDataPath(const QString &baseDir, const QString &username)
{
    return QDir(QDir(baseDir).filePath(QStringLiteral("users")))
        .filePath(userDirectoryName(username));
}

bool SessionContext::activateUser(const AuthSession &source, bool migrateLegacy, QString *error)
{
    if (m_mode == StorageMode::Direct) {
        if (error)
            *error = QStringLiteral("ROOT 压测目录不支持普通用户激活");
        return false;
    }
    if (!m_open || m_closing || !validSession(source)) {
        if (error)
            *error = QStringLiteral("登录响应缺少有效的用户名、用户 ID 或访问令牌");
        return false;
    }
    AuthSession session = source;
    session.user.username = normalizedUsername(session.user.username);
    if (m_store) {
        if (normalizedUsername(m_session.user.username) != session.user.username) {
            if (error)
                *error = QStringLiteral("请先关闭当前用户会话");
            return false;
        }
        setSession(session);
        return true;
    }

    const QString userDir = userDataPath(m_baseDir, session.user.username);
    const QString usersDir = QDir(m_baseDir).filePath(QStringLiteral("users"));
    const QString relative = QDir(usersDir).relativeFilePath(userDir);
    if (relative.startsWith(QStringLiteral("..")) || QDir::isAbsolutePath(relative)
        || (QFileInfo::exists(userDir) && QFileInfo(userDir).isSymLink())) {
        if (error)
            *error = QStringLiteral("用户名无法映射到安全的用户数据目录");
        return false;
    }

    const AuthSession legacy = LocalStore(m_baseDir).loadSession();
    const bool sameLegacyIdentity = validSession(legacy)
        && normalizedUsername(legacy.user.username) == session.user.username;
    const bool anonymousLegacy = hasLegacyData() && !validSession(legacy);
    const bool migrateBaseLegacy = sameLegacyIdentity || (anonymousLegacy && migrateLegacy);
    if (!migratePrefixedUserData(userDir, session, migrateBaseLegacy, error))
        return false;

    auto *candidate = new LocalStore(userDir);
    if (!candidate->initialize(error) || !lockStore(candidate, false, error)) {
        delete candidate;
        return false;
    }

    if (migrateBaseLegacy) {
        if (!migrateLegacyData(userDir, error)) {
            m_lock->unlock();
            m_userLocked = false;
            delete candidate;
            return false;
        }
    }

    m_store = candidate;
    m_store->setParent(this);
    m_api->setSessionStore(m_store);
    setSession(session);
    return true;
}

AuthSession SessionContext::sessionToRestore() const
{
    if (m_mode != StorageMode::UserBase || !m_open)
        return {};
    QList<AuthSession> candidates;
    const AuthSession legacy = LocalStore(m_baseDir).loadSession();
    if (validSession(legacy))
        candidates.append(legacy);

    QDir users(QDir(m_baseDir).filePath(QStringLiteral("users")));
    const QFileInfoList directories = users.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot
                                                          | QDir::Hidden);
    for (const QFileInfo &directory : directories) {
        if (directory.isSymLink())
            continue;
        const AuthSession saved = LocalStore(directory.absoluteFilePath()).loadSession();
        if (validSession(saved))
            candidates.append(saved);
    }

    const QString preferred = normalizedUsername(m_preferredUsername);
    QList<AuthSession> matching;
    for (const AuthSession &candidate : candidates) {
        if (preferred.isEmpty()
            || normalizedUsername(candidate.user.username) == preferred)
            matching.append(candidate);
    }
    // 多会话绝不按目录顺序猜测；--username 可把选择收窄到唯一身份。
    return matching.size() == 1 ? matching.first() : AuthSession{};
}

bool SessionContext::hasLegacyData() const
{
    if (m_mode != StorageMode::UserBase)
        return false;
    return hasEntries(QDir(m_baseDir).filePath(QStringLiteral("config")),
                      {QStringLiteral("client.lock")})
        || hasEntries(QDir(m_baseDir).filePath(QStringLiteral("downloads")));
}

bool SessionContext::legacyMigrationNeedsConfirmation() const
{
    return hasLegacyData() && !validSession(LocalStore(m_baseDir).loadSession());
}

void SessionContext::setSession(const AuthSession &session)
{
    if (m_mode == StorageMode::UserBase && !session.user.id.isEmpty()) {
        if (!m_store || (!m_session.user.username.isEmpty()
            && normalizedUsername(m_session.user.username)
                != normalizedUsername(session.user.username)))
            return;
    }
    m_session = session;
    if (!m_session.user.username.isEmpty())
        m_session.user.username = normalizedUsername(m_session.user.username);
    m_api->setAuthSession(m_session);
}

void SessionContext::closeUser()
{
    m_api->setSessionStore(nullptr);
    setSession({});
    if (m_mode == StorageMode::Direct)
        return;
    if (m_store) {
        LocalStore *old = m_store;
        m_store = nullptr;
        delete old;
    }
    if (m_userLocked) {
        m_lock->unlock();
        m_userLocked = false;
    }
}

void SessionContext::shutdown()
{
    if (m_closing)
        return;
    m_closing = true;
    m_api->shutdown();
    closeUser();
    if (m_mode == StorageMode::Direct) {
        m_api->setSessionStore(nullptr);
        delete m_store;
        m_store = nullptr;
        if (m_userLocked) {
            m_lock->unlock();
            m_userLocked = false;
        }
    }
    m_open = false;
}

bool SessionContext::migratePrefixedUserData(const QString &userDir,
                                              const AuthSession &session,
                                              bool hasAnotherSource,
                                              QString *error) const
{
    const QString legacyName = QStringLiteral("u-")
        + userDirectoryName(session.user.username);
    const QDir base(m_baseDir);
    QStringList candidates{
        QDir(base.filePath(QStringLiteral("users"))).filePath(legacyName),
        QDir(base.filePath(QStringLiteral("alice/users"))).filePath(legacyName),
        QDir(base.filePath(QStringLiteral("bob/users"))).filePath(legacyName)};
    candidates.removeDuplicates();

    QStringList matching;
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (!info.exists() || !info.isDir() || info.isSymLink())
            continue;
        const AuthSession saved = LocalStore(candidate).loadSession();
        // A directory name is not an identity. Both backend user ID and the
        // normalized backend username must match before any data is moved.
        if (validSession(saved) && saved.user.id == session.user.id
            && normalizedUsername(saved.user.username) == session.user.username)
            matching.append(info.absoluteFilePath());
    }
    if (matching.isEmpty())
        return true;
    if (hasAnotherSource || matching.size() > 1) {
        if (error) {
            *error = matching.size() > 1
                ? QStringLiteral("检测到多个属于 %1 的旧用户目录，已拒绝合并").arg(session.user.username)
                : QStringLiteral("检测到多个旧数据来源，已拒绝合并到 %1").arg(userDir);
        }
        return false;
    }
    if (QFileInfo::exists(userDir)
        && (!QFileInfo(userDir).isDir() || hasEntries(userDir))) {
        if (error)
            *error = QStringLiteral("迁移目标 %1 非空，已禁止合并旧用户目录").arg(userDir);
        return false;
    }

    const QString source = matching.first();
    {
        QLockFile oldLock(QDir(source).filePath(QStringLiteral("client.lock")));
        oldLock.setStaleLockTime(30000);
        if (!oldLock.tryLock(0)) {
            if (error) {
                *error = QStringLiteral(
                    "旧用户目录锁仍被占用，已停止迁移且保留原数据：%1").arg(source);
            }
            return false;
        }
        oldLock.unlock();
    }

    // Rename the complete directory without staging or deleting its contents.
    // If either rmdir or rename fails, the source remains in place for recovery.
    if (!QDir().mkpath(QFileInfo(userDir).absolutePath())) {
        if (error)
            *error = QStringLiteral("无法创建迁移目标父目录，原数据保持不变：%1").arg(userDir);
        return false;
    }
    if (QFileInfo::exists(userDir) && !QDir().rmdir(userDir)) {
        if (error)
            *error = QStringLiteral("无法移除空的迁移目标容器，原数据保持不变：%1").arg(userDir);
        return false;
    }
    if (!QDir().rename(source, userDir)) {
        if (error)
            *error = QStringLiteral("旧用户目录 rename 失败，原数据已保留：%1").arg(source);
        return false;
    }

    // Clean up only empty historical containers. Never recursively remove an
    // alias/users directory: unrelated data makes rmdir fail and is preserved.
    const QDir sourceUsers(QFileInfo(source).absolutePath());
    const QString historicalAlias = sourceUsers.dirName() == QLatin1String("users")
        ? QDir(sourceUsers.absolutePath()).filePath(QStringLiteral("..")) : QString{};
    QDir().rmdir(sourceUsers.absolutePath());
    if (!historicalAlias.isEmpty())
        QDir().rmdir(QDir::cleanPath(historicalAlias));
    return true;
}

bool SessionContext::migrateLegacyData(const QString &userDir, QString *error) const
{
    const QStringList names{QStringLiteral("config"), QStringLiteral("downloads")};
    QStringList components;
    for (const QString &name : names) {
        const QString source = QDir(m_baseDir).filePath(name);
        if (hasEntries(source, name == QLatin1String("config")
                ? QSet<QString>{QStringLiteral("client.lock")} : QSet<QString>{}))
            components.append(name);
    }
    if (components.isEmpty())
        return true;

    // Never merge legacy files into an existing user's non-empty config/downloads.
    for (const QString &name : components) {
        const QString target = QDir(userDir).filePath(name);
        if (hasEntries(target)) {
            if (error)
                *error = QStringLiteral("迁移目标 %1 非空，已禁止合并旧数据").arg(target);
            return false;
        }
    }

    const QString staging = QDir(QDir(m_baseDir).filePath(QStringLiteral("users")))
        .filePath(QStringLiteral(".migration-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(staging)) {
        if (error)
            *error = QStringLiteral("无法创建旧数据迁移临时目录：") + staging;
        return false;
    }

    const auto rollback = [&]() {
        // Committed targets are first returned to staging, then every staged
        // component is renamed back to its original legacy location.
        for (const QString &name : components) {
            const QString target = QDir(userDir).filePath(name);
            const QString temporary = QDir(staging).filePath(name);
            if (QFileInfo::exists(target) && !QFileInfo::exists(temporary))
                QDir().rename(target, temporary);
        }
        for (const QString &name : components) {
            const QString source = QDir(m_baseDir).filePath(name);
            const QString temporary = QDir(staging).filePath(name);
            if (QFileInfo::exists(temporary)) {
                QDir().mkpath(QFileInfo(source).absolutePath());
                QDir().rename(temporary, source);
            }
        }
        // 若任何回滚 rename 也失败，保留临时目录中的剩余数据供恢复，
        // 绝不能为了清理现场而删除尚未回到原位的唯一副本。
        if (!hasEntries(staging))
            QDir().rmdir(staging);
    };

    for (const QString &name : components) {
        const QString source = QDir(m_baseDir).filePath(name);
        const QString temporary = QDir(staging).filePath(name);
        if (!QDir().rename(source, temporary)) {
            rollback();
            if (error)
                *error = QStringLiteral("无法暂存旧数据目录：%1").arg(source);
            return false;
        }
    }

    const QString oldLock = QDir(staging).filePath(QStringLiteral("config/client.lock"));
    if (QFileInfo::exists(oldLock) && !QFile::remove(oldLock)) {
        rollback();
        if (error)
            *error = QStringLiteral("无法移除旧目录锁，迁移已取消");
        return false;
    }

    QStringList committed;
    for (const QString &name : components) {
        const QString target = QDir(userDir).filePath(name);
        const QString temporary = QDir(staging).filePath(name);
        if (QFileInfo::exists(target) && !QDir().rmdir(target)) {
            rollback();
            if (error)
                *error = QStringLiteral("迁移目标在操作期间变为非空，已禁止合并：%1").arg(target);
            return false;
        }
        if (!QDir().rename(temporary, target)) {
            rollback();
            for (const QString &created : committed)
                QDir().mkpath(QDir(userDir).filePath(created));
            if (error)
                *error = QStringLiteral("无法迁入旧数据目录：%1").arg(target);
            return false;
        }
        committed.append(name);
    }
    QDir().rmdir(staging);
    return true;
}
