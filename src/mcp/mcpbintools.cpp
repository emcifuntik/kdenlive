/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "bin/bincommands.h"
#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "jobs/scenesplittask.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QColor>
#include <QDomDocument>
#include <QFileInfo>

using namespace Mcp;

namespace {
QString parentFolder(const QJsonObject &a)
{
    return a["folder_id"].toString(pCore->projectItemModel()->getRootFolder()->clipId());
}
QJsonObject created(const QString &id)
{
    return id.isEmpty() || id == QLatin1String("-1") ? failure("Could not create bin item.") : success({{"bin_id", id}, {"state", "loading"}});
}
bool validTitle(const QString &xml)
{
    QDomDocument doc;
    return !xml.contains(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive) && doc.setContent(xml) &&
           doc.documentElement().tagName() == QLatin1String("kdenlivetitle");
}
} // namespace

QJsonObject McpTools::clipInfo(const std::shared_ptr<ProjectClip> &clip)
{
    QJsonObject info{{"bin_id", clip->clipId()},
                     {"name", clip->name()},
                     {"folder_id", clip->lastParentId()},
                     {"ready", clip->statusReady()},
                     {"status", int(clip->clipStatus())},
                     {"description", clip->description()},
                     {"usage_count", int(clip->refCount())}};
    if (clip->statusReady()) {
        info.insert("path", clip->clipUrl());
        info.insert("duration", clip->getFramePlaytime());
        info.insert("type", int(clip->clipType()));
        info.insert("has_audio", clip->hasAudio());
        info.insert("has_video", clip->hasVideo());
        info.insert("source_fps", clip->originalFps());
        info.insert("width", clip->getFrameSize().width());
        info.insert("height", clip->getFrameSize().height());
        info.insert("proxy", clip->getProducerProperty(QStringLiteral("kdenlive:proxy")));
        info.insert("video_codec", clip->codec(false));
        info.insert("audio_codec", clip->codec(true));
    }
    return info;
}

