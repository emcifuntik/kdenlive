/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptoolregistry.h"

#include <QJsonDocument>
#include <cmath>

namespace Mcp {
QJsonObject success(const QJsonObject &data)
{
    return {
        {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"), QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))}}}},
        {QStringLiteral("structuredContent"), data},
        {QStringLiteral("isError"), false}};
}

QJsonObject failure(const QString &message)
{
    auto result = success({{QStringLiteral("error"), message}});
    result.insert(QStringLiteral("isError"), true);
    return result;
}

QJsonObject booleanResult(bool ok, const QString &message)
{
    return ok ? success({{QStringLiteral("ok"), true}}) : failure(message);
}

QJsonObject string(const QString &description)
{
    return {{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), description}, {QStringLiteral("maxLength"), 2097152}};
}
QJsonObject integer(int minimum, int maximum)
{
    return {{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), minimum}, {QStringLiteral("maximum"), maximum}};
}
QJsonObject number(double minimum, double maximum)
{
    return {{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), minimum}, {QStringLiteral("maximum"), maximum}};
}
QJsonObject boolean()
{
    return {{QStringLiteral("type"), QStringLiteral("boolean")}};
}
QJsonObject enumeration(const QStringList &values)
{
    return {{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("enum"), QJsonArray::fromStringList(values)}};
}
QJsonObject array(const QJsonObject &items, int minimum, int maximum)
{
    return {{QStringLiteral("type"), QStringLiteral("array")},
            {QStringLiteral("items"), items},
            {QStringLiteral("minItems"), minimum},
            {QStringLiteral("maxItems"), maximum}};
}
QJsonObject object(const QJsonObject &properties, const QStringList &required)
{
    return {{QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"), properties},
            {QStringLiteral("required"), QJsonArray::fromStringList(required)},
            {QStringLiteral("additionalProperties"), false}};
}

QString validate(const QJsonValue &value, const QJsonObject &schema, const QString &path)
{
    const auto type = schema.value(QStringLiteral("type")).toString();
    const auto bad = [&]() { return path + QStringLiteral(" must be ") + type; };
    if (type == QLatin1String("object")) {
        if (!value.isObject()) {
            return bad();
        }
        const auto obj = value.toObject();
        const auto props = schema.value(QStringLiteral("properties")).toObject();
        for (const auto &key : schema.value(QStringLiteral("required")).toArray()) {
            if (!obj.contains(key.toString())) {
                return path + QLatin1Char('.') + key.toString() + QStringLiteral(" is required");
            }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!props.contains(it.key())) {
                return path + QLatin1Char('.') + it.key() + QStringLiteral(" is not supported");
            }
            const auto error = validate(it.value(), props.value(it.key()).toObject(), path + QLatin1Char('.') + it.key());
            if (!error.isEmpty()) {
                return error;
            }
        }
    } else if (type == QLatin1String("array")) {
        if (!value.isArray()) {
            return bad();
        }
        const auto items = value.toArray();
        if (items.size() < schema.value(QStringLiteral("minItems")).toInt() || items.size() > schema.value(QStringLiteral("maxItems")).toInt()) {
            return path + QStringLiteral(" has an invalid number of items");
        }
        for (int i = 0; i < items.size(); ++i) {
            const auto error = validate(items[i], schema.value(QStringLiteral("items")).toObject(), path + QStringLiteral("[%1]").arg(i));
            if (!error.isEmpty()) {
                return error;
            }
        }
    } else if (type == QLatin1String("string")) {
        if (!value.isString()) {
            return bad();
        }
        if (schema.contains(QStringLiteral("maxLength")) && value.toString().size() > schema.value(QStringLiteral("maxLength")).toInt()) {
            return path + QStringLiteral(" is too long");
        }
    } else if (type == QLatin1String("boolean")) {
        if (!value.isBool()) {
            return bad();
        }
    } else if (type == QLatin1String("integer") || type == QLatin1String("number")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || (type == QLatin1String("integer") && std::floor(value.toDouble()) != value.toDouble())) {
            return bad();
        }
        if (value.toDouble() < schema.value(QStringLiteral("minimum")).toDouble() || value.toDouble() > schema.value(QStringLiteral("maximum")).toDouble()) {
            return path + QStringLiteral(" is out of range");
        }
    } else {
        return path + QStringLiteral(" has an unsupported schema");
    }
    if (schema.contains(QStringLiteral("enum")) && !schema.value(QStringLiteral("enum")).toArray().contains(value)) {
        return path + QStringLiteral(" is not an allowed value");
    }
    return {};
}
} // namespace Mcp

void McpToolRegistry::add(const QString &name, const QString &description, const QJsonObject &schema, bool readOnly, Handler handler)
{
    Q_ASSERT(!m_tools.contains(name));
    m_tools.insert(name, {{{QStringLiteral("name"), name},
                           {QStringLiteral("description"), description},
                           {QStringLiteral("inputSchema"), schema},
                           {QStringLiteral("annotations"), QJsonObject{{QStringLiteral("readOnlyHint"), readOnly},
                                                                       {QStringLiteral("destructiveHint"), !readOnly},
                                                                       {QStringLiteral("idempotentHint"), readOnly},
                                                                       {QStringLiteral("openWorldHint"), false}}}},
                          std::move(handler)});
}

QJsonArray McpToolRegistry::tools() const
{
    QJsonArray result;
    for (const auto &tool : m_tools) {
        result.append(tool.definition);
    }
    return result;
}

bool McpToolRegistry::contains(const QString &name) const
{
    return m_tools.contains(name);
}
void McpToolRegistry::setGuard(std::function<QString()> guard)
{
    m_guard = std::move(guard);
}

QJsonObject McpToolRegistry::call(const QString &name, const QJsonObject &arguments) const
{
    const auto it = m_tools.constFind(name);
    if (it == m_tools.cend()) {
        return Mcp::failure(QStringLiteral("Unknown tool: ") + name);
    }
    const auto error = Mcp::validate(arguments, it->definition.value(QStringLiteral("inputSchema")).toObject());
    if (!error.isEmpty()) {
        return Mcp::failure(error);
    }
    if (m_guard) {
        const auto reason = m_guard();
        if (!reason.isEmpty()) {
            return Mcp::failure(reason);
        }
    }
    return it->handler(arguments);
}
