/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "bin/clipcreator.hpp"
#include "bin/projectclip.h"
#include "bin/projectfolder.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/docundostack.hpp"
#include "doc/kdenlivedoc.h"
#include "profiles/profilemodel.hpp"
#include "profiles/profilerepository.hpp"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

using namespace Mcp;

QJsonObject McpTools::projectInfo()
{
    const auto &profile = pCore->getCurrentProfile();
    auto *doc = pCore->currentDoc();
    return {{"path", doc->url().toLocalFile()},
            {"modified", doc->isModified()},
            {"profile", pCore->getCurrentProfilePath()},
            {"fps", profile->fps()},
            {"fps_numerator", profile->frame_rate_num()},
            {"fps_denominator", profile->frame_rate_den()},
            {"width", profile->width()},
            {"height", profile->height()},
            {"progressive", profile->progressive()},
            {"sample_aspect_ratio", profile->sar()},
            {"display_aspect_ratio", profile->dar()},
            {"colorspace", profile->colorspace()},
            {"duration", timeline()->duration()},
            {"sequence_uuid", timeline()->uuid().toString()},
            {"audio_channels", doc->audioChannels()},
            {"undo_index", pCore->undoStack()->index()},
            {"time_unit", "frames"},
            {"notes", pCore->projectManager()->documentNotes()}};
}

