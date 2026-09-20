/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "core.h"
#include "doc/kdenlivedoc.h"
#include "project/projectmanager.h"
#include "render/renderserver.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

McpTools::McpTools(RenderServer *renderServer, QObject *parent)
    : QObject(parent)
    , m_renderServer(renderServer)
{
    m_registry.setGuard([]() -> QString {
        if (!pCore || pCore->closing || !pCore->window()) {
            return QStringLiteral("The editor is shutting down or not ready.");
        }
        if (QThread::currentThread() != qApp->thread()) {
            return QStringLiteral("Editor operations must run on the GUI thread.");
        }
        if (QApplication::activeModalWidget()) {
            return QStringLiteral("The editor has an open modal dialog. Close it before retrying.");
        }
        return {};
    });
    registerProjectTools();
    registerBinTools();
    registerTimelineTools();
    registerAssetTools();
    registerAnnotationTools();
    registerRenderTools();
}

McpToolRegistry &McpTools::registry()
{
    return m_registry;
}

void McpTools::add(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required, bool readOnly,
                   McpToolRegistry::Handler handler)
{
    m_registry.add(name, description, Mcp::object(properties, required), readOnly, [name, handler](const QJsonObject &args) {
        if (name != QLatin1String("project_new") && name != QLatin1String("project_open") && name != QLatin1String("profiles_list") &&
            (!pCore->currentDoc() || !timeline())) {
            return Mcp::failure(QStringLiteral("No active project/sequence. Create or open a project first."));
        }
        return handler(args);
    });
}

std::shared_ptr<TimelineItemModel> McpTools::timeline()
{
    return pCore->projectManager()->getTimeline();
}

QString McpTools::outputError(const QString &path, bool overwrite)
{
    const QFileInfo file(path);
    if (path.isEmpty() || !file.isAbsolute() || file.isDir()) {
        return QStringLiteral("An absolute local file path is required.");
    }
    if (!QFileInfo(file.absolutePath()).isDir()) {
        return QStringLiteral("The output directory does not exist.");
    }
    if (file.exists() && !overwrite) {
        return QStringLiteral("The output file exists. Set overwrite=true to replace it.");
    }
    if ((file.exists() && !file.isWritable()) || !QFileInfo(file.absolutePath()).isWritable()) {
        return QStringLiteral("The output is not writable.");
    }
    return {};
}
