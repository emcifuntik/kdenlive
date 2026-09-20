/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include "mcptoolregistry.h"
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

/** Bounded HTTP/1.1 server implementing MCP Streamable HTTP with JSON responses.
 * Runs on the GUI thread: model operations are serialized with interactive editing.
 * GET/SSE is optional in MCP and is deliberately answered with 405.
 */
class McpServer : public QObject
{
    Q_OBJECT
public:
    explicit McpServer(McpToolRegistry &registry, QObject *parent = nullptr);
    ~McpServer() override;
    bool start(quint16 port);
    void stop();
    quint16 port() const;
    QString errorString() const;

private:
    struct Connection
    {
        QByteArray buffer;
        bool responding = false;
    };
    struct Session
    {
        QString version;
        QDateTime touched;
        bool initialized = false;
    };
    QTcpServer m_listener;
    McpToolRegistry &m_registry;
    QHash<QTcpSocket *, Connection> m_connections;
    QHash<QByteArray, Session> m_sessions;
    bool m_dispatching = false;
    void acceptConnections();
    void read(QTcpSocket *socket);
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
    void reply(QTcpSocket *socket, int status, const QJsonObject &body = {}, const QByteArray &session = {});
};