void McpTools::registerProjectTools()
{
    add("project_info", "Inspect the active project, exact frame rate, dimensions, sequence, duration and undo position.", {}, {}, true,
        [](const QJsonObject &) { return success(projectInfo()); });

    add("profiles_list", "List every installed project profile. Use its path with project_new.", {}, {}, true, [](const QJsonObject &) {
        QJsonArray profiles;
        for (const auto &entry : ProfileRepository::get()->getAllProfiles()) {
            profiles.append(QJsonObject{{"description", entry.first}, {"path", entry.second}});
        }
        return success({{"profiles", profiles}});
    });

    add("project_new", "Create a project without a settings dialog. Save existing changes first, or explicitly discard them. Not undoable.",
        {{"profile", string()}, {"discard_changes", boolean()}}, {"profile"}, false, [](const QJsonObject &a) {
            if (!ProfileRepository::get()->profileExists(a["profile"].toString())) {
                return failure("Unknown profile. Use profiles_list.");
            }
            auto *manager = pCore->projectManager();
            if (pCore->currentDoc() && pCore->currentDoc()->isModified()) {
                if (!a["discard_changes"].toBool()) {
                    return failure("The project has unsaved changes. Save it or set discard_changes=true.");
                }
                if (!manager->closeCurrentDocument(false)) {
                    return failure("Could not close the current project.");
                }
            }
            manager->newFile(a["profile"].toString(), false);
            return pCore->currentDoc() && timeline() ? success(projectInfo()) : failure("Project creation failed.");
        });

    add("project_open", "Open an existing local .kdenlive project. Missing media or migration may require user interaction. Not undoable.",
        {{"path", string()}, {"discard_changes", boolean()}}, {"path"}, false, [](const QJsonObject &a) {
            const QFileInfo file(a["path"].toString());
            if (!file.isAbsolute() || !file.isFile() || file.suffix().compare(QLatin1String("kdenlive"), Qt::CaseInsensitive) != 0) {
                return failure("Expected an existing absolute .kdenlive file path.");
            }
            auto *manager = pCore->projectManager();
            if (pCore->currentDoc() && pCore->currentDoc()->isModified()) {
                if (!a["discard_changes"].toBool()) {
                    return failure("The project has unsaved changes. Save it or set discard_changes=true.");
                }
                if (!manager->closeCurrentDocument(false)) {
                    return failure("Could not close the current project.");
                }
            }
            manager->openFile(QUrl::fromLocalFile(file.absoluteFilePath()));
            if (!pCore->currentDoc() || !timeline() || QFileInfo(pCore->currentDoc()->url().toLocalFile()).canonicalFilePath() != file.canonicalFilePath()) {
                return failure("The requested project did not open.");
            }
            return success(projectInfo());
        });

    add("project_save", "Save to an absolute .kdenlive path, or save the current document if path is omitted. copy=true saves a separate copy. Not undoable.",
        {{"path", string()}, {"overwrite", boolean()}, {"copy", boolean()}}, {}, false, [](const QJsonObject &a) {
            const QString current = pCore->currentDoc()->url().toLocalFile();
            const QString path = a["path"].toString(current);
            if (QFileInfo(path).suffix().compare(QLatin1String("kdenlive"), Qt::CaseInsensitive) != 0) {
                return failure("Use a .kdenlive extension.");
            }
            const bool same = !current.isEmpty() && QFileInfo(path).absoluteFilePath() == QFileInfo(current).absoluteFilePath();
            if (same && a["copy"].toBool()) {
                return failure("A project copy needs a different output path.");
            }
            const auto error = outputError(path, same || a["overwrite"].toBool());
            if (!error.isEmpty()) {
                return failure(error);
            }
            return booleanResult(pCore->projectManager()->saveFileAs(path, true, a["copy"].toBool()), "Could not save the project.");
        });

    add("project_xml", "Get the native project XML, including all sequences and asset settings, for inspection or archival.", {}, {}, true,
        [](const QJsonObject &) { return success({{"xml", pCore->projectManager()->projectSceneList(QString()).first}}); });

    add("project_set_notes", "Replace project notes. Undoable.", {{"text", string()}}, {"text"}, false, [](const QJsonObject &a) {
        auto *manager = pCore->projectManager();
        const QString before = manager->documentNotes();
        const QString after = a["text"].toString();
        Fun undo = [manager, before]() {
            QString text = before;
            manager->setDocumentNotes(text);
            pCore->currentDoc()->setModified(true);
            return true;
        };
        Fun redo = [manager, after]() {
            QString text = after;
            manager->setDocumentNotes(text);
            pCore->currentDoc()->setModified(true);
            return true;
        };
        redo();
        pCore->pushUndo(undo, redo, QStringLiteral("Edit project notes"));
        return success();
    });

    add("history_get", "List the undo history and current position. Changes made interactively also appear here.", {}, {}, true, [](const QJsonObject &) {
        const auto stack = pCore->undoStack();
        QJsonArray commands;
        for (int i = 0; i < stack->count(); ++i) {
            commands.append(stack->text(i));
        }
        return success({{"index", stack->index()}, {"can_undo", stack->canUndo()}, {"can_redo", stack->canRedo()}, {"commands", commands}});
    });
    for (const bool undo : {true, false}) {
        add(undo ? "history_undo" : "history_redo", "Move through the shared editor history. Refresh IDs/state afterwards.", {{"steps", integer(1, 100)}}, {},
            false, [undo](const QJsonObject &a) {
                const auto stack = pCore->undoStack();
                const int steps = a["steps"].toInt(1);
                if (steps > (undo ? stack->index() : stack->count() - stack->index())) {
                    return failure("Not enough history entries.");
                }
                for (int i = 0; i < steps; ++i) {
                    if (undo) {
                        stack->undo();
                    } else {
                        stack->redo();
                    }
                }
                return success({{"undo_index", stack->index()}});
            });
    }

    add("sequences_list", "List all project sequences, including nested sequences, with bin IDs and UUIDs.", {}, {}, true, [](const QJsonObject &) {
        QJsonArray sequences;
        const auto all = pCore->projectItemModel()->getAllSequenceClips();
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            auto clip = pCore->projectItemModel()->getClipByBinID(it.value());
            sequences.append(QJsonObject{
                {"uuid", it.key().toString()}, {"bin_id", it.value()}, {"name", clip ? clip->name() : QString()}, {"active", it.key() == timeline()->uuid()}});
        }
        return success({{"sequences", sequences}});
    });

    add("sequence_create", "Create and activate an empty sequence; insert its bin ID on another timeline to nest it.",
        {{"name", string()}, {"video_tracks", integer(0, 64)}, {"audio_tracks", integer(0, 64)}}, {"name", "video_tracks", "audio_tracks"}, false,
        [](const QJsonObject &a) {
            if (a["video_tracks"].toInt() + a["audio_tracks"].toInt() == 0) {
                return failure("A sequence needs at least one track.");
            }
            const auto model = pCore->projectItemModel();
            const auto id = ClipCreator::createPlaylistClip(a["name"].toString(), {a["audio_tracks"].toInt(), a["video_tracks"].toInt()},
                                                            model->getRootFolder()->clipId(), model);
            return id.isEmpty() || id == QLatin1String("-1") ? failure("Could not create sequence.") : success({{"bin_id", id}});
        });

    add("sequence_activate", "Open and activate a sequence by UUID. Subsequent timeline tools address this sequence.", {{"uuid", string()}}, {"uuid"}, false,
        [](const QJsonObject &a) {
            const QUuid uuid(a["uuid"].toString());
            const auto model = pCore->projectItemModel();
            if (uuid.isNull() || !model->hasSequenceId(uuid)) {
                return failure("Unknown sequence UUID.");
            }
            pCore->projectManager()->openTimeline(model->getSequenceId(uuid), -1, uuid);
            pCore->projectManager()->activateDocument(uuid);
            return timeline() && timeline()->uuid() == uuid ? success(projectInfo()) : failure("Could not activate sequence.");
        });
}
