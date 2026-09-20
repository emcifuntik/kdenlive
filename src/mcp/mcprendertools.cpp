/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kthumb.h"
#include "kdenlivesettings.h"
#include "monitor/monitor.h"
#include "monitor/monitorproxy.h"
#include "project/projectmanager.h"
#include "render/renderrequest.h"
#include "render/renderserver.h"
#include "renderpresets/renderpresetrepository.hpp"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>

using namespace Mcp;

McpTools::~McpTools()
{
    bool running = false;
    for (auto &job : m_renderJobs) {
        job.cancelRequested = true;
        running = running || (job.process && job.process->state() != QProcess::NotRunning);
        if (m_renderServer && job.state["connected"].toBool() && job.step < job.outputs.size()) {
            m_renderServer->abortJob(job.outputs[job.step]);
        }
    }
    // Let the renderer receive the native abort message and stop its melt child.
    // Pending handshakes/progress also deliver cancellation during this interval.
    QElapsedTimer timeout;
    timeout.start();
    while (running && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        running = false;
        for (const auto &job : m_renderJobs) {
            if (job.process && job.process->state() != QProcess::NotRunning) {
                running = !job.process->waitForFinished(10) || running;
            }
        }
    }
    for (const auto &job : m_renderJobs) {
        if (job.process && job.process->state() != QProcess::NotRunning) {
            job.process->disconnect(this);
            job.process->kill();
            job.process->waitForFinished(1000);
        }
        for (const auto &path : job.temporaryFiles) {
            QFile::remove(path);
        }
    }
}

