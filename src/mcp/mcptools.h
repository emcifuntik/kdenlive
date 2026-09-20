/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#pragma once

#include "mcptoolregistry.h"
#include <QObject>
#include <QPointer>
#include <memory>

class AssetParameterModel;
class EffectStackModel;
class ProjectClip;
class QProcess;
class RenderServer;
class TimelineItemModel;

/** Editor adapters. Every mutation uses an explicit model/controller API. */
class McpTools : public QObject
{
    Q_OBJECT
public:
    explicit McpTools(RenderServer *renderServer = nullptr, QObject *parent = nullptr);
    ~McpTools() override;
    McpToolRegistry &registry();

private:
    McpToolRegistry m_registry;
    struct RenderJob
    {
        QPointer<QProcess> process;
        QJsonObject state;
        QList<QStringList> commands;
        QStringList outputs;
        QStringList finalOutputs;
        QStringList temporaryFiles;
        int step = 0;
        int rendererStatus = 0;
        int exitCode = 0;
        bool processFinished = false;
        bool crashed = false;
        bool cancelRequested = false;
    };
    QMap<QString, RenderJob> m_renderJobs;
    QPointer<RenderServer> m_renderServer;
    void add(const QString &name, const QString &description, const QJsonObject &properties, const QStringList &required, bool readOnly,
             McpToolRegistry::Handler handler);
    void registerProjectTools();
    void registerBinTools();
    void registerTimelineTools();
    void registerAssetTools();
    void registerAnnotationTools();
    void registerRenderTools();
    void startRenderStep(const QString &id);
    void finishRenderStep(const QString &id);

    static std::shared_ptr<TimelineItemModel> timeline();
    static QJsonObject projectInfo();
    static QJsonObject clipInfo(const std::shared_ptr<ProjectClip> &clip);
    static QJsonObject itemInfo(int id);
    static QJsonObject trackInfo(int id);
    static QJsonArray timelineItems(int trackId = -1);
    static QJsonObject assetInfo(const std::shared_ptr<AssetParameterModel> &model);
    static std::shared_ptr<EffectStackModel> effectStack(const QJsonObject &args);
    static std::shared_ptr<AssetParameterModel> asset(const QJsonObject &args);
    static QJsonObject targetSchema();
    static QString outputError(const QString &path, bool overwrite);
};
