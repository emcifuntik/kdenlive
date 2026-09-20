/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcpserver.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
constexpr qsizetype MaxHeaders = 16384;
constexpr qsizetype MaxBody = 4 * 1024 * 1024;
const QStringList Versions{QStringLiteral("2025-11-25"), QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26")};

QJsonObject rpcResult(const QJsonValue &id, const QJsonObject &result)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), id}, {QStringLiteral("result"), result}};
}
QJsonObject rpcError(const QJsonValue &id, int code, const QString &message)
{
    return {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id.isUndefined() ? QJsonValue(QJsonValue::Null) : id},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}
} // namespace

McpServer::McpServer(McpToolRegistry &registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
    connect(&m_listener, &QTcpServer::newConnection, this, &McpServer::acceptConnections);
    m_listener.setMaxPendingConnections(32);
}

bool McpServer::start(quint16 requestedPort)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_listener.isListening()) {
        return false;
    }
    return m_listener.listen(QHostAddress::LocalHost, requestedPort);
}

McpServer::~McpServer()
{
    stop();
}

void McpServer::stop()
{
    m_listener.close();
    const auto sockets = m_connections.keys();
    for (auto *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    m_connections.clear();
    m_sessions.clear();
}
quint16 McpServer::port() const
{
    return m_listener.serverPort();
}
QString McpServer::errorString() const
{
    return m_listener.errorString();
}

void McpServer::acceptConnections()
{
    while (m_listener.hasPendingConnections()) {
        auto *socket = m_listener.nextPendingConnection();
        if (m_connections.size() >= 32) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(MaxHeaders + MaxBody + 1);
        m_connections.insert(socket, {});
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, socket, [socket]() { socket->abort(); });
        timeout->start(15000);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { read(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_connections.remove(socket);
            socket->deleteLater();
        });
    }
}

void McpServer::read(QTcpSocket *socket)
{
    auto it = m_connections.find(socket);
    if (it == m_connections.end() || it->responding) {
        return;
    }
    it->buffer += socket->readAll();
    const auto end = it->buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (it->buffer.size() > MaxHeaders) {
            reply(socket, 431);
        }
        return;
    }
    if (end > MaxHeaders) {
        reply(socket, 431);
        return;
    }
    const auto lines = it->buffer.left(end).split('\n');
    const auto request = lines.first().trimmed().split(' ');
    if (request.size() != 3 || request[2] != "HTTP/1.1") {
        reply(socket, 400);
        return;
    }
    if (request[1] != "/mcp") {
        reply(socket, 404);
        return;
    }
    QHash<QByteArray, QByteArray> headers;
    static const QRegularExpression fieldName(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$"));
    for (int i = 1; i < lines.size(); ++i) {
        const auto line = lines[i];
        const auto colon = line.indexOf(':');
        const auto name = line.left(colon).toLower();
        if (colon <= 0 || !fieldName.match(QString::fromLatin1(name)).hasMatch() || headers.contains(name)) {
            reply(socket, 400);
            return;
        }
        const auto value = line.mid(colon + 1).trimmed();
        if (value.contains('\r') || value.contains('\0')) {
            reply(socket, 400);
            return;
        }
        headers.insert(name, value);
    }
    const QByteArray portSuffix = ':' + QByteArray::number(port());
    const auto host = headers.value("host").toLower();
    if (host != "127.0.0.1" + portSuffix && host != "localhost" + portSuffix) {
        reply(socket, 403);
        return;
    }
    if (headers.contains("origin")) {
        const auto origin = headers.value("origin");
        if (origin != "http://127.0.0.1" + portSuffix && origin != "http://localhost" + portSuffix) {
            reply(socket, 403);
            return;
        }
    }
    if (headers.contains("transfer-encoding")) {
        reply(socket, 501);
        return;
    }
    if (headers.contains("content-encoding")) {
        reply(socket, 415);
        return;
    }
    qint64 length = 0;
    if (headers.contains("content-length")) {
        const auto raw = headers.value("content-length");
        static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
        bool ok = false;
        length = raw.toLongLong(&ok);
        if (!ok || !digits.match(QString::fromLatin1(raw)).hasMatch()) {
            reply(socket, 400);
            return;
        }
    } else if (request[0] == "POST") {
        reply(socket, 411);
        return;
    }
    if (length > MaxBody) {
        reply(socket, 413);
        return;
    }
    if (it->buffer.size() - end - 4 < length) {
        return;
    }
    if (it->buffer.size() - end - 4 != length) {
        reply(socket, 400);
        return;
    }
    const QByteArray body = it->buffer.mid(end + 4, length);
    it->responding = true;
    // Nested UI event loops must not execute a second editor operation mid-command.
    if (m_dispatching) {
        reply(socket, 503);
        return;
    }
    QScopedValueRollback<bool> dispatching(m_dispatching, true);
    dispatch(socket, request[0], headers, body);
}