namespace {
QStringList renderedFiles(const QString &path)
{
    const QFileInfo output(path);
    static const QRegularExpression counter(QStringLiteral("%[0-9]*d"));
    const auto match = counter.match(output.fileName());
    if (!match.hasMatch()) {
        return output.exists() ? QStringList{output.absoluteFilePath()} : QStringList();
    }
    const auto name = output.fileName();
    const QRegularExpression pattern(QStringLiteral("^") + QRegularExpression::escape(name.left(match.capturedStart())) + QStringLiteral("[0-9]+") +
                                     QRegularExpression::escape(name.mid(match.capturedEnd())) + QStringLiteral("$"));
    QStringList result;
    for (const auto &file : output.dir().entryInfoList(QDir::Files)) {
        if (pattern.match(file.fileName()).hasMatch()) {
            result.append(file.absoluteFilePath());
        }
    }
    return result;
}

QJsonObject imageResult(const QImage &image)
{
    if (image.isNull()) {
        return failure("Could not decode frame.");
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        return failure("PNG encoding failed.");
    }
    return {{"content", QJsonArray{QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", QString::fromLatin1(png.toBase64())}}}},
            {"isError", false}};
}
} // namespace

void McpTools::startRenderStep(const QString &id)
{
    auto &job = m_renderJobs[id];
    if (job.cancelRequested || job.step >= job.commands.size()) {
        return;
    }
    auto *process = new QProcess(this);
    job.process = process;
    job.rendererStatus = 0;
    job.processFinished = false;
    job.crashed = false;
    job.state.insert("state", "starting");
    job.state.insert("connected", false);
    job.state.insert("step", job.step + 1);
    job.state.insert("steps", job.commands.size());
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::started, this, [this, id]() { m_renderJobs[id].state.insert("state", "running"); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, id, process]() {
        auto &state = m_renderJobs[id].state;
        state.insert("log", (state["log"].toString() + QString::fromUtf8(process->readAllStandardOutput())).right(32768));
    });
    connect(process, &QProcess::errorOccurred, this, [this, id, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            auto &job = m_renderJobs[id];
            job.rendererStatus = -2;
            job.processFinished = true;
            job.exitCode = -1;
            job.state.insert("error", process->errorString());
            finishRenderStep(id);
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, id](int code, QProcess::ExitStatus exitStatus) {
        auto &job = m_renderJobs[id];
        job.processFinished = true;
        job.exitCode = code;
        job.crashed = exitStatus != QProcess::NormalExit;
        job.state.insert("state", "finishing");
        const int step = job.step;
        finishRenderStep(id);
        // Process exit and local-socket completion can be delivered in either order.
        QTimer::singleShot(1500, this, [this, id, step]() {
            auto it = m_renderJobs.find(id);
            if (it != m_renderJobs.end() && it->step == step && it->processFinished && it->rendererStatus == 0 &&
                it->state["state"] == QLatin1String("finishing")) {
                it->rendererStatus = -2;
                it->state.insert("error", "Renderer exited without a completion acknowledgement.");
                finishRenderStep(id);
            }
        });
    });
    process->start(KdenliveSettings::kdenliverendererpath(), job.commands[job.step]);
}

void McpTools::finishRenderStep(const QString &id)
{
    auto &job = m_renderJobs[id];
    if (!job.processFinished || (!job.cancelRequested && !job.crashed && job.exitCode == 0 && job.rendererStatus == 0)) {
        return;
    }
    job.state.insert("exit_code", job.exitCode);
    if (job.process) {
        job.process->deleteLater();
    }
    if (job.cancelRequested || job.rendererStatus == -3) {
        job.state.insert("state", "cancelled");
    } else if (job.exitCode != 0 || job.crashed || job.rendererStatus != -1) {
        job.state.insert("state", "failed");
    } else if (++job.step < job.commands.size()) {
        startRenderStep(id);
        return;
    } else {
        bool complete = true;
        for (const auto &output : job.finalOutputs) {
            const auto files = renderedFiles(output);
            complete = complete && !files.isEmpty();
            for (const auto &file : files) {
                complete = complete && QFileInfo(file).size() > 0;
            }
        }
        job.state.insert("state", complete ? "finished" : "failed");
        if (complete) {
            job.state.insert("progress", 100);
        } else {
            job.state.insert("error", "Renderer exited without producing non-empty output files.");
        }
    }
    job.state.insert("finished_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    for (const auto &path : job.temporaryFiles) {
        QFile::remove(path);
    }
    job.temporaryFiles.clear();
}

void McpTools::registerRenderTools()
{
    auto *renderServer = m_renderServer.data();
    if (renderServer) {
        connect(renderServer, &RenderServer::renderingStarted, this, [this, renderServer](const QString &path) {
            for (auto &job : m_renderJobs) {
                if (job.step < job.outputs.size() && job.outputs[job.step] == path && job.process && job.process->state() != QProcess::NotRunning) {
                    job.state.insert("connected", true);
                    if (job.cancelRequested) {
                        renderServer->abortJob(path);
                    }
                }
            }
        });
        connect(renderServer, &RenderServer::setRenderingProgress, this, [this, renderServer](const QString &path, int progress, int frame) {
            for (auto &job : m_renderJobs) {
                if (job.step >= job.outputs.size() || job.outputs[job.step] != path || !job.process || job.process->state() == QProcess::NotRunning) {
                    continue;
                }
                job.state.insert("progress", progress);
                job.state.insert("frame", frame);
                job.state.insert("connected", true);
                if (job.cancelRequested) {
                    renderServer->abortJob(path);
                }
            }
        });
        connect(renderServer, &RenderServer::setRenderingFinished, this, [this](const QString &path, int status, const QString &error) {
            for (auto it = m_renderJobs.begin(); it != m_renderJobs.end(); ++it) {
                auto &job = it.value();
                if (job.step >= job.outputs.size() || job.outputs[job.step] != path ||
                    (job.state["state"] != QLatin1String("running") && job.state["state"] != QLatin1String("starting") &&
                     job.state["state"] != QLatin1String("cancelling") && job.state["state"] != QLatin1String("finishing"))) {
                    continue;
                }
                job.rendererStatus = status;
                if (status == -3) {
                    job.cancelRequested = true;
                }
                if (!error.isEmpty()) {
                    job.state.insert("error", error);
                }
                finishRenderStep(it.key());
            }
        });
    }

    add("monitor_get", "Inspect project and clip monitor positions, playback state and in/out zones.", {}, {}, true, [](const QJsonObject &) {
        QJsonObject result;
        for (const auto id : {Kdenlive::ProjectMonitor, Kdenlive::ClipMonitor}) {
            auto *monitor = pCore->getMonitor(id);
            if (!monitor) {
                continue;
            }
            const auto zone = monitor->getZoneInfo();
            result.insert(id == Kdenlive::ProjectMonitor ? QStringLiteral("project") : QStringLiteral("clip"),
                          QJsonObject{{"position", monitor->position()}, {"playing", monitor->isPlaying()}, {"zone_in", zone.x()}, {"zone_end", zone.y()}});
        }
        return success(result);
    });

    add("monitor_seek", "Seek the project or clip monitor to a project frame.",
        {{"monitor", enumeration({"project", "clip"})}, {"frame", integer(0, 1000000000)}}, {"monitor", "frame"}, false, [](const QJsonObject &a) {
            const auto id = a["monitor"] == QLatin1String("clip") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor;
            pCore->seekMonitor(id, a["frame"].toInt());
            return success({{"requested_frame", a["frame"]}});
        });

    add("monitor_play", "Start or pause a monitor. playing is an explicit desired state.",
        {{"monitor", enumeration({"project", "clip"})}, {"playing", boolean()}}, {"monitor", "playing"}, false, [](const QJsonObject &a) {
            auto *monitor = pCore->getMonitor(a["monitor"] == QLatin1String("clip") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
            if (!monitor) {
                return failure("Monitor is unavailable.");
            }
            if (monitor->isPlaying() != a["playing"].toBool()) {
                monitor->slotPlay();
            }
            return success();
        });

    add("monitor_set_zone", "Set a monitor's in/end zone in project frames. End is exclusive.",
        {{"monitor", enumeration({"project", "clip"})}, {"in", integer(0, 1000000000)}, {"end", integer(1, 1000000000)}}, {"monitor", "in", "end"}, false,
        [](const QJsonObject &a) {
            if (a["in"].toInt() >= a["end"].toInt()) {
                return failure("Zone end must exceed in.");
            }
            pCore->setMonitorZone(a["monitor"] == QLatin1String("clip") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor,
                                  QPoint(a["in"].toInt(), a["end"].toInt()));
            return success();
        });

    add("monitor_open_clip", "Select a ready bin clip and open it in the clip monitor.", {{"bin_id", string()}}, {"bin_id"}, false, [](const QJsonObject &a) {
        const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
        if (!clip || !clip->statusReady()) {
            return failure("Expected a ready bin clip.");
        }
        pCore->selectBinClip(clip->clipId());
        return success();
    });

    add("monitor_frame",
        "Return the currently displayed monitor frame as an MCP PNG image for visual review. Seek first; inspect monitor_get to verify position.",
        {{"monitor", enumeration({"project", "clip"})}, {"width", integer(64, 1920)}}, {"monitor"}, true, [](const QJsonObject &a) {
            auto *monitor = pCore->getMonitor(a["monitor"] == QLatin1String("clip") ? Kdenlive::ClipMonitor : Kdenlive::ProjectMonitor);
            if (!monitor) {
                return failure("Monitor is unavailable.");
            }
            const int width = a["width"].toInt(960);
            const int height = qBound(1, qRound(width / pCore->getCurrentDar()), 2160);
            return imageResult(monitor->getControllerProxy()->extractFrame(QString(), width, height));
        });

    add("bin_frame", "Decode any source clip frame as an MCP PNG image without moving the playhead. Times use project frames.",
        {{"bin_id", string()}, {"frame", integer(0, 1000000000)}, {"width", integer(64, 1920)}}, {"bin_id", "frame"}, true, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip || !clip->statusReady() || a["frame"].toInt() >= clip->getFramePlaytime()) {
                return failure("Unknown/not ready source clip or frame out of range.");
            }
            auto producer = clip->getThumbProducer();
            if (!producer || !producer->is_valid()) {
                return failure("Could not load source producer.");
            }
            const int width = a["width"].toInt(960), height = qBound(1, qRound(width / pCore->getCurrentDar()), 2160);
            return imageResult(KThumb::getFrame(producer.get(), a["frame"].toInt(), width, height));
        });

