/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcp/mcpserver.h"

#include <QJsonDocument>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

class McpTransportTest : public QObject
{
    Q_OBJECT
private:
    McpToolRegistry registry;
    std::unique_ptr<McpServer> server;
    QByteArray session;
    int calls = 0;

    QByteArray request(const QByteArray &body, const QByteArray &extra = {}, const QByteArray &method = "POST")
    {
        QByteArray raw = method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(server->port()) +
                         "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n";
        raw += extra;
        return raw + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    }

    QByteArray exchange(const QByteArray &request, int split = -1)
    {
        QTcpSocket socket;
        QByteArray result;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&socket, &QTcpSocket::readyRead, &loop, [&]() { result += socket.readAll(); });
        connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        connect(&socket, &QTcpSocket::connected, &loop, [&]() {
            if (split < 0) {
                socket.write(request);
            } else {
                socket.write(request.left(split));
                QTimer::singleShot(10, &socket, [&]() { socket.write(request.mid(split)); });
            }
        });
        socket.connectToHost(QHostAddress::LocalHost, server->port());
        deadline.start(3000);
        loop.exec();
        result += socket.readAll();
        return result;
    }
    QJsonObject json(const QByteArray &response) { return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object(); }
    QByteArray headers() { return "Mcp-Session-Id: " + session + "\r\nMcp-Protocol-Version: 2025-11-25\r\n"; }
    QByteArray initialize()
    {
        const auto response = exchange(request(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})"));
        for (const auto &line : response.split('\n')) {
            if (line.startsWith("Mcp-Session-Id:")) {
                session = line.mid(15).trimmed();
            }
        }
        return response;
    }
    void ready()
    {
        initialize();
        exchange(request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", headers()));
    }

private Q_SLOTS:
    void init()
    {
        calls = 0;
        session.clear();
        registry = McpToolRegistry();
        registry.add("edit", "Test edit", Mcp::object({{"frame", Mcp::integer(0, 100)}}, {"frame"}), false, [this](const QJsonObject &a) {
            ++calls;
            return Mcp::success({{"frame", a["frame"]}});
        });
        server = std::make_unique<McpServer>(registry);
        QVERIFY(server->start(0));
    }
    void cleanup()
    {
        server->stop();
        server.reset();
    }

    void lifecycleWithoutAuthentication()
    {
        const auto first = initialize();
        QVERIFY(first.startsWith("HTTP/1.1 200"));
        QVERIFY(!first.contains("WWW-Authenticate:"));
        QVERIFY(!session.isEmpty());
        QCOMPARE(json(first)["result"].toObject()["protocolVersion"].toString(), QStringLiteral("2025-11-25"));
        const auto notification = exchange(request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", headers()));
        QVERIFY(notification.startsWith("HTTP/1.1 202"));
        QVERIFY(notification.endsWith("\r\n\r\n"));
        const auto list = exchange(request(R"({"jsonrpc":"2.0","id":"list","method":"tools/list"})", headers()));
        QCOMPARE(json(list)["id"].toString(), QStringLiteral("list"));
        QCOMPARE(json(list)["result"].toObject()["tools"].toArray().size(), 1);
        const auto edit = exchange(request(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"edit","arguments":{"frame":12}}})", headers()));
        QCOMPARE(calls, 1);
        QCOMPARE(json(edit)["result"].toObject()["structuredContent"].toObject()["frame"].toInt(), 12);
    }

    void originAndHost()
    {
        QVERIFY(exchange(request("{}", "Origin: https://evil.example\r\n")).startsWith("HTTP/1.1 403"));
        QVERIFY(exchange(request("{}", "Origin: null\r\n")).startsWith("HTTP/1.1 403"));
        auto badHost = request("{}");
        badHost.replace("Host: 127.0.0.1:", "Host: attacker.example:");
        QVERIFY(exchange(badHost).startsWith("HTTP/1.1 403"));
        QCOMPARE(calls, 0);
    }

    void sessionAndVersion()
    {
        QVERIFY(exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")).startsWith("HTTP/1.1 400"));
        initialize();
        QVERIFY(exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", headers())).startsWith("HTTP/1.1 400"));
        QVERIFY(exchange(request("{}", "Mcp-Session-Id: expired\r\n")).startsWith("HTTP/1.1 404"));
        QVERIFY(exchange(request("{}", "Mcp-Protocol-Version: 1900-01-01\r\n")).startsWith("HTTP/1.1 400"));
        QVERIFY(exchange(request("", headers(), "DELETE")).startsWith("HTTP/1.1 200"));
        QVERIFY(exchange(request("{}", headers())).startsWith("HTTP/1.1 404"));
    }

    void malformedMessages()
    {
        ready();
        QCOMPARE(json(exchange(request("{", headers())))["error"].toObject()["code"].toInt(), -32700);
        QCOMPARE(json(exchange(request("[]", headers())))["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(json(exchange(request(R"({"jsonrpc":"2.0","id":null,"method":"ping"})", headers())))["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(json(exchange(request(R"({"jsonrpc":"1.0","id":1,"method":"ping"})", headers())))["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(json(exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"unknown"})", headers())))["error"].toObject()["code"].toInt(), -32601);
        QCOMPARE(json(exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"missing"}})", headers())))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
    }

    void validationPreventsMutation_data()
    {
        QTest::addColumn<QByteArray>("args");
        QTest::newRow("missing") << QByteArray("{}");
        QTest::newRow("negative") << QByteArray(R"({"frame":-1})");
        QTest::newRow("fractional") << QByteArray(R"({"frame":1.5})");
        QTest::newRow("wrong-type") << QByteArray(R"({"frame":"2"})");
        QTest::newRow("null") << QByteArray(R"({"frame":null})");
        QTest::newRow("overflow") << QByteArray(R"({"frame":1e100})");
        QTest::newRow("unknown-property") << QByteArray(R"({"frame":1,"extra":true})");
    }
    void validationPreventsMutation()
    {
        QFETCH(QByteArray, args);
        ready();
        const auto response =
            exchange(request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"edit\",\"arguments\":" + args + "}}", headers()));
        QVERIFY(json(response)["result"].toObject()["isError"].toBool());
        QCOMPARE(calls, 0);
    }

    void framingAndLimits()
    {
        auto raw = request("{}");
        raw.replace("Content-Length: 2", "Content-Length: 5000000");
        QVERIFY(exchange(raw).startsWith("HTTP/1.1 413"));
        QVERIFY(exchange(request("{}", "Transfer-Encoding: chunked\r\n")).startsWith("HTTP/1.1 501"));
        QVERIFY(exchange(request("{}", "Content-Length: 2\r\n")).startsWith("HTTP/1.1 400"));
        QVERIFY(exchange(request("{}", "Content-Encoding: gzip\r\n")).startsWith("HTTP/1.1 415"));
        QVERIFY(exchange(request("{}") + request("{}")).startsWith("HTTP/1.1 400"));
        QVERIFY(exchange("POST /mcp HTTP/1.1\r\nX: " + QByteArray(17000, 'x')).startsWith("HTTP/1.1 431"));
        raw = request("{}");
        raw.replace("Content-Length: 2", "Content-Length: -1");
        QVERIFY(exchange(raw).startsWith("HTTP/1.1 400"));
    }

    void fragmentedRequest()
    {
        ready();
        const auto raw = request(R"({"jsonrpc":"2.0","id":42,"method":"ping"})", headers());
        QCOMPARE(json(exchange(raw, raw.size() - 5))["id"].toInt(), 42);
        QCOMPARE(json(exchange(raw, 5))["id"].toInt(), 42);
    }

    void optionalSseAndUnknownEndpoint()
    {
        QVERIFY(exchange(request("", {}, "GET")).startsWith("HTTP/1.1 405"));
        auto raw = request("{}");
        raw.replace("/mcp ", "/other ");
        QVERIFY(exchange(raw).startsWith("HTTP/1.1 404"));
    }

    void nestedSchema()
    {
        const auto schema = Mcp::object({{"items", Mcp::array(Mcp::object({{"kind", Mcp::enumeration({"clip", "track"})}}, {"kind"}), 1, 2)}}, {"items"});
        QVERIFY(Mcp::validate(QJsonObject{{"items", QJsonArray{QJsonObject{{"kind", "clip"}}}}}, schema).isEmpty());
        QVERIFY(!Mcp::validate(QJsonObject{{"items", QJsonArray{QJsonObject{{"kind", "wrong"}}}}}, schema).isEmpty());
        QVERIFY(!Mcp::validate(QJsonObject{{"items", QJsonArray{}}}, schema).isEmpty());
        QVERIFY(!Mcp::validate(QJsonObject{{"items", QJsonArray{QJsonObject{}}}}, schema).isEmpty());
    }

    void guardPreventsMutation()
    {
        ready();
        registry.setGuard([]() { return QStringLiteral("Busy"); });
        const auto response =
            exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"edit","arguments":{"frame":1}}})", headers()));
        QVERIFY(json(response)["result"].toObject()["isError"].toBool());
        QCOMPARE(calls, 0);
    }

    void reentrantDispatchIsRejected()
    {
        ready();
        registry.add("nested", "Exercise a nested UI event loop", Mcp::object({}), true, [this](const QJsonObject &) {
            const auto nested = exchange(request(R"({"jsonrpc":"2.0","id":99,"method":"ping"})", headers()));
            return Mcp::success({{"busy", nested.startsWith("HTTP/1.1 503")}});
        });
        const auto reply = exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nested"}})", headers()));
        QVERIFY(json(reply)["result"].toObject()["structuredContent"].toObject()["busy"].toBool());
        QVERIFY(exchange(request(R"({"jsonrpc":"2.0","id":2,"method":"ping"})", headers())).startsWith("HTTP/1.1 200"));
    }

    void listenerAndRestart()
    {
        ready();
        const auto oldSession = headers();
        const auto originalPort = server->port();
        QVERIFY(!server->start(0));
        QCOMPARE(server->port(), originalPort);
        server->stop();
        QTcpServer occupied;
        QVERIFY(occupied.listen(QHostAddress::LocalHost, 0));
        QVERIFY(!server->start(occupied.serverPort()));
        QVERIFY(server->start(0));
        QVERIFY(exchange(request(R"({"jsonrpc":"2.0","id":1,"method":"ping"})", oldSession)).startsWith("HTTP/1.1 404"));
        QVERIFY(initialize().startsWith("HTTP/1.1 200"));
    }

    void sessionLimit()
    {
        for (int i = 0; i < 32; ++i) {
            QVERIFY(initialize().startsWith("HTTP/1.1 200"));
        }
        QVERIFY(initialize().startsWith("HTTP/1.1 503"));
        QVERIFY(exchange(request("", headers(), "DELETE")).startsWith("HTTP/1.1 200"));
        QVERIFY(initialize().startsWith("HTTP/1.1 200"));
    }

    void versionNegotiation()
    {
        auto raw = request(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"future","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
        QCOMPARE(json(exchange(raw))["result"].toObject()["protocolVersion"].toString(), QStringLiteral("2025-11-25"));
        const auto old = exchange(request(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})"));
        QCOMPARE(json(old)["result"].toObject()["protocolVersion"].toString(), QStringLiteral("2025-06-18"));
    }
};

QTEST_GUILESS_MAIN(McpTransportTest)
#include "mcptransporttest.moc"