void McpServer::dispatch(QTcpSocket *socket, const QByteArray &verb, const QHash<QByteArray, QByteArray> &headers, const QByteArray &body)
{
    const auto now = QDateTime::currentDateTimeUtc();
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it->touched.secsTo(now) > 3600) {
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
    const auto sessionId = headers.value("mcp-session-id");
    if (!sessionId.isEmpty() && !m_sessions.contains(sessionId)) {
        reply(socket, 404);
        return;
    }
    const auto headerVersion = QString::fromLatin1(headers.value("mcp-protocol-version"));
    if (!headerVersion.isEmpty() && !Versions.contains(headerVersion)) {
        reply(socket, 400);
        return;
    }
    if (!sessionId.isEmpty() && !headerVersion.isEmpty() && m_sessions[sessionId].version != headerVersion) {
        reply(socket, 400);
        return;
    }
    if (verb == "GET") {
        reply(socket, 405);
        return;
    }
    if (verb == "DELETE") {
        if (sessionId.isEmpty()) {
            reply(socket, 400);
            return;
        }
        m_sessions.remove(sessionId);
        reply(socket, 200);
        return;
    }
    if (verb != "POST") {
        reply(socket, 405);
        return;
    }
    if (headers.value("content-type").split(';').first().trimmed().toLower() != "application/json") {
        reply(socket, 415);
        return;
    }
    const auto accept = headers.value("accept").toLower();
    if (!accept.contains("application/json") || !accept.contains("text/event-stream")) {
        reply(socket, 406);
        return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        reply(socket, 400, rpcError({}, -32700, QStringLiteral("Invalid JSON")));
        return;
    }
    if (!document.isObject()) {
        reply(socket, 400, rpcError({}, -32600, QStringLiteral("Expected one JSON-RPC object; batches are not supported")));
        return;
    }
    const auto request = document.object();
    const auto id = request.value(QStringLiteral("id"));
    const auto method = request.value(QStringLiteral("method"));
    const auto paramsValue = request.value(QStringLiteral("params"));
    if (request.value(QStringLiteral("jsonrpc")) != QLatin1String("2.0") || (!id.isUndefined() && !id.isString() && !id.isDouble()) ||
        (!paramsValue.isUndefined() && !paramsValue.isObject())) {
        reply(socket, 400, rpcError({}, -32600, QStringLiteral("Invalid JSON-RPC envelope")));
        return;
    }
    const auto params = paramsValue.toObject();
    if (method == QLatin1String("initialize")) {
        if (id.isUndefined() || !sessionId.isEmpty() || !params.value(QStringLiteral("protocolVersion")).isString() ||
            !params.value(QStringLiteral("capabilities")).isObject() || !params.value(QStringLiteral("clientInfo")).isObject()) {
            reply(socket, 400, rpcError(id, -32602, QStringLiteral("Invalid initialization")));
            return;
        }
        if (m_sessions.size() >= 32) {
            reply(socket, 503);
            return;
        }
        const auto requested = params.value(QStringLiteral("protocolVersion")).toString();
        const auto version = Versions.contains(requested) ? requested : Versions.first();
        const auto newId = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        m_sessions.insert(newId, {version, now, false});
        reply(socket, 200,
              rpcResult(
                  id,
                  {{QStringLiteral("protocolVersion"), version},
                   {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
                   {QStringLiteral("serverInfo"),
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("kdenlive")}, {QStringLiteral("version"), QCoreApplication::applicationVersion()}}},
                   {QStringLiteral("instructions"),
                    QStringLiteral(
                        "Edit the open Kdenlive project. First inspect project_info, timeline_get and bin_list. "
                        "All editing times are integer frames at project FPS. IDs belong to the current project/sequence; refresh after opening projects or "
                        "undo. "
                        "Discover installed effects and transitions using assets_list and assets_describe. Imports are asynchronous; poll bin_get until ready. "
                        "Use history_undo to reverse edits. Read tool schemas for units and non-undoable operations.")}}),
              newId);
        return;
    }
    if (sessionId.isEmpty()) {
        reply(socket, 400, rpcError(id, -32000, QStringLiteral("Initialize a session first")));
        return;
    }
    m_sessions[sessionId].touched = now;
    if (id.isUndefined()) {
        if (!method.isString() || !method.toString().startsWith(QLatin1String("notifications/"))) {
            reply(socket, 400);
            return;
        }
        if (method == QLatin1String("notifications/initialized")) {
            m_sessions[sessionId].initialized = true;
        }
        reply(socket, 202);
        return;
    }
    if (!method.isString()) {
        reply(socket, 400, rpcError(id, -32600, QStringLiteral("Missing method")));
        return;
    }
    if (method == QLatin1String("ping")) {
        reply(socket, 200, rpcResult(id, {}));
        return;
    }
    if (!m_sessions[sessionId].initialized) {
        reply(socket, 400, rpcError(id, -32000, QStringLiteral("Send notifications/initialized first")));
        return;
    }
    if (method == QLatin1String("tools/list")) {
        if (params.contains(QStringLiteral("cursor"))) {
            reply(socket, 200, rpcError(id, -32602, QStringLiteral("No pagination cursor is available")));
            return;
        }
        reply(socket, 200, rpcResult(id, {{QStringLiteral("tools"), m_registry.tools()}}));
    } else if (method == QLatin1String("tools/call")) {
        const auto name = params.value(QStringLiteral("name"));
        const auto args = params.value(QStringLiteral("arguments"));
        if (!name.isString() || (!args.isUndefined() && !args.isObject()) || !m_registry.contains(name.toString())) {
            reply(socket, 200, rpcError(id, -32602, QStringLiteral("Unknown tool or invalid tools/call parameters")));
            return;
        }
        reply(socket, 200, rpcResult(id, m_registry.call(name.toString(), args.toObject())));
    } else {
        reply(socket, 200, rpcError(id, -32601, QStringLiteral("Method not found")));
    }
}

void McpServer::reply(QTcpSocket *socket, int status, const QJsonObject &body, const QByteArray &session)
{
    if (!m_connections.contains(socket)) {
        return;
    }
    m_connections[socket].responding = true;
    const auto data = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    static const QHash<int, QByteArray> reasons{{200, "OK"},
                                                {202, "Accepted"},
                                                {400, "Bad Request"},
                                                {403, "Forbidden"},
                                                {404, "Not Found"},
                                                {405, "Method Not Allowed"},
                                                {406, "Not Acceptable"},
                                                {411, "Length Required"},
                                                {413, "Content Too Large"},
                                                {415, "Unsupported Media Type"},
                                                {431, "Request Header Fields Too Large"},
                                                {501, "Not Implemented"},
                                                {503, "Service Unavailable"}};
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reasons.value(status) + "\r\nConnection: close\r\nCache-Control: no-store\r\n";
    response += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(data.size()) + "\r\n";
    if (status == 405) {
        response += "Allow: POST, DELETE\r\n";
    }
    if (!session.isEmpty()) {
        response += "Mcp-Session-Id: " + session + "\r\n";
    }
    socket->write(response + "\r\n" + data);
    socket->disconnectFromHost();
}