    add("timeline_frame",
        "Decode an exact frame of the active sequence, including compositing/effects, as an MCP PNG image. Uses an isolated producer and does not move the "
        "playhead.",
        {{"frame", integer(0, 1000000000)}, {"width", integer(64, 1920)}}, {"frame"}, true, [](const QJsonObject &a) {
            if (a["frame"].toInt() >= timeline()->duration()) {
                return failure("Frame lies outside the timeline.");
            }
            const auto xml = pCore->projectManager()->projectSceneList(QString(), true).first.toUtf8();
            Mlt::Producer producer(pCore->getProjectProfile(), "xml-string", xml.constData());
            if (!producer.is_valid()) {
                return failure("Could not create a timeline snapshot producer.");
            }
            const int width = a["width"].toInt(960), height = qBound(1, qRound(width / pCore->getCurrentDar()), 2160);
            return imageResult(KThumb::getFrame(&producer, a["frame"].toInt(), width, height));
        });

    add("render_presets", "List every installed export preset with codec parameters and validity.", {}, {}, true, [](const QJsonObject &) {
        QJsonArray presets;
        for (const auto &name : RenderPresetRepository::get()->getAllPresets()) {
            const auto &preset = RenderPresetRepository::get()->getPreset(name);
            auto params = preset->params();
            presets.append(QJsonObject{{"name", name},
                                       {"extension", preset->extension()},
                                       {"parameters", params.toString()},
                                       {"valid", preset->isValid()},
                                       {"error", preset->error()},
                                       {"note", preset->note()}});
        }
        return success({{"presets", presets}});
    });

