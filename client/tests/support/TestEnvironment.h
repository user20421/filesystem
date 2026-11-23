#pragma once

#include <QByteArray>
#include <QtGlobal>

// ScopedEnvironment 使用 RAII 保存并恢复环境变量，未设置的变量在析构时恢复为未设置状态，避免测试之间互相污染。
class ScopedEnvironment final {
public:
    explicit ScopedEnvironment(const char *name)
        : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)), m_value(qgetenv(name)) {}
    ~ScopedEnvironment()
    {
        if (m_hadValue)
            qputenv(m_name.constData(), m_value);
        else
            qunsetenv(m_name.constData());
    }

    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
    QByteArray m_name;
    bool m_hadValue;
    QByteArray m_value;
};
