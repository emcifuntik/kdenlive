/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <functional>

namespace Mcp {
QJsonObject success(const QJsonObject &data = {});
QJsonObject failure(const QString &message);
QJsonObject booleanResult(bool ok, const QString &message = QStringLiteral("The operation was rejected by the editor."));
QJsonObject string(const QString &description = {});
QJsonObject integer(int minimum = 0, int maximum = 2147483647);
QJsonObject number(double minimum, double maximum);
QJsonObject boolean();
QJsonObject enumeration(const QStringList &values);
QJsonObject array(const QJsonObject &items, int minimum = 0, int maximum = 10000);
QJsonObject object(const QJsonObject &properties, const QStringList &required = {});
// Validates precisely the subset of JSON Schema emitted by this registry.
QString validate(const QJsonValue &value, const QJsonObject &schema, const QString &path = QStringLiteral("arguments"));
} // namespace Mcp

/** Transport-independent, explicitly registered editor operations. No arbitrary Qt method invocation. */
class McpToolRegistry
{
public:
    using Handler = std::function<QJsonObject(const QJsonObject &)>;
    void add(const QString &name, const QString &description, const QJsonObject &schema, bool readOnly, Handler handler);
    QJsonArray tools() const;
    bool contains(const QString &name) const;
    QJsonObject call(const QString &name, const QJsonObject &arguments) const;
    void setGuard(std::function<QString()> guard);

private:
    struct Tool
    {
        QJsonObject definition;
        Handler handler;
    };
    QMap<QString, Tool> m_tools;
    std::function<QString()> m_guard;
};