    add("render_start",
        "Render the active sequence asynchronously using an installed preset or explicit MLT codec parameters. Returns a job ID; poll render_status. Export is "
        "not undoable.",
        {{"path", string()},
         {"preset", string()},
         {"parameters", string("Optional complete native MLT/avformat parameters, replacing the preset.")},
         {"overwrite", boolean()},
         {"in", integer(0, 1000000000)},
         {"out", integer(0, 1000000000)},
         {"proxy", boolean()},
         {"embed_subtitles", boolean()},
         {"two_pass", boolean()},
         {"audio_per_track", boolean()},
         {"guide_sections", boolean()},
         {"guide_category", integer(-1, 999)},
         {"overlay", string("Native render overlay expression.")}},
        {"path"}, false, [this](const QJsonObject &a) {
            const QString path = a["path"].toString();
            const auto error = outputError(path, a["overwrite"].toBool());
            if (!error.isEmpty()) {
                return failure(error);
            }
            if (timeline()->duration() <= 0 || !QFileInfo(KdenliveSettings::kdenliverendererpath()).isExecutable()) {
                return failure("Empty timeline or missing kdenlive_render executable.");
            }
            for (const auto &job : m_renderJobs) {
                if ((job.process && job.process->state() != QProcess::NotRunning) || job.state["state"] == QLatin1String("finishing")) {
                    return failure("An MCP render is already running. Wait for it to finish or cancel it.");
                }
            }
            if (m_renderJobs.size() >= 100) {
                return failure("Render history is full. Use render_forget on completed jobs.");
            }
            const int in = a["in"].toInt(), out = a["out"].toInt(timeline()->duration() - 1);
            if (in > out || out >= timeline()->duration()) {
                return failure("Invalid inclusive render bounds.");
            }
            RenderRequest request;
            if (a.contains("parameters")) {
                RenderPresetParams params;
                params.insertFromString(a["parameters"].toString(), true);
                // Output destinations are owned by RenderRequest, not by arbitrary consumer parameters.
                for (const auto &key :
                     {QStringLiteral("target"), QStringLiteral("resource"), QStringLiteral("mlt_service"), QStringLiteral("in"), QStringLiteral("out")}) {
                    if (params.contains(key)) {
                        return failure("Reserved render parameter: " + key);
                    }
                }
                if (params.isEmpty()) {
                    return failure("Render parameters cannot be empty.");
                }
                request.setPresetParams(params);
            } else {
                const QString preset = a["preset"].toString(QStringLiteral("MP4-H264/AAC"));
                if (!RenderPresetRepository::get()->presetExists(preset) || !RenderPresetRepository::get()->getPreset(preset)->isValid()) {
                    return failure("Unknown or invalid preset. Use render_presets.");
                }
                request.loadPresetParams(preset);
            }
            request.setBounds(in, out);
            request.setOutputFile(path);
            request.setProxyRendering(a["proxy"].toBool());
            request.setEmbedSubtitles(a["embed_subtitles"].toBool());
            request.setTwoPass(a["two_pass"].toBool());
            request.setAudioFilePerTrack(a["audio_per_track"].toBool());
            request.setOverlayData(a["overlay"].toString());
            if (a["guide_category"].toInt(-1) >= 0 && !pCore->markerTypes.contains(a["guide_category"].toInt())) {
                return failure("Unknown guide category.");
            }
            request.setGuideParams(pCore->projectManager()->getGuideModel(), a["guide_sections"].toBool(), a["guide_category"].toInt(-1));
            const auto jobs = request.process();
            const auto cleanup = [&jobs]() {
                for (const auto &entry : jobs) {
                    QFile::remove(entry.playlistPath);
                    if (!entry.subtitlePath.isEmpty()) {
                        QFile::remove(entry.subtitlePath);
                    }
                }
            };
            if (jobs.empty() || !request.errorMessages().isEmpty()) {
                cleanup();
                return failure("Render preparation failed: " + request.errorMessages().join(QLatin1Char('\n')));
            }
            for (const auto &entry : jobs) {
                const auto error = outputError(entry.outputFile, a["overwrite"].toBool());
                if (!error.isEmpty() || (!a["overwrite"].toBool() && !renderedFiles(entry.outputFile).isEmpty())) {
                    cleanup();
                    return failure(error.isEmpty() ? QStringLiteral("Generated export files already exist. Set overwrite=true to replace them.") : error);
                }
            }
            const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            RenderJob job;
            job.state = {{"job_id", id},
                         {"output", path},
                         {"state", "queued"},
                         {"progress", 0},
                         {"sequence_uuid", timeline()->uuid().toString()},
                         {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
            for (const auto &entry : jobs) {
                job.commands.append(RenderRequest::argsByJob(entry));
                job.outputs.append(entry.outputPath);
                if (!job.finalOutputs.contains(entry.outputFile)) {
                    job.finalOutputs.append(entry.outputFile);
                }
                job.temporaryFiles.append(entry.playlistPath);
                if (!entry.subtitlePath.isEmpty()) {
                    job.temporaryFiles.append(entry.subtitlePath);
                }
            }
            job.state.insert("outputs", QJsonArray::fromStringList(job.finalOutputs));
            m_renderJobs.insert(id, job);
            startRenderStep(id);
            return success(m_renderJobs[id].state);
        });

    add("render_status", "Read one MCP render job or list the entire MCP render history, including progress, error and bounded log output.",
        {{"job_id", string()}}, {}, true, [this](const QJsonObject &a) {
            if (a.contains("job_id")) {
                const auto it = m_renderJobs.constFind(a["job_id"].toString());
                return it == m_renderJobs.cend() ? failure("Unknown render job.") : success(it->state);
            }
            QJsonArray jobs;
            for (const auto &job : m_renderJobs) {
                jobs.append(job.state);
            }
            return success({{"jobs", jobs}});
        });

    add("render_cancel",
        "Request cancellation through Kdenlive's renderer IPC. Poll render_status until cancelled; partial output may be removed by the renderer.",
        {{"job_id", string()}}, {"job_id"}, false, [this](const QJsonObject &a) {
            auto it = m_renderJobs.find(a["job_id"].toString());
            if (it == m_renderJobs.end()) {
                return failure("Unknown render job.");
            }
            if (!it->process || it->process->state() == QProcess::NotRunning) {
                return failure("Render job is no longer running.");
            }
            auto *server = m_renderServer.data();
            if (!server) {
                return failure("Renderer IPC server is unavailable.");
            }
            it->cancelRequested = true;
            it->state.insert("state", "cancelling");
            if (it->state["connected"].toBool()) {
                server->abortJob(it->outputs[it->step]);
            }
            // If still starting, the first progress notification will deliver cancellation.
            return success(it->state);
        });

    add("render_forget", "Forget a completed MCP render's status/log, freeing a history slot. The rendered output file is preserved.", {{"job_id", string()}},
        {"job_id"}, false, [this](const QJsonObject &a) {
            auto it = m_renderJobs.find(a["job_id"].toString());
            if (it == m_renderJobs.end()) {
                return failure("Unknown render job.");
            }
            if ((it->process && it->process->state() != QProcess::NotRunning) || it->state["state"] == QLatin1String("finishing")) {
                return failure("Cannot forget a running render.");
            }
            for (const auto &path : it->temporaryFiles) {
                QFile::remove(path);
            }
            m_renderJobs.erase(it);
            return success();
        });
}
