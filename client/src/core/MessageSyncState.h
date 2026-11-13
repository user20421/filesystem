#pragma once

#include "ApiTypes.h"

#include <QMap>
#include <QSet>

// UI-independent history synchronization state.  In particular, a history
// response for an optimistic message must advance its cursor before the
// duplicate message is discarded.
class MessageSyncState {
public:
    bool accept(const QString &peerId, const ChatMessage &message)
    {
        if (!message.cursor.isEmpty()) {
            bool incomingOk = false;
            bool currentOk = false;
            const qulonglong incoming = message.cursor.toULongLong(&incomingOk);
            const qulonglong current = m_cursors.value(peerId).toULongLong(&currentOk);
            if (!currentOk || (incomingOk && incoming >= current))
                m_cursors[peerId] = message.cursor;
        }
        if (!message.id.isEmpty() && m_messageIds.contains(message.id))
            return false;
        if (!message.id.isEmpty())
            m_messageIds.insert(message.id);
        return true;
    }

    QString cursor(const QString &peerId) const { return m_cursors.value(peerId); }

private:
    QMap<QString, QString> m_cursors;
    QSet<QString> m_messageIds;
};
