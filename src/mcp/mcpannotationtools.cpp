/*
    SPDX-FileCopyrightText: 2026 Kdenlive contributors
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/
#include "mcptools.h"

#include "bin/model/markerlistmodel.hpp"
#include "bin/model/subtitlemodel.hpp"
#include "bin/projectclip.h"
#include "bin/projectitemmodel.h"
#include "core.h"
#include "doc/kdenlivedoc.h"
#include "project/projectmanager.h"
#include "timeline2/model/timelineitemmodel.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

using namespace Mcp;

namespace {
std::shared_ptr<MarkerListModel> markers(const QJsonObject &args)
{
    if (!args.contains("bin_id")) {
        return pCore->projectManager()->getGuideModel();
    }
    const auto clip = pCore->projectItemModel()->getClipByBinID(args["bin_id"].toString());
    return clip && clip->statusReady() ? clip->getMarkerModel() : nullptr;
}
QJsonArray subtitleItems(const std::shared_ptr<SubtitleModel> &model)
{
    QJsonArray items;
    if (!model) {
        return items;
    }
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        items.append(QJsonObject{{"item_id", model->data(index, SubtitleModel::IdRole).toInt()},
                                 {"start", model->data(index, SubtitleModel::StartFrameRole).toInt()},
                                 {"end", model->data(index, SubtitleModel::EndFrameRole).toInt()},
                                 {"text", model->data(index, SubtitleModel::SubtitleRole).toString()},
                                 {"layer", model->data(index, SubtitleModel::LayerRole).toInt()},
                                 {"style", model->data(index, SubtitleModel::StyleNameRole).toString()}});
    }
    return items;
}
} // namespace

void McpTools::registerAnnotationTools()
{
    const auto frame = integer(0, 1000000000);

    add("markers_list", "List sequence guides, or source clip markers when bin_id is supplied. Times and range durations are project frames.",
        {{"bin_id", string()}}, {}, true, [](const QJsonObject &a) {
            const auto model = markers(a);
            if (!model) {
                return failure("Unknown marker owner.");
            }
            QJsonArray items;
            const double fps = pCore->getCurrentFps();
            for (const auto &marker : model->getAllMarkers()) {
                items.append(QJsonObject{{"frame", marker.time().frames(fps)},
                                         {"text", marker.comment()},
                                         {"category", marker.markerType()},
                                         {"duration", marker.duration().frames(fps)}});
            }
            return success({{"markers", items}, {"categories", QJsonArray::fromStringList(model->categoriesToStringList())}});
        });

    add("marker_set", "Create/update a sequence guide or bin-clip marker. Optional duration creates a range marker. Undoable.",
        {{"bin_id", string()}, {"frame", frame}, {"text", string()}, {"category", integer(-1, 999)}, {"duration", integer(0, 100000000)}}, {"frame", "text"},
        false, [](const QJsonObject &a) {
            const auto model = markers(a);
            if (!model) {
                return failure("Unknown marker owner.");
            }
            const GenTime pos(a["frame"].toInt(), pCore->getCurrentFps());
            if (a["category"].toInt(-1) >= 0 && !pCore->markerTypes.contains(a["category"].toInt())) {
                return failure("Unknown marker category. Inspect markers_list.");
            }
            if (a["duration"].toInt() > 0) {
                return booleanResult(
                    model->addRangeMarker(pos, GenTime(a["duration"].toInt(), pCore->getCurrentFps()), a["text"].toString(), a["category"].toInt(-1)));
            }
            return booleanResult(model->addMarker(pos, a["text"].toString(), a["category"].toInt(-1)));
        });

    add("marker_delete", "Delete a sequence guide or source marker at a frame. Undoable.", {{"bin_id", string()}, {"frame", frame}}, {"frame"}, false,
        [](const QJsonObject &a) {
            const auto model = markers(a);
            return model ? booleanResult(model->removeMarker(GenTime(a["frame"].toInt(), pCore->getCurrentFps()))) : failure("Unknown marker owner.");
        });

    add("marker_move", "Move a guide/source marker to another frame, preserving text, category and duration. Undoable.",
        {{"bin_id", string()}, {"frame", frame}, {"new_frame", frame}}, {"frame", "new_frame"}, false, [](const QJsonObject &a) {
            const auto model = markers(a);
            if (!model || !model->hasMarker(a["frame"].toInt())) {
                return failure("Unknown marker.");
            }
            return booleanResult(model->moveMarker(model->markerIdAtFrame(a["frame"].toInt()), GenTime(a["new_frame"].toInt(), pCore->getCurrentFps())));
        });

    add("markers_import", "Import native Kdenlive marker JSON. Use markers_export to inspect its format. ignore_conflicts controls replacement. Undoable.",
        {{"bin_id", string()}, {"json", string()}, {"ignore_conflicts", boolean()}}, {"json"}, false, [](const QJsonObject &a) {
            const auto model = markers(a);
            return model ? booleanResult(model->importFromJson(a["json"].toString(), a["ignore_conflicts"].toBool())) : failure("Unknown marker owner.");
        });

    add("markers_export", "Export sequence guides or source markers in Kdenlive's native JSON format.", {{"bin_id", string()}}, {}, true,
        [](const QJsonObject &a) {
            const auto model = markers(a);
            return model ? success({{"json", model->toJson()}}) : failure("Unknown marker owner.");
        });

    add("subtitles_list", "Read all subtitles in the active subtitle file, with timeline IDs and layers. End times are exclusive project frames.", {}, {}, true,
        [](const QJsonObject &) {
            const auto model = timeline()->getSubtitleModel();
            return success({{"subtitles", subtitleItems(model)},
                            {"locked", model && model->isLocked()},
                            {"disabled", model && model->isDisabled()},
                            {"max_layer", model ? model->getMaxLayer() : 0}});
        });

    add("subtitle_add", "Add subtitle text with exact start/end frames. Text may contain ASS override tags. End is exclusive. Undoable.",
        {{"start", frame}, {"end", frame}, {"text", string()}, {"layer", integer(0, 50)}, {"style", string()}}, {"start", "end", "text"}, false,
        [](const QJsonObject &a) {
            if (a["start"].toInt() >= a["end"].toInt()) {
                return failure("Subtitle end must exceed start.");
            }
            auto model = timeline()->getSubtitleModel();
            if (!model) {
                model = timeline()->createSubtitleModel();
            }
            if (!model || model->isLocked()) {
                return failure("Subtitles are unavailable or locked.");
            }
            const int layer = a["layer"].toInt();
            const GenTime start(a["start"].toInt(), pCore->getCurrentFps());
            const QString style = a["style"].toString(QStringLiteral("Default"));
            if (layer > model->getMaxLayer() || model->getIdForStartPos(layer, start) >= 0) {
                return failure("Unknown layer or a subtitle already starts at that frame.");
            }
            if (model->getAllSubtitleStyles().count(style) == 0) {
                return failure("Unknown subtitle style.");
            }
            const SubtitleEvent event(true, GenTime(a["end"].toInt(), pCore->getCurrentFps()), style, QString(), 0, 0, 0, QString(), a["text"].toString());
            Fun undo = []() { return true; }, redo = undo;
            if (!model->addSubtitle({layer, start}, event, undo, redo)) {
                undo();
                return failure("Subtitle insertion failed.");
            }
            pCore->pushUndo(undo, redo, QStringLiteral("Add subtitle"));
            return success({{"item_id", model->getIdForStartPos(layer, start)}});
        });

    add("subtitle_edit", "Replace subtitle text. Undoable.", {{"item_id", integer()}, {"text", string()}}, {"item_id", "text"}, false,
        [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            const int id = a["item_id"].toInt();
            if (!model || !model->hasSubtitle(id) || model->isLocked()) {
                return failure("Unknown or locked subtitle.");
            }
            model->editSubtitle(id, a["text"].toString(), model->getText(id));
            return success();
        });

    add("subtitle_move", "Move a subtitle to a timeline frame and optional layer. Undoable. Use timeline_item_resize to change duration.",
        {{"item_id", integer()}, {"position", frame}, {"layer", integer(0, 50)}}, {"item_id", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            const int id = a["item_id"].toInt();
            if (!model || !model->hasSubtitle(id)) {
                return failure("Unknown subtitle.");
            }
            const int layer = a["layer"].toInt(model->getLayerForId(id));
            if (layer > model->getMaxLayer()) {
                return failure("Unknown layer.");
            }
            return booleanResult(timeline()->requestSubtitleMove(id, layer, a["position"].toInt(), true, true, true));
        });

    add("subtitle_delete", "Delete a subtitle. Undoable.", {{"item_id", integer()}}, {"item_id"}, false, [](const QJsonObject &a) {
        if (!timeline()->isSubTitle(a["item_id"].toInt())) {
            return failure("Unknown subtitle.");
        }
        return booleanResult(timeline()->requestItemDeletion(a["item_id"].toInt()));
    });

    add("subtitle_cut", "Split a subtitle at a timeline frame in the specified layer. Undoable.", {{"layer", integer(0, 50)}, {"position", frame}},
        {"layer", "position"}, false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            if (!model || a["layer"].toInt() > model->getMaxLayer()) {
                return failure("Unknown subtitle layer.");
            }
            return booleanResult(model->cutSubtitle(a["layer"].toInt(), a["position"].toInt()));
        });

    add("subtitles_import",
        "Import a local SRT, ASS, SSA or VTT file, with an optional offset in project frames. Existing subtitle import rules apply. Undoable.",
        {{"path", string()}, {"offset", integer(-100000000, 100000000)}}, {"path"}, false, [](const QJsonObject &a) {
            const QFileInfo file(a["path"].toString());
            if (!file.isAbsolute() || !file.isFile() || file.size() > 16 * 1024 * 1024 ||
                !QStringList{QStringLiteral("srt"), QStringLiteral("ass"), QStringLiteral("ssa"), QStringLiteral("vtt")}.contains(file.suffix().toLower())) {
                return failure("Expected a local subtitle file of at most 16 MiB.");
            }
            auto model = timeline()->getSubtitleModel();
            if (!model) {
                model = timeline()->createSubtitleModel();
            }
            if (!model || model->isLocked()) {
                return failure("Subtitles are unavailable or locked.");
            }
            const int before = model->rowCount();
            model->importSubtitle(file.absoluteFilePath(), a["offset"].toInt(), true, pCore->getCurrentFps(), pCore->getCurrentFps());
            if (model->rowCount() == before) {
                return failure("No subtitles were imported (invalid file or conflicts).");
            }
            return success({{"added", model->rowCount() - before}, {"subtitles", subtitleItems(model)}});
        });

    add("subtitles_export", "Export the active subtitle file as ASS, or a frame range as SRT (end exclusive). File export is not undoable.",
        {{"path", string()}, {"overwrite", boolean()}, {"start", frame}, {"end", frame}}, {"path"}, false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            if (!model) {
                return failure("No subtitles.");
            }
            const QString path = a["path"].toString();
            const auto error = outputError(path, a["overwrite"].toBool());
            if (!error.isEmpty()) {
                return failure(error);
            }
            const int start = a["start"].toInt(), end = a["end"].toInt(timeline()->duration());
            if (start >= end) {
                return failure("Invalid export range.");
            }
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QLatin1String("srt")) {
                auto subtitles = model->getAllSubtitles();
                std::stable_sort(subtitles.begin(), subtitles.end(),
                                 [](const auto &left, const auto &right) { return left.first.second < right.first.second; });
                QByteArray data;
                int count = 0;
                const double fps = pCore->getCurrentFps();
                for (const auto &entry : subtitles) {
                    const int first = qMax(start, entry.first.second.frames(fps)), last = qMin(end, entry.second.endTime().frames(fps));
                    if (first >= last || !entry.second.isDialogue()) {
                        continue;
                    }
                    QString text = entry.second.text();
                    text.remove(QRegularExpression(QStringLiteral("\\{[^}]*\\}")));
                    text.replace(QStringLiteral("\\N"), QStringLiteral("\n"));
                    text.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
                    text.replace(QStringLiteral("\\h"), QStringLiteral(" "));
                    data +=
                        QStringLiteral("%1\n%2 --> %3\n%4\n\n")
                            .arg(++count)
                            .arg(SubtitleEvent::timeToString(GenTime(first - start, fps), 1), SubtitleEvent::timeToString(GenTime(last - start, fps), 1), text)
                            .toUtf8();
                }
                QSaveFile dest(path);
                if (!dest.open(QIODevice::WriteOnly) || dest.write(data) != data.size() || !dest.commit()) {
                    return failure("Subtitle export failed.");
                }
                return success({{"subtitles", count}, {"path", path}});
            } else if (suffix == QLatin1String("ass") && !a.contains("start") && !a.contains("end")) {
                QFile source(model->getUrl());
                QSaveFile dest(path);
                if (!source.open(QIODevice::ReadOnly) || !dest.open(QIODevice::WriteOnly)) {
                    return failure("Cannot open subtitle export files.");
                }
                const auto data = source.readAll();
                if (dest.write(data) != data.size() || !dest.commit()) {
                    return failure("Subtitle export failed.");
                }
            } else {
                return failure("Use .srt for a range or .ass for the complete subtitle file.");
            }
            return booleanResult(QFileInfo(path).isFile() && QFileInfo(path).size() > 0, "Subtitle export failed.");
        });

    add("subtitles_set_state", "Set subtitle visibility and/or lock state. Undoable.", {{"disabled", boolean()}, {"locked", boolean()}}, {}, false,
        [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            if (!model || (!a.contains("disabled") && !a.contains("locked"))) {
                return failure("No subtitle model or state fields.");
            }
            const bool oldDisabled = model->isDisabled(), oldLocked = model->isLocked();
            const bool disabled = a["disabled"].toBool(oldDisabled), locked = a["locked"].toBool(oldLocked);
            const auto apply = [model](bool d, bool l) {
                if (model->isDisabled() != d) {
                    model->switchDisabled();
                }
                if (model->isLocked() != l) {
                    model->switchLocked();
                }
                return true;
            };
            Fun undo = [apply, oldDisabled, oldLocked]() { return apply(oldDisabled, oldLocked); };
            Fun redo = [apply, disabled, locked]() { return apply(disabled, locked); };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("Set subtitle state"));
            return success();
        });

    add("subtitle_layer_create", "Create a subtitle layer at a zero-based position (append by default). Undoable.", {{"position", integer(0, 50)}}, {}, false,
        [](const QJsonObject &a) {
            auto model = timeline()->getSubtitleModel();
            if (!model) {
                model = timeline()->createSubtitleModel();
            }
            if (!model || model->isLocked() || model->getMaxLayer() >= 50 || a["position"].toInt() > model->getMaxLayer() + 1) {
                return failure("Invalid layer position or locked model.");
            }
            model->requestCreateLayer(-1, a["position"].toInt(-1));
            return success({{"max_layer", model->getMaxLayer()}});
        });

    add("subtitle_layer_delete", "Delete a subtitle layer and its subtitles. Undoable. At least one layer must remain.", {{"layer", integer(0, 50)}}, {"layer"},
        false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            if (!model || model->isLocked() || model->getMaxLayer() < 1 || a["layer"].toInt() > model->getMaxLayer()) {
                return failure("Invalid, last or locked layer.");
            }
            model->requestDeleteLayer(a["layer"].toInt());
            return success();
        });

    add("subtitle_styles_list", "List all styles in the active ASS subtitle file and the global force-style override.", {}, {}, true, [](const QJsonObject &) {
        const auto model = timeline()->getSubtitleModel();
        if (!model) {
            return failure("No subtitle model.");
        }
        QJsonArray styles;
        for (const auto &entry : model->getAllSubtitleStyles()) {
            styles.append(QJsonObject{{"name", entry.first}, {"ass", entry.second.toString(entry.first)}});
        }
        return success({{"styles", styles}, {"force_style", model->getForceStyle()}});
    });

    add("subtitle_style_set",
        "Create/replace an ASS style using a complete 'Style: ...' line with 23 fields. Inspect subtitle_styles_list for examples. Undoable.",
        {{"name", string()}, {"ass", string()}}, {"name", "ass"}, false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            const QString name = a["name"].toString(), line = a["ass"].toString();
            if (!model || model->isLocked() || name.isEmpty() || name.contains(QLatin1Char(',')) || line.count(QLatin1Char(',')) != 22 ||
                !line.startsWith(QLatin1String("Style:"))) {
                return failure("Expected a valid 23-field ASS Style line and unlocked model.");
            }
            const auto styles = model->getAllSubtitleStyles();
            const auto it = styles.find(name);
            const bool existed = it != styles.end();
            const SubtitleStyle style(line);
            const SubtitleStyle old = existed ? it->second : style;
            Fun undo = [model, name, existed, old]() {
                if (existed) {
                    model->setSubtitleStyle(name, old);
                } else {
                    model->deleteSubtitleStyle(name);
                }
                return true;
            };
            Fun redo = [model, name, style]() {
                model->setSubtitleStyle(name, style);
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("Set subtitle style"));
            return success();
        });

    add("subtitle_assign_style", "Assign an existing ASS style to a subtitle. Undoable.", {{"item_id", integer()}, {"style", string()}}, {"item_id", "style"},
        false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            const int id = a["item_id"].toInt();
            const QString style = a["style"].toString();
            if (!model || model->isLocked() || !model->hasSubtitle(id) || model->getAllSubtitleStyles().count(style) == 0) {
                return failure("Unknown subtitle/style or locked model.");
            }
            const QString old = model->getStyleName(id);
            Fun undo = [model, id, old]() {
                model->setStyleName(id, old);
                return true;
            };
            Fun redo = [model, id, style]() {
                model->setStyleName(id, style);
                return true;
            };
            redo();
            pCore->pushUndo(undo, redo, QStringLiteral("Assign subtitle style"));
            return success();
        });

    add("subtitles_force_style", "Set the libass global style override, e.g. FontName=Sans,FontSize=48. Empty clears the override. Undoable.",
        {{"style", string()}}, {"style"}, false, [](const QJsonObject &a) {
            const auto model = timeline()->getSubtitleModel();
            if (!model || model->isLocked()) {
                return failure("No subtitle model or locked model.");
            }
            model->setForceStyle(a["style"].toString());
            return success();
        });
}