void McpTools::registerBinTools()
{
    add("bin_list", "List bin clips, folders and subclips, with IDs and loading status. Offset/limit paginate large projects.",
        {{"offset", integer()}, {"limit", integer(1, 1000)}}, {}, true, [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            QJsonArray items;
            int count = 0;
            const int offset = a["offset"].toInt(), limit = a["limit"].toInt(200);
            std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
                for (int row = 0; row < model->rowCount(parent); ++row) {
                    const auto index = model->index(row, 0, parent);
                    const auto item = model->getBinItemByIndex(index);
                    if (count >= offset && items.size() < limit) {
                        items.append(QJsonObject{{"bin_id", item->clipId()},
                                                 {"name", item->name()},
                                                 {"folder_id", item->lastParentId()},
                                                 {"kind", int(item->itemType())},
                                                 {"ready", item->statusReady()},
                                                 {"status", int(item->clipStatus())},
                                                 {"duration", QJsonValue::fromVariant(item->getData(AbstractProjectItem::ParentDuration))}});
                    }
                    ++count;
                    visit(index);
                }
            };
            visit({});
            return success(
                {{"items", items}, {"total", count}, {"next_offset", offset + items.size() < count ? QJsonValue(offset + items.size()) : QJsonValue()}});
        });

    add("bin_get", "Inspect one clip, its source metadata and readiness. xml=true includes native producer data (including title XML).",
        {{"bin_id", string()}, {"xml", boolean()}}, {"bin_id"}, true, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip) {
                return failure("Unknown bin clip.");
            }
            auto info = clipInfo(clip);
            if (a["xml"].toBool() && clip->statusReady()) {
                QDomDocument doc;
                doc.appendChild(clip->toXml(doc, true));
                info.insert("xml", doc.toString());
            }
            return success(info);
        });

    add("bin_import", "Import a local media, image, audio, animation or playlist file. Returns immediately; poll bin_get.ready before inserting.",
        {{"path", string()}, {"folder_id", string()}}, {"path"}, false, [](const QJsonObject &a) {
            const QFileInfo file(a["path"].toString());
            const auto model = pCore->projectItemModel();
            if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) {
                return failure("Expected an existing readable absolute file path.");
            }
            if (!model->getFolderByBinId(parentFolder(a))) {
                return failure("Unknown parent folder.");
            }
            Fun undo = []() { return true; }, redo = undo;
            const QString id = ClipCreator::createClipFromFile(file.absoluteFilePath(), parentFolder(a), model, undo, redo);
            if (id.isEmpty() || id == QLatin1String("-1")) {
                undo();
                return failure("Media import failed.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Import media"));
            return created(id);
        });

    add("bin_folder_create", "Create an undoable project-bin folder.", {{"name", string()}, {"folder_id", string()}}, {"name"}, false,
        [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            if (!model->getFolderByBinId(parentFolder(a))) {
                return failure("Unknown parent folder.");
            }
            Fun undo = []() { return true; }, redo = undo;
            QString id;
            if (!model->requestAddFolder(id, a["name"].toString(), parentFolder(a), undo, redo)) {
                undo();
                return failure("Could not create folder.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Create bin folder"));
            return success({{"folder_id", id}});
        });

    add("bin_rename", "Rename a clip or folder. Undoable.", {{"bin_id", string()}, {"name", string()}}, {"bin_id", "name"}, false, [](const QJsonObject &a) {
        const auto model = pCore->projectItemModel();
        const auto item = model->getItemByBinId(a["bin_id"].toString());
        if (!item || item == model->getRootFolder()) {
            return failure("Unknown or root bin item.");
        }
        if (item->itemType() == AbstractProjectItem::FolderItem) {
            return booleanResult(model->requestRenameFolder(item, a["name"].toString()));
        }
        const auto clip = model->getClipByBinID(a["bin_id"].toString());
        if (!clip || !clip->statusReady()) {
            return failure("Expected a ready bin clip.");
        }
        const QString key = QStringLiteral("kdenlive:clipname");
        pCore->pushUndo(new EditClipCommand(pCore->bin(), clip->clipId(), {{key, clip->getProducerProperty(key)}}, {{key, a["name"].toString()}}, true));
        return success();
    });

    add("bin_move", "Move a clip or folder to another bin folder. Undoable.", {{"bin_id", string()}, {"folder_id", string()}}, {"bin_id", "folder_id"}, false,
        [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            const auto item = model->getItemByBinId(a["bin_id"].toString());
            const auto folder = model->getFolderByBinId(a["folder_id"].toString());
            if (!item || !folder || item == model->getRootFolder() || !item->parent()) {
                return failure("Invalid bin item or folder.");
            }
            for (std::shared_ptr<AbstractProjectItem> current = folder; current; current = current->parent()) {
                if (current == item) {
                    return failure("A folder cannot be moved inside itself.");
                }
            }
            if (item->itemType() == AbstractProjectItem::FolderItem) {
                pCore->pushUndo(new MoveBinFolderCommand(pCore->bin(), item->clipId(), item->parent()->clipId(), folder->clipId()));
            } else if (item->itemType() == AbstractProjectItem::ClipItem) {
                pCore->pushUndo(new MoveBinClipCommand(pCore->bin(), {{item->clipId(), {item->parent()->clipId(), folder->clipId()}}}));
            } else {
                return failure("Subclips must stay with their source clip.");
            }
            return success();
        });

    add("bin_delete", "Delete a bin clip/subclip/folder and its timeline instances. Does not delete source files. Undoable. Active sequences are protected.",
        {{"bin_id", string()}}, {"bin_id"}, false, [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            const auto item = model->getItemByBinId(a["bin_id"].toString());
            if (!item || item == model->getRootFolder()) {
                return failure("Unknown or root bin item.");
            }
            auto active = model->getSequenceClip(timeline()->uuid());
            for (std::shared_ptr<AbstractProjectItem> current = active; current; current = current->parent()) {
                if (current == item) {
                    return failure("Cannot delete the active sequence or its containing folder.");
                }
            }
            Fun undo = []() { return true; }, redo = undo;
            if (!model->requestBinClipDeletion(item, undo, redo)) {
                undo();
                return failure("Bin deletion failed.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Delete bin item"));
            return success();
        });

    add("bin_subclip_create", "Create a named source subclip. Source in/out are inclusive project frames.",
        {{"bin_id", string()}, {"in", integer()}, {"out", integer()}, {"name", string()}}, {"bin_id", "in", "out", "name"}, false, [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            const auto clip = model->getClipByBinID(a["bin_id"].toString());
            const int in = a["in"].toInt(), out = a["out"].toInt();
            if (!clip || !clip->statusReady() || in > out || out >= clip->getFramePlaytime()) {
                return failure("Invalid source clip or subclip range.");
            }
            QString id;
            if (!model->requestAddBinSubClip(id, in, out, {{QStringLiteral("name"), a["name"].toString()}}, clip->clipId())) {
                return failure("Could not create subclip.");
            }
            return success({{"bin_id", id}});
        });

    add("bin_relink", "Replace a clip's media path and reload all its timeline instances. Undoable; loading is asynchronous.",
        {{"bin_id", string()}, {"path", string()}}, {"bin_id", "path"}, false, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            const QFileInfo file(a["path"].toString());
            if (!clip || !file.isAbsolute() || !file.isFile()) {
                return failure("Unknown clip or invalid media file.");
            }
            if (clip->clipType() == ClipType::Timeline) {
                return failure("Sequence clips cannot be relinked to media files.");
            }
            const QString key = QStringLiteral("resource");
            pCore->pushUndo(new EditClipCommand(pCore->bin(), clip->clipId(), {{key, clip->getProducerProperty(key)}}, {{key, file.absoluteFilePath()}}, true));
            return success({{"bin_id", clip->clipId()}, {"state", "loading"}});
        });

    add("bin_proxy", "Enable or disable a proxy for a ready clip using the project's proxy settings. Creation is asynchronous.",
        {{"bin_id", string()}, {"enabled", boolean()}}, {"bin_id", "enabled"}, false, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip || !clip->statusReady()) {
                return failure("Expected a ready bin clip.");
            }
            if (a["enabled"].toBool() && !pCore->currentDoc()->useProxy()) {
                return failure("Enable proxy clips in project settings first.");
            }
            pCore->currentDoc()->slotProxyCurrentItem(a["enabled"].toBool(), {clip});
            return success({{"bin_id", clip->clipId()}, {"state", "requested"}});
        });

    add("bin_jobs", "Inspect background import, proxy, analysis and thumbnail job status for a bin clip.", {{"bin_id", string()}}, {"bin_id"}, true,
        [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip) {
                return failure("Unknown bin clip.");
            }
            const ObjectId owner(KdenliveObjectType::BinClip, clip->clipId().toInt(), QUuid());
            return success({{"pending", pCore->taskManager.hasPendingJob(owner)},
                            {"progress", pCore->taskManager.getJobProgressForClip(owner)},
                            {"status", int(pCore->taskManager.jobStatus(owner))},
                            {"ready", clip->statusReady()}});
        });

    add("bin_jobs_cancel", "Cancel all pending background jobs for a bin clip. Not undoable.", {{"bin_id", string()}}, {"bin_id"}, false,
        [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip) {
                return failure("Unknown bin clip.");
            }
            pCore->taskManager.discardJobs(ObjectId(KdenliveObjectType::BinClip, clip->clipId().toInt(), QUuid()));
            return success();
        });

    add("bin_detect_scenes", "Run scene detection asynchronously, creating source markers and optionally subclips. Poll bin_jobs, then markers_list.",
        {{"bin_id", string()},
         {"threshold", number(0.01, 1.)},
         {"category", integer(0, 999)},
         {"subclips", boolean()},
         {"range_markers", boolean()},
         {"minimum_interval", integer(1, 100000)}},
        {"bin_id"}, false, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip || !clip->statusReady() || !clip->hasVideo() || !QFileInfo(clip->clipUrl()).isFile()) {
                return failure("Expected a ready video file clip.");
            }
            const ObjectId owner(KdenliveObjectType::BinClip, clip->clipId().toInt(), QUuid());
            if (pCore->taskManager.hasPendingJob(owner)) {
                return failure("The clip already has pending jobs.");
            }
            if (!pCore->markerTypes.contains(a["category"].toInt(0))) {
                return failure("Unknown marker category. Inspect markers_list.");
            }
            auto *task = new SceneSplitTask(owner, a["threshold"].toDouble(0.3), a["category"].toInt(0), a["range_markers"].toBool(), a["subclips"].toBool(),
                                            a["minimum_interval"].toInt(1), clip.get());
            pCore->taskManager.startTask(owner.itemId, task);
            return success({{"bin_id", clip->clipId()}, {"state", "queued"}});
        });

    add("generator_color", "Create a solid color clip. Accepts a CSS/Qt color such as #ff0000 or #80ff0000 (ARGB). Poll bin_get before inserting.",
        {{"name", string()}, {"color", string()}, {"duration", integer(1, 100000000)}, {"folder_id", string()}}, {"name", "color", "duration"}, false,
        [](const QJsonObject &a) {
            const QColor color(a["color"].toString());
            const auto model = pCore->projectItemModel();
            if (!color.isValid() || !model->getFolderByBinId(parentFolder(a))) {
                return failure("Invalid color or parent folder.");
            }
            const QString rgba = QStringLiteral("0x%1%2%3%4")
                                     .arg(color.red(), 2, 16, QLatin1Char('0'))
                                     .arg(color.green(), 2, 16, QLatin1Char('0'))
                                     .arg(color.blue(), 2, 16, QLatin1Char('0'))
                                     .arg(color.alpha(), 2, 16, QLatin1Char('0'));
            return created(ClipCreator::createColorClip(rgba, a["duration"].toInt(), a["name"].toString(), parentFolder(a), model));
        });

    add("title_create",
        "Create a title from native kdenlivetitle XML. Supports text, rich text, shapes, images, gradients and viewport animation. Poll bin_get.",
        {{"name", string()}, {"xml", string()}, {"duration", integer(1, 100000000)}, {"folder_id", string()}}, {"name", "xml", "duration"}, false,
        [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            if (!validTitle(a["xml"].toString()) || !model->getFolderByBinId(parentFolder(a))) {
                return failure("Invalid kdenlivetitle XML or parent folder.");
            }
            return created(ClipCreator::createTitleClip({{QStringLiteral("xmldata"), a["xml"].toString()}}, a["duration"].toInt(), a["name"].toString(),
                                                        parentFolder(a), model));
        });

    add("title_update", "Replace native title XML and update timeline instances. Undoable.", {{"bin_id", string()}, {"xml", string()}}, {"bin_id", "xml"},
        false, [](const QJsonObject &a) {
            const auto clip = pCore->projectItemModel()->getClipByBinID(a["bin_id"].toString());
            if (!clip || !clip->statusReady() || clip->clipType() != ClipType::Text || !validTitle(a["xml"].toString())) {
                return failure("Expected a ready title and valid XML.");
            }
            const QString key = QStringLiteral("xmldata");
            pCore->pushUndo(new EditClipCommand(pCore->bin(), clip->clipId(), {{key, clip->getProducerProperty(key)}}, {{key, a["xml"].toString()}}, true));
            return success();
        });

    add("title_template", "Create a title from a local .kdenlivetitle template and replace its template text.",
        {{"path", string()}, {"text", string()}, {"name", string()}, {"duration", integer(1, 100000000)}, {"folder_id", string()}},
        {"path", "text", "name", "duration"}, false, [](const QJsonObject &a) {
            const QFileInfo file(a["path"].toString());
            const auto model = pCore->projectItemModel();
            if (!file.isAbsolute() || !file.isFile() || !model->getFolderByBinId(parentFolder(a))) {
                return failure("Invalid template or parent folder.");
            }
            return created(ClipCreator::createTitleTemplate(
                file.absoluteFilePath(), a["text"].toString(), a["name"].toString(), parentFolder(a), model, [](const QString &) {}, a["duration"].toInt()));
        });

    add("generator_slideshow", "Create an image sequence/slideshow from an absolute MLT image pattern (for example /media/frame%04d.png).",
        {{"path", string()},
         {"name", string()},
         {"duration", integer(1, 100000000)},
         {"frame_duration", integer(1, 100000)},
         {"loop", boolean()},
         {"crop", boolean()},
         {"folder_id", string()}},
        {"path", "name", "duration", "frame_duration"}, false, [](const QJsonObject &a) {
            const auto model = pCore->projectItemModel();
            if (!QFileInfo(a["path"].toString()).isAbsolute() || !model->getFolderByBinId(parentFolder(a))) {
                return failure("Invalid pattern or parent folder.");
            }
            return created(ClipCreator::createSlideshowClip(a["path"].toString(), a["duration"].toInt(), a["name"].toString(), parentFolder(a),
                                                            {{QStringLiteral("ttl"), QString::number(a["frame_duration"].toInt())},
                                                             {QStringLiteral("loop"), a["loop"].toBool() ? QStringLiteral("1") : QStringLiteral("0")},
                                                             {QStringLiteral("crop"), a["crop"].toBool() ? QStringLiteral("1") : QStringLiteral("0")}},
                                                            model));
        });
}
